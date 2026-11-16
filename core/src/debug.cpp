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
 * Mesen already has what the family's debuggers had to write by hand: its
 * NesTraceLogger is the Trace/History ring, its MemoryDumper is every
 * memory space, its ExpressionEvaluator is the condition grammar and its
 * BreakpointManager covers the CPU bus and the PPU, palette and OAM spaces.
 * This file wires them to nesdebug.h and adds what they lack: `old` in a
 * condition, hit counts, the stop in words, the memory map, search.
 *
 * Notifications from Mesen (CodeBreak, DebuggerResumed) reach the engine
 * through a listener of its own, on the emulation thread: the stop record
 * (serial, reason, address, value, old) is written in one critical section.
 *
 * The FujiNet trace is the exception to "only while attached": the
 * cartridge's link feeds it from the session's start (fuji_trace.h).
 *
 * The console (nesdebug_command) is in prompt.cpp, the Cart & I/O and Sound
 * text in iostate.cpp, watches and range parsing in memtools.cpp.
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
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "MesenHost.h"

#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/DebuggerRequest.h"
#include "Shared/NotificationManager.h"
#include "Shared/SettingTypes.h"
#include "Shared/Interfaces/INotificationListener.h"
#include "Debugger/Breakpoint.h"
#include "Debugger/DebugTypes.h"
#include "Debugger/Debugger.h"
#include "Debugger/Disassembler.h"
#include "Debugger/ExpressionEvaluator.h"
#include "Debugger/ITraceLogger.h"
#include "Debugger/LabelManager.h"
#include "Debugger/MemoryDumper.h"
#include "NES/NesTypes.h"
#include "NES/Mappers/Homebrew/FujiNetCart.h"
#include "NES/Mappers/Homebrew/FujiNet/fuji_mailbox.h"

extern "C" {
#include "session_internal.h"
#include "nesdebug.h"
}

#include "fuji_trace.h"
#include "nesvidtab.h"
#include "expr.h"

MesenHost* nessession_host(nessession* s);

namespace {

struct Bp
{
  int id;
  int type;
  int space;
  uint32_t start, end;
  bool enabled;
  std::string condition;
  unsigned hits;
};

struct Label
{
  MemoryType type;
  uint32_t address;
  std::string name;
  int cpu;                 // the CPU address it was given for, -1 if unknown
};

// The run-to target rides as a breakpoint with an id no user one can have.
constexpr int RunToId = 0x7FFFFFFF;

// What the next CpuStep break means, set by whoever asked for the step.
enum { KindStep = 0, KindOneShot, KindFrame };

// The log format the engine asks Mesen for; parse_trace_row reads it back.
const char* const TraceFormat =
  "[PC,4h]|[A,2h]|[X,2h]|[Y,2h]|[SP,2h]|[P,2h]|[CycleCount]|[Scanline]|[Cycle]|[FrameCount]|[Disassembly]";

} // namespace

class EngineListener;

struct nesdebug
{
  nessession* session{nullptr};
  std::atomic<unsigned> generation{0};
  std::atomic<unsigned> stops{0};      // CodeBreak notifications seen
  std::atomic<bool> attached{false};
  std::atomic<bool> quiesce{false};    // a power cycle is replacing the console
  std::atomic<int> busy{0};            // engine calls in flight
  std::recursive_mutex api;            // Mesen's debugger is not for two threads at once
  std::atomic<int> kind{KindStep};
  FujiTrace fuji;                // locks itself
  std::mutex mutex;              // guards everything below
  // the stop, as one record
  std::string reason;
  int reasonAddr{-1};
  bool stopped{false};
  uint64_t serial{0};
  nesdebug_stop_info stop{};
  std::string stopText{"running"};
  std::vector<Bp> bps;
  int nextId{1};
  int runTo{-1};
  std::vector<Label> labels;     // from symbol files and the user, re-applied on attach/load
  std::atomic<unsigned> symgen{1};
  std::vector<std::pair<uint16_t, std::string>> symcache;
  unsigned symcacheGen{0};
  unsigned symcacheMap{0};
  // trace: what survives the logger (power cycle, recording off)
  bool traceOn{false};
  std::vector<nesdebug_trace_entry> saved;   // oldest first
  uint64_t traceBase{0};
  // our listener on the current emulator
  std::shared_ptr<EngineListener> listener;
  Emulator* listenerEmu{nullptr};
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

// Counts an engine call in flight, so a power cycle (nesdebug_before_load)
// can wait for every poller to be out of Mesen before the console goes.
struct Busy
{
  nesdebug* d;
  explicit Busy(nesdebug* dd) : d(dd) { ++d->busy; }
  ~Busy() { --d->busy; }
};

// Run fn(Debugger*) if the engine is up; otherwise return def.
template<typename R, typename F>
R with_dbg(nesdebug* d, R def, F&& fn)
{
  if(!d || d->quiesce.load()) return def;
  Busy busy(d);
  std::lock_guard<std::recursive_mutex> api(d->api);
  if(d->quiesce.load()) return def;
  if(!live(d) || !d->attached.load()) return def;
  DebuggerRequest req = emu_of(d)->GetDebugger(false);
  Debugger* dbg = req.GetDebugger();
  if(!dbg) return def;
  return fn(dbg);
}

template<typename F>
void with_dbg_void(nesdebug* d, F&& fn)
{
  if(!d || d->quiesce.load()) return;
  Busy busy(d);
  std::lock_guard<std::recursive_mutex> api(d->api);
  if(d->quiesce.load()) return;
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

void set_err(char* err, int errsz, const std::string& msg)
{
  if(err && errsz > 0) snprintf(err, static_cast<size_t>(errsz), "%s", msg.c_str());
}

// ---- spaces ----------------------------------------------------------------------------

MemoryType bp_memory_type(int space)
{
  switch(space)
  {
    case NESDEBUG_SPACE_PPU: return MemoryType::NesPpuMemory;
    case NESDEBUG_SPACE_PALETTE: return MemoryType::NesPaletteRam;
    case NESDEBUG_SPACE_OAM: return MemoryType::NesSpriteRam;
    default: return MemoryType::NesMemory;
  }
}

int space_of_memory_type(MemoryType t)
{
  switch(t)
  {
    case MemoryType::NesPpuMemory: return NESDEBUG_SPACE_PPU;
    case MemoryType::NesPaletteRam: return NESDEBUG_SPACE_PALETTE;
    case MemoryType::NesSpriteRam: return NESDEBUG_SPACE_OAM;
    default: return NESDEBUG_SPACE_CPU;
  }
}

uint32_t bp_space_limit(int space)
{
  switch(space)
  {
    case NESDEBUG_SPACE_PPU: return 0x3FFF;
    case NESDEBUG_SPACE_PALETTE: return 0x1F;
    case NESDEBUG_SPACE_OAM: return 0xFF;
    default: return 0xFFFF;
  }
}

// ---- conditions (the text-level helpers are in expr.cpp) ---------------------------------

bool condition_parses(Debugger* dbg, const std::string& translated)
{
  if(translated.empty()) return true;
  std::string why;
  if(!syntax_ok(translated, why)) return false;
  EvalResultType rt = EvalResultType::Numeric;
  dbg->EvaluateExpression(translated, CpuType::Nes, rt, false);
  return rt != EvalResultType::Invalid;
}

// ---- the engine's push of breakpoints, labels and config -----------------------------------

// Mesen's own predictive breakpoints stop a read/write breakpoint at the START
// of the instruction that makes the access, which is where the family
// stops; SingleBreakpointPerInstruction keeps the real access from firing
// the same breakpoint again.
void set_debug_config(Emulator* emu, bool predictive)
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
  cfg.UsePredictiveBreakpoints = predictive;
  cfg.SingleBreakpointPerInstruction = true;
  emu->GetSettings()->SetDebugConfig(cfg);
}

// Build Mesen's breakpoint list from ours; the caller holds the mutex only
// for the copy, never across Mesen (whose calls may wait for the emulation
// thread, which may be waiting for the mutex in the break listener).
void push_breakpoints(nesdebug* d, Debugger* dbg)
{
  std::vector<Breakpoint> list;
  bool predictive = false;
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    auto add = [&](int id, int type, int space, uint32_t start, uint32_t end, bool enabled, const std::string& cond) {
      Breakpoint b;
      int flags = 0;
      if(type & NESDEBUG_BP_EXEC) flags |= (int)BreakpointTypeFlags::Execute;
      if(type & NESDEBUG_BP_READ) flags |= (int)BreakpointTypeFlags::Read;
      if(type & NESDEBUG_BP_WRITE) flags |= (int)BreakpointTypeFlags::Write;
      std::string translated, err;
      if(!translate_condition(space, cond, translated, err)) translated.clear();
      b.Init((uint32_t)id, CpuType::Nes, bp_memory_type(space), (BreakpointTypeFlags)flags,
             (int32_t)start, (int32_t)end, enabled, false, true, translated.c_str());
      list.push_back(b);
      if(enabled && space == NESDEBUG_SPACE_CPU && (type & (NESDEBUG_BP_READ | NESDEBUG_BP_WRITE)))
        predictive = true;
    };
    for(const Bp& b : d->bps)
      add(b.id, b.type, b.space, b.start, b.end, b.enabled, b.condition);
    if(d->runTo >= 0)
      add(RunToId, NESDEBUG_BP_EXEC, NESDEBUG_SPACE_CPU, (uint32_t)d->runTo, (uint32_t)d->runTo, true, "");
  }
  Emulator* emu = dbg->GetEmulator();
  if(emu->GetSettings()->GetDebugConfig().UsePredictiveBreakpoints != predictive)
    set_debug_config(emu, predictive);
  dbg->SetBreakpoints(list.data(), (uint32_t)list.size());
}

