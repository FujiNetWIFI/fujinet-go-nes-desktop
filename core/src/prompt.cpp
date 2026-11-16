/*
 * The console (nesdebug_command): one command per call over the public
 * engine API, so the pure parts can be tested with no machine.
 *
 * Verbs: help, step [n], over, out, run, stop, frame [n], scanline [n],
 * runto <addr>, break <addr> [cond], bpr/bpw/bpa [ppu|pal|oam] <addr>[-<end>]
 * [cond], delete <id|addr>, enable/disable <id>, clear, hits [clear], breaks,
 * print <expr>, regs, set <reg> <expr>, mem [space] <addr> [len],
 * poke [space] <addr> <val...>, ppu <addr> [len], disasm [addr] [n],
 * find [space] [hex] <bytes> | find [space] text <string>, trace [on|off|clear|n],
 * watch [expr|lo-hi], unwatch <n|all>, symbols [text], label <addr> <name>,
 * labels <file>, map, cart, io, sound, video [section], save <kind> <file>.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#include "nesdebug.h"
#include "nesmemtools.h"
}

namespace {

const char* (*gVideoText)(const char* section) = nullptr;

std::string hex(uint32_t v, int digits)
{
  char buf[16];
  snprintf(buf, sizeof buf, "%0*X", digits, v);
  return buf;
}

int put(char* dst, int dstsz, const std::string& s)
{
  if(!dst || dstsz <= 0) return 0;
  return snprintf(dst, (size_t)dstsz, "%s", s.c_str());
}

std::string lower(std::string s)
{
  for(char& c : s) c = (char)tolower((unsigned char)c);
  return s;
}

const char* const Commands[] = {
  "help", "step", "over", "out", "run", "stop", "frame", "scanline", "runto", "break", "bpr", "bpw", "bpa",
  "delete", "enable", "disable", "clear", "hits", "breaks", "print", "regs", "set", "mem", "poke", "ppu",
  "disasm", "find", "trace", "watch", "unwatch", "symbols", "label", "labels", "map", "cart", "io", "sound",
  "video", "save",
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
    "bpr|bpw|bpa [ppu|pal|oam] <a>[-<b>] [cond]\n"
    "                     read / write / read-or-write breakpoint on a range;\n"
    "                     ppu = pattern+nametable, pal = palette RAM, oam = sprites\n"
    "                     cond: value address old, regs, [addr], labels ...\n"
    "delete <id|addr>     remove a breakpoint;  clear  remove all\n"
    "enable|disable <id>  switch one;  breaks  list;  hits [clear]  hit counts\n"
    "print <expr>         evaluate (a x y sp ps pc cycle scanline frame [$addr]\n"
    "                     {$addr16} labels ...)\n"
    "regs                 CPU registers;  set <reg> <expr>  (pc sp a x y ps n v d i z c)\n"
    "mem [space] <addr> [len]   dump (cpu ppu pal oam prg chr wram ram)\n"
    "ppu <addr> [len]     PPU memory;  poke [space] <addr> <val...>  write\n"
    "disasm [addr] [n]    disassemble\n"
    "find [space] [hex] <bytes> | find [space] text <string>\n"
    "trace [on|off|clear|n]   record / show the last n instructions\n"
    "watch [expr|lo-hi]   add a watch (none: list);  unwatch <n|all>\n"
    "symbols [text]       list labels;  label <addr> <name>;  labels <file>  load\n"
    "map                  the CPU memory map;  cart  the cartridge;  io  I/O state\n"
    "sound                the 2A03 channels;  video [section]  the Video tab\n"
    "save <dis|prg|chr|ram|wram> <file>\n";
}

// "cpu", "ppu", "pal"/"palette", "oam", "prg", "chr", "wram", "ram"
int space_word(const std::string& w)
{
  const std::string l = lower(w);
  if(l == "cpu") return NESDEBUG_SPACE_CPU;
  if(l == "ppu") return NESDEBUG_SPACE_PPU;
  if(l == "pal" || l == "palette") return NESDEBUG_SPACE_PALETTE;
  if(l == "oam") return NESDEBUG_SPACE_OAM;
  if(l == "prg") return NESDEBUG_SPACE_PRG;
  if(l == "chr") return NESDEBUG_SPACE_CHR;
  if(l == "wram") return NESDEBUG_SPACE_WRAM;
  if(l == "ram") return NESDEBUG_SPACE_RAM;
  return -1;
}

// An address in a space: a number or label in the CPU space, plain hex
// elsewhere ($ and 0x accepted).
bool space_address(nesdebug* d, int space, const std::string& tok, uint32_t& out)
{
  if(tok.empty()) return false;
  if(space == NESDEBUG_SPACE_CPU)
  {
    uint16_t lo = 0;
    if(!nessym_parse_range(d, tok.c_str(), &lo, nullptr)) return false;
    out = lo;
    return true;
  }
  std::string t = tok;
  if(t[0] == '$') t = t.substr(1);
  else if(t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t = t.substr(2);
  if(t.empty() || t.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
  out = (uint32_t)strtoul(t.c_str(), nullptr, 16);
  return true;
}

// A byte value: $FF, 0xFF or decimal.
bool byte_value(const std::string& tok, unsigned& out)
{
  if(tok.empty()) return false;
  std::string t = tok;
  int base = 10;
  if(t[0] == '$') { t = t.substr(1); base = 16; }
  else if(t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) { t = t.substr(2); base = 16; }
  if(t.empty()) return false;
  char* end = nullptr;
  const unsigned long v = strtoul(t.c_str(), &end, base);
  if(!end || *end || v > 0xFF) return false;
  out = (unsigned)v;
  return true;
}

std::string hexdump(const uint8_t* data, int n, uint32_t base, int digits)
{
  std::string s;
  for(int i = 0; i < n; i += 16)
  {
    s += hex(base + (uint32_t)i, digits) + ":";
    for(int j = 0; j < 16 && i + j < n; j++) s += " " + hex(data[i + j], 2);
    s += "\n";
  }
  return s;
}

std::string stop_text(nesdebug* d)
{
  char buf[200];
  nesdebug_stop_text(d, buf, sizeof buf, nullptr);
  return buf;
}

std::string bp_line(nesdebug* d, const nesdebug_breakpoint& b)
{
  char desc[400];
  nesbp_describe(d, &b, desc, sizeof desc);
  std::string s = std::to_string(b.id) + ": " + desc;
  if(!b.enabled) s += "  (disabled)";
  if(b.hits) s += "  hits " + std::to_string(b.hits);
  return s;
}

} // namespace

extern "C" void nesdebug_set_video_text(const char* (*func)(const char* section))
{
  gVideoText = func;
}

extern "C" int nesdebug_command(nesdebug* d, const char* command, char* dst, int dstsz)
{
  if(!d || !command) return put(dst, dstsz, "");
  if(!nesdebug_is_attached(d))
  {
    nesdebug_attach(d);
    if(!nesdebug_is_attached(d)) return put(dst, dstsz, "the emulator is not running");
  }

  std::string line(command);
  while(!line.empty() && isspace((unsigned char)line.back())) line.pop_back();
  std::istringstream in(line);
  std::string cmd;
  in >> cmd;
  cmd = lower(cmd);
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
  char err[160];

  std::string out;
  if(cmd.empty()) out = "";
  else if(cmd == "help" || cmd == "?") out = help_text();
  else if(cmd == "step" || cmd == "s") { nesdebug_step_count(d, count_arg(0, 1)); out = stop_text(d); }
  else if(cmd == "over" || cmd == "o") { nesdebug_step_over(d); out = stop_text(d); }
  else if(cmd == "out") { nesdebug_step_out(d); out = stop_text(d); }
  else if(cmd == "frame") { nesdebug_frame(d, count_arg(0, 1)); out = stop_text(d); }
  else if(cmd == "scanline") { nesdebug_scanline(d, count_arg(0, 1)); out = stop_text(d); }
  else if(cmd == "run" || cmd == "go" || cmd == "g") { nesdebug_resume(d); out = "running"; }
  else if(cmd == "stop" || (cmd == "break" && args.empty()))
  {
    nesdebug_stop(d);
    out = stop_text(d);
  }
  else if(cmd == "runto")
  {
    uint32_t a;
    if(args.empty() || !space_address(d, NESDEBUG_SPACE_CPU, args[0], a)) out = "usage: runto <addr>";
    else { nesdebug_run_to(d, (uint16_t)a); out = "running to $" + hex(a, 4); }
  }
  else if(cmd == "break" || cmd == "bp")
  {
    uint32_t a;
    if(args.empty()) out = "usage: break <addr> [condition]";
    else if(!space_address(d, NESDEBUG_SPACE_CPU, args[0], a)) out = "bad address: " + args[0];
    else if(args.size() == 1)
      out = nesdebug_breakpoint_toggle(d, (uint16_t)a) ? "breakpoint set at $" + hex(a, 4)
                                                     : "breakpoint cleared at $" + hex(a, 4);
    else
    {
      const int id = nesdebug_breakpoint_add_in(d, NESDEBUG_SPACE_CPU, NESDEBUG_BP_EXEC, a, a,
                                                rest_from(1).c_str(), err, sizeof err);
      out = id < 0 ? std::string(err) : "breakpoint " + std::to_string(id) + " at $" + hex(a, 4);
    }
  }
  else if(cmd == "bpr" || cmd == "bpw" || cmd == "bpa")
  {
    size_t first = 0;
    int space = NESDEBUG_SPACE_CPU;
    if(!args.empty())
    {
      const int w = space_word(args[0]);
      if(w >= NESDEBUG_SPACE_PPU && w <= NESDEBUG_SPACE_OAM) { space = w; first = 1; }
    }
    if(args.size() <= first) out = "usage: " + cmd + " [ppu|pal|oam] <addr>[-<end>] [condition]";
    else
    {
      const std::string range = args[first];
      const size_t dash = range.find('-');
      uint32_t a, b;
      if(!space_address(d, space, range.substr(0, dash), a)) out = "bad address: " + range;
      else
      {
        b = a;
        if(dash != std::string::npos && !space_address(d, space, range.substr(dash + 1), b)) b = a;
        const int type = cmd == "bpr" ? NESDEBUG_BP_READ : cmd == "bpw" ? NESDEBUG_BP_WRITE : NESDEBUG_BP_ACCESS;
        const int id = nesdebug_breakpoint_add_in(d, space, type, a, b, rest_from(first + 1).c_str(), err, sizeof err);
        if(id < 0) out = err;
        else
        {
          nesdebug_breakpoint list[NESDEBUG_MAX_BREAKPOINTS];
          const int n = nesdebug_breakpoint_list(d, list, NESDEBUG_MAX_BREAKPOINTS);
          out = "breakpoint " + std::to_string(id);
          for(int i = 0; i < n; i++)
            if(list[i].id == id)
            {
              char desc[400];
              nesbp_describe(d, &list[i], desc, sizeof desc);
              out += ": " + std::string(desc);
            }
        }
      }
    }
  }
  else if(cmd == "delete" || cmd == "del")
  {
    if(args.empty()) out = "usage: delete <id|addr>";
    else
    {
      nesdebug_breakpoint list[NESDEBUG_MAX_BREAKPOINTS];
      const int n = nesdebug_breakpoint_list(d, list, NESDEBUG_MAX_BREAKPOINTS);
      const bool plain = args[0].find_first_not_of("0123456789") == std::string::npos;
      int id = plain ? atoi(args[0].c_str()) : -1;
      bool found = false;
      for(int i = 0; i < n; i++)
        if(list[i].id == id) found = true;
      if(found) { nesdebug_breakpoint_remove(d, id); out = "breakpoint " + std::to_string(id) + " deleted"; }
      else
      {
        uint32_t a;
        if(space_address(d, NESDEBUG_SPACE_CPU, args[0], a) && nesdebug_breakpoint_check(d, (uint16_t)a))
        {
          nesdebug_breakpoint_toggle(d, (uint16_t)a);
          out = "breakpoint cleared at $" + hex(a, 4);
        }
        else out = "no such breakpoint: " + args[0];
      }
    }
  }
  else if(cmd == "enable" || cmd == "disable")
  {
    if(args.empty()) out = "usage: " + cmd + " <id>";
    else { nesdebug_breakpoint_enable(d, atoi(args[0].c_str()), cmd == "enable"); out = "ok"; }
  }
  else if(cmd == "clear") { nesdebug_breakpoint_clear(d); out = "all breakpoints cleared"; }
  else if(cmd == "hits")
  {
    if(!args.empty() && lower(args[0]) == "clear") { nesdebug_breakpoint_clear_hits(d); out = "hit counts cleared"; }
    else
    {
      nesdebug_breakpoint list[NESDEBUG_MAX_BREAKPOINTS];
      const int n = nesdebug_breakpoint_list(d, list, NESDEBUG_MAX_BREAKPOINTS);
      if(!n) out = "no breakpoints";
      for(int i = 0; i < n; i++) out += std::to_string(list[i].id) + ": " + std::to_string(list[i].hits) + "\n";
    }
  }
  else if(cmd == "breaks" || cmd == "list")
  {
    nesdebug_breakpoint list[NESDEBUG_MAX_BREAKPOINTS];
    const int n = nesdebug_breakpoint_list(d, list, NESDEBUG_MAX_BREAKPOINTS);
    if(!n) out = "no breakpoints";
    for(int i = 0; i < n; i++) out += bp_line(d, list[i]) + "\n";
  }
  else if(cmd == "print" || cmd == "p" || cmd == "eval")
  {
    long v = 0;
    if(nesdebug_eval(d, rest_from(0).c_str(), &v, err, sizeof err) != 0) out = err;
    else out = std::to_string(v) + "  $" + hex((uint32_t)v & 0xFFFFFFFFu, v > 0xFFFF ? 8 : v > 0xFF ? 4 : 2);
  }
  else if(cmd == "regs" || cmd == "r")
  {
    nesdebug_cpu c;
    nesdebug_cpu_get(d, &c);
    char buf[200];
    snprintf(buf, sizeof buf, "PC=%04X A=%02X X=%02X Y=%02X SP=%02X P=%02X [%c%c-%c%c%c%c] cyc=%llu sl=%d dot=%d%s%s",
             c.pc, c.a, c.x, c.y, c.sp, c.ps, c.n ? 'N' : 'n', c.v ? 'V' : 'v', c.d ? 'D' : 'd',
             c.i ? 'I' : 'i', c.z ? 'Z' : 'z', c.c ? 'C' : 'c', (unsigned long long)c.total_cycles,
             c.scanline, c.dot, c.irq ? " IRQ" : "", c.nmi ? " NMI" : "");
    out = buf;
  }
  else if(cmd == "set")
  {
    long v = 0;
    if(args.size() < 2) out = "usage: set <reg> <expr>   (pc sp a x y ps n v d i z c)";
    else if(nesdebug_eval(d, rest_from(1).c_str(), &v, err, sizeof err) != 0) out = err;
    else if(!nesdebug_register_set(d, args[0].c_str(), v, err, sizeof err)) out = err;
    else out = "ok";
  }
  else if(cmd == "mem" || cmd == "m" || cmd == "ppu")
  {
    size_t first = 0;
    int space = cmd == "ppu" ? NESDEBUG_SPACE_PPU : NESDEBUG_SPACE_CPU;
    if(cmd != "ppu" && !args.empty() && space_word(args[0]) >= 0) { space = space_word(args[0]); first = 1; }
    uint32_t a;
    if(args.size() <= first || !space_address(d, space, args[first], a)) out = "usage: " + cmd + " [space] <addr> [len]";
    else
    {
      const int len = std::min(1024, args.size() > first + 1 ? (int)strtoul(args[first + 1].c_str(), nullptr, 0) : 64);
      std::vector<uint8_t> buf((size_t)std::max(1, len));
      const int got = nesdebug_read_space(d, space, a, buf.data(), len);
      const int digits = space == NESDEBUG_SPACE_PALETTE || space == NESDEBUG_SPACE_OAM ? 2
                       : space == NESDEBUG_SPACE_CPU || space == NESDEBUG_SPACE_PPU ? 4 : 6;
      out = got > 0 ? hexdump(buf.data(), got, a, digits) : "that space is not available";
    }
  }
  else if(cmd == "poke" || cmd == "w")
  {
    size_t first = 0;
    int space = NESDEBUG_SPACE_CPU;
    if(!args.empty() && space_word(args[0]) >= 0) { space = space_word(args[0]); first = 1; }
    uint32_t a;
    std::vector<uint8_t> bytes;
    for(size_t i = first + 1; i < args.size(); i++)
    {
      unsigned v;
      if(!byte_value(args[i], v)) { bytes.clear(); break; }
      bytes.push_back((uint8_t)v);
    }
    if(args.size() <= first + 1 || bytes.empty() || !space_address(d, space, args[first], a))
      out = "usage: poke [space] <addr> <value...>";
    else
    {
      nesdebug_write_space(d, space, a, bytes.data(), (int)bytes.size());
      out = "ok";
    }
  }
  else if(cmd == "label")
  {
    uint32_t a;
    if(args.size() < 2 || !space_address(d, NESDEBUG_SPACE_CPU, args[0], a)) out = "usage: label <addr> <name>";
    else out = nesdebug_set_label(d, (uint16_t)a, args[1].c_str()) == 0 ? "ok" : "failed";
  }
  else if(cmd == "labels")
  {
    char msg[512];
    nesdebug_load_symbols(d, args.empty() ? nullptr : rest_from(0).c_str(), msg, sizeof msg);
    out = msg;
  }
  else if(cmd == "symbols")
  {
    std::vector<int> idx(512);
    const int n = nesdebug_symbols_filter(d, rest_from(0).c_str(), idx.data(), (int)idx.size());
    if(!n) out = "no symbols";
    for(int i = 0; i < n; i++)
    {
      uint16_t a;
      char name[96];
      if(nesdebug_symbols_get(d, idx[(size_t)i], &a, name, sizeof name)) out += "$" + hex(a, 4) + "  " + name + "\n";
    }
  }
  else if(cmd == "disasm" || cmd == "u")
  {
    uint32_t a = 0;
    nesdebug_cpu c;
    nesdebug_cpu_get(d, &c);
    if(args.empty() || !space_address(d, NESDEBUG_SPACE_CPU, args[0], a)) a = (uint32_t)c.pc;
    const int n = std::min(200, count_arg(1, 16));
    std::vector<nesdebug_line> lines((size_t)n);
    const int got = nesdebug_disassemble(d, (uint16_t)a, lines.data(), n, nullptr);
    for(int i = 0; i < got; i++)
    {
      char buf[220];
      snprintf(buf, sizeof buf, "%c%c %04X  %-9s %-14s %s\n", lines[(size_t)i].has_breakpoint ? '*' : ' ',
               lines[(size_t)i].is_pc ? '>' : ' ', lines[(size_t)i].address, lines[(size_t)i].bytes,
               lines[(size_t)i].label, lines[(size_t)i].disasm);
      out += buf;
    }
  }
  else if(cmd == "find")
  {
    size_t first = 0;
    int space = NESDEBUG_SPACE_CPU;
    if(!args.empty() && space_word(args[0]) >= 0) { space = space_word(args[0]); first = 1; }
    int kind = NESSEARCH_HEX;
    if(args.size() > first && lower(args[first]) == "text") { kind = NESSEARCH_TEXT; first++; }
    else if(args.size() > first && lower(args[first]) == "hex") first++;
    uint8_t pat[NESSEARCH_MAX], mask = 0xFF;
    int count = 0, flags = 0;
    if(args.size() <= first || !nessearch_parse(kind, rest_from(first).c_str(), pat, &count, &mask, &flags))
      out = "usage: find [space] [hex] <bytes> | find [space] text <string>";
    else
    {
      uint32_t hits[64];
      const int size = nesdebug_space_size(d, space);
      const int n = nesdebug_search_in(d, space, pat, count, mask, flags, 0, size > 0 ? (uint32_t)size - 1 : 0, hits, 64);
      if(n < 0) out = "that space is not available";
      else if(!n) out = "not found";
      else
      {
        for(int i = 0; i < n; i++) out += "$" + hex(hits[i], space == NESDEBUG_SPACE_CPU ? 4 : 6) + "\n";
        if(n == 64) out += "(first 64 shown)\n";
      }
    }
  }
  else if(cmd == "trace")
  {
    const std::string a0 = args.empty() ? "" : lower(args[0]);
    if(a0 == "on") { nesdebug_trace_enable(d, 1); out = "trace on (it slows the emulation)"; }
    else if(a0 == "off") { nesdebug_trace_enable(d, 0); out = "trace off"; }
    else if(a0 == "clear") { nesdebug_trace_clear(d); out = "trace cleared"; }
    else
    {
      const int want = std::min(200, count_arg(0, 16));
      std::vector<nesdebug_trace_entry> rows((size_t)want);
      const int n = nesdebug_trace_read(d, rows.data(), want);
      if(!n) out = nesdebug_trace_enabled(d) ? "nothing recorded yet" : "tracing is off (trace on)";
      for(int i = 0; i < n; i++)
      {
        char buf[200];
        nesdebug_trace_format(&rows[(size_t)i], buf, sizeof buf);
        out += std::string(buf) + "\n";
      }
    }
  }
  else if(cmd == "watch")
  {
    if(args.empty())
    {
      const int n = neswatch_count();
      if(!n) out = "no watches";
      for(int i = 0; i < n; i++)
      {
        char buf[200];
        neswatch_format(d, i, buf, sizeof buf);
        out += std::to_string(i) + ": " + buf + "\n";
      }
    }
    else
    {
      const int i = neswatch_add(rest_from(0).c_str());
      if(i < 0) out = "no more watches";
      else
      {
        char buf[200];
        neswatch_format(d, i, buf, sizeof buf);
        out = std::to_string(i) + ": " + buf;
      }
    }
  }
  else if(cmd == "unwatch")
  {
    if(args.empty()) out = "usage: unwatch <n|all>";
    else if(lower(args[0]) == "all") { neswatch_clear(); out = "ok"; }
    else { neswatch_remove(atoi(args[0].c_str())); out = "ok"; }
  }
  else if(cmd == "map")
  {
    nesdebug_region map[32];
    const int n = nesdebug_memory_map(d, map, 32);
    for(int i = 0; i < n; i++)
    {
      char buf[160];
      snprintf(buf, sizeof buf, "$%04X-$%04X  %c%c  %s\n", map[i].low, map[i].high, map[i].readable ? 'r' : '-',
               map[i].writable ? 'w' : '-', map[i].name);
      out += buf;
    }
  }
  else if(cmd == "cart")
  {
    char buf[1100];
    nesdebug_cart_text(d, buf, sizeof buf);
    out = buf;
  }
  else if(cmd == "io")
  {
    char buf[4096];
    nesdebug_io_text(d, buf, sizeof buf);
    out = buf;
  }
  else if(cmd == "sound")
  {
    char buf[2048];
    nesdebug_sound_text(d, buf, sizeof buf);
    out = buf;
  }
  else if(cmd == "video")
  {
    const char* text = gVideoText ? gVideoText(rest_from(0).c_str()) : nullptr;
    out = text ? text : "no video state yet";
  }
  else if(cmd == "save")
  {
    if(args.size() < 2) out = "usage: save <dis|prg|chr|ram|wram> <file>";
    else
    {
      char msg[300];
      nesdebug_save(d, args[0].c_str(), rest_from(1).c_str(), msg, sizeof msg);
      out = msg;
    }
  }
  else out = "unknown command: " + cmd + " (try help)";

  nesdebug_touch(d);
  return put(dst, dstsz, out);
}

extern "C" int nesdebug_completions(nesdebug* d, const char* prefix, char* dst, int dstsz)
{
  if(!prefix || !dst || dstsz <= 0) return 0;
  dst[0] = '\0';
  const std::string pre = lower(prefix);
  int len = 0, n = 0;
  auto emit = [&](const std::string& s) {
    if(lower(s.substr(0, pre.size())) != pre) return;
    if(len + (int)s.size() + 2 >= dstsz) return;
    len += snprintf(dst + len, (size_t)(dstsz - len), "%s\n", s.c_str());
    n++;
  };
  for(const char* c : Commands) emit(c);
  for(int i = 0; nesdebug_reg_names[i]; i++) emit(nesdebug_reg_names[i]);
  if(d)
  {
    const int count = nesdebug_symbols_count(d);
    for(int i = 0; i < count; i++)
    {
      char name[96];
      if(nesdebug_symbols_get(d, i, nullptr, name, sizeof name)) emit(name);
    }
  }
  return n;
}
