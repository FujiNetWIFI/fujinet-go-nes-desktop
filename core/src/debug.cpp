/*
 * debug.cpp -- the debugger contract (core/include/nesdebug.h) over
 * MesenCE's own debugger engine.
 *
 * Every call goes through a DebuggerRequest, Mesen's handle for using the
 * debugger from another thread (the same pattern its InteropDLL uses for
 * the C# UI). The engine exists only while a window holds it attached;
 * breakpoints and loaded labels live here, so they survive a detach, a
 * power cycle (which replaces the console and its debugger) and a reattach.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "pch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "MesenHost.h"

#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/DebuggerRequest.h"
#include "Shared/SettingTypes.h"
#include "Debugger/Breakpoint.h"
#include "Debugger/DebugTypes.h"
#include "Debugger/Debugger.h"
#include "Debugger/Disassembler.h"
#include "Debugger/ExpressionEvaluator.h"
#include "Debugger/LabelManager.h"
#include "Debugger/MemoryDumper.h"
#include "NES/NesTypes.h"
#include "NES/Mappers/Homebrew/FujiNetCart.h"
#include "NES/Mappers/Homebrew/FujiNet/fuji_mailbox.h"

extern "C" {
#include "session_internal.h"
#include "nesdebug.h"
}

MesenHost* nessession_host(nessession* s);

namespace {

struct Bp
{
  int id;
  int type;
  uint16_t start, end;
  bool enabled;
  std::string condition;
};

struct Label
{
  MemoryType type;
  uint32_t address;
  std::string name;
};

// The run-to target rides as a breakpoint with an id no user one can have.
constexpr int RunToId = 0x7FFFFFFF;

} // namespace

struct nesdebug
{
  nessession* session{nullptr};
  std::atomic<unsigned> generation{0};
  std::atomic<unsigned> stops{0};      // CodeBreak notifications seen
  std::atomic<bool> attached{false};
  std::mutex mutex;              // guards everything below
  std::string reason;
  int reasonAddr{-1};
  std::vector<Bp> bps;
  int nextId{1};
  int runTo{-1};
  std::vector<Label> labels;     // from symbol files, re-applied on attach/load
};

namespace {

Emulator* emu_of(nesdebug* d)
{
  MesenHost* h = nessession_host(d->session);
  return h ? h->GetEmulator() : nullptr;
}

bool live(nesdebug* d)
{
  return d && d->session->running && emu_of(d) && emu_of(d)->IsRunning();
}

// Run fn(Debugger*) if the engine is up; otherwise return def.
template<typename R, typename F>
R with_dbg(nesdebug* d, R def, F&& fn)
{
  if(!live(d) || !d->attached.load()) return def;
  DebuggerRequest req = emu_of(d)->GetDebugger(false);
  Debugger* dbg = req.GetDebugger();
  if(!dbg) return def;
  return fn(dbg);
}

template<typename F>
void with_dbg_void(nesdebug* d, F&& fn)
{
  if(!live(d) || !d->attached.load()) return;
  DebuggerRequest req = emu_of(d)->GetDebugger(false);
  Debugger* dbg = req.GetDebugger();
  if(dbg) fn(dbg);
}

int put(char* dst, int dstsz, const std::string& s)
{
  if(!dst || dstsz <= 0) return 0;
  return snprintf(dst, static_cast<size_t>(dstsz), "%s", s.c_str());
}

std::string hex(uint32_t v, int digits)
{
  char buf[16];
  snprintf(buf, sizeof buf, "%0*X", digits, v);
  return buf;
}

void bump(nesdebug* d)
{
  ++d->generation;
}

// Push our breakpoint list (plus a pending run-to) into the engine.
void push_breakpoints_locked(nesdebug* d, Debugger* dbg)
{
  std::vector<Breakpoint> list;
  auto add = [&](int id, int type, uint16_t start, uint16_t end, bool enabled, const std::string& cond) {
    Breakpoint b;
    int flags = 0;
    if(type & NESDEBUG_BP_EXEC) flags |= (int)BreakpointTypeFlags::Execute;
    if(type & NESDEBUG_BP_READ) flags |= (int)BreakpointTypeFlags::Read;
    if(type & NESDEBUG_BP_WRITE) flags |= (int)BreakpointTypeFlags::Write;
    b.Init((uint32_t)id, CpuType::Nes, MemoryType::NesMemory, (BreakpointTypeFlags)flags,
           start, end, enabled, false, true, cond.c_str());
    list.push_back(b);
  };
  for(const Bp& b : d->bps)
    add(b.id, b.type, b.start, b.end, b.enabled, b.condition);
  if(d->runTo >= 0)
    add(RunToId, NESDEBUG_BP_EXEC, (uint16_t)d->runTo, (uint16_t)d->runTo, true, "");
  dbg->SetBreakpoints(list.data(), (uint32_t)list.size());
}

void push_breakpoints(nesdebug* d)
{
  with_dbg_void(d, [&](Debugger* dbg) {
    std::lock_guard<std::mutex> lock(d->mutex);
    push_breakpoints_locked(d, dbg);
  });
}

// The NES's own registers and the FujiNet mailbox, named the way the
// nesdev wiki and fuji_mailbox.h name them.
void seed_labels(Debugger* dbg)
{
  LabelManager* lm = dbg->GetLabelManager();
  static const struct { uint16_t a; const char* n; } regs[] = {
    { 0x2000, "PPUCTRL" }, { 0x2001, "PPUMASK" }, { 0x2002, "PPUSTATUS" }, { 0x2003, "OAMADDR" },
    { 0x2004, "OAMDATA" }, { 0x2005, "PPUSCROLL" }, { 0x2006, "PPUADDR" }, { 0x2007, "PPUDATA" },
    { 0x4000, "SQ1_VOL" }, { 0x4001, "SQ1_SWEEP" }, { 0x4002, "SQ1_LO" }, { 0x4003, "SQ1_HI" },
    { 0x4004, "SQ2_VOL" }, { 0x4005, "SQ2_SWEEP" }, { 0x4006, "SQ2_LO" }, { 0x4007, "SQ2_HI" },
    { 0x4008, "TRI_LINEAR" }, { 0x400A, "TRI_LO" }, { 0x400B, "TRI_HI" },
    { 0x400C, "NOISE_VOL" }, { 0x400E, "NOISE_LO" }, { 0x400F, "NOISE_HI" },
    { 0x4010, "DMC_FREQ" }, { 0x4011, "DMC_RAW" }, { 0x4012, "DMC_START" }, { 0x4013, "DMC_LEN" },
    { 0x4014, "OAMDMA" }, { 0x4015, "SND_CHN" }, { 0x4016, "JOY1" }, { 0x4017, "JOY2" },
    // the mailbox: console addresses
    { FN_ARENA_BASE + FN_H_REGSEL, "FN_REGSEL" },
    { FN_ARENA_BASE + FN_H_REGSEL + FN_REG_DEVICE, "FN_REG_DEVICE" },
    { FN_ARENA_BASE + FN_H_REGSEL + FN_REG_CMD, "FN_REG_CMD" },
    { FN_ARENA_BASE + FN_H_REGSEL + FN_REG_NPARAM, "FN_REG_NPARAM" },
    { FN_ARENA_BASE + FN_H_REGSEL + FN_REG_DATA_RST, "FN_REG_DATA_RST" },
    { FN_ARENA_BASE + FN_H_REGSEL + FN_REG_RXSLICE, "FN_REG_RXSLICE" },
    { FN_ARENA_BASE + FN_H_REGSEL + FN_REG_SEQ, "FN_REG_SEQ" },
    { FN_ARENA_BASE + FN_H_REGSEL + FN_REG_BOOTLOCK, "FN_REG_BOOTLOCK" },
    { FN_ARENA_BASE + FN_H_REGSEL + FN_REG_SLICE_ACK, "FN_REG_SLICE_ACK" },
    { FN_ARENA_BASE + FN_H_REGSEL + FN_HOT_SWAP, "FN_HOT_SWAP" },
    { FN_ARENA_BASE + FN_H_DATA, "FN_TXPAGE" },
  };
  for(const auto& r : regs)
    lm->SetLabel(r.a, MemoryType::NesMemory, r.n, "");

  // The painted half lives in the cartridge's arena (mapper RAM)
  static const struct { uint16_t o; const char* n; } arena[] = {
    { FN_R_DATA, "FN_REPLY" }, { FN_R_ACKSEQ, "FN_ACKSEQ" }, { FN_R_STATUS, "FN_STATUS" },
    { FN_R_ERR, "FN_ERRCODE" }, { FN_R_REPLY_CMD, "FN_REPLYCMD" }, { FN_R_RXLEN_LO, "FN_RXLEN_LO" },
    { FN_R_RXLEN_HI, "FN_RXLEN_HI" }, { FN_R_BOOT_STATE, "FN_BOOTSTAT" }, { FN_R_BOOT_PCT, "FN_BOOTPCT" },
    { FN_R_BOOT_ERR, "FN_BOOTERR" }, { FN_R_MAGIC0, "FN_MAGIC0" }, { FN_R_MAGIC1, "FN_MAGIC1" },
    { FN_R_PROTO_VER, "FN_PROTOVER" }, { FN_R_LOAD_STATE, "FN_LOAD_STATE" }, { FN_R_LOAD_DST, "FN_LOAD_DST" },
    { FN_R_LOAD_OFF, "FN_LOAD_OFF" }, { FN_R_LOAD_SEQ, "FN_LOAD_SEQ" }, { FN_R_LOAD_PCT, "FN_LOAD_PCT" },
    { FN_R_SRAM_STATE, "FN_SRAM_STATE" }, { FN_R_DIAG_RMW, "FN_DIAG_RMW" }, { FN_R_MAPPER, "FN_MAPPER" },
    { FN_R_LINK, "FN_LINK" }, { FN_LOADER, "FN_LOADER" },
  };
  for(const auto& a : arena)
    lm->SetLabel(a.o, MemoryType::NesMapperRam, a.n, "");
}

void apply_labels_locked(nesdebug* d, Debugger* dbg)
{
  seed_labels(dbg);
  LabelManager* lm = dbg->GetLabelManager();
  for(const Label& l : d->labels)
    lm->SetLabel(l.address, l.type, l.name, "");
}

void set_debug_config(Emulator* emu)
{
  DebugConfig cfg = emu->GetSettings()->GetDebugConfig();
  cfg.ShowJumpLabels = true;
  cfg.ShowVerifiedData = true;
  cfg.ShowUnidentifiedData = true;
  cfg.DisassembleUnidentifiedData = true;
  cfg.DisassembleVerifiedData = false;
  cfg.UseLowerCaseDisassembly = false;
  cfg.NesBreakOnCpuCrash = true;
  cfg.NesBreakOnBrk = false;
  emu->GetSettings()->SetDebugConfig(cfg);
}

// A pending run-to has done its job once the machine is stopped on it.
void settle_run_to(nesdebug* d)
{
  int target;
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    target = d->runTo;
  }
  if(target < 0) return;
  const bool there = with_dbg(d, false, [&](Debugger* dbg) {
    if(!dbg->IsExecutionStopped()) return false;
    NesCpuState cpu = {};
    dbg->GetCpuState(cpu, CpuType::Nes);
    return cpu.PC == target;
  });
  if(!there) return;
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    d->runTo = -1;
  }
  push_breakpoints(d);
}

// Stepping and stopping are requests the emulation thread acts on; the
// family's debuggers treat them as synchronous (the 2600's runs Stella's
// commands on its thread and returns when done), so wait a moment for the stop to land.
// A frame or a long step-out may legitimately take longer: then the window
// sees it through the generation counter like any other stop.
void wait_for_stop(nesdebug* d, unsigned before, int timeoutMs)
{
  for(int waited = 0; waited < timeoutMs; waited += 2)
  {
    if(d->stops.load() != before) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

AddressInfo cpu_addr(uint16_t a)
{
  return AddressInfo { (int32_t)a, MemoryType::NesMemory };
}

} // namespace

// ---- lifecycle -------------------------------------------------------------

extern "C" nesdebug* nesdebug_get(nessession* s)
{
  if(!s) return nullptr;
  if(!s->debugger)
  {
    auto* d = new nesdebug();
    d->session = s;
    s->debugger = d;
  }
  return static_cast<nesdebug*>(s->debugger);
}

void nesdebug_destroy(nessession* s)
{
  if(!s || !s->debugger) return;
  delete static_cast<nesdebug*>(s->debugger);
  s->debugger = nullptr;
}

// Called on the emulation thread from the CodeBreak notification.
void nesdebug_note_stop(nessession* s, const char* msg, int addr)
{
  auto* d = nesdebug_get(s);
  if(!d) return;
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    d->reason = msg ? msg : "";
    d->reasonAddr = addr;
  }
  ++d->stops;
  bump(d);
}

void nesdebug_note_resume(nessession* s)
{
  auto* d = nesdebug_get(s);
  if(d) bump(d);
}

// A power cycle replaces the console, and Mesen re-creates its debugger for
// the new one; breakpoints and labels go back in afterwards.
void nesdebug_before_load(nessession*) { }

void nesdebug_after_load(nessession* s)
{
  auto* d = static_cast<nesdebug*>(s ? s->debugger : nullptr);
  if(!d || !d->attached.load()) return;
  with_dbg_void(d, [&](Debugger* dbg) {
    std::lock_guard<std::mutex> lock(d->mutex);
    push_breakpoints_locked(d, dbg);
    apply_labels_locked(d, dbg);
  });
  bump(d);
}

extern "C" void nesdebug_attach(nesdebug* d)
{
  if(!live(d)) return;
  Emulator* emu = emu_of(d);
  if(!d->attached.load())
  {
    set_debug_config(emu);
    // The engine only breaks for a CPU whose debug window is "open"
    emu->GetSettings()->SetDebuggerFlag(DebuggerFlags::NesDebuggerEnabled, true);
    emu->InitDebugger();
    d->attached.store(true);
    with_dbg_void(d, [&](Debugger* dbg) {
      std::lock_guard<std::mutex> lock(d->mutex);
      push_breakpoints_locked(d, dbg);
      apply_labels_locked(d, dbg);
    });
  }
  nesdebug_stop(d);
}

extern "C" void nesdebug_detach(nesdebug* d)
{
  if(!d || !d->attached.load()) return;
  with_dbg_void(d, [](Debugger* dbg) { dbg->Run(); });
  if(live(d))
  {
    Emulator* emu = emu_of(d);
    emu->GetSettings()->SetDebuggerFlag(DebuggerFlags::NesDebuggerEnabled, false);
    emu->StopDebugger();
  }
  d->attached.store(false);
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    d->runTo = -1;
    d->reason = "running";
    d->reasonAddr = -1;
  }
  bump(d);
}

extern "C" int nesdebug_is_attached(nesdebug* d)
{
  return d && d->attached.load() ? 1 : 0;
}

extern "C" int nesdebug_is_stopped(nesdebug* d)
{
  return with_dbg(d, 0, [](Debugger* dbg) { return dbg->IsExecutionStopped() ? 1 : 0; });
}

extern "C" void nesdebug_stop(nesdebug* d)
{
  if(!d) return;
  const bool already = nesdebug_is_stopped(d) != 0;
  const unsigned before = d->stops.load();
  with_dbg_void(d, [](Debugger* dbg) {
    dbg->Step(CpuType::Nes, 1, StepType::Step, BreakSource::Pause);
  });
  if(!already) wait_for_stop(d, before, 500);
}

extern "C" void nesdebug_resume(nesdebug* d)
{
  settle_run_to(d);
  with_dbg_void(d, [](Debugger* dbg) { dbg->Run(); });
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    d->reason = "running";
    d->reasonAddr = -1;
  }
  bump(d);
}

extern "C" int nesdebug_stop_reason(nesdebug* d, char* dst, int dstsz, int* address)
{
  if(!d) return 0;
  std::lock_guard<std::mutex> lock(d->mutex);
  if(address) *address = d->reasonAddr;
  return put(dst, dstsz, d->reason);
}

extern "C" unsigned nesdebug_generation(nesdebug* d)
{
  return d ? d->generation.load() : 0;
}

// ---- stepping --------------------------------------------------------------

namespace {
void step(nesdebug* d, StepType type, int count)
{
  settle_run_to(d);
  const unsigned before = d->stops.load();
  with_dbg_void(d, [&](Debugger* dbg) {
    dbg->Step(CpuType::Nes, count > 0 ? count : 1, type, BreakSource::CpuStep);
  });
  wait_for_stop(d, before, type == StepType::Step || type == StepType::StepOver ? 500 : 250);
  bump(d);
}
}

extern "C" void nesdebug_step(nesdebug* d) { step(d, StepType::Step, 1); }
extern "C" void nesdebug_step_over(nesdebug* d) { step(d, StepType::StepOver, 1); }
extern "C" void nesdebug_step_out(nesdebug* d) { step(d, StepType::StepOut, 1); }
extern "C" void nesdebug_scanline(nesdebug* d, int n) { step(d, StepType::PpuScanline, n); }
extern "C" void nesdebug_frame(nesdebug* d, int n) { step(d, StepType::PpuFrame, n); }

extern "C" void nesdebug_run_to(nesdebug* d, uint16_t addr)
{
  if(!d) return;
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    d->runTo = addr;
  }
  push_breakpoints(d);
  with_dbg_void(d, [](Debugger* dbg) { dbg->Run(); });
  bump(d);
}

// ---- CPU -------------------------------------------------------------------

extern "C" void nesdebug_cpu_get(nesdebug* d, nesdebug_cpu* out)
{
  if(!out) return;
  memset(out, 0, sizeof *out);
  out->scanline = out->dot = -1;
  with_dbg_void(d, [&](Debugger* dbg) {
    NesCpuState cpu = {};
    dbg->GetCpuState(cpu, CpuType::Nes);
    out->pc = cpu.PC; out->sp = cpu.SP; out->a = cpu.A; out->x = cpu.X; out->y = cpu.Y;
    out->ps = cpu.PS | 0x20;
    out->n = (cpu.PS >> 7) & 1; out->v = (cpu.PS >> 6) & 1; out->d = (cpu.PS >> 3) & 1;
    out->i = (cpu.PS >> 2) & 1; out->z = (cpu.PS >> 1) & 1; out->c = cpu.PS & 1;
    out->irq = cpu.IrqFlag ? 1 : 0;
    out->nmi = cpu.NmiFlag ? 1 : 0;
    out->total_cycles = cpu.CycleCount;
    NesPpuState ppu = {};
    dbg->GetPpuState(ppu, CpuType::Nes);
    out->scanline = ppu.Scanline;
    out->dot = (int)ppu.Cycle;
    out->frame = ppu.FrameCount;
  });
}

extern "C" void nesdebug_cpu_set(nesdebug* d, int reg, int value)
{
  with_dbg_void(d, [&](Debugger* dbg) {
    NesCpuState cpu = {};
    dbg->GetCpuState(cpu, CpuType::Nes);
    auto flag = [&](int bit) {
      cpu.PS = (uint8_t)(value ? (cpu.PS | (1 << bit)) : (cpu.PS & ~(1 << bit)));
    };
    switch(reg)
    {
      case NES_REG_PC: cpu.PC = (uint16_t)value; break;
      case NES_REG_SP: cpu.SP = (uint8_t)value; break;
      case NES_REG_A: cpu.A = (uint8_t)value; break;
      case NES_REG_X: cpu.X = (uint8_t)value; break;
      case NES_REG_Y: cpu.Y = (uint8_t)value; break;
      case NES_REG_PS: cpu.PS = (uint8_t)value; break;
      case NES_FLAG_N: flag(7); break;
      case NES_FLAG_V: flag(6); break;
      case NES_FLAG_D: flag(3); break;
      case NES_FLAG_I: flag(2); break;
      case NES_FLAG_Z: flag(1); break;
      case NES_FLAG_C: flag(0); break;
      default: return;
    }
    dbg->SetCpuState(cpu, CpuType::Nes);
  });
  bump(d);
}

// ---- PPU -------------------------------------------------------------------

namespace {
const char* mirroring_name(MirroringType m)
{
  switch(m)
  {
    case MirroringType::Horizontal: return "Horizontal";
    case MirroringType::Vertical: return "Vertical";
    case MirroringType::ScreenAOnly: return "Screen A";
    case MirroringType::ScreenBOnly: return "Screen B";
    case MirroringType::FourScreens: return "Four-screen";
  }
  return "?";
}

MirroringType current_mirroring(Debugger* dbg)
{
  NesState st = {};
  dbg->GetConsoleState(reinterpret_cast<BaseState&>(st), ConsoleType::Nes);
  return st.Cartridge.Mirroring;
}
}

extern "C" void nesdebug_ppu_get(nesdebug* d, nesdebug_ppu* out)
{
  if(!out) return;
  memset(out, 0, sizeof *out);
  with_dbg_void(d, [&](Debugger* dbg) {
    NesPpuState p = {};
    dbg->GetPpuState(p, CpuType::Nes);
    const PpuControlFlags& c = p.Control;
    const PpuMaskFlags& m = p.Mask;
    out->nmi_on_vblank = c.NmiOnVerticalBlank;
    out->sprite_size_16 = c.LargeSprites;
    out->bg_table_1000 = c.BackgroundPatternAddr == 0x1000;
    out->spr_table_1000 = c.SpritePatternAddr == 0x1000;
    out->increment_32 = c.VerticalWrite;
    out->ctrl = (uint8_t)((p.TmpVideoRamAddr >> 10) & 0x03)
              | (c.VerticalWrite ? 0x04 : 0) | (c.SpritePatternAddr == 0x1000 ? 0x08 : 0)
              | (c.BackgroundPatternAddr == 0x1000 ? 0x10 : 0) | (c.LargeSprites ? 0x20 : 0)
              | (c.SecondaryPpu ? 0x40 : 0) | (c.NmiOnVerticalBlank ? 0x80 : 0);
    out->show_bg = m.BackgroundEnabled;
    out->show_spr = m.SpritesEnabled;
    out->show_bg_left = m.BackgroundMask;
    out->show_spr_left = m.SpriteMask;
    out->grayscale = m.Grayscale;
    out->mask = (uint8_t)((m.Grayscale ? 0x01 : 0) | (m.BackgroundMask ? 0x02 : 0) | (m.SpriteMask ? 0x04 : 0)
              | (m.BackgroundEnabled ? 0x08 : 0) | (m.SpritesEnabled ? 0x10 : 0)
              | (m.IntensifyRed ? 0x20 : 0) | (m.IntensifyGreen ? 0x40 : 0) | (m.IntensifyBlue ? 0x80 : 0));
    out->vblank = p.StatusFlags.VerticalBlank;
    out->sprite0_hit = p.StatusFlags.Sprite0Hit;
    out->sprite_overflow = p.StatusFlags.SpriteOverflow;
    out->status = (uint8_t)((p.StatusFlags.SpriteOverflow ? 0x20 : 0) | (p.StatusFlags.Sprite0Hit ? 0x40 : 0)
                | (p.StatusFlags.VerticalBlank ? 0x80 : 0));
    out->oam_addr = p.SpriteRamAddr;
    out->vram_addr = p.VideoRamAddr;
    out->tmp_addr = p.TmpVideoRamAddr;
    out->fine_x = p.ScrollX;
    out->write_toggle = p.WriteToggle;
    out->scanline = p.Scanline;
    out->dot = (int)p.Cycle;
    out->frame = p.FrameCount;
    snprintf(out->mirroring, sizeof out->mirroring, "%s", mirroring_name(current_mirroring(dbg)));
  });
}

extern "C" uint32_t nesdebug_color(nesdebug* d, uint8_t index)
{
  MesenHost* h = d ? nessession_host(d->session) : nullptr;
  return h ? h->PaletteColor(index) : 0;
}

extern "C" void nesdebug_oam_get(nesdebug* d, nesdebug_sprite out[64])
{
  if(!out) return;
  memset(out, 0, sizeof(nesdebug_sprite) * 64);
  with_dbg_void(d, [&](Debugger* dbg) {
    uint8_t oam[256] = {};
    dbg->GetMemoryDumper()->GetMemoryValues(MemoryType::NesSpriteRam, 0, 255, oam);
    for(int i = 0; i < 64; i++)
    {
      out[i].y = oam[i * 4];
      out[i].tile = oam[i * 4 + 1];
      out[i].attr = oam[i * 4 + 2];
      out[i].x = oam[i * 4 + 3];
    }
  });
}

extern "C" int nesdebug_ppu_view(nesdebug* d, int view, int palette, uint32_t* dst,
                                 int* width, int* height)
{
  if(!dst) return 0;
  return with_dbg(d, 0, [&](Debugger* dbg) {
    MemoryDumper* md = dbg->GetMemoryDumper();
    std::vector<uint8_t> vram(0x3000);
    md->GetMemoryValues(MemoryType::NesPpuMemory, 0, 0x2FFF, vram.data());
    uint8_t pal[32] = {};
    md->GetMemoryValues(MemoryType::NesPaletteRam, 0, 31, pal);
    uint32_t rgb[32];
    for(int i = 0; i < 32; i++)
    {
      // $3F10/$3F14/$3F18/$3F1C mirror the backdrop entries
      uint8_t idx = ((i & 3) == 0) ? pal[0] : pal[i];
      rgb[i] = nesdebug_color(d, idx & 0x3F);
    }
    NesPpuState ppu = {};
    dbg->GetPpuState(ppu, CpuType::Nes);

    auto tile_px = [&](uint16_t base, int tile, int x, int y) -> int {
      uint16_t a = (uint16_t)(base + tile * 16 + y);
      int lo = (vram[a] >> (7 - x)) & 1;
      int hi = (vram[a + 8] >> (7 - x)) & 1;
      return lo | (hi << 1);
    };

    switch(view)
    {
      case NESDEBUG_VIEW_NAMETABLES: {
        const int W = 512, H = 480;
        const uint16_t bg = ppu.Control.BackgroundPatternAddr;
        for(int nt = 0; nt < 4; nt++)
        {
          const uint16_t base = (uint16_t)(0x2000 + nt * 0x400);
          const int ox = (nt & 1) * 256, oy = (nt >> 1) * 240;
          for(int row = 0; row < 30; row++)
            for(int col = 0; col < 32; col++)
            {
              const int tile = vram[base + row * 32 + col];
              const uint8_t at = vram[base + 0x3C0 + (row / 4) * 8 + col / 4];
              const int shift = ((row & 2) << 1) | (col & 2);
              const int p = (at >> shift) & 3;
              for(int y = 0; y < 8; y++)
                for(int x = 0; x < 8; x++)
                {
                  const int px = tile_px(bg, tile, x, y);
                  dst[(oy + row * 8 + y) * W + ox + col * 8 + x] = px ? rgb[p * 4 + px] : rgb[0];
                }
            }
        }
        // The scroll window, from t (where the next frame starts) and fine X
        const uint16_t t = ppu.TmpVideoRamAddr;
        const int sx = ((t & 0x1F) * 8 + ppu.ScrollX + ((t >> 10) & 1) * 256) % W;
        const int sy = ((((t >> 5) & 0x1F) * 8 + ((t >> 12) & 7)) + ((t >> 11) & 1) * 240) % H;
        const uint32_t edge = NESSESSION_ACCENT_RGB;
        for(int i = 0; i < 256; i++)
        {
          dst[sy * W + (sx + i) % W] = edge;
          dst[((sy + 239) % H) * W + (sx + i) % W] = edge;
        }
        for(int i = 0; i < 240; i++)
        {
          dst[((sy + i) % H) * W + sx] = edge;
          dst[((sy + i) % H) * W + (sx + 255) % W] = edge;
        }
        *width = W; *height = H;
        return 1;
      }

      case NESDEBUG_VIEW_PATTERNS: {
        const int W = 256, H = 128;
        const int p = std::max(0, std::min(7, palette));
        for(int table = 0; table < 2; table++)
          for(int tile = 0; tile < 256; tile++)
          {
            const int tx = table * 128 + (tile % 16) * 8, ty = (tile / 16) * 8;
            for(int y = 0; y < 8; y++)
              for(int x = 0; x < 8; x++)
              {
                const int px = tile_px((uint16_t)(table * 0x1000), tile, x, y);
                dst[(ty + y) * W + tx + x] = px ? rgb[p * 4 + px] : rgb[0];
              }
          }
        *width = W; *height = H;
        return 1;
      }

      case NESDEBUG_VIEW_SPRITES: {
        const int W = 256, H = 240;
        for(int i = 0; i < W * H; i++) dst[i] = 0x202020;
        uint8_t oam[256] = {};
        md->GetMemoryValues(MemoryType::NesSpriteRam, 0, 255, oam);
        const bool tall = ppu.Control.LargeSprites;
        // Back to front, so sprite 0 ends up on top as on the console
        for(int s = 63; s >= 0; s--)
        {
          const int sy = oam[s * 4] + 1, tileIdx = oam[s * 4 + 1], attr = oam[s * 4 + 2], sx = oam[s * 4 + 3];
          const int p = 4 + (attr & 3);
          const bool hflip = attr & 0x40, vflip = attr & 0x80;
          const int h = tall ? 16 : 8;
          for(int y = 0; y < h; y++)
            for(int x = 0; x < 8; x++)
            {
              const int yy = vflip ? h - 1 - y : y;
              const int xx = hflip ? 7 - x : x;
              uint16_t base;
              int tile;
              if(tall)
              {
                base = (tileIdx & 1) ? 0x1000 : 0x0000;
                tile = (tileIdx & 0xFE) + (yy >= 8 ? 1 : 0);
              }
              else
              {
                base = ppu.Control.SpritePatternAddr;
                tile = tileIdx;
              }
              const int px = tile_px(base, tile, xx, yy & 7);
              const int X = sx + x, Y = sy + y;
              if(px && X < W && Y < H)
                dst[Y * W + X] = rgb[p * 4 + px];
            }
        }
        *width = W; *height = H;
        return 1;
      }

      case NESDEBUG_VIEW_PALETTE: {
        const int W = 256, H = 32;
        for(int i = 0; i < 32; i++)
        {
          const int cx = (i % 16) * 16, cy = (i / 16) * 16;
          const uint32_t c = nesdebug_color(d, pal[i] & 0x3F);
          for(int y = 0; y < 16; y++)
            for(int x = 0; x < 16; x++)
              dst[(cy + y) * W + cx + x] = c;
        }
        *width = W; *height = H;
        return 1;
      }
    }
    return 0;
  });
}

// ---- APU / controllers -------------------------------------------------------

extern "C" void nesdebug_apu_get(nesdebug* d, nesdebug_apu* out)
{
  if(!out) return;
  memset(out, 0, sizeof *out);
  with_dbg_void(d, [&](Debugger* dbg) {
    NesState st = {};
    dbg->GetConsoleState(reinterpret_cast<BaseState&>(st), ConsoleType::Nes);
    const ApuState& a = st.Apu;
    out->pulse1_period = a.Square1.Period; out->pulse1_volume = a.Square1.OutputVolume;
    out->pulse1_duty = a.Square1.Duty; out->pulse1_enabled = a.Square1.Enabled;
    out->pulse2_period = a.Square2.Period; out->pulse2_volume = a.Square2.OutputVolume;
    out->pulse2_duty = a.Square2.Duty; out->pulse2_enabled = a.Square2.Enabled;
    out->triangle_period = a.Triangle.Period; out->triangle_enabled = a.Triangle.Enabled;
    out->noise_period = a.Noise.Period; out->noise_volume = a.Noise.OutputVolume;
    out->noise_enabled = a.Noise.Enabled;
    out->dmc_enabled = a.Dmc.BytesRemaining > 0; out->dmc_bytes_left = a.Dmc.BytesRemaining;
    out->frame_irq = a.FrameCounter.IrqEnabled; out->dmc_irq = a.Dmc.IrqEnabled;
  });
  MesenHost* h = d ? nessession_host(d->session) : nullptr;
  if(h)
  {
    out->pad[0] = h->GetButtons(0);
    out->pad[1] = h->GetButtons(1);
  }
  if(d && d->session->running)
  {
    out->keyboard = nessession_keyboard(d->session);
    out->tape = nessession_tape_state(d->session);
    const nes_kbd_key* keys = nullptr;
    const int n = nessession_keyboard_layout(out->keyboard, &keys);
    std::string held;
    std::vector<int> seen;
    for(int i = 0; i < n; i++)
    {
      if(std::find(seen.begin(), seen.end(), keys[i].index) != seen.end()) continue;
      seen.push_back(keys[i].index);
      if(nessession_keyboard_held(d->session, keys[i].index))
        held += (held.empty() ? "" : " ") + std::string(keys[i].label);
    }
    snprintf(out->keys_held, sizeof out->keys_held, "%s", held.c_str());
  }
}

// ---- memory ------------------------------------------------------------------

extern "C" int nesdebug_read(nesdebug* d, uint16_t addr, uint8_t* dst, int n)
{
  if(!dst || n <= 0) return 0;
  return with_dbg(d, 0, [&](Debugger* dbg) {
    MemoryDumper* md = dbg->GetMemoryDumper();
    for(int i = 0; i < n; i++)
      dst[i] = md->GetMemoryValue(MemoryType::NesMemory, (uint16_t)(addr + i), true);
    return n;
  });
}

extern "C" void nesdebug_write(nesdebug* d, uint16_t addr, uint8_t value)
{
  with_dbg_void(d, [&](Debugger* dbg) {
    // Every address from $4020 up is a register on the FujiNet cartridge,
    // where a bus write is ignored by the debugger: write the memory behind
    // the address instead.
    AddressInfo abs = dbg->GetAbsoluteAddress(cpu_addr(addr));
    if(abs.Address >= 0)
      dbg->GetMemoryDumper()->SetMemoryValue(abs.Type, (uint32_t)abs.Address, value, true);
    else
      dbg->GetMemoryDumper()->SetMemoryValue(MemoryType::NesMemory, addr, value, true);
  });
  bump(d);
}

extern "C" void nesdebug_ram_get(nesdebug* d, uint8_t out[2048])
{
  if(!out) return;
  memset(out, 0, 2048);
  with_dbg_void(d, [&](Debugger* dbg) {
    dbg->GetMemoryDumper()->GetMemoryValues(MemoryType::NesInternalRam, 0, 0x7FF, out);
  });
}

extern "C" int nesdebug_ppu_read(nesdebug* d, uint16_t addr, uint8_t* dst, int n)
{
  if(!dst || n <= 0) return 0;
  return with_dbg(d, 0, [&](Debugger* dbg) {
    MemoryDumper* md = dbg->GetMemoryDumper();
    for(int i = 0; i < n; i++)
    {
      const uint16_t a = (uint16_t)((addr + i) & 0x3FFF);
      if(a >= 0x3F00)
        dst[i] = md->GetMemoryValue(MemoryType::NesPaletteRam, a & 0x1F, true);
      else
        dst[i] = md->GetMemoryValue(MemoryType::NesPpuMemory, a, true);
    }
    return n;
  });
}

// ---- disassembly -------------------------------------------------------------

extern "C" int nesdebug_row_address(nesdebug* d, uint16_t addr, int rows)
{
  return with_dbg(d, (int)addr, [&](Debugger* dbg) {
    const int32_t a = dbg->GetDisassembler()->GetDisassemblyRowAddress(CpuType::Nes, addr, rows);
    return a < 0 ? (int)addr : (int)(a & 0xFFFF);
  });
}

extern "C" int nesdebug_disassemble(nesdebug* d, uint16_t addr, nesdebug_line* out,
                                    int max, int* pc_line)
{
  if(pc_line) *pc_line = -1;
  if(!out || max <= 0) return 0;
  return with_dbg(d, 0, [&](Debugger* dbg) {
    NesCpuState cpu = {};
    dbg->GetCpuState(cpu, CpuType::Nes);

    // Mesen's listing has rows of its own for labels, comments and block
    // boundaries; fetch enough to fill `max` instruction lines once they are
    // folded away.
    const int fetch = max * 3 + 8;
    std::vector<CodeLineData> rows((size_t)fetch);
    const uint32_t got = dbg->GetDisassembler()->GetDisassemblyOutput(CpuType::Nes, addr, rows.data(), (uint32_t)fetch);

    std::vector<Bp> bps;
    {
      std::lock_guard<std::mutex> lock(d->mutex);
      bps = d->bps;
    }

    std::string pendingLabel;
    int n = 0;
    for(uint32_t r = 0; r < got && n < max; r++)
    {
      const CodeLineData& row = rows[r];
      if(row.Flags & LineFlags::Label)
      {
        pendingLabel = row.Text;
        if(!pendingLabel.empty() && pendingLabel.back() == ':') pendingLabel.pop_back();
        continue;
      }
      if(row.Flags & (LineFlags::BlockStart | LineFlags::BlockEnd | LineFlags::Empty))
        continue;
      if((row.Flags & LineFlags::Comment) && row.Text[0] == 0)
        continue;
      if(row.Address < 0)
        continue;

      nesdebug_line& l = out[n];
      memset(&l, 0, sizeof l);
      l.address = (uint16_t)row.Address;
      l.is_pc = row.Address == cpu.PC;
      l.is_code = !(row.Flags & LineFlags::ShowAsData);
      for(const Bp& b : bps)
        if(b.enabled && (b.type & NESDEBUG_BP_EXEC) && l.address >= b.start && l.address <= b.end)
          l.has_breakpoint = 1;
      std::string bytes;
      for(int i = 0; i < row.OpSize && i < 3; i++)
        bytes += (i ? " " : "") + hex(row.ByteCode[i], 2);
      snprintf(l.bytes, sizeof l.bytes, "%s", bytes.c_str());
      snprintf(l.label, sizeof l.label, "%s", pendingLabel.c_str());
      snprintf(l.disasm, sizeof l.disasm, "%s", row.Text);
      snprintf(l.comment, sizeof l.comment, "%s", row.Comment);
      if(l.is_pc && pc_line) *pc_line = n;
      pendingLabel.clear();
      ++n;
    }
    return n;
  });
}

extern "C" int nesdebug_label_address(nesdebug* d, const char* label)
{
  if(!label || !*label) return -1;
  std::string l(label);
  return with_dbg(d, -1, [&](Debugger* dbg) {
    const int32_t a = dbg->GetLabelManager()->GetLabelRelativeAddress(l, CpuType::Nes);
    return a < 0 ? -1 : (int)(a & 0xFFFF);
  });
}

extern "C" int nesdebug_address_label(nesdebug* d, uint16_t addr, char* dst, int dstsz)
{
  if(!dst || dstsz <= 0) return 0;
  dst[0] = '\0';
  return with_dbg(d, 0, [&](Debugger* dbg) {
    return put(dst, dstsz, dbg->GetLabelManager()->GetLabel(cpu_addr(addr)));
  });
}

extern "C" int nesdebug_set_label(nesdebug* d, uint16_t addr, const char* label)
{
  if(!d) return -1;
  return with_dbg(d, -1, [&](Debugger* dbg) {
    AddressInfo abs = dbg->GetAbsoluteAddress(cpu_addr(addr));
    Label l;
    if(abs.Address >= 0) { l.type = abs.Type; l.address = (uint32_t)abs.Address; }
    else { l.type = MemoryType::NesMemory; l.address = addr; }
    l.name = label ? label : "";
    dbg->GetLabelManager()->SetLabel(l.address, l.type, l.name, "");
    std::lock_guard<std::mutex> lock(d->mutex);
    d->labels.push_back(l);
    bump(d);
    return 0;
  });
}

namespace {

// A label for CPU address `a` as the machine is mapped now.
bool add_cpu_label(Debugger* dbg, std::vector<Label>& out, uint32_t a, const std::string& name)
{
  if(name.empty() || a > 0xFFFF) return false;
  AddressInfo abs = dbg->GetAbsoluteAddress(cpu_addr((uint16_t)a));
  Label l;
  if(abs.Address >= 0) { l.type = abs.Type; l.address = (uint32_t)abs.Address; }
  else { l.type = MemoryType::NesMemory; l.address = a; }
  l.name = name;
  out.push_back(l);
  return true;
}

MemoryType mlb_type(const std::string& t)
{
  if(t == "NesPrgRom" || t == "P") return MemoryType::NesPrgRom;
  if(t == "NesInternalRam" || t == "R") return MemoryType::NesInternalRam;
  if(t == "NesWorkRam" || t == "W") return MemoryType::NesWorkRam;
  if(t == "NesSaveRam" || t == "S") return MemoryType::NesSaveRam;
  if(t == "NesMemory" || t == "G") return MemoryType::NesMemory;
  return MemoryType::None;
}

// ld65 -Ln / VICE: "al 00C123 .name" (or "al C:C123 .name")
// Mesen .mlb:      "NesPrgRom:1F23:name[:comment]"
// ca65 .dbg:       sym\tid=..,name="name",...,val=0xC123,...,type=lab
int parse_symbols(Debugger* dbg, std::istream& in, std::vector<Label>& out)
{
  int count = 0;
  std::string line;
  while(std::getline(in, line))
  {
    while(!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if(line.rfind("al ", 0) == 0)
    {
      std::istringstream ls(line.substr(3));
      std::string addr, name;
      ls >> addr >> name;
      const size_t colon = addr.find(':');
      if(colon != std::string::npos) addr = addr.substr(colon + 1);
      if(!name.empty() && name[0] == '.') name = name.substr(1);
      if(add_cpu_label(dbg, out, (uint32_t)strtoul(addr.c_str(), nullptr, 16), name)) count++;
    }
    else if(line.rfind("sym", 0) == 0 && line.find("name=\"") != std::string::npos)
    {
      const size_t n0 = line.find("name=\"") + 6;
      const size_t n1 = line.find('"', n0);
      const size_t v = line.find("val=0x");
      if(n1 == std::string::npos || v == std::string::npos) continue;
      if(line.find("type=lab") == std::string::npos) continue;
      const std::string name = line.substr(n0, n1 - n0);
      if(add_cpu_label(dbg, out, (uint32_t)strtoul(line.c_str() + v + 6, nullptr, 16), name)) count++;
    }
    else
    {
      // MemoryType:hexaddr[-hexend]:label[:comment]
      const size_t c1 = line.find(':');
      if(c1 == std::string::npos) continue;
      const size_t c2 = line.find(':', c1 + 1);
      if(c2 == std::string::npos) continue;
      const MemoryType t = mlb_type(line.substr(0, c1));
      if(t == MemoryType::None) continue;
      const uint32_t a = (uint32_t)strtoul(line.c_str() + c1 + 1, nullptr, 16);
      size_t c3 = line.find(':', c2 + 1);
      const std::string name = line.substr(c2 + 1, c3 == std::string::npos ? std::string::npos : c3 - c2 - 1);
      if(name.empty()) continue;
      out.push_back(Label{ t, a, name });
      count++;
    }
  }
  return count;
}

} // namespace

extern "C" int nesdebug_load_symbols(nesdebug* d, const char* path, char* msg, int msgsz)
{
  if(!d) return -1;
  std::vector<std::string> candidates;
  if(path && *path)
    candidates.push_back(path);
  else
  {
    std::string cart = d->session->cart_path;
    if(cart.empty())
    {
      put(msg, msgsz, "No cartridge file is open; choose a symbol file.");
      return -1;
    }
    const size_t dot = cart.find_last_of('.');
    const std::string stem = dot == std::string::npos ? cart : cart.substr(0, dot);
    for(const char* ext : { ".dbg", ".lbl", ".mlb", ".sym", ".labels" })
      candidates.push_back(stem + ext);
  }

  return with_dbg(d, -1, [&](Debugger* dbg) {
    for(const std::string& p : candidates)
    {
      std::ifstream in(p);
      if(!in) continue;
      std::vector<Label> got;
      const int n = parse_symbols(dbg, in, got);
      LabelManager* lm = dbg->GetLabelManager();
      for(const Label& l : got)
        lm->SetLabel(l.address, l.type, l.name, "");
      {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->labels.insert(d->labels.end(), got.begin(), got.end());
      }
      bump(d);
      put(msg, msgsz, std::to_string(n) + " labels loaded from " + p);
      return n;
    }
    put(msg, msgsz, candidates.size() == 1 ? "Cannot read " + candidates[0]
                                           : std::string("No symbol file next to the cartridge"));
    return -1;
  });
}

// ---- the cartridge -------------------------------------------------------------

extern "C" void nesdebug_cart_get(nesdebug* d, nesdebug_cart* out)
{
  (void)d;
  if(!out) return;
  memset(out, 0, sizeof *out);
  FujiNetCartStatus st;
  if(!FujiNetCart::GetStatus(st)) return;
  out->present = 1;
  out->link_up = st.LinkUp;
  out->busy = st.Busy;
  out->mailbox_live = st.MailboxLive;
  out->sram_enabled = st.SramEnabled;
  out->loading = st.Loading;
  out->booted_image = st.BootedImage;
  out->load_pct = st.LoadPct;
  out->mapper = st.Mapper;
  snprintf(out->mapper_name, sizeof out->mapper_name, "%s", st.MapperName);
  memcpy(out->prg_slot, st.PrgSlot, sizeof out->prg_slot);
  memcpy(out->chr_slot, st.ChrSlot, sizeof out->chr_slot);
  static const char* const mir[] = { "Vertical", "Horizontal", "Screen A", "Screen B", "Four-screen" };
  snprintf(out->mirroring, sizeof out->mirroring, "%s", st.Mirror < 5 ? mir[st.Mirror] : "?");
  out->wram_enabled = st.WramEnabled;
  out->wram_protected = st.WramProtected;
  out->chr_writable = st.ChrWritable;
  out->irq_enabled = st.IrqEnabled;
  out->irq_line = st.IrqLine;
  out->irq_latch = st.IrqLatch;
  out->irq_counter = st.IrqCounter;
  out->ackseq = st.AckSeq;
  out->status = st.Status;
  out->last_error = st.LastError;
  out->boot_state = st.BootState;
  out->boot_pct = st.BootPct;
  out->boot_err = st.BootErr;
  out->diag_rmw = st.DiagRmw;
  out->queue_depth = st.QueueDepth;
  snprintf(out->link_error, sizeof out->link_error, "%s", st.LinkError);
}

extern "C" int nesdebug_cart_info(nesdebug* d, char* dst, int dstsz)
{
  nesdebug_cart c;
  nesdebug_cart_get(d, &c);
  if(!c.present) return put(dst, dstsz, "no cartridge running");
  std::string s = "FujiNet cartridge: ";
  s += c.booted_image ? "game" : "CONFIG";
  if(c.loading)
    s += ", loading " + std::to_string(c.load_pct) + "%";
  else if(c.mapper || c.mapper_name[0])
    s += ", mapper " + std::to_string(c.mapper) + (c.mapper_name[0] ? std::string(" (") + c.mapper_name + ")" : "");
  s += c.link_up ? ", link up" : ", link down";
  if(!c.mailbox_live) s += ", mailbox closed";
  return put(dst, dstsz, s);
}

// ---- breakpoints ---------------------------------------------------------------

extern "C" int nesdebug_breakpoint_check(nesdebug* d, uint16_t addr)
{
  if(!d) return 0;
  std::lock_guard<std::mutex> lock(d->mutex);
  for(const Bp& b : d->bps)
    if((b.type & NESDEBUG_BP_EXEC) && b.start == addr && b.end == addr) return 1;
  return 0;
}

extern "C" int nesdebug_breakpoint_toggle(nesdebug* d, uint16_t addr)
{
  if(!d) return 0;
  int now = 0;
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    auto it = std::find_if(d->bps.begin(), d->bps.end(), [&](const Bp& b) {
      return (b.type & NESDEBUG_BP_EXEC) && b.start == addr && b.end == addr;
    });
    if(it != d->bps.end())
      d->bps.erase(it);
    else
    {
      d->bps.push_back(Bp{ d->nextId++, NESDEBUG_BP_EXEC, addr, addr, true, "" });
      now = 1;
    }
  }
  push_breakpoints(d);
  bump(d);
  return now;
}

extern "C" int nesdebug_breakpoint_add(nesdebug* d, int type, uint16_t start, uint16_t end,
                                       const char* condition)
{
  if(!d || !(type & (NESDEBUG_BP_EXEC | NESDEBUG_BP_READ | NESDEBUG_BP_WRITE))) return -1;
  std::string cond = condition ? condition : "";
  if(!cond.empty())
  {
    // Refuse a condition the evaluator cannot parse rather than storing one
    // that silently never matches.
    const bool ok = with_dbg(d, true, [&](Debugger* dbg) {
      EvalResultType rt = EvalResultType::Numeric;
      dbg->EvaluateExpression(cond, CpuType::Nes, rt, false);
      return rt != EvalResultType::Invalid;
    });
    if(!ok) return -1;
  }
  int id;
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    id = d->nextId++;
    d->bps.push_back(Bp{ id, type, start, std::max(start, end), true, cond });
  }
  push_breakpoints(d);
  bump(d);
  return id;
}

extern "C" void nesdebug_breakpoint_remove(nesdebug* d, int id)
{
  if(!d) return;
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    d->bps.erase(std::remove_if(d->bps.begin(), d->bps.end(), [&](const Bp& b) { return b.id == id; }),
                 d->bps.end());
  }
  push_breakpoints(d);
  bump(d);
}

extern "C" void nesdebug_breakpoint_enable(nesdebug* d, int id, int enabled)
{
  if(!d) return;
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    for(Bp& b : d->bps)
      if(b.id == id) b.enabled = enabled != 0;
  }
  push_breakpoints(d);
  bump(d);
}

extern "C" int nesdebug_breakpoint_list(nesdebug* d, nesdebug_breakpoint* out, int max)
{
  if(!d || !out || max <= 0) return 0;
  std::lock_guard<std::mutex> lock(d->mutex);
  int n = 0;
  for(const Bp& b : d->bps)
  {
    if(n >= max) break;
    nesdebug_breakpoint& o = out[n++];
    memset(&o, 0, sizeof o);
    o.id = b.id;
    o.type = b.type;
    o.start = b.start;
    o.end = b.end;
    o.enabled = b.enabled;
    snprintf(o.condition, sizeof o.condition, "%s", b.condition.c_str());
  }
  return n;
}

extern "C" void nesdebug_breakpoint_clear(nesdebug* d)
{
  if(!d) return;
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    d->bps.clear();
  }
  push_breakpoints(d);
  bump(d);
}

// ---- files ---------------------------------------------------------------------

extern "C" int nesdebug_save(nesdebug* d, const char* kind, const char* path,
                             char* msg, int msgsz)
{
  if(!d || !kind || !path || !*path) return -1;
  const std::string k(kind);
  return with_dbg(d, -1, [&](Debugger* dbg) {
    MemoryDumper* md = dbg->GetMemoryDumper();
    std::ofstream out(path, std::ios::binary);
    if(!out)
    {
      put(msg, msgsz, std::string("Cannot write ") + path);
      return -1;
    }
    FujiNetCartStatus st;
    FujiNetCart::GetStatus(st);
    auto dump = [&](MemoryType t, uint32_t limit) {
      const uint32_t size = md->GetMemorySize(t);
      const uint32_t n = limit ? std::min(limit, size) : size;
      std::vector<uint8_t> buf(size);
      md->GetMemoryState(t, buf.data());
      out.write(reinterpret_cast<const char*>(buf.data()), n);
      return n;
    };
    uint32_t n = 0;
    if(k == "prg") n = dump(MemoryType::NesPrgRom, st.PrgSize);
    else if(k == "chr") n = dump(MemoryType::NesChrRam, st.ChrSize);
    else if(k == "ram") n = dump(MemoryType::NesInternalRam, 0);
    else if(k == "wram") n = dump(MemoryType::NesWorkRam, 0x2000);
    else if(k == "dis")
    {
      std::vector<CodeLineData> rows(4096);
      uint32_t addr = 0x8000;
      std::string pendingLabel;
      while(addr <= 0xFFFF)
      {
        const uint32_t got = dbg->GetDisassembler()->GetDisassemblyOutput(CpuType::Nes, addr, rows.data(), (uint32_t)rows.size());
        if(!got) break;
        uint32_t last = addr;
        bool wrapped = false;
        for(uint32_t r = 0; r < got; r++)
        {
          const CodeLineData& row = rows[r];
          if(row.Address >= 0 && (uint32_t)row.Address < addr) { wrapped = true; break; }
          if(row.Flags & LineFlags::Label) { out << row.Text << "\n"; continue; }
          if(row.Flags & (LineFlags::BlockStart | LineFlags::BlockEnd | LineFlags::Empty)) continue;
          if(row.Address < 0) continue;
          std::string bytes;
          for(int i = 0; i < row.OpSize && i < 3; i++) bytes += hex(row.ByteCode[i], 2) + " ";
          char line[1200];
          snprintf(line, sizeof line, "%04X  %-9s  %s%s%s\n", row.Address, bytes.c_str(), row.Text,
                   row.Comment[0] ? "  " : "", row.Comment);
          out << line;
          last = (uint32_t)row.Address + std::max<uint32_t>(1, row.OpSize);
          n++;
        }
        if(wrapped || last <= addr) break;
        addr = last;
      }
    }
    else
    {
      put(msg, msgsz, "unknown save kind");
      return -1;
    }
    put(msg, msgsz, k == "dis" ? std::to_string(n) + " lines written to " + path
                               : std::to_string(n) + " bytes written to " + path);
    return 0;
  });
}

// ---- the prompt ----------------------------------------------------------------

namespace {

bool parse_addr(nesdebug* d, const std::string& tok, uint32_t& out)
{
  if(tok.empty()) return false;
  std::string t = tok;
  if(t[0] == '$') t = t.substr(1);
  else if(t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t = t.substr(2);
  else if(!isxdigit((unsigned char)t[0]) || t.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
  {
    const int a = nesdebug_label_address(d, tok.c_str());
    if(a < 0) return false;
    out = (uint32_t)a;
    return true;
  }
  char* end = nullptr;
  out = (uint32_t)strtoul(t.c_str(), &end, 16);
  return end && *end == '\0';
}

std::string hexdump(const uint8_t* data, int n, uint32_t base)
{
  std::string s;
  for(int i = 0; i < n; i += 16)
  {
    s += hex(base + i, 4) + ":";
    for(int j = 0; j < 16 && i + j < n; j++) s += " " + hex(data[i + j], 2);
    s += "\n";
  }
  return s;
}

const char* const Commands[] = {
  "help", "step", "over", "out", "frame", "scanline", "run", "stop", "runto",
  "break", "bpr", "bpw", "delete", "clear", "breaks", "print", "mem", "poke",
  "ppu", "label", "labels", "disasm", "cart", "regs",
};

std::string help_text()
{
  return
    "step [n]             one (or n) instructions (F7)\n"
    "over                 step over a JSR (F8)\n"
    "out                  run to the RTS/RTI (Shift+F8)\n"
    "frame [n]            run n frames;  scanline [n]  n scanlines\n"
    "run / stop           resume (F5) / stop\n"
    "runto <addr>         run until PC reaches addr\n"
    "break <addr> [cond]  execute breakpoint (toggles with no condition)\n"
    "bpr|bpw <a>[-<b>] [cond]  read / write breakpoint on a range\n"
    "delete <id>          remove a breakpoint;  clear  remove all;  breaks  list\n"
    "print <expr>         evaluate a Mesen expression (a x y sp pc ps cycle\n"
    "                     scanline frame [$addr] {$addr16} labels ...)\n"
    "regs                 CPU registers\n"
    "mem <addr> [len]     CPU memory;  ppu <addr> [len]  PPU memory\n"
    "poke <addr> <val>    write memory (RAM, WRAM, cartridge SRAMs)\n"
    "label <addr> <name>  name an address;  labels <file>  load a symbol file\n"
    "disasm [addr] [n]    disassemble\n"
    "cart                 the FujiNet cartridge's state\n";
}

std::string bp_type_name(int t)
{
  std::string s;
  if(t & NESDEBUG_BP_EXEC) s += "x";
  if(t & NESDEBUG_BP_READ) s += "r";
  if(t & NESDEBUG_BP_WRITE) s += "w";
  return s;
}

} // namespace

extern "C" int nesdebug_command(nesdebug* d, const char* command, char* dst, int dstsz)
{
  if(!d || !command) return put(dst, dstsz, "");
  if(!live(d)) return put(dst, dstsz, "the emulator is not running");
  if(!d->attached.load()) nesdebug_attach(d);

  std::string line(command);
  while(!line.empty() && isspace((unsigned char)line.back())) line.pop_back();
  std::istringstream in(line);
  std::string cmd;
  in >> cmd;
  for(char& c : cmd) c = (char)tolower((unsigned char)c);
  std::vector<std::string> args;
  for(std::string a; in >> a;) args.push_back(a);
  auto rest_from = [&](size_t idx) {
    std::string r;
    for(size_t i = idx; i < args.size(); i++) r += (i > idx ? " " : "") + args[i];
    return r;
  };
  auto count_arg = [&](size_t idx, int def) {
    return idx < args.size() ? std::max(1, atoi(args[idx].c_str())) : def;
  };

  std::string out;
  if(cmd.empty()) out = "";
  else if(cmd == "help" || cmd == "?") out = help_text();
  else if(cmd == "step" || cmd == "s") { step(d, StepType::Step, count_arg(0, 1)); out = "ok"; }
  else if(cmd == "over" || cmd == "o" || cmd == "trace") { nesdebug_step_over(d); out = "ok"; }
  else if(cmd == "out") { nesdebug_step_out(d); out = "ok"; }
  else if(cmd == "frame") { nesdebug_frame(d, count_arg(0, 1)); out = "ok"; }
  else if(cmd == "scanline") { nesdebug_scanline(d, count_arg(0, 1)); out = "ok"; }
  else if(cmd == "run" || cmd == "go" || cmd == "g") { nesdebug_resume(d); out = "running"; }
  else if(cmd == "stop" || (cmd == "break" && args.empty())) { nesdebug_stop(d); out = "stopped"; }
  else if(cmd == "runto")
  {
    uint32_t a;
    if(args.empty() || !parse_addr(d, args[0], a)) out = "usage: runto <addr>";
    else { nesdebug_run_to(d, (uint16_t)a); out = "running to $" + hex(a, 4); }
  }
  else if(cmd == "break" || cmd == "bp")
  {
    uint32_t a;
    if(!parse_addr(d, args[0], a)) out = "bad address: " + args[0];
    else if(args.size() == 1)
      out = nesdebug_breakpoint_toggle(d, (uint16_t)a) ? "breakpoint set at $" + hex(a, 4)
                                                     : "breakpoint cleared at $" + hex(a, 4);
    else
    {
      const int id = nesdebug_breakpoint_add(d, NESDEBUG_BP_EXEC, (uint16_t)a, (uint16_t)a, rest_from(1).c_str());
      out = id < 0 ? "condition does not parse" : "breakpoint " + std::to_string(id) + " at $" + hex(a, 4);
    }
  }
  else if(cmd == "bpr" || cmd == "bpw")
  {
    if(args.empty()) out = "usage: " + cmd + " <addr>[-<end>] [condition]";
    else
    {
      const std::string range = args[0];
      const size_t dash = range.find('-');
      uint32_t a, b;
      if(!parse_addr(d, range.substr(0, dash), a)) out = "bad address: " + range;
      else
      {
        b = a;
        if(dash != std::string::npos && !parse_addr(d, range.substr(dash + 1), b)) b = a;
        const int id = nesdebug_breakpoint_add(d, cmd == "bpr" ? NESDEBUG_BP_READ : NESDEBUG_BP_WRITE,
                                               (uint16_t)a, (uint16_t)b, rest_from(1).c_str());
        out = id < 0 ? "condition does not parse"
                     : "breakpoint " + std::to_string(id) + " (" + (cmd == "bpr" ? "read" : "write")
                       + ") on $" + hex(a, 4) + (b != a ? "-$" + hex(b, 4) : "");
      }
    }
  }
  else if(cmd == "delete" || cmd == "del")
  {
    if(args.empty()) out = "usage: delete <id>";
    else { nesdebug_breakpoint_remove(d, atoi(args[0].c_str())); out = "ok"; }
  }
  else if(cmd == "clear") { nesdebug_breakpoint_clear(d); out = "all breakpoints cleared"; }
  else if(cmd == "breaks" || cmd == "list")
  {
    nesdebug_breakpoint list[256];
    const int n = nesdebug_breakpoint_list(d, list, 256);
    if(!n) out = "no breakpoints";
    for(int i = 0; i < n; i++)
    {
      out += std::to_string(list[i].id) + ": " + bp_type_name(list[i].type) + " $" + hex(list[i].start, 4);
      if(list[i].end != list[i].start) out += "-$" + hex(list[i].end, 4);
      if(!list[i].enabled) out += " (disabled)";
      if(list[i].condition[0]) out += std::string(" if ") + list[i].condition;
      out += "\n";
    }
  }
  else if(cmd == "print" || cmd == "p" || cmd == "eval")
  {
    const std::string expr = rest_from(0);
    out = with_dbg(d, std::string("no debugger"), [&](Debugger* dbg) {
      EvalResultType rt = EvalResultType::Numeric;
      const int64_t v = dbg->EvaluateExpression(expr, CpuType::Nes, rt, false);
      switch(rt)
      {
        case EvalResultType::Invalid: return std::string("does not parse: ") + expr;
        case EvalResultType::DivideBy0: return std::string("division by zero");
        case EvalResultType::OutOfScope: return std::string("out of scope");
        case EvalResultType::Boolean: return std::string(v ? "true" : "false");
        default: break;
      }
      return std::to_string(v) + "  $" + hex((uint32_t)v & 0xFFFF, v > 0xFF ? 4 : 2);
    });
  }
  else if(cmd == "regs" || cmd == "r")
  {
    nesdebug_cpu c;
    nesdebug_cpu_get(d, &c);
    char buf[160];
    snprintf(buf, sizeof buf, "PC=%04X A=%02X X=%02X Y=%02X SP=%02X P=%02X [%c%c-%c%c%c%c] cyc=%llu sl=%d dot=%d",
             c.pc, c.a, c.x, c.y, c.sp, c.ps, c.n ? 'N' : 'n', c.v ? 'V' : 'v', c.d ? 'D' : 'd',
             c.i ? 'I' : 'i', c.z ? 'Z' : 'z', c.c ? 'C' : 'c', (unsigned long long)c.total_cycles,
             c.scanline, c.dot);
    out = buf;
  }
  else if(cmd == "mem" || cmd == "m" || cmd == "ppu")
  {
    uint32_t a;
    if(args.empty() || !parse_addr(d, args[0], a)) out = "usage: " + cmd + " <addr> [len]";
    else
    {
      const int len = std::min(1024, args.size() > 1 ? (int)strtoul(args[1].c_str(), nullptr, 0) : 64);
      std::vector<uint8_t> buf((size_t)std::max(1, len));
      if(cmd == "ppu") nesdebug_ppu_read(d, (uint16_t)a, buf.data(), len);
      else nesdebug_read(d, (uint16_t)a, buf.data(), len);
      out = hexdump(buf.data(), len, a);
    }
  }
  else if(cmd == "poke" || cmd == "w")
  {
    uint32_t a;
    if(args.size() < 2 || !parse_addr(d, args[0], a)) out = "usage: poke <addr> <value>";
    else
    {
      std::string v = args[1];
      if(v[0] == '$') v = "0x" + v.substr(1);
      nesdebug_write(d, (uint16_t)a, (uint8_t)strtoul(v.c_str(), nullptr, 0));
      out = "ok";
    }
  }
  else if(cmd == "label")
  {
    uint32_t a;
    if(args.size() < 2 || !parse_addr(d, args[0], a)) out = "usage: label <addr> <name>";
    else out = nesdebug_set_label(d, (uint16_t)a, args[1].c_str()) == 0 ? "ok" : "failed";
  }
  else if(cmd == "labels" || cmd == "symbols")
  {
    char msg[512];
    nesdebug_load_symbols(d, args.empty() ? nullptr : rest_from(0).c_str(), msg, sizeof msg);
    out = msg;
  }
  else if(cmd == "disasm" || cmd == "u")
  {
    uint32_t a = 0;
    nesdebug_cpu c;
    nesdebug_cpu_get(d, &c);
    if(args.empty() || !parse_addr(d, args[0], a)) a = (uint32_t)c.pc;
    const int n = std::min(200, count_arg(1, 16));
    std::vector<nesdebug_line> lines((size_t)n);
    const int got = nesdebug_disassemble(d, (uint16_t)a, lines.data(), n, nullptr);
    for(int i = 0; i < got; i++)
    {
      char buf[220];
      snprintf(buf, sizeof buf, "%c%c %04X  %-9s %-14s %s\n", lines[i].has_breakpoint ? '*' : ' ',
               lines[i].is_pc ? '>' : ' ', lines[i].address, lines[i].bytes, lines[i].label, lines[i].disasm);
      out += buf;
    }
  }
  else if(cmd == "cart")
  {
    nesdebug_cart c;
    nesdebug_cart_get(d, &c);
    char info[256];
    nesdebug_cart_info(d, info, sizeof info);
    char buf[512];
    snprintf(buf, sizeof buf,
             "%s\nPRG slots %02X %02X %02X %02X  CHR slots %03X %03X %03X %03X %03X %03X %03X %03X\n"
             "mirroring %s  WRAM %s%s  CHR %s  IRQ %s line=%d latch=%d counter=%d\n"
             "ACKSEQ=%02X status=%02X err=%u boot=%02X/%u%% bootErr=%u RMW dropped=%u queue=%u",
             info, c.prg_slot[0], c.prg_slot[1], c.prg_slot[2], c.prg_slot[3], c.chr_slot[0], c.chr_slot[1],
             c.chr_slot[2], c.chr_slot[3], c.chr_slot[4], c.chr_slot[5], c.chr_slot[6], c.chr_slot[7],
             c.mirroring, c.wram_enabled ? "on" : "off", c.wram_protected ? " (protected)" : "",
             c.chr_writable ? "RAM" : "ROM", c.irq_enabled ? "on" : "off", c.irq_line, c.irq_latch,
             c.irq_counter, c.ackseq, c.status, c.last_error, c.boot_state, c.boot_pct, c.boot_err,
             c.diag_rmw, c.queue_depth);
    out = buf;
  }
  else out = "unknown command: " + cmd + " (try help)";

  bump(d);
  return put(dst, dstsz, out);
}

extern "C" int nesdebug_completions(nesdebug* d, const char* prefix, char* dst, int dstsz)
{
  if(!prefix || !dst || dstsz <= 0) return 0;
  dst[0] = '\0';
  const std::string pre(prefix);
  int len = 0, n = 0;
  auto emit = [&](const std::string& s) {
    if(s.compare(0, pre.size(), pre) != 0) return;
    if(len + (int)s.size() + 2 >= dstsz) return;
    len += snprintf(dst + len, (size_t)(dstsz - len), "%s\n", s.c_str());
    n++;
  };
  for(const char* c : Commands) emit(c);
  if(d)
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    for(const Label& l : d->labels) emit(l.name);
  }
  return n;
}