void push_breakpoints(nesdebug* d)
{
  with_dbg_void(d, [&](Debugger* dbg) { push_breakpoints(d, dbg); });
}

// The NES's own registers and the FujiNet mailbox, named the way the
// nesdev wiki and fuji_mailbox.h name them. (CPU address, name.)
struct Sym { uint16_t cpu; const char* name; };

const Sym RegisterSyms[] = {
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

// The painted half lives in the cartridge's arena (mapper RAM); offsets.
struct ArenaSym { uint16_t offset; const char* name; };
const ArenaSym ArenaSyms[] = {
  { FN_R_DATA, "FN_REPLY" }, { FN_R_ACKSEQ, "FN_ACKSEQ" }, { FN_R_STATUS, "FN_STATUS" },
  { FN_R_ERR, "FN_ERRCODE" }, { FN_R_REPLY_CMD, "FN_REPLYCMD" }, { FN_R_RXLEN_LO, "FN_RXLEN_LO" },
  { FN_R_RXLEN_HI, "FN_RXLEN_HI" }, { FN_R_BOOT_STATE, "FN_BOOTSTAT" }, { FN_R_BOOT_PCT, "FN_BOOTPCT" },
  { FN_R_BOOT_ERR, "FN_BOOTERR" }, { FN_R_MAGIC0, "FN_MAGIC0" }, { FN_R_MAGIC1, "FN_MAGIC1" },
  { FN_R_PROTO_VER, "FN_PROTOVER" }, { FN_R_LOAD_STATE, "FN_LOAD_STATE" }, { FN_R_LOAD_DST, "FN_LOAD_DST" },
  { FN_R_LOAD_OFF, "FN_LOAD_OFF" }, { FN_R_LOAD_SEQ, "FN_LOAD_SEQ" }, { FN_R_LOAD_PCT, "FN_LOAD_PCT" },
  { FN_R_SRAM_STATE, "FN_SRAM_STATE" }, { FN_R_DIAG_RMW, "FN_DIAG_RMW" }, { FN_R_MAPPER, "FN_MAPPER" },
  { FN_R_LINK, "FN_LINK" }, { FN_LOADER, "FN_LOADER" },
};

void seed_labels(Debugger* dbg)
{
  LabelManager* lm = dbg->GetLabelManager();
  for(const auto& r : RegisterSyms)
    lm->SetLabel(r.cpu, MemoryType::NesMemory, r.name, "");
  for(const auto& a : ArenaSyms)
    lm->SetLabel(a.offset, MemoryType::NesMapperRam, a.name, "");
}

void apply_labels(nesdebug* d, Debugger* dbg)
{
  std::vector<Label> labels;
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    labels = d->labels;
  }
  seed_labels(dbg);
  LabelManager* lm = dbg->GetLabelManager();
  for(const Label& l : labels)
    lm->SetLabel(l.address, l.type, l.name, "");
}

AddressInfo cpu_addr(uint16_t a)
{
  return AddressInfo { (int32_t)a, MemoryType::NesMemory };
}

// ---- the PPU registers, rebuilt from the state ----------------------------------------------

uint8_t ctrl_byte(const NesPpuState& p)
{
  const PpuControlFlags& c = p.Control;
  return (uint8_t)(((p.TmpVideoRamAddr >> 10) & 0x03) | (c.VerticalWrite ? 0x04 : 0) |
                   (c.SpritePatternAddr == 0x1000 ? 0x08 : 0) | (c.BackgroundPatternAddr == 0x1000 ? 0x10 : 0) |
                   (c.LargeSprites ? 0x20 : 0) | (c.SecondaryPpu ? 0x40 : 0) | (c.NmiOnVerticalBlank ? 0x80 : 0));
}

uint8_t mask_byte(const NesPpuState& p)
{
  const PpuMaskFlags& m = p.Mask;
  return (uint8_t)((m.Grayscale ? 0x01 : 0) | (m.BackgroundMask ? 0x02 : 0) | (m.SpriteMask ? 0x04 : 0) |
                   (m.BackgroundEnabled ? 0x08 : 0) | (m.SpritesEnabled ? 0x10 : 0) |
                   (m.IntensifyRed ? 0x20 : 0) | (m.IntensifyGreen ? 0x40 : 0) | (m.IntensifyBlue ? 0x80 : 0));
}

// $4015 as a write sets it: which channels are enabled.
uint8_t apu_enable_byte(const ApuState& a)
{
  return (uint8_t)((a.Square1.Enabled ? 1 : 0) | (a.Square2.Enabled ? 2 : 0) | (a.Triangle.Enabled ? 4 : 0) |
                   (a.Noise.Enabled ? 8 : 0) | (a.Dmc.BytesRemaining ? 16 : 0));
}

// The byte at CPU address `a` before a write to it lands, -1 when it cannot
// be known (write-only registers).
int old_cpu_byte(Debugger* dbg, uint16_t a)
{
  if(a >= 0x2000 && a <= 0x3FFF)
  {
    NesPpuState p = {};
    dbg->GetPpuState(p, CpuType::Nes);
    switch(a & 7)
    {
      case 0: return ctrl_byte(p);
      case 1: return mask_byte(p);
      case 3: return p.SpriteRamAddr;
      default: return -1;
    }
  }
  if(a == 0x4015)
  {
    NesState st = {};
    dbg->GetConsoleState(reinterpret_cast<BaseState&>(st), ConsoleType::Nes);
    return apu_enable_byte(st.Apu);
  }
  if(a >= 0x4000 && a <= 0x401F) return -1;
  return dbg->GetMemoryDumper()->GetMemoryValue(MemoryType::NesMemory, a, true);
}

// ---- the stop, in words -----------------------------------------------------------------

std::string space_prefix(int space)
{
  switch(space)
  {
    case NESDEBUG_SPACE_PPU: return "ppu ";
    case NESDEBUG_SPACE_PALETTE: return "palette ";
    case NESDEBUG_SPACE_OAM: return "oam ";
    default: return "";
  }
}

std::string addr_text(int space, uint32_t a)
{
  return "$" + hex(a, space == NESDEBUG_SPACE_CPU || space == NESDEBUG_SPACE_PPU ? 4 : 2);
}

std::string make_stop_text(const nesdebug_stop_info& s, const char* other)
{
  const std::string pc = "$" + hex(s.pc, 4);
  switch(s.reason)
  {
    case NESDEBUG_STOP_PAUSE: return "paused at " + pc;
    case NESDEBUG_STOP_STEP: return "step at " + pc;
    case NESDEBUG_STOP_ONESHOT: return "stopped at " + pc;
    case NESDEBUG_STOP_FRAME: return "frame at " + pc;
    case NESDEBUG_STOP_BREAKPOINT: return "breakpoint: exec " + pc;
    case NESDEBUG_STOP_WATCH_READ:
      return "read " + space_prefix(s.space) + addr_text(s.space, s.addr) + " = $" + hex((uint32_t)s.value & 0xFF, 2) +
             " at " + pc;
    case NESDEBUG_STOP_WATCH_WRITE: {
      std::string t = "wrote " + space_prefix(s.space) + addr_text(s.space, s.addr) + " = $" +
                      hex((uint32_t)s.value & 0xFF, 2);
      if(s.old_value >= 0) t += " (was $" + hex((uint32_t)s.old_value & 0xFF, 2) + ")";
      return t + " at " + pc;
    }
    case NESDEBUG_STOP_OTHER: return std::string(other && *other ? other : "stopped") + " at " + pc;
    default: return "stopped at " + pc;
  }
}

