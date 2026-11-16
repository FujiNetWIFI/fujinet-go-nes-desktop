/*
 * vidcap.cpp -- the Video tab's capture (core/include/nesvidcap.h) over
 * MesenCE.
 *
 * Three things happen per frame, on three threads, and are put together by
 * the PPU's frame number:
 *
 *   emulation thread, scanline 240 (ConsoleNotificationType::PpuFrameDone):
 *       the PPU's memory and registers as the frame left them;
 *   video decoder thread (MesenHost's frame tap): the published pixels;
 *   emulation thread, the next frame's pre-render line (EventViewerRefresh,
 *       sent just before MesenCE's event manager clears its frame): the
 *       frame's register accesses, replayed through ppumodel.c into the
 *       state each line started from. The real PPU's registers read at that
 *       moment are the next frame's seed, so a missed event can never drift
 *       the model (resyncs counts the disagreements).
 *
 * The events are MesenCE's own: NesEventManager records every PPU, APU and
 * mapper register access with its scanline and dot while a debugger exists,
 * and TakeEventSnapshot/GetEvents hand them out. Called on the emulation
 * thread (as here) the snapshot does not stop anything; called by the UI
 * thread it needs the machine parked (the live fill does that).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "pch.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "MesenHost.h"

#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/DebuggerRequest.h"
#include "Shared/NotificationManager.h"
#include "Shared/Interfaces/INotificationListener.h"
#include "Debugger/Debugger.h"
#include "Debugger/DebugTypes.h"
#include "Debugger/BaseEventManager.h"
#include "Debugger/MemoryDumper.h"
#include "NES/NesConsole.h"
#include "NES/BaseNesPpu.h"
#include "NES/NesTypes.h"
#include "NES/NesDefaultVideoFilter.h"
#include "NES/Debugger/NesEventManager.h"

extern "C" {
#include "session_internal.h"
#include "nesvidcap.h"
}
#include "ppumodel.h"

MesenHost* nessession_host(nessession* session);

namespace {

struct Slot
{
  bool used = false;
  bool done = false;
  bool pix = false, snap = false, log = false, expectLog = false;
  uint32_t frame = 0;
  std::unique_ptr<nesvid_capture> cap;
};

void configure_events(BaseEventManager* manager)
{
  NesEventViewerConfig cfg = {};
  EventViewerCategoryCfg shown = {};
  shown.Visible = true;
  shown.Color = 0xFFFFFF;
  cfg.Irq = shown;
  cfg.Nmi = shown;
  cfg.SpriteZeroHit = shown;
  cfg.MapperRegisterWrites = shown;
  cfg.Ppu2000Write = shown;
  cfg.Ppu2001Write = shown;
  cfg.Ppu2003Write = shown;
  cfg.Ppu2004Write = shown;
  cfg.Ppu2005Write = shown;
  cfg.Ppu2006Write = shown;
  cfg.Ppu2007Write = shown;
  cfg.Ppu2002Read = shown;
  cfg.Ppu2007Read = shown;
  cfg.ShowPreviousFrameEvents = false;
  manager->SetConfiguration(cfg);
}

bool is_write(MemoryOperationType type)
{
  return type == MemoryOperationType::Write || type == MemoryOperationType::DmaWrite ||
         type == MemoryOperationType::DummyWrite;
}

void convert_events(const DebugEventInfo* input, uint32_t count, std::vector<nesvid_ev>& out)
{
  out.clear();
  out.reserve(count);
  for(uint32_t item = 0; item < count; item++) {
    const DebugEventInfo& evt = input[item];
    nesvid_ev outev = {};
    outev.line = evt.Scanline;
    outev.dot = evt.Cycle;
    switch(evt.Type) {
      case DebugEventType::Register: {
        const uint32_t addr = evt.Operation.Address;
        const bool write = is_write(evt.Operation.Type);
        if(addr >= 0x2000 && addr <= 0x3FFF) {
          outev.kind = write ? NESVID_EVK_WRITE : NESVID_EVK_READ;
          outev.addr = (uint16_t)addr;
          outev.value = (uint8_t)evt.Operation.Value;
          if(write && (evt.Flags & (uint32_t)EventFlags::WithTargetMemory)) {
            if(evt.TargetMemory.MemType == MemoryType::NesSpriteRam) {
              outev.ttype = NESVID_TT_OAM;
              outev.target = (uint16_t)(evt.TargetMemory.Address & 0xFF);
            } else if(evt.TargetMemory.MemType == MemoryType::NesPpuMemory) {
              outev.ttype = NESVID_TT_PPU;
              outev.target = (uint16_t)(evt.TargetMemory.Address & 0x3FFF);
            }
          }
        } else if(addr >= 0x4020 && write) {
          outev.kind = NESVID_EVK_MAPPER;
          outev.addr = (uint16_t)addr;
          outev.value = (uint8_t)evt.Operation.Value;
        } else {
          continue;
        }
        break;
      }
      case DebugEventType::Nmi: outev.kind = NESVID_EVK_NMI; break;
      case DebugEventType::Irq: outev.kind = NESVID_EVK_IRQ; break;
      case DebugEventType::SpriteZeroHit: outev.kind = NESVID_EVK_SPRITE0; break;
      default: continue;
    }
    out.push_back(outev);
  }
}

uint8_t ctrl_of(const NesPpuState& state)
{
  return (uint8_t)(((state.TmpVideoRamAddr >> 10) & 0x03) | (state.Control.VerticalWrite ? 0x04 : 0) |
                   (state.Control.SpritePatternAddr == 0x1000 ? 0x08 : 0) |
                   (state.Control.BackgroundPatternAddr == 0x1000 ? 0x10 : 0) | (state.Control.LargeSprites ? 0x20 : 0) |
                   (state.Control.SecondaryPpu ? 0x40 : 0) | (state.Control.NmiOnVerticalBlank ? 0x80 : 0));
}

uint8_t mask_of(const NesPpuState& state)
{
  const PpuMaskFlags& flags = state.Mask;
  return (uint8_t)((flags.Grayscale ? 0x01 : 0) | (flags.BackgroundMask ? 0x02 : 0) | (flags.SpriteMask ? 0x04 : 0) |
                   (flags.BackgroundEnabled ? 0x08 : 0) | (flags.SpritesEnabled ? 0x10 : 0) | (flags.IntensifyRed ? 0x20 : 0) |
                   (flags.IntensifyGreen ? 0x40 : 0) | (flags.IntensifyBlue ? 0x80 : 0));
}

// The PPU part of a capture, read from the debugger's memory dumper.
void read_ppu(MesenHost* host, Debugger* dbg, const NesPpuState& state, nesvid_capture* cap)
{
  MemoryDumper* dumper = dbg->GetMemoryDumper();
  dumper->GetMemoryValues(MemoryType::NesPpuMemory, 0, NESVID_CHR - 1, cap->chr);
  dumper->GetMemoryValues(MemoryType::NesPpuMemory, 0x2000, 0x2FFF, cap->ntab);
  dumper->GetMemoryValues(MemoryType::NesPaletteRam, 0, 31, cap->pal);
  dumper->GetMemoryValues(MemoryType::NesSpriteRam, 0, 255, cap->oam);
  for(int item = 0; item < 64; item++) {
    cap->rgb[item] = 0xFF000000u | host->PaletteColor((uint8_t)item);
  }
  cap->ctrl = ctrl_of(state);
  cap->mask = mask_of(state);
  cap->status = (uint8_t)((state.StatusFlags.SpriteOverflow ? 0x20 : 0) | (state.StatusFlags.Sprite0Hit ? 0x40 : 0) |
                          (state.StatusFlags.VerticalBlank ? 0x80 : 0));
  cap->oam_addr = state.SpriteRamAddr;
  cap->fine_x = state.ScrollX;
  cap->wtoggle = state.WriteToggle ? 1 : 0;
  cap->vaddr = state.VideoRamAddr;
  cap->taddr = state.TmpVideoRamAddr;
  cap->nlines = (int)state.ScanlineCount;
  cap->nmi_line = (int)state.NmiScanline;
  cap->region = state.ScanlineCount <= 262 ? NESVID_NTSC : (state.NmiScanline > 250 ? NESVID_DENDY : NESVID_PAL);
  {
    NesState full = {};
    dbg->GetConsoleState(reinterpret_cast<BaseState&>(full), ConsoleType::Nes);
    cap->mirroring = (int)full.Cartridge.Mirroring;
  }
  cap->frame = state.FrameCount;
  cap->mem_valid = 1;
}

class VidCap : public INotificationListener
{
public:
  MesenHost* host;
  Emulator* emu;
  std::atomic<bool> enabled{false};
  bool installed = false;           // g_mutex

  std::mutex mtx;               // slots, latest, serial
  Slot slots[4];
  std::unique_ptr<nesvid_capture> latest;
  bool haveLatest = false;
  uint64_t serial = 0;

  std::mutex modelMtx;          // the model's seed and the counters
  bool haveStart = false;
  nesvid_model start = {};
  uint64_t batches = 0, resyncs = 0;
  std::unique_ptr<nesvid_frame_log> scratch;
  std::vector<nesvid_ev> evBuf;
  std::vector<DebugEventInfo> infoBuf;

  VidCap(MesenHost* hostp, Emulator* emup) : host(hostp), emu(emup)
  {
    latest.reset(new nesvid_capture());
    scratch.reset(new nesvid_frame_log());
    for(Slot& slot : slots) {
      slot.cap.reset(new nesvid_capture());
    }
  }

  // A tab came up after none was there: nothing the host saw before counts.
  void reset()
  {
    {
      std::lock_guard<std::mutex> lock(mtx);
      for(Slot& slot : slots) {
        slot.used = false;
      }
      haveLatest = false;
    }
    std::lock_guard<std::mutex> lock(modelMtx);
    haveStart = false;
  }

  Slot& slot_for(uint32_t frame, bool reset)
  {
    Slot& slot = slots[frame & 3];
    if(reset || !slot.used || slot.frame != frame) {
      slot.used = true;
      slot.done = slot.pix = slot.snap = slot.log = slot.expectLog = false;
      slot.frame = frame;
      memset(slot.cap.get(), 0, sizeof(nesvid_capture));
    }
    return slot;
  }

  // mtx held
  void try_complete(Slot& slot)
  {
    if(slot.done || !slot.pix || !slot.snap || (slot.expectLog && !slot.log)) {
      return;
    }
    slot.done = true;
    memcpy(latest.get(), slot.cap.get(), sizeof(nesvid_capture));
    haveLatest = true;
    latest->serial = ++serial;
  }

  // ---- video decoder thread ----
  void OnPixels(const uint32_t* argb, uint32_t width, uint32_t height, uint32_t frame)
  {
    if(!enabled || width != NESVID_W || height != NESVID_H || frame == 0xFFFFFFFFu) {
      return;
    }
    std::lock_guard<std::mutex> lock(mtx);
    Slot& slot = slot_for(frame, false);
    memcpy(slot.cap->pixels, argb, sizeof(slot.cap->pixels));
    slot.cap->fb_valid = 1;
    slot.pix = true;
    if(slot.done && haveLatest) {
      // a later repeat of the frame (a debugger redraw): the newest pixels win
      memcpy(latest->pixels, argb, sizeof(latest->pixels));
      latest->serial = ++serial;
    } else {
      try_complete(slot);
    }
  }

  // ---- emulation thread, scanline 240 ----
  void OnFrameDone()
  {
    std::shared_ptr<IConsole> console = emu->GetConsole();
    if(!console || console->GetConsoleType() != ConsoleType::Nes) {
      return;
    }
    NesConsole* nes = (NesConsole*)console.get();
    NesPpuState state = {};
    nes->GetPpu()->GetState(state);
    Debugger* dbg = emu->InternalGetDebugger();
    bool expect;
    {
      std::lock_guard<std::mutex> lock(modelMtx);
      expect = dbg && haveStart;
    }
    std::lock_guard<std::mutex> lock(mtx);
    Slot& slot = slot_for(state.FrameCount, true);
    slot.expectLog = expect;
    if(dbg) {
      read_ppu(host, dbg, state, slot.cap.get());
    }
    slot.snap = true;
    try_complete(slot);
  }

  // ---- emulation thread, pre-render line ----
  void OnStartFrame()
  {
    Debugger* dbg = emu->InternalGetDebugger();
    if(!dbg) {
      std::lock_guard<std::mutex> lock(modelMtx);
      haveStart = false;
      return;
    }
    NesPpuState truth = {};
    dbg->GetPpuState(truth, CpuType::Nes);
    BaseEventManager* manager = dbg->GetEventManager(CpuType::Nes);
    if(!manager) {
      return;
    }
    configure_events(manager);
    manager->TakeEventSnapshot(true);
    uint32_t count = manager->GetEventCount();

    std::lock_guard<std::mutex> modelLock(modelMtx);
    infoBuf.resize(count);
    manager->GetEvents(infoBuf.data(), count);
    convert_events(infoBuf.data(), count, evBuf);
    if(haveStart) {
      nesvid_model model = start;
      nesvid_model_run(&model, evBuf.data(), (int)evBuf.size(), -2, 0, scratch.get());
      batches++;
      const bool rendering = (mask_of(truth) & 0x18) != 0;
      const uint16_t mask_v = rendering ? 0x7FE0 : 0x7FFF;
      if((model.vaddr & mask_v) != (truth.VideoRamAddr & mask_v) || model.taddr != truth.TmpVideoRamAddr ||
         model.fine_x != truth.ScrollX || model.wtoggle != (truth.WriteToggle ? 1 : 0) ||
         (model.ctrl & 0xFC) != (ctrl_of(truth) & 0xFC) || model.mask != mask_of(truth)) {
        resyncs++;
      }
      if(truth.FrameCount > 0) {
        std::lock_guard<std::mutex> lock(mtx);
        Slot& slot = slot_for((uint32_t)(truth.FrameCount - 1), false);
        if(!slot.done || slot.log == false) {
          slot.cap->flog = *scratch;
          slot.cap->log_valid = 1;
          slot.cap->log_frame = truth.FrameCount - 1;
          slot.log = true;
          if(slot.done && haveLatest) {
            // the frame was published without its log (the debugger came late): add it
            latest->flog = slot.cap->flog;
            latest->log_valid = 1;
            latest->log_frame = slot.cap->log_frame;
            latest->serial = ++serial;
          } else {
            try_complete(slot);
          }
        }
      }
    }
    nesvid_model_seed(&start, ctrl_of(truth), mask_of(truth), truth.VideoRamAddr, truth.TmpVideoRamAddr,
                      truth.ScrollX, truth.WriteToggle ? 1 : 0);
    haveStart = true;
  }

  void ProcessNotification(ConsoleNotificationType type, void* parameter) override
  {
    if(!enabled) {
      return;
    }
    switch(type) {
      case ConsoleNotificationType::PpuFrameDone:
        OnFrameDone();
        break;
      case ConsoleNotificationType::EventViewerRefresh:
        if((CpuType)(intptr_t)parameter == CpuType::Nes) {
          OnStartFrame();
        }
        break;
      case ConsoleNotificationType::GameLoaded:
      case ConsoleNotificationType::GameReset:
      case ConsoleNotificationType::StateLoaded: {
        std::lock_guard<std::mutex> lock(modelMtx);
        haveStart = false;
        break;
      }
      default:
        break;
    }
  }
};

void frame_tap(void* ctx, const uint32_t* argb, uint32_t width, uint32_t height, uint32_t frame)
{
  static_cast<VidCap*>(ctx)->OnPixels(argb, width, height, frame);
}

struct Entry
{
  int count = 0;
  std::shared_ptr<VidCap> cap;
};

std::mutex g_mutex;
std::map<nessession*, Entry> g_entries;

// g_mutex held. Brings the capture up to date with the session's current host.
VidCap* ensure(nessession* session, Entry& entry)
{
  MesenHost* host = session->running ? nessession_host(session) : nullptr;
  Emulator* emu = host ? host->GetEmulator() : nullptr;
  if(!host || !emu) {
    return entry.cap.get();
  }
  if(!entry.cap || entry.cap->emu != emu || entry.cap->host != host) {
    entry.cap.reset(new VidCap(host, emu));
    emu->GetNotificationManager()->RegisterNotificationListener(entry.cap);
  }
  if(entry.count > 0 && !entry.cap->installed) {
    entry.cap->reset();
    entry.cap->enabled = true;
    entry.cap->installed = true;
    host->SetFrameTap(frame_tap, entry.cap.get());
  }
  return entry.cap.get();
}

VidCap* find(nessession* session)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  auto found = g_entries.find(session);
  if(found == g_entries.end() || !session) {
    return nullptr;
  }
  return ensure(session, found->second);
}

} // namespace

extern "C" void nesvid_host_capture_enable(nessession* session, int enable)
{
  if(!session) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  Entry& entry = g_entries[session];
  if(enable) {
    entry.count++;
    ensure(session, entry);
  } else if(entry.count > 0 && --entry.count == 0) {
    if(entry.cap) {
      entry.cap->enabled = false;
      if(entry.cap->installed && session->running) {
        MesenHost* host = nessession_host(session);
        if(host && host->GetEmulator() == entry.cap->emu) {
          host->SetFrameTap(nullptr, nullptr);
        }
      }
      entry.cap->installed = false;
    }
  }
}

extern "C" int nesvid_host_capture_get(nessession* session, nesvid_capture* dst, uint64_t* serial_inout)
{
  VidCap* vidcap = find(session);
  if(!vidcap || !dst) return 0;
  std::lock_guard<std::mutex> lock(vidcap->mtx);
  if(!vidcap->haveLatest || vidcap->serial == 0) return 0;
  if(serial_inout && *serial_inout == vidcap->serial) return 0;
  memcpy(dst, vidcap->latest.get(), sizeof(nesvid_capture));
  if(serial_inout) *serial_inout = vidcap->serial;
  return 1;
}

extern "C" void nesvid_host_capture_stats(nessession* session, uint64_t* batches, uint64_t* resyncs)
{
  VidCap* vidcap = find(session);
  if(batches) *batches = 0;
  if(resyncs) *resyncs = 0;
  if(!vidcap) return;
  std::lock_guard<std::mutex> lock(vidcap->modelMtx);
  if(batches) *batches = vidcap->batches;
  if(resyncs) *resyncs = vidcap->resyncs;
}

extern "C" int nesvid_host_capture_fill(nessession* session, nesvid_capture* dst)
{
  if(!session || !dst || !session->running) return 0;
  MesenHost* host = nessession_host(session);
  Emulator* emu = host ? host->GetEmulator() : nullptr;
  if(!emu || !emu->IsRunning()) return 0;
  DebuggerRequest req = emu->GetDebugger(false);
  Debugger* dbg = req.GetDebugger();
  if(!dbg || !dbg->IsExecutionStopped()) return 0;
  std::shared_ptr<IConsole> console = emu->GetConsole();
  if(!console || console->GetConsoleType() != ConsoleType::Nes) return 0;
  NesConsole* nes = (NesConsole*)console.get();

  NesPpuState state = {};
  dbg->GetPpuState(state, CpuType::Nes);
  NesCpuState cpu = {};
  dbg->GetCpuState(cpu, CpuType::Nes);
  read_ppu(host, dbg, state, dst);
  dst->beam_line = (int)state.Scanline;
  dst->beam_dot = (int)state.Cycle;
  dst->cycles = cpu.CycleCount;

  // the frame as the PPU is drawing it: rows above the beam are this frame's,
  // the rest still the last finished one
  {
    BaseNesPpu* ppu = nes->GetPpu();
    uint32_t full[512];
    NesDefaultVideoFilter::GetFullPalette(full, nes->GetNesConfig(), ppu->GetPpuModel());
    const uint16_t* cur = ppu->GetScreenBuffer(false, true);
    const uint16_t* prev = ppu->GetScreenBuffer(true);
    const int line = (int)state.Scanline, dot = (int)state.Cycle;
    for(int row = 0; row < NESVID_H; row++) {
      for(int col = 0; col < NESVID_W; col++) {
        const bool fresh = line >= NESVID_H || row < line || (row == line && col < dot - 1);
        const uint16_t idx = (fresh ? cur : prev)[row * NESVID_W + col] & 0x1FF;
        dst->live_pixels[row * NESVID_W + col] = 0xFF000000u | full[idx];
      }
    }
    dst->live_pixels_valid = 1;
  }

  // the frame in progress, from the events since its pre-render line
  dst->cur_valid = 0;
  memset(&dst->clog, 0, sizeof dst->clog);
  dst->clog.last_line = -1;
  {
    VidCap* vidcap = find(session);
    BaseEventManager* manager = dbg->GetEventManager(CpuType::Nes);
    if(vidcap && manager) {
      nesvid_model model;
      bool have;
      {
        std::lock_guard<std::mutex> lock(vidcap->modelMtx);
        have = vidcap->haveStart;
        model = vidcap->start;
      }
      if(have) {
        std::vector<DebugEventInfo> info;
        std::vector<nesvid_ev> evs;
        configure_events(manager);
        manager->TakeEventSnapshot(false);
        uint32_t count = manager->GetEventCount();
        info.resize(count);
        manager->GetEvents(info.data(), count);
        convert_events(info.data(), count, evs);
        nesvid_model_run(&model, evs.data(), (int)evs.size(), (int)state.Scanline, (int)state.Cycle, &dst->clog);
        dst->cur_valid = 1;
      }
    }
  }
  dst->live = 1;
  return 1;
}
