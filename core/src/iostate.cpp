/*
 * The Cart & I/O and Sound tabs as text (core/include/nesdebug.h): the same
 * lines in every frontend and in the console's `cart`, `io` and `sound`.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include "nesdebug.h"
}

namespace {

int put(char* dst, int dstsz, const std::string& s)
{
  if(!dst || dstsz <= 0) return 0;
  return snprintf(dst, (size_t)dstsz, "%s", s.c_str());
}

std::string pad_text(uint8_t bits)
{
  static const char* const names[8] = { "A", "B", "Select", "Start", "Up", "Down", "Left", "Right" };
  std::string s;
  for(int i = 0; i < 8; i++)
    if(bits & (1 << i)) s += (s.empty() ? "" : " ") + std::string(names[i]);
  return s.empty() ? "-" : s;
}

const char* onoff(int v) { return v ? "on" : "off"; }

} // namespace

extern "C" int nesdebug_cart_text(nesdebug* d, char* dst, int dstsz)
{
  nesdebug_cart c;
  nesdebug_cart_get(d, &c);
  char info[256];
  nesdebug_cart_info(d, info, sizeof info);
  if(!c.present) return put(dst, dstsz, std::string(info) + "\n");
  char buf[1024];
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
  return put(dst, dstsz, buf);
}

extern "C" int nesdebug_io_text(nesdebug* d, char* dst, int dstsz)
{
  nesdebug_apu a;
  nesdebug_apu_get(d, &a);
  nesdebug_cpu cpu;
  nesdebug_cpu_get(d, &cpu);
  nesdebug_ppu ppu;
  nesdebug_ppu_get(d, &ppu);
  char cart[1100];
  nesdebug_cart_text(d, cart, sizeof cart);

  std::string s = cart;
  s += "\n\nControllers\n";
  s += "  Port 1   " + pad_text(a.pad[0]) + "\n";
  s += "  Port 2   " + pad_text(a.pad[1]) + "\n";
  s += "Expansion port\n";
  s += std::string("  Keyboard ") + (a.keyboard ? (a.keyboard == 2 ? "Subor" : "Family BASIC") : "none");
  if(a.keys_held[0]) s += std::string("  held: ") + a.keys_held;
  s += "\n";
  s += std::string("  Recorder ") + (a.tape == 2 ? "recording" : a.tape == 1 ? "playing" : "idle") + "\n";
  char buf[256];
  snprintf(buf, sizeof buf,
           "\nCPU lines\n  IRQ %s  NMI %s\nBeam\n  scanline %d  dot %d  frame %u\n"
           "PPU\n  $2000 %02X  $2001 %02X  $2002 %02X  OAMADDR %02X\n  v %04X  t %04X  x %d  w %d  mirroring %s\n",
           cpu.irq ? "pending" : "-", cpu.nmi ? "pending" : "-", ppu.scanline, ppu.dot, ppu.frame, ppu.ctrl,
           ppu.mask, ppu.status, ppu.oam_addr, ppu.vram_addr, ppu.tmp_addr, ppu.fine_x, ppu.write_toggle,
           ppu.mirroring);
  s += buf;
  return put(dst, dstsz, s);
}

extern "C" int nesdebug_sound_text(nesdebug* d, char* dst, int dstsz)
{
  nesdebug_sound snd;
  if(!nesdebug_sound_get(d, &snd)) return put(dst, dstsz, "no sound state (the machine is off)\n");
  std::string s;
  char buf[320];
  auto pulse = [&](const char* name, const nesdebug_voice& v) {
    snprintf(buf, sizeof buf,
             "%-9s %-3s %8.2f Hz  period %-4d vol %-2d duty %d  length %-3d%s  env %s%s %d/%d  sweep %s%s p%d s%d\n",
             name, onoff(v.enabled), v.hz, v.period, v.volume, v.duty, v.length, v.length_halt ? " halt" : "",
             v.env_constant ? "const" : "decay", v.env_loop ? " loop" : "", v.env_volume, v.env_counter,
             onoff(v.sweep_enabled), v.sweep_negate ? " neg" : "", v.sweep_period, v.sweep_shift);
    s += buf;
  };
  pulse("Pulse 1", snd.pulse1);
  pulse("Pulse 2", snd.pulse2);
  snprintf(buf, sizeof buf, "%-9s %-3s %8.2f Hz  period %-4d vol %-2d length %-3d%s  linear %d/%d\n", "Triangle",
           onoff(snd.triangle.enabled), snd.triangle.hz, snd.triangle.period, snd.triangle.volume,
           snd.triangle.length, snd.triangle.length_halt ? " halt" : "", snd.triangle.linear,
           snd.triangle.linear_reload);
  s += buf;
  snprintf(buf, sizeof buf, "%-9s %-3s %8.2f Hz  period %-4d vol %-2d length %-3d%s  env %s%s %d/%d  %s\n",
           "Noise", onoff(snd.noise.enabled), snd.noise.hz, snd.noise.period, snd.noise.volume, snd.noise.length,
           snd.noise.length_halt ? " halt" : "", snd.noise.env_constant ? "const" : "decay",
           snd.noise.env_loop ? " loop" : "", snd.noise.env_volume, snd.noise.env_counter,
           snd.noise.noise_mode ? "short" : "long");
  s += buf;
  snprintf(buf, sizeof buf, "%-9s %-3s %8.2f smp/s  period %-4d start $%04X now $%04X len %u left %d  %s%s  out %d\n",
           "DMC", onoff(snd.dmc.enabled), snd.dmc.rate_hz, snd.dmc.period, snd.dmc.start_address, snd.dmc.address,
           snd.dmc.length, snd.dmc.bytes_left, snd.dmc.loop ? "loop " : "", snd.dmc.irq ? "irq" : "no irq",
           snd.dmc.volume);
  s += buf;
  snprintf(buf, sizeof buf, "$4015 channels running: %02X   frame counter: %s-step, IRQ %s   APU clock %.0f Hz\n",
           snd.status, snd.five_step ? "5" : "4", onoff(snd.frame_irq), snd.cpu_hz);
  s += buf;
  return put(dst, dstsz, s);
}