// On the emulation thread, from Mesen's CodeBreak: write the stop record.
void record_stop(nesdebug* d, Emulator* emu, const BreakEvent* evt)
{
  nesdebug_stop_info st = {};
  st.reason = NESDEBUG_STOP_PAUSE;
  st.value = -1;
  st.old_value = -1;
  const char* other = "";
  int hitId = -1;
  {
    // Not Emulator::GetDebugger: its lock is held by an attach/detach that is
    // waiting for this very thread to park.
    DebuggerRequest req(emu);
    Debugger* dbg = req.GetDebugger();
    if(dbg)
      st.pc = (uint16_t)dbg->GetProgramCounter(CpuType::Nes, true);
    st.addr = st.pc;
    int kind = d->kind.exchange(KindStep);
    if(evt)
    {
      switch(evt->Source)
      {
        case BreakSource::Pause: st.reason = NESDEBUG_STOP_PAUSE; break;
        case BreakSource::CpuStep:
        case BreakSource::PpuStep:
          st.reason = kind == KindFrame ? NESDEBUG_STOP_FRAME
                    : kind == KindOneShot ? NESDEBUG_STOP_ONESHOT : NESDEBUG_STOP_STEP;
          break;
        case BreakSource::Breakpoint: {
          hitId = evt->BreakpointId;
          st.bp_id = hitId;
          if(hitId == RunToId) { st.reason = NESDEBUG_STOP_ONESHOT; st.bp_id = 0; break; }
          const MemoryOperationInfo& op = evt->Operation;
          const bool write = op.Type == MemoryOperationType::Write || op.Type == MemoryOperationType::DmaWrite ||
                             op.Type == MemoryOperationType::DummyWrite;
          const bool exec = op.Type == MemoryOperationType::ExecOpCode || op.Type == MemoryOperationType::ExecOperand;
          if(exec) { st.reason = NESDEBUG_STOP_BREAKPOINT; st.addr = op.Address & 0xFFFF; break; }
          st.reason = write ? NESDEBUG_STOP_WATCH_WRITE : NESDEBUG_STOP_WATCH_READ;
          st.space = space_of_memory_type(op.MemType);
          st.addr = op.Address;
          st.value = op.Value & 0xFF;
          {
            // The breakpoint's own space wins: Mesen reports a palette
            // write as the PPU address $3Fxx
            std::lock_guard<std::mutex> lock(d->mutex);
            for(const Bp& b : d->bps)
              if(b.id == hitId) st.space = b.space;
          }
          if(st.space == NESDEBUG_SPACE_PALETTE) st.addr = op.Address & 0x1F;
          if(write && dbg)
          {
            if(st.space == NESDEBUG_SPACE_CPU)
              st.old_value = old_cpu_byte(dbg, (uint16_t)op.Address);
            else if(st.space == NESDEBUG_SPACE_PALETTE)
              st.old_value = dbg->GetMemoryDumper()->GetMemoryValue(MemoryType::NesPaletteRam, st.addr, true);
            else
              st.old_value = dbg->GetMemoryDumper()->GetMemoryValue(op.MemType, op.Address, true);
          }
          break;
        }
        case BreakSource::BreakOnBrk: st.reason = NESDEBUG_STOP_OTHER; other = "BRK"; break;
        case BreakSource::NesBreakOnCpuCrash: st.reason = NESDEBUG_STOP_OTHER; other = "CPU crash (illegal opcode)"; break;
        case BreakSource::BreakOnUnofficialOpCode: st.reason = NESDEBUG_STOP_OTHER; other = "unofficial opcode"; break;
        case BreakSource::BreakOnUnstableOpCode: st.reason = NESDEBUG_STOP_OTHER; other = "unstable opcode"; break;
        case BreakSource::BreakOnUninitMemoryRead: st.reason = NESDEBUG_STOP_OTHER; other = "uninitialized memory read"; break;
        default: st.reason = NESDEBUG_STOP_OTHER; other = "stopped"; break;
      }
    }
  }
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    st.serial = ++d->serial;
    d->stop = st;
    d->stopped = true;
    d->stopText = make_stop_text(st, other);
    d->reason = d->stopText;
    d->reasonAddr = st.pc;
    for(Bp& b : d->bps)
      if(b.id == hitId) b.hits++;
  }
  ++d->stops;
  bump(d);
}

} // namespace

class EngineListener : public INotificationListener
{
  nesdebug* _d;
  Emulator* _emu;

public:
  EngineListener(nesdebug* d, Emulator* emu) : _d(d), _emu(emu) {}

  void ProcessNotification(ConsoleNotificationType type, void* parameter) override
  {
    if(!_d->attached.load()) return;
    if(type == ConsoleNotificationType::CodeBreak)
      record_stop(_d, _emu, static_cast<const BreakEvent*>(parameter));
    else if(type == ConsoleNotificationType::DebuggerResumed)
    {
      {
        std::lock_guard<std::mutex> lock(_d->mutex);
        _d->stopped = false;
        _d->stopText = "running";
      }
      bump(_d);
    }
  }
};

namespace {

void ensure_listener(nesdebug* d, Emulator* emu)
{
  if(d->listenerEmu == emu && d->listener) return;
  d->listener = std::make_shared<EngineListener>(d, emu);
  d->listenerEmu = emu;
  emu->GetNotificationManager()->RegisterNotificationListener(d->listener);
}

// The UI side of "the machine is running again".
void mark_running(nesdebug* d)
{
  std::lock_guard<std::mutex> lock(d->mutex);
  d->stopped = false;
  d->stopText = "running";
  d->reason = "running";
  d->reasonAddr = -1;
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
    nesvid_tab_register_console(d);   /* the console's `video` command */
  }
  return static_cast<nesdebug*>(s->debugger);
}

namespace {

nesdebug* gTraced = nullptr;     // the debugger the cartridge's link feeds

// On the cartridge's worker thread: every frame on its link.
void fuji_trace_hook(void* user, int dir, const uint8_t* raw, size_t len)
{
  static_cast<nesdebug*>(user)->fuji.Feed(dir, raw, len);
}

void trace_apply(nesdebug* d, Debugger* dbg);
void trace_stash(nesdebug* d);

} // namespace

void nesdebug_destroy(nessession* s)
{
  if(!s || !s->debugger) return;
  auto* d = static_cast<nesdebug*>(s->debugger);
  if(gTraced == d)
  {
    FujiNetCart::SetTrace(nullptr, nullptr);
    gTraced = nullptr;
  }
  d->attached.store(false);
  d->listener.reset();
  delete d;
  s->debugger = nullptr;
}

// The FujiNet trace is always on: it is cheap, and it is most useful about
// what happened before anyone thought to look. Also the start of a session:
// the new emulator has no debugger and none of our listeners.
void nesdebug_trace_install(nessession* s)
{
  gTraced = nesdebug_get(s);
  gTraced->quiesce.store(false);
  gTraced->attached.store(false);
  gTraced->listener.reset();
  gTraced->listenerEmu = nullptr;
  {
    std::lock_guard<std::mutex> lock(gTraced->mutex);
    gTraced->stopped = false;
    gTraced->stopText = "running";
  }
  FujiNetCart::SetTrace(fuji_trace_hook, gTraced);
}

// Called on the emulation thread from the CodeBreak notification. While the
// engine is attached its own listener writes the stop record (and this is a
// no-op); detached, nothing reads it.
void nesdebug_note_stop(nessession* s, const char* msg, int addr)
{
  auto* d = nesdebug_get(s);
  if(!d || d->attached.load()) return;
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
// the new one. What the old one logged is kept first, then every poller is
// waited out of Mesen: the console is about to be freed under it.
void nesdebug_before_load(nessession* s)
{
  auto* d = static_cast<nesdebug*>(s ? s->debugger : nullptr);
  if(!d) return;
  if(d->attached.load()) trace_stash(d);
  d->quiesce.store(true);
  for(int waited = 0; d->busy.load() > 0 && waited < 3000; waited++)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    d->stopped = false;
    d->stopText = "running";
  }
}

void nesdebug_after_load(nessession* s)
{
  auto* d = static_cast<nesdebug*>(s ? s->debugger : nullptr);
  if(!d) return;
  d->quiesce.store(false);
  if(!d->attached.load()) return;
  with_dbg_void(d, [&](Debugger* dbg) {
    push_breakpoints(d, dbg);
    apply_labels(d, dbg);
    trace_apply(d, dbg);
  });
  bump(d);
}

extern "C" void nesdebug_attach(nesdebug* d)
{
  if(!live(d)) return;
  Emulator* emu = emu_of(d);
  if(!d->attached.load())
  {
    set_debug_config(emu, false);
    // The engine only breaks for a CPU whose debug window is "open"
    emu->GetSettings()->SetDebuggerFlag(DebuggerFlags::NesDebuggerEnabled, true);
    emu->InitDebugger();
    ensure_listener(d, emu);
    d->attached.store(true);
    with_dbg_void(d, [&](Debugger* dbg) {
      push_breakpoints(d, dbg);
      apply_labels(d, dbg);
      trace_apply(d, dbg);
    });
  }
  nesdebug_stop(d);
}

extern "C" void nesdebug_detach(nesdebug* d)
{
  if(!d || !d->attached.load()) return;
  with_dbg_void(d, [&](Debugger* dbg) {
    trace_stash(d);
    dbg->Run();
  });
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
    d->stopped = false;
    d->stopText = "running";
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
  // Our own record, not Mesen's flag: right after a resume Mesen still says
  // "stopped" for a few milliseconds, and a stop that is not waited for
  // leaves the caller reading the last stop.
  bool already;
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    already = d->stopped;
  }
  const unsigned before = d->stops.load();
  with_dbg_void(d, [](Debugger* dbg) {
    dbg->Step(CpuType::Nes, 1, StepType::Step, BreakSource::Pause);
  });
  if(!already) wait_for_stop(d, before, 500);
}

