/*
 * debug_test -- the debugger contract against a running machine: attach
 * stops it, registers, step / over / out, disassembly with the PC line,
 * breakpoints that hit, memory, the PPU views, the cartridge tab, the
 * prompt, and detach lets it run again.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nessession.h"
#include "nesdebug.h"
#include "test_tmpdir.h"
#include "test_rom.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int wait_stopped(nesdebug *d, int want, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        if (!!nesdebug_is_stopped(d) == want) return 1;
        sleep_ms(5); waited += 5;
    }
    return 0;
}

int main(void)
{
    char cfg[512], data[512], rom[700], out[4096];
    nessession_paths p;
    nessession *s;
    nessession_start_opts o;
    nesdebug *d;
    nesdebug_cpu c, c2;
    nesdebug_line lines[48];
    int pc_line = -1, n, i, w, h;
    static uint32_t img[NESDEBUG_VIEW_MAX_PIXELS];

    test_tmpdir(cfg, sizeof cfg, "dcfg");
    test_tmpdir(data, sizeof data, "ddata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    snprintf(rom, sizeof rom, "%s/counter.nes", cfg);
    test_rom_write(rom, 0);

    s = nessession_new(&p);
    if (!s) return 1;
    nessession_default_opts(s, &o);
    o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
    o.cart_path = rom;
    check(nessession_start(s, &o) == 0, "session starts with the counter image");
    sleep_ms(300);

    d = nessession_debugger(s);
    check(d != NULL, "debugger handle");
    nesdebug_attach(d);
    check(nesdebug_is_attached(d), "attached");
    check(wait_stopped(d, 1, 3000), "attaching stops the machine");

    nesdebug_cpu_get(d, &c);
    check(c.pc >= 0xC005 && c.pc <= 0xC009, "the PC is in the counter loop");
    check(c.sp == 0xFD || c.sp == 0xFF, "the stack pointer was set by the reset code");

    nesdebug_step(d);
    check(wait_stopped(d, 1, 2000), "stopped after a step");
    nesdebug_cpu_get(d, &c2);
    check(c2.pc != c.pc, "a step moves the PC");
    check(c2.total_cycles > c.total_cycles, "and the cycle count");

    /* disassembly around the PC */
    n = nesdebug_disassemble(d, (uint16_t)nesdebug_row_address(d, (uint16_t)c2.pc, -4), lines, 48, &pc_line);
    check(n > 8, "disassembly lines");
    check(pc_line >= 0 && lines[pc_line].address == c2.pc && lines[pc_line].is_pc, "the PC line is marked");
    {
        int saw_inc = 0, saw_jmp = 0;
        for (i = 0; i < n; i++) {
            if (strstr(lines[i].disasm, "INC")) saw_inc = 1;
            if (strstr(lines[i].disasm, "JMP")) saw_jmp = 1;
        }
        check(saw_inc && saw_jmp, "the loop disassembles as INC / JMP");
    }

    /* breakpoints: an execute breakpoint on the JMP hits */
    check(nesdebug_breakpoint_toggle(d, 0xC007) == 1, "breakpoint set on the JMP");
    check(nesdebug_breakpoint_check(d, 0xC007), "and reported");
    nesdebug_resume(d);
    sleep_ms(50);
    check(wait_stopped(d, 1, 3000), "the breakpoint hit");
    nesdebug_cpu_get(d, &c);
    check(c.pc == 0xC007, "stopped at the breakpoint's address");
    {
        char why[128]; int addr;
        nesdebug_stop_reason(d, why, sizeof why, &addr);
        check(strstr(why, "breakpoint") != NULL, "the stop reason says breakpoint");
    }
    n = nesdebug_disassemble(d, 0xC005, lines, 4, NULL);
    check(n >= 2 && lines[1].address == 0xC007 && lines[1].has_breakpoint, "the line shows the breakpoint");
    check(nesdebug_breakpoint_toggle(d, 0xC007) == 0, "toggled off");

    /* a write breakpoint on the counter, with a condition */
    check(nesdebug_breakpoint_add(d, NESDEBUG_BP_WRITE, 0x10, 0x10, "this bogus ((") == -1,
          "an unparsable condition is refused");
    {
        int id = nesdebug_breakpoint_add(d, NESDEBUG_BP_WRITE, 0x10, 0x10, "");
        nesdebug_breakpoint list[8];
        check(id > 0, "write breakpoint added");
        check(nesdebug_breakpoint_list(d, list, 8) == 1 && list[0].type == NESDEBUG_BP_WRITE, "and listed");
        nesdebug_resume(d);
        sleep_ms(20);
        check(wait_stopped(d, 1, 3000), "the write breakpoint hit");
        nesdebug_breakpoint_clear(d);
        check(nesdebug_breakpoint_list(d, list, 8) == 0, "cleared");
    }

    /* memory: read, and a debugger write into RAM */
    {
        uint8_t ram[2048], v = 0;
        nesdebug_write(d, 0x0300, 0x5A);
        nesdebug_ram_get(d, ram);
        check(ram[0x300] == 0x5A, "a RAM write lands");
        nesdebug_read(d, 0xFFFC, &v, 1);
        check(v == 0x00, "the reset vector reads through the cartridge's PRG");
        nesdebug_read(d, 0xFFFD, &v, 1);
        check(v == 0xC0, "the reset vector's high byte");
        nesdebug_write(d, 0xC00A, 0xEA);
        nesdebug_read(d, 0xC00A, &v, 1);
        check(v == 0xEA, "a debugger write reaches the PRG SRAM");
        nesdebug_write(d, 0xC00A, 0x40);
    }

    /* registers */
    nesdebug_cpu_set(d, NES_REG_A, 0x42);
    nesdebug_cpu_set(d, NES_FLAG_C, 1);
    nesdebug_cpu_get(d, &c);
    check(c.a == 0x42 && c.c == 1, "registers and flags are editable");

    /* the PPU views */
    check(nesdebug_ppu_view(d, NESDEBUG_VIEW_NAMETABLES, 0, img, &w, &h) && w == 512 && h == 480, "nametables view");
    check(nesdebug_ppu_view(d, NESDEBUG_VIEW_PATTERNS, 0, img, &w, &h) && w == 256 && h == 128, "patterns view");
    check(nesdebug_ppu_view(d, NESDEBUG_VIEW_SPRITES, 0, img, &w, &h) && w == 256 && h == 240, "sprites view");
    check(nesdebug_ppu_view(d, NESDEBUG_VIEW_PALETTE, 0, img, &w, &h) && w == 256 && h == 32, "palette view");
    {
        nesdebug_ppu ppu;
        nesdebug_ppu_get(d, &ppu);
        check(strcmp(ppu.mirroring, "Vertical") == 0, "the cartridge's mirroring is reported");
    }

    /* the cartridge */
    {
        nesdebug_cart cart;
        nesdebug_cart_get(d, &cart);
        check(cart.present && cart.sram_enabled && cart.booted_image, "cartridge tab: a booted image");
        check(cart.mapper == 0 && !cart.mailbox_live, "mapper 0, mailbox closed");
        check(nesdebug_cart_info(d, out, sizeof out) > 0 && strstr(out, "mapper 0"), "cart info line");
    }

    /* the prompt */
    nesdebug_command(d, "print a", out, sizeof out);
    check(strstr(out, "66") != NULL, "print evaluates a Mesen expression");
    nesdebug_command(d, "mem $C000 4", out, sizeof out);
    check(strstr(out, "78 D8 A2 FF") != NULL, "mem dumps CPU memory");
    nesdebug_command(d, "label $C005 loop", out, sizeof out);
    /* NROM-128 mirrors its 16K at $8000 and $C000: either is the label's */
    check((nesdebug_label_address(d, "loop") & 0x3FFF) == 0x0005, "labels resolve");
    nesdebug_command(d, "help", out, sizeof out);
    check(strstr(out, "runto") != NULL, "help lists the commands");
    nesdebug_command(d, "frame", out, sizeof out);
    check(wait_stopped(d, 1, 3000), "frame+1 stops again");

    /* step out / over do not wedge the machine */
    nesdebug_step_over(d);
    check(wait_stopped(d, 1, 3000), "step over");

    /* detach: running again */
    nesdebug_detach(d);
    check(!nesdebug_is_attached(d), "detached");
    {
        uint8_t a = 0, b = 0;
        sleep_ms(100);
        nesdebug_attach(d);
        nesdebug_read(d, 0x10, &a, 1);
        nesdebug_resume(d);
        sleep_ms(200);
        nesdebug_read(d, 0x10, &b, 1);
        check(a != b, "the machine runs after resume");
        nesdebug_detach(d);
    }

    /* the debugger survives a power cycle */
    nesdebug_attach(d);
    nesdebug_breakpoint_toggle(d, 0xC007);
    check(nessession_reset_to_config(s) == 0, "reset to CONFIG with the debugger attached");
    check(nessession_load_cart(s, rom) == 0, "reopen the cartridge");
    nesdebug_resume(d);
    check(wait_stopped(d, 1, 3000), "the breakpoint survives the power cycle");
    nesdebug_breakpoint_clear(d);
    nesdebug_detach(d);

    nessession_stop(s);
    nessession_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