extern "C" void nesdebug_resume(nesdebug* d)
{
  settle_run_to(d);
  mark_running(d);
  with_dbg_void(d, [](Debugger* dbg) { dbg->Run(); });
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

extern "C" void nesdebug_touch(nesdebug* d)
{
  if(d) bump(d);
}

// ---- the stop record ---------------------------------------------------------

extern "C" int nesdebug_stop_text(nesdebug* d, char* dst, int dstsz, int* address)
{
  if(!d) return 0;
  std::lock_guard<std::mutex> lock(d->mutex);
  if(address) *address = d->stopped ? d->stop.pc : -1;
  return put(dst, dstsz, d->stopped ? d->stopText : std::string("running"));
}

extern "C" int nesdebug_last_stop(nesdebug* d, nesdebug_stop_info* out)
{
  if(!out) return 0;
  memset(out, 0, sizeof *out);
  out->value = out->old_value = -1;
  if(!d) return 0;
  std::lock_guard<std::mutex> lock(d->mutex);
  *out = d->stop;
  return d->stopped ? 1 : 0;
}

extern "C" uint64_t nesdebug_stop_serial(nesdebug* d)
{
  if(!d) return 0;
  std::lock_guard<std::mutex> lock(d->mutex);
  return d->serial;
}

extern "C" int nesdebug_stop_snapshot(nesdebug* d, nesdebug_snapshot* out)
{
  if(!out) return 0;
  memset(out, 0, sizeof *out);
  out->address = -1;
  out->stop.value = out->stop.old_value = -1;
  if(!d)
  {
    snprintf(out->text, sizeof out->text, "running");
    return 0;
  }
  std::lock_guard<std::mutex> lock(d->mutex);
  out->paused = d->stopped ? 1 : 0;
  out->serial = d->serial;
  out->stop = d->stop;
  snprintf(out->text, sizeof out->text, "%s", d->stopped ? d->stopText.c_str() : "running");
  out->address = d->stopped ? d->stop.pc : -1;
  return out->paused;
}

// ---- stepping --------------------------------------------------------------

namespace {
void step(nesdebug* d, StepType type, int count)
{
  settle_run_to(d);
  const unsigned before = d->stops.load();
  d->kind.store(type == StepType::Step ? KindStep
                : (type == StepType::PpuFrame || type == StepType::PpuScanline) ? KindFrame : KindOneShot);
  mark_running(d);
  with_dbg_void(d, [&](Debugger* dbg) {
    dbg->Step(CpuType::Nes, count > 0 ? count : 1, type, BreakSource::CpuStep);
  });
  wait_for_stop(d, before, type == StepType::Step || type == StepType::StepOver ? 500 : 250);
  bump(d);
}
}

extern "C" void nesdebug_step(nesdebug* d) { step(d, StepType::Step, 1); }
extern "C" void nesdebug_step_count(nesdebug* d, int n) { step(d, StepType::Step, n); }
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
  d->kind.store(KindOneShot);
  push_breakpoints(d);
  mark_running(d);
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

namespace {

// Edit the registers of a stopped machine. The PC goes through Mesen's own
// SetProgramCounter, which also resets the debugger's idea of the current
// instruction.
bool regs_write(nesdebug* d, const nesdebug_cpu* in, int reg, int value, char* err, int errsz)
{
  if(!d || !live(d)) { set_err(err, errsz, "the emulator is not running"); return false; }
  if(!d->attached.load()) { set_err(err, errsz, "the debugger is not attached"); return false; }
  const bool stopped = with_dbg(d, false, [](Debugger* dbg) { return dbg->IsExecutionStopped(); });
  if(!stopped) { set_err(err, errsz, "the machine is running: stop it first"); return false; }
  with_dbg_void(d, [&](Debugger* dbg) {
    NesCpuState cpu = {};
    dbg->GetCpuState(cpu, CpuType::Nes);
    uint16_t pc = cpu.PC;
    if(in)
    {
      pc = (uint16_t)in->pc;
      cpu.SP = (uint8_t)in->sp; cpu.A = (uint8_t)in->a; cpu.X = (uint8_t)in->x;
      cpu.Y = (uint8_t)in->y; cpu.PS = (uint8_t)in->ps;
    }
    else
    {
      auto flag = [&](int bit) {
        cpu.PS = (uint8_t)(value ? (cpu.PS | (1 << bit)) : (cpu.PS & ~(1 << bit)));
      };
      switch(reg)
      {
        case NES_REG_PC: pc = (uint16_t)value; break;
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
    }
    dbg->SetCpuState(cpu, CpuType::Nes);
    if(pc != cpu.PC || reg == NES_REG_PC || in)
      dbg->SetProgramCounter(CpuType::Nes, pc);
  });
  bump(d);
  return true;
}

} // namespace

extern "C" void nesdebug_cpu_set(nesdebug* d, int reg, int value)
{
  regs_write(d, nullptr, reg, value, nullptr, 0);
}

extern "C" int nesdebug_regs_set(nesdebug* d, const nesdebug_cpu* regs, char* err, int errsz)
{
  if(!regs) { set_err(err, errsz, "no registers"); return 0; }
  if(regs->pc < 0 || regs->pc > 0xFFFF || regs->sp < 0 || regs->sp > 0xFF || regs->a < 0 || regs->a > 0xFF ||
     regs->x < 0 || regs->x > 0xFF || regs->y < 0 || regs->y > 0xFF || regs->ps < 0 || regs->ps > 0xFF)
  {
    set_err(err, errsz, "a register is out of range");
    return 0;
  }
  return regs_write(d, regs, 0, 0, err, errsz) ? 1 : 0;
}

const char* const nesdebug_reg_names[] = {
  "pc", "sp", "a", "x", "y", "ps", "p", "n", "v", "d", "i", "z", "c", nullptr
};

extern "C" int nesdebug_register_set(nesdebug* d, const char* name, long value, char* err, int errsz)
{
  if(!name || !*name) { set_err(err, errsz, "which register?"); return 0; }
  std::string n(name);
  for(char& c : n) c = (char)tolower((unsigned char)c);
  struct { const char* name; int reg; long max; } tab[] = {
    { "pc", NES_REG_PC, 0xFFFF }, { "sp", NES_REG_SP, 0xFF }, { "a", NES_REG_A, 0xFF },
    { "x", NES_REG_X, 0xFF }, { "y", NES_REG_Y, 0xFF }, { "ps", NES_REG_PS, 0xFF },
    { "p", NES_REG_PS, 0xFF }, { "n", NES_FLAG_N, 1 }, { "v", NES_FLAG_V, 1 },
    { "d", NES_FLAG_D, 1 }, { "i", NES_FLAG_I, 1 }, { "z", NES_FLAG_Z, 1 }, { "c", NES_FLAG_C, 1 },
  };
  for(const auto& t : tab)
    if(n == t.name)
    {
      if(value < 0 || value > t.max)
      {
        set_err(err, errsz, n + " is 0.." + std::to_string(t.max));
        return 0;
      }
      return regs_write(d, nullptr, t.reg, (int)value, err, errsz) ? 1 : 0;
    }
  set_err(err, errsz, "unknown register: " + n);
  return 0;
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
      for(const Bp& b : d->bps)
        if(b.space == NESDEBUG_SPACE_CPU && (b.type & NESDEBUG_BP_EXEC)) bps.push_back(b);
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
        if(l.address >= b.start && l.address <= b.end)
        {
          l.has_breakpoint = 1;
          if(b.enabled) l.breakpoint_enabled = 1;
        }
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


// ---- labels and symbols ----------------------------------------------------------

namespace {

// The CPU address a user or file label was given for, when the engine knows
// it (Mesen only knows the absolute address, which can be reached through
// more than one CPU address: NROM-128 is at both $8000 and $C000).
int known_label_address(nesdebug* d, const std::string& name)
{
  std::lock_guard<std::mutex> lock(d->mutex);
  for(const Label& l : d->labels)
    if(l.cpu >= 0 && l.name == name) return l.cpu;
  for(const auto& r : RegisterSyms)
    if(name == r.name) return r.cpu;
  return -1;
}

// Names in an expression that the engine can resolve exactly, as numbers.
std::string substitute_labels(nesdebug* d, const std::string& expr)
{
  std::string out;
  for(size_t i = 0; i < expr.size();)
  {
    const char c = expr[i];
    const bool start = isalpha((unsigned char)c) || c == '_' || c == '@' || c == '.';
    const bool after_prefix = i > 0 && (expr[i - 1] == '$' || isalnum((unsigned char)expr[i - 1]));
    if(start && !after_prefix)
    {
      size_t j = i;
      while(j < expr.size() && (isalnum((unsigned char)expr[j]) || expr[j] == '_' || expr[j] == '@' || expr[j] == '.')) j++;
      const std::string word = expr.substr(i, j - i);
      const int a = known_label_address(d, word);
      out += a >= 0 ? std::to_string(a) : word;
      i = j;
    }
    else if(isalnum((unsigned char)c) || c == '$')
    {
      // a number: copy it whole so a hex digit is not taken for a name
      size_t j = i + 1;
      while(j < expr.size() && (isalnum((unsigned char)expr[j]) || expr[j] == '_')) j++;
      out += expr.substr(i, j - i);
      i = j;
    }
    else
      out += expr[i++];
  }
  return out;
}

} // namespace

extern "C" int nesdebug_label_address(nesdebug* d, const char* label)
{
  if(!label || !*label) return -1;
  std::string l(label);
  if(d)
  {
    const int known = known_label_address(d, l);
    if(known >= 0) return known;
  }
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
  l.cpu = (int)a;
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
      int cpu = -1;
      if(t == MemoryType::NesMemory) cpu = (int)(a & 0xFFFF);
      else
      {
        const AddressInfo rel = dbg->GetRelativeAddress(AddressInfo { (int32_t)a, t }, CpuType::Nes);
        if(rel.Address >= 0) cpu = rel.Address & 0xFFFF;
      }
      out.push_back(Label{ t, a, name, cpu });
      count++;
    }
  }
  return count;
}

void symbols_changed(nesdebug* d)
{
  ++d->symgen;
  bump(d);
}

} // namespace

extern "C" int nesdebug_set_label(nesdebug* d, uint16_t addr, const char* label)
{
  if(!d) return -1;
  return with_dbg(d, -1, [&](Debugger* dbg) {
    AddressInfo abs = dbg->GetAbsoluteAddress(cpu_addr(addr));
    Label l;
    if(abs.Address >= 0) { l.type = abs.Type; l.address = (uint32_t)abs.Address; }
    else { l.type = MemoryType::NesMemory; l.address = addr; }
    l.name = label ? label : "";
    l.cpu = addr;
    // One user name per address: a new one replaces the last
    {
      std::lock_guard<std::mutex> lock(d->mutex);
      d->labels.erase(std::remove_if(d->labels.begin(), d->labels.end(), [&](const Label& e) {
        return e.type == l.type && e.address == l.address;
      }), d->labels.end());
      if(!l.name.empty()) d->labels.push_back(l);
    }
    dbg->GetLabelManager()->SetLabel(l.address, l.type, l.name, "");
    symbols_changed(d);
    return 0;
  });
}

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
      symbols_changed(d);
      put(msg, msgsz, std::to_string(n) + " labels loaded from " + p);
      return n;
    }
    put(msg, msgsz, candidates.size() == 1 ? "Cannot read " + candidates[0]
                                           : std::string("No symbol file next to the cartridge"));
    return -1;
  });
}

extern "C" void nesdebug_clear_symbols(nesdebug* d)
{
  if(!d) return;
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    d->labels.clear();
  }
  with_dbg_void(d, [&](Debugger* dbg) {
    dbg->GetLabelManager()->ClearLabels();
    seed_labels(dbg);
  });
  symbols_changed(d);
}

namespace {

// The list the Symbols tab shows: the built-ins, then every loaded or user
// label with a CPU address. Rebuilt when the labels changed or the PRG
// windows moved (a label given by file offset follows its bank).
void symbols_refresh(nesdebug* d)
{
  FujiNetCartStatus st;
  unsigned map = 0;
  if(FujiNetCart::GetStatus(st))
    for(int i = 0; i < NESMAP_PRG_SLOTS; i++) map = map * 131 + st.PrgSlot[i] + 1;
  if(d->symcacheGen == d->symgen.load() && d->symcacheMap == map && !d->symcache.empty()) return;

  std::vector<std::pair<uint16_t, std::string>> list;
  for(const auto& r : RegisterSyms) list.emplace_back(r.cpu, r.name);
  for(const auto& a : ArenaSyms) list.emplace_back((uint16_t)(FN_ARENA_BASE + a.offset), a.name);
  std::vector<Label> labels = d->labels;
  for(const Label& l : labels)
  {
    if(l.cpu >= 0) list.emplace_back((uint16_t)l.cpu, l.name);
  }
  std::sort(list.begin(), list.end(), [](const auto& x, const auto& y) {
    return x.first != y.first ? x.first < y.first : x.second < y.second;
  });
  list.erase(std::unique(list.begin(), list.end()), list.end());
  d->symcache.swap(list);
  d->symcacheGen = d->symgen.load();
  d->symcacheMap = map;
}

std::string lower(std::string s)
{
  for(char& c : s) c = (char)tolower((unsigned char)c);
  return s;
}

} // namespace

extern "C" int nesdebug_symbols_count(nesdebug* d)
{
  if(!d) return 0;
  std::lock_guard<std::mutex> lock(d->mutex);
  symbols_refresh(d);
  return (int)d->symcache.size();
}

extern "C" int nesdebug_symbols_get(nesdebug* d, int index, uint16_t* addr, char* name, int namesz)
{
  if(!d) return 0;
  std::lock_guard<std::mutex> lock(d->mutex);
  symbols_refresh(d);
  if(index < 0 || index >= (int)d->symcache.size()) return 0;
  if(addr) *addr = d->symcache[(size_t)index].first;
  if(name && namesz > 0) snprintf(name, (size_t)namesz, "%s", d->symcache[(size_t)index].second.c_str());
  return 1;
}

extern "C" int nesdebug_symbols_filter(nesdebug* d, const char* text, int* out, int max)
{
  if(!d || !out || max <= 0) return 0;
  const std::string needle = lower(text ? text : "");
  std::lock_guard<std::mutex> lock(d->mutex);
  symbols_refresh(d);
  int n = 0;
  for(size_t i = 0; i < d->symcache.size() && n < max; i++)
    if(needle.empty() || lower(d->symcache[i].second).find(needle) != std::string::npos)
      out[n++] = (int)i;
  return n;
}

extern "C" unsigned nesdebug_symbols_generation(nesdebug* d)
{
  return d ? d->symgen.load() : 0;
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

// ---- FujiNet -----------------------------------------------------------------

extern "C" int nesdebug_fujinet_trace(nesdebug* d, nesdebug_fuji_packet* out, int max)
{
  return d ? d->fuji.Copy(out, max) : 0;
}

extern "C" void nesdebug_fujinet_trace_clear(nesdebug* d)
{
  if(d) d->fuji.Clear();
}

extern "C" int nesdebug_fujinet_state(nesdebug* d, char* dst, int dstsz)
{
  if(!d) return put(dst, dstsz, "");
  if(!d->session->running) return put(dst, dstsz, "The NES is off.\n");
  nesdebug_cart c;
  nesdebug_cart_get(d, &c);
  if(!c.present) return put(dst, dstsz, "The cartridge is starting.\n");
  const FujiNetCartHostConfig cfg = FujiNetCart::GetHostConfig();
  std::string s = "Link          " + std::string(c.link_up ? "up" : "down") +
                  (c.busy ? " (transaction in flight)" : "") + "\n";
  s += "Dials         " + cfg.Host + ":" + std::to_string(cfg.Port) + " (BoIP)\n";
  if(c.link_error[0]) s += std::string("Link error    ") + c.link_error + "\n";
  s += std::string("Mailbox       ") + (c.mailbox_live ? "live" : "closed") + "\n";
  s += "ACKSEQ        $" + hex(c.ackseq, 2) + "\n";
  s += "STATUS        $" + hex(c.status, 2) + "\n";
  s += "Last error    " + std::to_string(c.last_error) + "\n";
  s += "Queue         " + std::to_string(c.queue_depth) + " waiting\n";
  s += std::string("Image         ") + (c.booted_image ? "a booted game" : "CONFIG");
  if(c.loading) s += ", loading " + std::to_string(c.load_pct) + "%";
  s += "\n\nPackets       " + std::to_string(d->fuji.Total()) + " since the start\n";
  return put(dst, dstsz, s);
}


// ---- breakpoints ---------------------------------------------------------------

extern "C" const char* nesdebug_space_name(int space)
{
  switch(space)
  {
    case NESDEBUG_SPACE_CPU: return "CPU";
    case NESDEBUG_SPACE_PPU: return "PPU";
    case NESDEBUG_SPACE_PALETTE: return "Palette";
    case NESDEBUG_SPACE_OAM: return "OAM";
    case NESDEBUG_SPACE_PRG: return "PRG";
    case NESDEBUG_SPACE_CHR: return "CHR";
    case NESDEBUG_SPACE_WRAM: return "WRAM";
    case NESDEBUG_SPACE_RAM: return "RAM";
    default: return "?";
  }
}

extern "C" int nesdebug_breakpoint_check(nesdebug* d, uint16_t addr)
{
  if(!d) return 0;
  std::lock_guard<std::mutex> lock(d->mutex);
  for(const Bp& b : d->bps)
    if(b.space == NESDEBUG_SPACE_CPU && (b.type & NESDEBUG_BP_EXEC) && b.start == addr && b.end == addr) return 1;
  return 0;
}

extern "C" int nesdebug_breakpoint_toggle(nesdebug* d, uint16_t addr)
{
  if(!d) return 0;
  int now = 0;
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    auto it = std::find_if(d->bps.begin(), d->bps.end(), [&](const Bp& b) {
      return b.space == NESDEBUG_SPACE_CPU && (b.type & NESDEBUG_BP_EXEC) && b.start == addr && b.end == addr;
    });
    if(it != d->bps.end())
      d->bps.erase(it);
    else
    {
      d->bps.push_back(Bp{ d->nextId++, NESDEBUG_BP_EXEC, NESDEBUG_SPACE_CPU, addr, addr, true, "", 0 });
      now = 1;
    }
  }
  push_breakpoints(d);
  bump(d);
  return now;
}

extern "C" int nesdebug_breakpoint_add_in(nesdebug* d, int space, int type, uint32_t start, uint32_t end,
                                          const char* condition, char* err, int errsz)
{
  if(!d) { set_err(err, errsz, "no debugger"); return -1; }
  if(space < NESDEBUG_SPACE_CPU || space > NESDEBUG_SPACE_OAM)
  {
    set_err(err, errsz, "breakpoints take the CPU, PPU, palette and OAM spaces");
    return -1;
  }
  if(!(type & (NESDEBUG_BP_EXEC | NESDEBUG_BP_READ | NESDEBUG_BP_WRITE)))
  {
    set_err(err, errsz, "no breakpoint type");
    return -1;
  }
  if((type & NESDEBUG_BP_EXEC) && (space != NESDEBUG_SPACE_CPU || (type & ~NESDEBUG_BP_EXEC)))
  {
    set_err(err, errsz, "execute breakpoints are CPU-space only and stand alone");
    return -1;
  }
  if(end < start) std::swap(start, end);
  if(end > bp_space_limit(space))
  {
    set_err(err, errsz, std::string("address out of range for the ") + nesdebug_space_name(space) + " space");
    return -1;
  }
  const std::string cond = condition ? condition : "";
  std::string translated, why;
  if(!translate_condition(space, cond, translated, why))
  {
    set_err(err, errsz, why);
    return -1;
  }
  const bool ok = with_dbg(d, true, [&](Debugger* dbg) { return condition_parses(dbg, translated); });
  if(!ok)
  {
    set_err(err, errsz, "the condition does not parse: " + cond);
    return -1;
  }
  int id;
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    if((int)d->bps.size() >= NESDEBUG_MAX_BREAKPOINTS)
    {
      set_err(err, errsz, "too many breakpoints");
      return -1;
    }
    id = d->nextId++;
    d->bps.push_back(Bp{ id, type, space, start, end, true, cond, 0 });
  }
  push_breakpoints(d);
  bump(d);
  return id;
}

extern "C" int nesdebug_breakpoint_add(nesdebug* d, int type, uint16_t start, uint16_t end,
                                       const char* condition)
{
  return nesdebug_breakpoint_add_in(d, NESDEBUG_SPACE_CPU, type, start, end, condition, nullptr, 0);
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

extern "C" int nesdebug_breakpoint_set_condition(nesdebug* d, int id, const char* condition,
                                                 char* err, int errsz)
{
  if(!d) { set_err(err, errsz, "no debugger"); return 0; }
  const std::string cond = condition ? condition : "";
  int space = -1;
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    for(const Bp& b : d->bps)
      if(b.id == id) space = b.space;
  }
  if(space < 0) { set_err(err, errsz, "no such breakpoint"); return 0; }
  std::string translated, why;
  if(!translate_condition(space, cond, translated, why)) { set_err(err, errsz, why); return 0; }
  if(!with_dbg(d, true, [&](Debugger* dbg) { return condition_parses(dbg, translated); }))
  {
    set_err(err, errsz, "the condition does not parse: " + cond);
    return 0;
  }
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    for(Bp& b : d->bps)
      if(b.id == id) b.condition = cond;
  }
  push_breakpoints(d);
  bump(d);
  return 1;
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
    o.space = b.space;
    o.start = (uint16_t)b.start;
    o.end = (uint16_t)b.end;
    o.enabled = b.enabled;
    o.hits = b.hits;
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

extern "C" void nesdebug_breakpoint_clear_hits(nesdebug* d)
{
  if(!d) return;
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    for(Bp& b : d->bps) b.hits = 0;
  }
  bump(d);
}

// ---- expressions -----------------------------------------------------------------------

extern "C" int nesdebug_eval(nesdebug* d, const char* text, long* out, char* err, int errsz)
{
  if(!text || !*text) { set_err(err, errsz, "empty expression"); return -1; }
  if(!d || !live(d)) { set_err(err, errsz, "the emulator is not running"); return -1; }
  if(!d->attached.load()) { set_err(err, errsz, "the debugger is not attached"); return -1; }
  std::string expr = substitute_labels(d, text);
  // value, address and old name the access the machine is stopped at
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    if(d->stopped && (d->stop.reason == NESDEBUG_STOP_WATCH_READ || d->stop.reason == NESDEBUG_STOP_WATCH_WRITE))
    {
      expr = replace_word(expr, "value", std::to_string(d->stop.value), nullptr);
      expr = replace_word(expr, "address", std::to_string(d->stop.addr), nullptr);
      expr = replace_word(expr, "old", std::to_string(d->stop.old_value >= 0 ? d->stop.old_value : d->stop.value), nullptr);
    }
    else
      expr = replace_word(expr, "old", "0", nullptr);
  }
  {
    std::string why;
    if(!syntax_ok(expr, why)) { set_err(err, errsz, "does not parse: " + std::string(text) + " (" + why + ")"); return -1; }
  }
  return with_dbg(d, -1, [&](Debugger* dbg) {
    EvalResultType rt = EvalResultType::Numeric;
    const int64_t v = dbg->EvaluateExpression(expr, CpuType::Nes, rt, false);
    switch(rt)
    {
      case EvalResultType::Invalid: set_err(err, errsz, std::string("does not parse: ") + text); return -1;
      case EvalResultType::DivideBy0: set_err(err, errsz, "division by zero"); return -1;
      case EvalResultType::OutOfScope: set_err(err, errsz, "out of scope"); return -1;
      default: break;
    }
    if(out) *out = (long)v;
    return 0;
  });
}

// ---- memory in every space ------------------------------------------------------------------

namespace {

struct SpaceInfo
{
  MemoryType type;
  uint32_t size;
  bool ok;
};

SpaceInfo space_info(MemoryDumper* md, int space)
{
  SpaceInfo s = { MemoryType::NesMemory, 0, true };
  switch(space)
  {
    case NESDEBUG_SPACE_CPU: s.type = MemoryType::NesMemory; s.size = 0x10000; break;
    case NESDEBUG_SPACE_PPU: s.type = MemoryType::NesPpuMemory; s.size = 0x4000; break;
    case NESDEBUG_SPACE_PALETTE: s.type = MemoryType::NesPaletteRam; s.size = 32; break;
    case NESDEBUG_SPACE_OAM: s.type = MemoryType::NesSpriteRam; s.size = 256; break;
    case NESDEBUG_SPACE_PRG: s.type = MemoryType::NesPrgRom; s.size = md->GetMemorySize(s.type); break;
    case NESDEBUG_SPACE_CHR:
      s.type = MemoryType::NesChrRam; s.size = md->GetMemorySize(s.type);
      if(!s.size) { s.type = MemoryType::NesChrRom; s.size = md->GetMemorySize(s.type); }
      break;
    case NESDEBUG_SPACE_WRAM: s.type = MemoryType::NesWorkRam; s.size = md->GetMemorySize(s.type); break;
    case NESDEBUG_SPACE_RAM: s.type = MemoryType::NesInternalRam; s.size = 2048; break;
    default: s.ok = false; break;
  }
  return s;
}

uint8_t space_read_byte(MemoryDumper* md, const SpaceInfo& s, int space, uint32_t a)
{
  a %= s.size;
  if(space == NESDEBUG_SPACE_PPU && a >= 0x3F00)
    return md->GetMemoryValue(MemoryType::NesPaletteRam, a & 0x1F, true);
  return md->GetMemoryValue(s.type, a, true);
}

} // namespace

extern "C" int nesdebug_space_size(nesdebug* d, int space)
{
  return with_dbg(d, 0, [&](Debugger* dbg) {
    SpaceInfo s = space_info(dbg->GetMemoryDumper(), space);
    return s.ok ? (int)s.size : 0;
  });
}

extern "C" int nesdebug_read_space(nesdebug* d, int space, uint32_t addr, uint8_t* dst, int n)
{
  if(!dst || n <= 0) return 0;
  return with_dbg(d, 0, [&](Debugger* dbg) {
    MemoryDumper* md = dbg->GetMemoryDumper();
    SpaceInfo s = space_info(md, space);
    if(!s.ok || !s.size) return 0;
    for(int i = 0; i < n; i++)
      dst[i] = space_read_byte(md, s, space, addr + (uint32_t)i);
    return n;
  });
}

extern "C" int nesdebug_write_space(nesdebug* d, int space, uint32_t addr, const uint8_t* src, int n)
{
  if(!src || n <= 0) return 0;
  if(space == NESDEBUG_SPACE_CPU)
  {
    for(int i = 0; i < n; i++) nesdebug_write(d, (uint16_t)(addr + (uint32_t)i), src[i]);
    return n;
  }
  const int done = with_dbg(d, 0, [&](Debugger* dbg) {
    MemoryDumper* md = dbg->GetMemoryDumper();
    SpaceInfo s = space_info(md, space);
    if(!s.ok || !s.size) return 0;
    for(int i = 0; i < n; i++)
    {
      uint32_t a = (addr + (uint32_t)i) % s.size;
      if(space == NESDEBUG_SPACE_PPU && a >= 0x3F00)
        md->SetMemoryValue(MemoryType::NesPaletteRam, a & 0x1F, src[i], true);
      else
        md->SetMemoryValue(s.type, a, src[i], true);
    }
    return n;
  });
  bump(d);
  return done;
}

// ---- search --------------------------------------------------------------------------------------

namespace {

bool bytes_match(const std::vector<uint8_t>& buf, uint32_t pos, const uint8_t* pat, int count, uint8_t mask,
                 bool nocase)
{
  if(pos + (uint32_t)count > buf.size()) return false;
  for(int i = 0; i < count; i++)
  {
    uint8_t a = buf[pos + (uint32_t)i], b = pat[i];
    if(nocase && isalpha(b)) { a = (uint8_t)tolower(a); b = (uint8_t)tolower(b); }
    if((a & mask) != (b & mask)) return false;
  }
  return true;
}

// The whole space as bytes, for a scan.
bool space_snapshot(nesdebug* d, int space, std::vector<uint8_t>& buf)
{
  return with_dbg(d, false, [&](Debugger* dbg) {
    MemoryDumper* md = dbg->GetMemoryDumper();
    SpaceInfo s = space_info(md, space);
    if(!s.ok || !s.size) return false;
    buf.resize(s.size);
    for(uint32_t i = 0; i < s.size; i++) buf[i] = space_read_byte(md, s, space, i);
    return true;
  });
}

} // namespace

extern "C" int nesdebug_search_in(nesdebug* d, int space, const uint8_t* pat, int count, uint8_t mask, int flags,
                                  uint32_t from, uint32_t to, uint32_t* out, int max)
{
  if(!pat || count <= 0 || !out || max <= 0) return -1;
  std::vector<uint8_t> buf;
  if(!space_snapshot(d, space, buf)) return -1;
  int n = 0;
  const uint32_t last = std::min<uint32_t>(to, (uint32_t)buf.size() - 1);
  for(uint32_t a = from; a <= last && n < max; a++)
    if(bytes_match(buf, a, pat, count, mask, (flags & NESDEBUG_SEARCH_NOCASE) != 0)) out[n++] = a;
  return n;
}

extern "C" int nesdebug_search(nesdebug* d, const uint8_t* pat, int count, uint8_t mask, int flags,
                               uint16_t from, uint16_t to, uint16_t* out, int max)
{
  if(!out || max <= 0) return -1;
  std::vector<uint32_t> hits((size_t)max);
  const int n = nesdebug_search_in(d, NESDEBUG_SPACE_CPU, pat, count, mask, flags, from, to, hits.data(), max);
  for(int i = 0; i < n; i++) out[i] = (uint16_t)hits[(size_t)i];
  return n;
}

extern "C" int nesdebug_search_next_in(nesdebug* d, int space, const uint8_t* pat, int count, uint8_t mask,
                                       int flags, uint32_t from, int backward, uint32_t* found)
{
  if(!pat || count <= 0) return -1;
  std::vector<uint8_t> buf;
  if(!space_snapshot(d, space, buf)) return -1;
  const bool nocase = (flags & NESDEBUG_SEARCH_NOCASE) != 0;
  const uint32_t size = (uint32_t)buf.size();
  for(uint32_t step = 1; step <= size; step++)
  {
    const uint32_t a = backward ? (from + size - step % size) % size : (from + step) % size;
    if(bytes_match(buf, a, pat, count, mask, nocase))
    {
      if(found) *found = a;
      return 1;
    }
  }
  return 0;
}

extern "C" int nesdebug_search_next(nesdebug* d, const uint8_t* pat, int count, uint8_t mask, int flags,
                                    uint16_t from, int backward, uint16_t* found)
{
  uint32_t f = 0;
  const int r = nesdebug_search_next_in(d, NESDEBUG_SPACE_CPU, pat, count, mask, flags, from, backward, &f);
  if(r > 0 && found) *found = (uint16_t)f;
  return r;
}

// ---- the CPU memory map ----------------------------------------------------------------------------

extern "C" int nesdebug_memory_map(nesdebug* d, nesdebug_region* out, int max)
{
  if(!out || max <= 0) return 0;
  FujiNetCartStatus st;
  const bool cart = FujiNetCart::GetStatus(st);
  int n = 0;
  auto add = [&](uint16_t low, uint16_t high, int r, int w, int bank, const std::string& name) {
    if(n >= max) return;
    nesdebug_region& e = out[n++];
    e.low = low; e.high = high; e.readable = r; e.writable = w; e.bank = bank;
    snprintf(e.name, sizeof e.name, "%s", name.c_str());
  };
  add(0x0000, 0x07FF, 1, 1, -1, "Console RAM (2K)");
  add(0x0800, 0x1FFF, 1, 1, -1, "Console RAM mirrors");
  add(0x2000, 0x2007, 1, 1, -1, "PPU registers");
  add(0x2008, 0x3FFF, 1, 1, -1, "PPU register mirrors");
  add(0x4000, 0x4013, 0, 1, -1, "APU channel registers");
  add(0x4014, 0x4014, 0, 1, -1, "OAM DMA");
  add(0x4015, 0x4015, 1, 1, -1, "APU status / enable");
  add(0x4016, 0x4017, 1, 1, -1, "Controllers / frame counter");
  add(0x4018, 0x401F, 0, 0, -1, "APU test (disabled)");
  add(0x4020, 0x4FFF, 0, 0, -1, "Expansion (open bus)");
  if(!cart || st.MailboxLive)
    add(0x5000, 0x54FF, 1, 0, -1, "FujiNet mailbox: reply and status");
  else
    add(0x5000, 0x54FF, 0, 0, -1, "FujiNet mailbox (closed)");
  add(0x5500, 0x57FF, 0, 1, -1, "FujiNet mailbox: registers and data");
  add(0x5800, 0x5FFF, 1, 0, -1, "FujiNet loader ROM (2K)");
  if(!cart || st.WramEnabled)
    add(0x6000, 0x7FFF, 1, st.WramProtected ? 0 : 1, -1,
        st.WramProtected ? "WRAM (8K, write-protected)" : "WRAM (8K)");
  else
    add(0x6000, 0x7FFF, 0, 0, -1, "WRAM (off)");
  if(!cart || st.SramEnabled)
  {
    for(int i = 0; i < 4; i++)
    {
      const uint16_t low = (uint16_t)(0x8000 + i * 0x2000);
      // The bank the CPU would read now: the debugger's own view of the mapping. The cartridge's
      // published status is one frame old, and while the loader runs the $8000 window follows the
      // image slice by slice, so two reads of the status could name different banks.
      int bank = cart ? st.PrgSlot[i] : -1;
      const int live = with_dbg(d, -2, [&](Debugger* dbg) {
        const AddressInfo abs = dbg->GetAbsoluteAddress(cpu_addr(low));
        return (abs.Address >= 0 && abs.Type == MemoryType::NesPrgRom) ? (abs.Address >> 13) : -1;
      });
      if(live != -2) bank = live;
      add(low, (uint16_t)(low + 0x1FFF), 1, 0, bank,
          "PRG window $" + hex(low, 4) + (bank >= 0 ? " (bank " + std::to_string(bank) + ")" : "") +
          (cart && st.Loading ? " (loader)" : ""));
    }
  }
  else
  {
    add(0x8000, 0xFEFF, 0, 0, -1, "PRG off (SRAM disabled)");
    add(0xFF00, 0xFFFF, 1, 0, -1, "Vector page");
  }
  return n;
}

// ---- trace and history ----------------------------------------------------------------------------

namespace {

bool trace_apply_options(Debugger* dbg, bool on)
{
  ITraceLogger* logger = dbg->GetTraceLogger(CpuType::Nes);
  if(!logger) return false;
  TraceLoggerOptions o = {};
  o.Enabled = on;
  o.IndentCode = false;
  o.UseLabels = true;
  snprintf(o.Format, sizeof o.Format, "%s", TraceFormat);
  logger->SetOptions(o);
  return true;
}

bool parse_trace_row(const TraceRow& row, nesdebug_trace_entry& e)
{
  memset(&e, 0, sizeof e);
  unsigned pc, a, x, y, sp, ps, frame;
  unsigned long long cycles;
  int scanline, dot;
  int consumed = 0;
  if(sscanf(row.LogOutput, "%x|%x|%x|%x|%x|%x|%llu|%d|%d|%u|%n", &pc, &a, &x, &y, &sp, &ps, &cycles,
            &scanline, &dot, &frame, &consumed) < 10 || consumed <= 0)
    return false;
  e.pc = (uint16_t)pc;
  e.a = (uint8_t)a; e.x = (uint8_t)x; e.y = (uint8_t)y; e.sp = (uint8_t)sp; e.ps = (uint8_t)ps;
  e.cycles = cycles;
  e.scanline = (int16_t)scanline;
  e.dot = (uint16_t)dot;
  e.frame = frame;
  e.len = (uint8_t)std::min<int>(3, std::max<int>(1, row.ByteCodeSize));
  memcpy(e.bytes, row.ByteCode, e.len);
  snprintf(e.disasm, sizeof e.disasm, "%s", row.LogOutput + consumed);
  // trailing padding
  for(int i = (int)strlen(e.disasm) - 1; i >= 0 && e.disasm[i] == ' '; i--) e.disasm[i] = '\0';
  return true;
}

// Newest first, up to max, starting `skip` rows back.
int read_live(Debugger* dbg, std::vector<nesdebug_trace_entry>& out, int skip, int max)
{
  ITraceLogger* logger = dbg->GetTraceLogger(CpuType::Nes);
  if(!logger || !logger->IsEnabled() || max <= 0) return 0;
  std::vector<TraceRow> rows((size_t)max);
  const uint32_t got = dbg->GetExecutionTrace(rows.data(), (uint32_t)skip, (uint32_t)max);
  int n = 0;
  for(uint32_t i = 0; i < got; i++)
  {
    nesdebug_trace_entry e;
    if(parse_trace_row(rows[i], e)) { out.push_back(e); n++; }
  }
  return n;
}

int live_count(Debugger* dbg)
{
  ITraceLogger* logger = dbg->GetTraceLogger(CpuType::Nes);
  if(!logger || !logger->IsEnabled()) return 0;
  return (int)dbg->GetExecutionTrace(nullptr, 0, NESDEBUG_TRACE_SIZE);
}

// The logger's ring into our own copy (the logger is about to go away, or
// to be turned off, which hides its rows). Newest rows are appended last.
void trace_stash(nesdebug* d)
{
  with_dbg_void(d, [&](Debugger* dbg) {
    std::vector<nesdebug_trace_entry> rows;   // newest first
    for(int skip = 0; skip < NESDEBUG_TRACE_SIZE; skip += 1000)
    {
      const int got = read_live(dbg, rows, skip, 1000);
      if(got < 1000) break;
    }
    if(rows.empty()) return;
    std::lock_guard<std::mutex> lock(d->mutex);
    d->saved.insert(d->saved.end(), rows.rbegin(), rows.rend());
    if(d->saved.size() > (size_t)NESDEBUG_TRACE_SIZE)
      d->saved.erase(d->saved.begin(), d->saved.end() - NESDEBUG_TRACE_SIZE);
    dbg->ClearExecutionTrace();
  });
}

void trace_apply(nesdebug* d, Debugger* dbg)
{
  bool on;
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    on = d->traceOn;
  }
  trace_apply_options(dbg, on);
}

} // namespace

extern "C" void nesdebug_trace_enable(nesdebug* d, int enable)
{
  if(!d) return;
  const bool on = enable != 0;
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    if(d->traceOn == on) return;
  }
  if(!on) trace_stash(d);
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    d->traceOn = on;
  }
  with_dbg_void(d, [&](Debugger* dbg) { trace_apply_options(dbg, on); });
  bump(d);
}

extern "C" int nesdebug_trace_enabled(nesdebug* d)
{
  if(!d) return 0;
  std::lock_guard<std::mutex> lock(d->mutex);
  return d->traceOn ? 1 : 0;
}

extern "C" int nesdebug_trace_read(nesdebug* d, nesdebug_trace_entry* out, int max)
{
  if(!d || !out || max <= 0) return 0;
  std::vector<nesdebug_trace_entry> rows;
  with_dbg_void(d, [&](Debugger* dbg) { read_live(dbg, rows, 0, max); });
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    for(auto it = d->saved.rbegin(); it != d->saved.rend() && (int)rows.size() < max; ++it)
      rows.push_back(*it);
  }
  const int n = std::min<int>(max, (int)rows.size());
  for(int i = 0; i < n; i++) out[i] = rows[(size_t)i];
  return n;
}

extern "C" void nesdebug_trace_clear(nesdebug* d)
{
  if(!d) return;
  with_dbg_void(d, [&](Debugger* dbg) { dbg->ClearExecutionTrace(); });
  std::lock_guard<std::mutex> lock(d->mutex);
  d->saved.clear();
  d->traceBase = ITraceLogger::NextRowId;
  ++d->generation;
}

extern "C" int nesdebug_trace_count(nesdebug* d)
{
  if(!d) return 0;
  const int live = with_dbg(d, 0, [&](Debugger* dbg) { return live_count(dbg); });
  std::lock_guard<std::mutex> lock(d->mutex);
  return std::min<int>(NESDEBUG_TRACE_SIZE, live + (int)d->saved.size());
}

extern "C" uint64_t nesdebug_trace_total(nesdebug* d)
{
  if(!d) return 0;
  std::lock_guard<std::mutex> lock(d->mutex);
  return ITraceLogger::NextRowId - d->traceBase;
}

extern "C" int nesdebug_trace_format(const nesdebug_trace_entry* rec, char* dst, int dstsz)
{
  if(!rec || !dst || dstsz <= 0) return 0;
  return snprintf(dst, (size_t)dstsz, "%04X  %-22s A=%02X X=%02X Y=%02X SP=%02X P=%c%c--%c%c%c%c", rec->pc,
                  rec->disasm, rec->a, rec->x, rec->y, rec->sp, (rec->ps & 0x80) ? 'N' : 'n',
                  (rec->ps & 0x40) ? 'V' : 'v', (rec->ps & 0x08) ? 'D' : 'd', (rec->ps & 0x04) ? 'I' : 'i',
                  (rec->ps & 0x02) ? 'Z' : 'z', (rec->ps & 0x01) ? 'C' : 'c');
}

extern "C" int nesdebug_trace_match(const nesdebug_trace_entry* rec, int filter, uint16_t low, uint16_t high)
{
  if(!rec) return 0;
  switch(filter)
  {
    case NESDEBUG_TRACE_BRANCHES: {
      const uint8_t op = rec->bytes[0];
      // BRK RTI JSR RTS JMP abs/ind and the eight relative branches
      return op == 0x00 || op == 0x40 || op == 0x20 || op == 0x60 || op == 0x4C || op == 0x6C ||
             (op & 0x1F) == 0x10;
    }
    case NESDEBUG_TRACE_RANGE: return rec->pc >= low && rec->pc <= high;
    default: return 1;
  }
}

extern "C" void nesdebug_history_enable(nesdebug* d, int on) { nesdebug_trace_enable(d, on); }
extern "C" int nesdebug_history_enabled(nesdebug* d) { return nesdebug_trace_enabled(d); }
extern "C" void nesdebug_history_clear(nesdebug* d) { nesdebug_trace_clear(d); }
extern "C" uint64_t nesdebug_history_count(nesdebug* d) { return (uint64_t)nesdebug_trace_count(d); }

extern "C" int nesdebug_history_copy(nesdebug* d, nesdebug_hist_entry* out, int max)
{
  const int n = nesdebug_trace_read(d, out, max);
  std::reverse(out, out + n);
  return n;
}

extern "C" int nesdebug_history_format(const nesdebug_hist_entry* rec, char* dst, int dstsz)
{
  return nesdebug_trace_format(rec, dst, dstsz);
}

extern "C" int nesdebug_history_match(const nesdebug_hist_entry* rec, int filter, uint16_t low, uint16_t high)
{
  return nesdebug_trace_match(rec, filter, low, high);
}

// ---- sound ------------------------------------------------------------------------------------------

namespace {

void fill_voice(nesdebug_voice& v, bool enabled, int period, double hz, int volume, const ApuLengthCounterState& len)
{
  v.enabled = enabled;
  v.period = period;
  v.hz = hz;
  v.volume = volume;
  v.length = len.Counter;
  v.length_halt = len.Halt;
}

void fill_env(nesdebug_voice& v, const ApuEnvelopeState& e)
{
  v.env_constant = e.ConstantVolume;
  v.env_loop = e.Loop;
  v.env_volume = e.Volume;
  v.env_counter = e.Counter;
}

void fill_pulse(nesdebug_voice& v, const ApuSquareState& s)
{
  fill_voice(v, s.Enabled, s.Period, s.Frequency, s.OutputVolume, s.LengthCounter);
  fill_env(v, s.Envelope);
  v.duty = s.Duty;
  v.sweep_enabled = s.SweepEnabled;
  v.sweep_negate = s.SweepNegate;
  v.sweep_period = s.SweepPeriod;
  v.sweep_shift = s.SweepShift;
}

} // namespace

extern "C" int nesdebug_sound_get(nesdebug* d, nesdebug_sound* out)
{
  if(!out) return 0;
  memset(out, 0, sizeof *out);
  return with_dbg(d, 0, [&](Debugger* dbg) {
    NesState st = {};
    dbg->GetConsoleState(reinterpret_cast<BaseState&>(st), ConsoleType::Nes);
    const ApuState& a = st.Apu;
    out->present = 1;
    fill_pulse(out->pulse1, a.Square1);
    fill_pulse(out->pulse2, a.Square2);
    fill_voice(out->triangle, a.Triangle.Enabled, a.Triangle.Period, a.Triangle.Frequency,
               a.Triangle.OutputVolume, a.Triangle.LengthCounter);
    out->triangle.linear = a.Triangle.LinearCounter;
    out->triangle.linear_reload = a.Triangle.LinearCounterReload;
    fill_voice(out->noise, a.Noise.Enabled, a.Noise.Period, a.Noise.Frequency, a.Noise.OutputVolume,
               a.Noise.LengthCounter);
    fill_env(out->noise, a.Noise.Envelope);
    out->noise.noise_mode = a.Noise.ModeFlag;
    out->dmc.enabled = a.Dmc.BytesRemaining > 0;
    out->dmc.irq = a.Dmc.IrqEnabled;
    out->dmc.loop = a.Dmc.Loop;
    out->dmc.rate_hz = a.Dmc.SampleRate;
    out->dmc.period = a.Dmc.Period;
    out->dmc.address = a.Dmc.NextSampleAddr;
    out->dmc.start_address = a.Dmc.SampleAddr;
    out->dmc.length = a.Dmc.SampleLength;
    out->dmc.bytes_left = a.Dmc.BytesRemaining;
    out->dmc.volume = a.Dmc.OutputVolume;
    out->five_step = a.FrameCounter.FiveStepMode;
    out->frame_irq = a.FrameCounter.IrqEnabled;
    out->dmc_irq = a.Dmc.IrqEnabled;
    out->status = (uint8_t)((a.Square1.LengthCounter.Counter ? 1 : 0) | (a.Square2.LengthCounter.Counter ? 2 : 0) |
                            (a.Triangle.LengthCounter.Counter ? 4 : 0) | (a.Noise.LengthCounter.Counter ? 8 : 0) |
                            (a.Dmc.BytesRemaining ? 16 : 0));
    out->cpu_hz = st.ClockRate;
    return 1;
  });
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
