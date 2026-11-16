/*
 * debug_test -- the debugger contract against a running machine: attach
 * stops it, registers, step / over / out, disassembly with the PC line,
 * breakpoints that hit, memory, the PPU views, the cartridge tab, the
 * prompt, and detach lets it run again. Then the newer engine, against a
 * program that writes the PPU, the palette, OAM and RAM in a loop: access
 * breakpoints with value / address / old, PPU-space breakpoints, hit counts,
 * the stop in words and its snapshot, register edits, the memory map,
 * search, the trace ring, symbols, and the family's latent bugs (resuming
 * from a breakpoint at the PC, pause latency, a poller across a power
 * cycle, state across a session restart).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "nessession.h"
#include "nesdebug.h"
#include "nesmemtools.h"
#include "test_tmpdir.h"
#include "test_rom.h"
#include "test_engine_rom.h"

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


/* ---- the newer engine ---------------------------------------------------------- */

static unsigned long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)ts.tv_nsec / 1000000ULL;
}

static int is_start(unsigned pc)
{
    unsigned i;
    for (i = 0; i < sizeof engine_rom_starts / sizeof engine_rom_starts[0]; i++)
        if (engine_rom_starts[i] == pc) return 1;
    return 0;
}

/* Resume and wait for the next stop (a breakpoint, here). */
static int run_until_stop(nesdebug *d, int timeout_ms)
{
    nesdebug_snapshot snap;
    uint64_t before = nesdebug_stop_serial(d);
    int waited = 0;
    nesdebug_resume(d);
    while (waited < timeout_ms) {
        if (nesdebug_stop_snapshot(d, &snap) && snap.serial != before) return 1;
        sleep_ms(2); waited += 2;
    }
    {
        nesdebug_cpu cc;
        nesdebug_breakpoint bl[8];
        int nb = nesdebug_breakpoint_list(d, bl, 8), k;
        nesdebug_cpu_get(d, &cc);
        printf("   no stop: pc=%04X stopped=%d bps=%d ppu scanline %d\n", cc.pc, nesdebug_is_stopped(d), nb, cc.scanline);
        for (k = 0; k < nb; k++) printf("   bp %d type %d space %d %04X-%04X en %d cond '%s'\n", bl[k].id, bl[k].type, bl[k].space, bl[k].start, bl[k].end, bl[k].enabled, bl[k].condition);
    }
    return 0;
}

static int stop_text_has(nesdebug *d, const char *what)
{
    char text[200];
    nesdebug_stop_text(d, text, sizeof text, NULL);
    if (!strstr(text, what)) printf("   stop text was: %s\n", text);
    return strstr(text, what) != NULL;
}

static uint8_t ram_byte(nesdebug *d, uint16_t addr)
{
    uint8_t v = 0;
    nesdebug_read(d, addr, &v, 1);
    return v;
}

struct poller {
    nesdebug *d;
    volatile int stop;
    volatile int bad;
    volatile unsigned long loops;
};

/* A window polling the engine the way the frontends do, snapshot checks and all. */
static void *snapshot_poller(void *arg)
{
    struct poller *p = (struct poller *)arg;
    nesdebug_snapshot snap;
    while (!p->stop) {
        nesdebug_stop_snapshot(p->d, &snap);
        p->loops++;
        if (snap.paused) {
            /* the serial, the stop record and the words must belong together */
            if (snap.serial == 0 || snap.stop.serial != snap.serial) p->bad++;
            if (snap.text[0] == '\0' || strcmp(snap.text, "running") == 0) p->bad++;
            if (snap.address != (int)snap.stop.pc) p->bad++;
        } else {
            if (strcmp(snap.text, "running") != 0) p->bad++;
            if (snap.address != -1) p->bad++;
        }
    }
    return NULL;
}

/* Reads everything a window reads, over and over, across power cycles. */
static void *window_poller(void *arg)
{
    struct poller *p = (struct poller *)arg;
    nesdebug_cpu c;
    nesdebug_line lines[16];
    nesdebug_region map[24];
    nesdebug_trace_entry rows[32];
    uint8_t ram[2048], buf[64];
    nesdebug_sound snd;
    nesdebug_ppu ppu;
    while (!p->stop) {
        nesdebug_cpu_get(p->d, &c);
        nesdebug_ppu_get(p->d, &ppu);
        nesdebug_disassemble(p->d, 0xC000, lines, 16, NULL);
        nesdebug_ram_get(p->d, ram);
        nesdebug_read_space(p->d, NESDEBUG_SPACE_OAM, 0, buf, 64);
        nesdebug_read_space(p->d, NESDEBUG_SPACE_PPU, 0x2000, buf, 64);
        nesdebug_memory_map(p->d, map, 24);
        nesdebug_trace_read(p->d, rows, 32);
        nesdebug_sound_get(p->d, &snd);
        nesdebug_symbols_count(p->d);
        p->loops++;
    }
    return NULL;
}

static void engine_tests(nessession *s, nessession_start_opts *o, const char *writer_rom, const char *dir)
{
    nesdebug *d = nessession_debugger(s);
    nesdebug_snapshot snap;
    nesdebug_stop_info st;
    nesdebug_breakpoint list[16];
    nesdebug_cpu c;
    char err[200], out[4096];
    int id, n, i;

    check(nessession_load_cart(s, writer_rom) == 0, "engine: the writer image loads");
    nesdebug_attach(d);
    nesdebug_stop(d);
    check(wait_stopped(d, 1, 3000), "engine: stopped on the writer image");

    /* ---- a write breakpoint on PPUCTRL: value, old, and the stop in words ---- */
    nesdebug_breakpoint_clear(d);
    id = nesdebug_breakpoint_add_in(d, NESDEBUG_SPACE_CPU, NESDEBUG_BP_WRITE, 0x2000, 0x2000, "", err, sizeof err);
    check(id > 0, "engine: write breakpoint on $2000");
    nesdebug_command(d, "set pc $C005", out, sizeof out);       /* run the boot write again */
    nesdebug_cpu_get(d, &c);
    check(c.pc == 0xC005, "engine: the PC edit lands");
    check(run_until_stop(d, 3000), "engine: the $2000 write stopped the machine");
    check(nesdebug_stop_snapshot(d, &snap) == 1 && snap.paused, "engine: snapshot says paused");
    check(snap.stop.reason == NESDEBUG_STOP_WATCH_WRITE && snap.stop.bp_id == id, "engine: a write watch fired");
    check(snap.stop.space == NESDEBUG_SPACE_CPU && snap.stop.addr == 0x2000 && snap.stop.value == 0x10,
          "engine: $2000 = $10 is the value");
    check(snap.stop.pc == 0xC007, "engine: stopped AT the writing instruction");
    check(snap.address == 0xC007 && snap.serial == snap.stop.serial && snap.serial > 0, "engine: snapshot agrees with itself");
    check(stop_text_has(d, "wrote $2000 = $10 (was $") != 0, "engine: the stop text names value and old");
    nesdebug_cpu_get(d, &c);
    check(c.pc == 0xC007, "engine: the CPU is at the same address");
    {
        unsigned long long serial = nesdebug_stop_serial(d);
        check(run_until_stop(d, 3000), "engine: the second $2000 write");
        check(nesdebug_stop_serial(d) > serial, "engine: the stop serial advanced");
        nesdebug_last_stop(d, &st);
        check(st.value == 0x90 && st.old_value == 0x10 && st.pc == 0xC011, "engine: $90 replaces $10 at $C011");
        check(stop_text_has(d, "wrote $2000 = $90 (was $10) at $C011") != 0, "engine: the family's stop text");
        check(nesdebug_breakpoint_list(d, list, 16) == 1 && list[0].hits == 2, "engine: two hits counted");
        nesdebug_breakpoint_clear_hits(d);
        nesdebug_breakpoint_list(d, list, 16);
        check(list[0].hits == 0, "engine: Clear Hits");
    }
    nesdebug_stop_snapshot(d, &snap);
    nesdebug_resume(d);
    nesdebug_stop_snapshot(d, &snap);
    check(!snap.paused && strcmp(snap.text, "running") == 0 && snap.address == -1, "engine: running after resume");
    nesdebug_stop(d);
    check(wait_stopped(d, 1, 2000), "engine: stopped again");
    nesdebug_stop_snapshot(d, &snap);
    check(snap.stop.reason == NESDEBUG_STOP_PAUSE && stop_text_has(d, "paused at $C0") != 0, "engine: pause text");

    /* ---- conditions with value / old on RAM ---- */
    nesdebug_breakpoint_clear(d);
    check(nesdebug_breakpoint_add_in(d, NESDEBUG_SPACE_CPU, NESDEBUG_BP_WRITE, 0x301, 0x301, "old ==", err, sizeof err) == -1 &&
          err[0], "engine: a bad condition is refused with a reason");
    id = nesdebug_breakpoint_add_in(d, NESDEBUG_SPACE_CPU, NESDEBUG_BP_WRITE, 0x301, 0x301,
                                    "value >= 3 && old == value - 1", err, sizeof err);
    check(id > 0, "engine: a condition with value and old");
    nesdebug_command(d, "set pc $C00A", out, sizeof out);       /* counter back to 0 */
    check(run_until_stop(d, 3000), "engine: the conditional write stopped it");
    nesdebug_last_stop(d, &st);
    check(st.value == 3 && st.old_value == 2 && st.addr == 0x301, "engine: value 3, old 2");
    check(stop_text_has(d, "wrote $0301 = $03 (was $02) at $C01A") != 0, "engine: the RAM stop text");
    check(nesdebug_breakpoint_set_condition(d, id, "value ==", err, sizeof err) == 0 && err[0],
          "engine: an unparsable new condition is refused");
    check(nesdebug_breakpoint_set_condition(d, id, "value >= 6", err, sizeof err) == 1, "engine: set_condition");
    check(run_until_stop(d, 3000), "engine: the edited condition fires");
    nesdebug_last_stop(d, &st);
    check(st.value >= 6, "engine: and uses the new test");
    {
        long v = -1;
        check(nesdebug_eval(d, "value", &v, err, sizeof err) == 0 && v == st.value, "engine: eval value at a watch stop");
        check(nesdebug_eval(d, "old", &v, err, sizeof err) == 0 && v == st.old_value, "engine: eval old at a watch stop");
        check(nesdebug_eval(d, "address", &v, err, sizeof err) == 0 && v == 0x301, "engine: eval address at a watch stop");
        check(nesdebug_eval(d, "[$0300] - 1", &v, err, sizeof err) == 0, "engine: eval reads memory");
        check(nesdebug_eval(d, "this ((", &v, err, sizeof err) == -1 && err[0], "engine: eval refuses nonsense");
    }

    /* ---- disable / enable ---- */
    nesdebug_breakpoint_enable(d, id, 0);
    nesdebug_resume(d);
    sleep_ms(250);
    check(!nesdebug_is_stopped(d), "engine: a disabled breakpoint does not stop it");
    nesdebug_stop(d);
    check(wait_stopped(d, 1, 2000), "engine: pause with a disabled breakpoint set");
    nesdebug_breakpoint_enable(d, id, 1);
    nesdebug_breakpoint_list(d, list, 16);
    check(list[0].enabled == 1, "engine: enabled again");

    /* ---- access breakpoints: INC reads then writes ---- */
    nesdebug_breakpoint_clear(d);
    id = nesdebug_breakpoint_add_in(d, NESDEBUG_SPACE_CPU, NESDEBUG_BP_ACCESS, 0x300, 0x300, "", err, sizeof err);
    check(id > 0, "engine: access breakpoint on the counter");
    nesdebug_breakpoint_list(d, list, 16);
    check(list[0].type == NESDEBUG_BP_ACCESS, "engine: one entry, access type");
    nesdebug_command(d, "set pc $C00F", out, sizeof out);
    check(run_until_stop(d, 3000), "engine: the access stopped it");
    nesdebug_last_stop(d, &st);
    check(st.reason == NESDEBUG_STOP_WATCH_READ || st.reason == NESDEBUG_STOP_WATCH_WRITE, "engine: a read or write watch");
    check(st.pc == 0xC014 || st.pc == 0xC017, "engine: at INC or LDA of the counter");
    check(run_until_stop(d, 3000), "engine: and again");
    nesdebug_breakpoint_list(d, list, 16);
    check(list[0].hits >= 2, "engine: hits count each stop");
    nesdebug_command(d, "hits", out, sizeof out);
    check(strstr(out, ": ") != NULL, "engine: the hits command");

    /* ---- PPU-space breakpoints ---- */
    nesdebug_breakpoint_clear(d);
    id = nesdebug_breakpoint_add_in(d, NESDEBUG_SPACE_PPU, NESDEBUG_BP_WRITE, 0x2000, 0x2000, "", err, sizeof err);
    check(id > 0, "engine: nametable write breakpoint");
    check(nesdebug_breakpoint_add_in(d, NESDEBUG_SPACE_PPU, NESDEBUG_BP_WRITE, 0x4000, 0x4000, "", err, sizeof err) == -1,
          "engine: a PPU address past $3FFF is refused");
    check(nesdebug_breakpoint_add_in(d, NESDEBUG_SPACE_PPU, NESDEBUG_BP_WRITE, 0, 1, "old == 0", err, sizeof err) == -1 &&
          strstr(err, "old"), "engine: old is CPU-space only");
    check(nesdebug_breakpoint_add_in(d, NESDEBUG_SPACE_PPU, NESDEBUG_BP_EXEC, 0, 1, "", err, sizeof err) == -1,
          "engine: execute breakpoints are CPU-space only");
    check(run_until_stop(d, 3000), "engine: the nametable write stopped it");
    nesdebug_last_stop(d, &st);
    check(st.space == NESDEBUG_SPACE_PPU && st.addr == 0x2000 && st.value == 0x41 && st.reason == NESDEBUG_STOP_WATCH_WRITE,
          "engine: PPU $2000 = $41");
    check(stop_text_has(d, "wrote ppu $2000 = $41") != 0, "engine: the stop text names the space");
    nesdebug_breakpoint_clear(d);
    id = nesdebug_breakpoint_add_in(d, NESDEBUG_SPACE_PALETTE, NESDEBUG_BP_WRITE, 1, 1, "value == $21", err, sizeof err);
    check(id > 0, "engine: palette write breakpoint with a value condition");
    check(run_until_stop(d, 3000), "engine: the palette write stopped it");
    nesdebug_last_stop(d, &st);
    check(st.space == NESDEBUG_SPACE_PALETTE && st.addr == 1 && st.value == 0x21, "engine: palette[1] = $21");
    check(stop_text_has(d, "wrote palette $01 = $21") != 0, "engine: palette stop text");
    nesdebug_breakpoint_clear(d);
    id = nesdebug_breakpoint_add_in(d, NESDEBUG_SPACE_OAM, NESDEBUG_BP_WRITE, 0, 3, "", err, sizeof err);
    check(id > 0, "engine: OAM write breakpoint");
    check(run_until_stop(d, 3000), "engine: the OAM write stopped it");
    nesdebug_last_stop(d, &st);
    check(st.space == NESDEBUG_SPACE_OAM && st.value == 0x55, "engine: OAM = $55");
    check(stop_text_has(d, "wrote oam $00 = $55") != 0, "engine: OAM stop text");
    nesdebug_breakpoint_clear(d);

    /* ---- the console ---- */
    nesdebug_command(d, "bpw ppu 2000-2003", out, sizeof out);
    check(strstr(out, "breakpoint") && strstr(out, "ppu"), "engine: bpw ppu through the console");
    nesdebug_command(d, "bpa $0300", out, sizeof out);
    check(strstr(out, "Access") != NULL, "engine: bpa through the console");
    nesdebug_command(d, "break $C011 a == $90", out, sizeof out);
    check(strstr(out, "breakpoint") != NULL, "engine: a conditional exec breakpoint");
    nesdebug_command(d, "breaks", out, sizeof out);
    check(strstr(out, "Access") && strstr(out, "Execute") && strstr(out, "if a == $90"), "engine: breaks lists them");
    n = nesdebug_breakpoint_list(d, list, 16);
    {
        char cmd[40];
        snprintf(cmd, sizeof cmd, "disable %d", list[1].id);
        nesdebug_command(d, cmd, out, sizeof out);
        nesdebug_command(d, "breaks", out, sizeof out);
        check(strstr(out, "(disabled)") != NULL, "engine: disable through the console");
        snprintf(cmd, sizeof cmd, "delete %d", list[1].id);
        nesdebug_command(d, cmd, out, sizeof out);
        nesdebug_command(d, "breaks", out, sizeof out);
        check(strstr(out, "(disabled)") == NULL, "engine: delete through the console");
    }
    nesdebug_command(d, "bpw oam 99999", out, sizeof out);
    check(strstr(out, "range") != NULL, "engine: a bad OAM range is refused");
    nesdebug_command(d, "clear", out, sizeof out);
    nesdebug_command(d, "set a 5", out, sizeof out);
    nesdebug_command(d, "regs", out, sizeof out);
    check(strstr(out, "A=05") != NULL, "engine: set a, regs");
    nesdebug_command(d, "set x $zz", out, sizeof out);
    check(strstr(out, "parse") != NULL, "engine: set refuses nonsense");
    nesdebug_command(d, "mem pal 0 4", out, sizeof out);
    check(strstr(out, "00:") != NULL, "engine: mem pal");
    nesdebug_command(d, "poke ram 20 $5A 7", out, sizeof out);
    nesdebug_command(d, "mem ram 20 2", out, sizeof out);
    check(strstr(out, "5A 07") != NULL, "engine: poke / mem in the RAM space");
    nesdebug_command(d, "find 78 D8 A2 FF", out, sizeof out);
    check(strstr(out, "$C000") != NULL, "engine: find hex");
    nesdebug_command(d, "find prg 78 D8 A2 FF", out, sizeof out);
    check(strstr(out, "$") != NULL && !strstr(out, "not found"), "engine: find in the PRG image");
    nesdebug_command(d, "symbols ppuc", out, sizeof out);
    check(strstr(out, "PPUCTRL") != NULL, "engine: symbols filter");
    nesdebug_command(d, "map", out, sizeof out);
    check(strstr(out, "PRG window") != NULL, "engine: map");
    nesdebug_command(d, "io", out, sizeof out);
    check(strstr(out, "Controllers") && strstr(out, "Port 1"), "engine: io");
    nesdebug_command(d, "sound", out, sizeof out);
    check(strstr(out, "Pulse 1") && strstr(out, "DMC"), "engine: sound");
    nesdebug_command(d, "video", out, sizeof out);
    check(out[0] != '\0', "engine: video answers");
    nesdebug_command(d, "watch $0300", out, sizeof out);
    check(strstr(out, "$0300") != NULL, "engine: watch through the console");
    nesdebug_command(d, "unwatch all", out, sizeof out);
    nesdebug_command(d, "nonsense", out, sizeof out);
    check(strstr(out, "unknown command") != NULL, "engine: unknown verbs say so");
    nesdebug_command(d, "step 3", out, sizeof out);
    check(strstr(out, "step at") != NULL, "engine: step prints the stop");
    nesdebug_command(d, "frame", out, sizeof out);
    check(wait_stopped(d, 1, 3000) && stop_text_has(d, "frame at") != 0, "engine: frame stops with its words");
    nesdebug_command(d, "over", out, sizeof out);
    check(strstr(out, "stopped at") != NULL || strstr(out, "step at") != NULL, "engine: over prints the stop");

    /* ---- registers in bulk ---- */
    check(nesdebug_stop_snapshot(d, &snap) == 1, "engine: stopped for the register edits");
    nesdebug_cpu_get(d, &c);
    c.a = 0x11; c.x = 0x22; c.y = 0x33; c.sp = 0xE0; c.ps = 0xA5; c.pc = 0xC00F;
    check(nesdebug_regs_set(d, &c, err, sizeof err) == 1, "engine: regs_set");
    nesdebug_cpu_get(d, &c);
    check(c.a == 0x11 && c.x == 0x22 && c.y == 0x33 && c.sp == 0xE0 && c.ps == 0xA5 && c.pc == 0xC00F &&
          c.n == 1 && c.c == 1 && c.z == 0, "engine: the whole record, flags following ps");
    check(nesdebug_register_set(d, "z", 1, err, sizeof err) == 1, "engine: register_set a flag");
    nesdebug_cpu_get(d, &c);
    check(c.z == 1 && c.ps == 0xA7, "engine: the flag reaches ps");
    check(nesdebug_register_set(d, "a", 0x1FF, err, sizeof err) == 0 && err[0], "engine: out of range is refused");
    check(nesdebug_register_set(d, "bogus", 1, err, sizeof err) == 0 && strstr(err, "unknown"), "engine: unknown register");
    check(nesdebug_register_set(d, "PC", 0xC030, err, sizeof err) == 1, "engine: names are case-insensitive");
    nesdebug_step(d);
    nesdebug_cpu_get(d, &c);
    check(c.pc == 0xC033, "engine: a step after a PC edit runs the NEW address");

    /* ---- memory map ---- */
    {
        nesdebug_region map[24];
        int prg = 0, ok = 1;
        n = nesdebug_memory_map(d, map, 24);
        check(n >= 12 && n <= 24, "engine: the memory map has regions");
        check(map[0].low == 0 && map[n - 1].high == 0xFFFF, "engine: it spans $0000-$FFFF");
        for (i = 1; i < n; i++)
            if (map[i].low != map[i - 1].high + 1) ok = 0;
        check(ok, "engine: it is gap-free and ordered");
        for (i = 0; i < n; i++)
            if (strstr(map[i].name, "PRG window") && map[i].bank >= 0) prg++;
        check(prg == 4, "engine: four PRG windows with bank numbers");
        check(map[0].readable && map[0].writable && strstr(map[0].name, "RAM"), "engine: RAM first");
        {
            /* the bank numbers are the debugger's live mapping: the same at every read of a parked machine,
             * and the name carries the number the field holds */
            nesdebug_region again[24];
            int same = 1, named = 1;
            for (int round = 0; round < 20; round++) {
                const int count = nesdebug_memory_map(d, again, 24);
                if (count != n) { same = 0; break; }
                for (i = 0; i < n; i++) {
                    char want[16];
                    if (again[i].bank != map[i].bank) same = 0;
                    if (again[i].bank >= 0) {
                        snprintf(want, sizeof want, "(bank %d)", again[i].bank);
                        if (!strstr(again[i].name, want)) named = 0;
                    }
                }
                sleep_ms(2);
            }
            check(same, "engine: the PRG window banks are the same at every read while parked");
            check(named, "engine: and each window's name carries its bank number");
        }
        {
            char name[80];
            check(nesmem_region_name(map, n, 0x2002, name, sizeof name) > 0 && strstr(name, "PPU"), "engine: region_name finds $2002");
        }
    }

    /* ---- disassembly from an address nothing backs (Mesen lists only memory: RAM, PRG, WRAM, the loader) ---- */
    {
        static const uint16_t unbacked[] = { 0x2006, 0x4016, 0x4020, 0x4FFF };
        nesdebug_line rows[4];
        int got, ok = 1;
        for (i = 0; i < (int)(sizeof unbacked / sizeof unbacked[0]); i++) {
            got = nesdebug_disassemble(d, unbacked[i], rows, 4, NULL);
            if (got < 1 || rows[0].address <= unbacked[i] || nesdebug_row_address(d, unbacked[i], 0) != unbacked[i]) ok = 0;
        }
        check(ok, "engine: from an address with no memory behind it the listing starts at the next backed row (Mesen's own)");
        got = nesdebug_disassemble(d, 0x0100, rows, 4, NULL);
        check(got >= 1 && rows[0].address == 0x0100, "engine: from RAM the first row is the address asked for");
        got = nesdebug_disassemble(d, 0xC000, rows, 4, NULL);
        check(got >= 1 && rows[0].address <= 0xC000, "engine: from PRG the first row holds the address asked for");
    }

    /* ---- search over the spaces ---- */
    {
        static const uint8_t pat[] = { 0x78, 0xD8, 0xA2, 0xFF };
        uint16_t hits[16], found = 0;
        uint32_t big[16], f32 = 0;
        n = nesdebug_search(d, pat, 4, 0xFF, 0, 0, 0xFFFF, hits, 16);
        check(n >= 1 && hits[n - 1] == 0xC000, "engine: search finds the reset code");
        n = nesdebug_search(d, pat, 4, 0xFF, 0, 0xC001, 0xFFFF, hits, 16);
        check(n == 0, "engine: `to` and `from` bound the start address");
        check(nesdebug_search_next(d, pat, 4, 0xFF, 0, 0xC000, 0, &found) == 1 && found != 0xC000 &&
              (found < 0xC000 || found > 0xC000),
              "engine: next finds another copy (the 6502 loader ROM has one too)");
        check(nesdebug_search_next(d, pat, 4, 0xFF, 0, 0x0000, 0, &found) == 1 && found >= 0x5800,
              "engine: next from the start finds the first");
        check(nesdebug_search_next(d, pat, 4, 0xFF, 0, 0xC000, 1, &found) == 1, "engine: previous");
        {
            static const uint8_t mixed[] = { 'A', 'b' };
            uint8_t poke[] = { 'a', 'B' };
            nesdebug_write_space(d, NESDEBUG_SPACE_RAM, 0x40, poke, 2);
            n = nesdebug_search_in(d, NESDEBUG_SPACE_RAM, mixed, 2, 0xFF, NESDEBUG_SEARCH_NOCASE, 0, 2047, big, 16);
            check(n == 1 && big[0] == 0x40, "engine: text search ignores case");
            n = nesdebug_search_in(d, NESDEBUG_SPACE_RAM, mixed, 2, 0xFF, 0, 0, 2047, big, 16);
            check(n == 0, "engine: and matches case without the flag");
            poke[0] = 0x0F; poke[1] = 0xF5;
            nesdebug_write_space(d, NESDEBUG_SPACE_RAM, 0x50, poke, 2);
            {
                static const uint8_t m2[] = { 0x05 };
                n = nesdebug_search_in(d, NESDEBUG_SPACE_RAM, m2, 1, 0x0F, 0, 0x50, 0x51, big, 16);
                check(n == 1 && big[0] == 0x51, "engine: a mask compares only its bits");
            }
        }
        n = nesdebug_search_in(d, NESDEBUG_SPACE_PRG, pat, 4, 0xFF, 0, 0, 0xFFFFF, big, 16);
        check(n >= 1, "engine: search in the PRG image");
        check(nesdebug_search_next_in(d, NESDEBUG_SPACE_PRG, pat, 4, 0xFF, 0, 0, 0, &f32) >= 0, "engine: search_next in PRG");
        check(nesdebug_space_size(d, NESDEBUG_SPACE_CPU) == 65536 && nesdebug_space_size(d, NESDEBUG_SPACE_OAM) == 256 &&
              nesdebug_space_size(d, NESDEBUG_SPACE_PALETTE) == 32 && nesdebug_space_size(d, NESDEBUG_SPACE_PPU) == 16384 &&
              nesdebug_space_size(d, NESDEBUG_SPACE_RAM) == 2048, "engine: space sizes");
        {
            uint8_t b[4] = { 0, 0, 0, 0 }, w[2] = { 0x11, 0x22 };
            nesdebug_write_space(d, NESDEBUG_SPACE_OAM, 0xFC, w, 2);
            nesdebug_read_space(d, NESDEBUG_SPACE_OAM, 0xFC, b, 2);
            check(b[0] == 0x11 && b[1] == 0x22, "engine: OAM edits");
            nesdebug_write_space(d, NESDEBUG_SPACE_PPU, 0x2100, w, 2);
            nesdebug_read_space(d, NESDEBUG_SPACE_PPU, 0x2100, b, 2);
            check(b[0] == 0x11 && b[1] == 0x22, "engine: nametable edits");
            nesdebug_write_space(d, NESDEBUG_SPACE_PALETTE, 5, w, 1);
            nesdebug_read_space(d, NESDEBUG_SPACE_PPU, 0x3F05, b, 1);
            check(b[0] == 0x11, "engine: the palette shows through PPU $3F00+");
            nesdebug_read_space(d, NESDEBUG_SPACE_CPU, 0xFFFC, b, 2);
            check(b[0] == 0x00 && b[1] == 0xC0, "engine: CPU space reads the vectors");
        }
    }

    /* ---- tools that need the machine: labels in ranges, watches, sound, io ---- */
    {
        uint16_t lo = 0, hi = 0;
        nesdebug_sound snd;
        nesdebug_set_label(d, 0xC00F, "mainloop");
        check(nessym_parse_range(d, "mainloop", &lo, &hi) == 1 && lo == 0xC00F, "engine: a label is a range");
        check(nessym_parse_range(d, "mainloop+3..mainloop+5", &lo, &hi) == 1 && lo == 0xC012 && hi == 0xC014,
              "engine: label arithmetic in a range");
        check(nessym_parse_range(d, "{$FFFC}", &lo, &hi) == 1 && lo == 0xC000, "engine: a pointer expression");
        check(nessym_parse_range(d, "PPUCTRL", &lo, &hi) == 1 && lo == 0x2000, "engine: a built-in name");
        neswatch_clear();
        neswatch_add("$0300");
        neswatch_add("mainloop");
        neswatch_add("$FFFC..$FFFF");
        neswatch_format(d, 0, out, sizeof out);
        check(strstr(out, "$0300") && strstr(out, "(") && !strstr(out, " -"), "engine: a watch shows its value while stopped");
        neswatch_format(d, 2, out, sizeof out);
        check(strstr(out, "00 C0") != NULL, "engine: a range watch shows bytes");
        neswatch_clear();
        check(nesdebug_sound_get(d, &snd) == 1 && snd.present && snd.cpu_hz > 1000000.0, "engine: the sound state answers");
        nesdebug_sound_text(d, out, sizeof out);
        check(strstr(out, "Pulse 1") && strstr(out, "Triangle") && strstr(out, "Noise") && strstr(out, "DMC"), "engine: sound text");
        nesdebug_io_text(d, out, sizeof out);
        check(strstr(out, "mapper") && strstr(out, "Port 1") && strstr(out, "Beam"), "engine: Cart & I/O text");
        check(nesdebug_completions(d, "wat", out, sizeof out) >= 1 && strstr(out, "watch"), "engine: completions: commands");
        check(nesdebug_completions(d, "mainl", out, sizeof out) == 1, "engine: completions: labels");
        check(nesdebug_completions(d, "ppuc", out, sizeof out) == 1, "engine: completions: built-in names");
        nesdebug_clear_symbols(d);
    }

    /* ---- symbols ---- */
    {
        uint16_t a = 0;
        char name[64], msg[200];
        int idx[8];
        unsigned gen = nesdebug_symbols_generation(d);
        check(nesdebug_symbols_count(d) > 30, "engine: the built-in names are listed");
        check(nesdebug_symbols_filter(d, "ppuctrl", idx, 8) == 1 && nesdebug_symbols_get(d, idx[0], &a, name, sizeof name) &&
              a == 0x2000, "engine: filter is case-insensitive and gives CPU addresses");
        check(nesdebug_set_label(d, 0xC00F, "mainloop") == 0, "engine: set_label");
        check(nesdebug_symbols_generation(d) != gen, "engine: the generation moved");
        check(nesdebug_symbols_filter(d, "mainl", idx, 8) == 1 && nesdebug_symbols_get(d, idx[0], &a, name, sizeof name) &&
              a == 0xC00F, "engine: a user label is listed at its CPU address");
        {
            FILE *f;
            char path[700];
            snprintf(path, sizeof path, "%s/labels.sym", dir);
            f = fopen(path, "w");
            fputs("al 00C00F .loadedloop\nal C:C020 .loadedtail\n", f);
            fclose(f);
            check(nesdebug_load_symbols(d, path, msg, sizeof msg) == 2, "engine: a ld65 symbol file loads");
            check(nesdebug_symbols_filter(d, "loadedl", idx, 8) == 1 && nesdebug_symbols_get(d, idx[0], &a, name, sizeof name) &&
                  a == 0xC00F, "engine: a loaded label is listed at its CPU address");
            {
                nesdebug_line lines[8];
                int got = nesdebug_disassemble(d, 0xC00F, lines, 8, NULL);
                check(got > 0 && lines[0].address == 0xC00F, "engine: the disassembly starts at the label");
            }
        }
        check(nesdebug_symbols_get(d, 0, &a, name, sizeof name) == 1 && nesdebug_symbols_get(d, 100000, &a, name, sizeof name) == 0,
              "engine: get past the end says so");
        nesdebug_clear_symbols(d);
        check(nesdebug_symbols_filter(d, "mainl", idx, 8) == 0 && nesdebug_symbols_filter(d, "ppuctrl", idx, 8) == 1,
              "engine: Clear Symbols keeps the built-ins");
        (void)msg;
    }

    /* ---- trace and history ---- */
    {
        static nesdebug_trace_entry rows[400];
        static nesdebug_hist_entry hist[100];
        char line[200];
        uint64_t total0, total1;
        int count0, seen_inc = 0;
        nesdebug_trace_clear(d);
        nesdebug_set_label(d, 0xC00F, "mainloop");
        check(nesdebug_trace_count(d) == 0 && !nesdebug_trace_enabled(d), "engine: trace starts empty and off");
        nesdebug_trace_enable(d, 1);
        check(nesdebug_trace_enabled(d), "engine: Record on");
        total0 = nesdebug_trace_total(d);
        nesdebug_command(d, "set pc $C00F", out, sizeof out);
        nesdebug_resume(d);
        sleep_ms(300);
        nesdebug_stop(d);
        check(wait_stopped(d, 1, 3000), "engine: stopped after recording");
        total1 = nesdebug_trace_total(d);
        count0 = nesdebug_trace_count(d);
        check(total1 > total0 + 200 && count0 > 200, "engine: the ring filled while running");
        n = nesdebug_trace_read(d, rows, 400);
        check(n == 400, "engine: a full read");
        nesdebug_cpu_get(d, &c);
        check(rows[0].pc != 0 && is_start(rows[0].pc), "engine: the newest row is an instruction start");
        for (i = 0; i < n; i++) {
            if (!is_start(rows[i].pc)) break;
            if (strstr(rows[i].disasm, "INC")) seen_inc = 1;
        }
        check(i == n, "engine: every row is an instruction boundary");
        check(seen_inc && rows[0].len >= 1 && rows[0].bytes[0] != 0, "engine: disassembly and bytes");
        check(rows[0].cycles > rows[1].cycles, "engine: newest first");
        nesdebug_trace_format(&rows[0], line, sizeof line);
        check(strstr(line, "A=") && strstr(line, "SP=") && strstr(line, "P="), "engine: row text");
        check(nesdebug_trace_match(&rows[0], NESDEBUG_TRACE_ALL, 0, 0) == 1, "engine: filter All");
        {
            int branches = 0, ranged = 0;
            for (i = 0; i < n; i++) {
                if (nesdebug_trace_match(&rows[i], NESDEBUG_TRACE_BRANCHES, 0, 0)) branches++;
                if (nesdebug_trace_match(&rows[i], NESDEBUG_TRACE_RANGE, 0xC030, 0xC05B)) ranged++;
            }
            check(branches > 20 && branches < n, "engine: Branches keeps JSR/RTS/JMP only");
            check(ranged > 20 && ranged < n, "engine: Range keeps the subroutine");
        }
        n = nesdebug_history_copy(d, hist, 100);
        check(n == 100 && hist[0].cycles < hist[99].cycles, "engine: history is oldest first");
        check(nesdebug_history_count(d) >= 200, "engine: history count");
        nesdebug_history_format(&hist[99], line, sizeof line);
        check(line[0] != '\0', "engine: history row text");
        /* turning Record off keeps the rows */
        nesdebug_trace_enable(d, 0);
        check(nesdebug_trace_count(d) >= 400 && nesdebug_trace_read(d, rows, 10) == 10, "engine: Record off keeps the rows");
        nesdebug_trace_enable(d, 1);
        nesdebug_command(d, "trace 3", out, sizeof out);
        check(strstr(out, "A=") != NULL, "engine: the trace command shows rows");
        nesdebug_trace_clear(d);
        check(nesdebug_trace_count(d) == 0 && nesdebug_trace_read(d, rows, 10) == 0, "engine: Clear");
    }

    /* ---- breakpoints, labels and the trace across a power cycle ---- */
    {
        static nesdebug_trace_entry rows[50];
        int before, after;
        nesdebug_breakpoint_clear(d);
        nesdebug_set_label(d, 0xC00F, "mainloop");
        nesdebug_trace_enable(d, 1);
        nesdebug_set_label(d, 0xC014, "bump");
        id = nesdebug_breakpoint_add_in(d, NESDEBUG_SPACE_CPU, NESDEBUG_BP_EXEC, 0xC011, 0xC011, "", err, sizeof err);
        nesdebug_command(d, "set pc $C00F", out, sizeof out);
        nesdebug_resume(d);
        sleep_ms(100);
        check(wait_stopped(d, 1, 3000), "engine: an exec breakpoint on the loop");
        before = nesdebug_trace_count(d);
        check(before > 0, "engine: and rows recorded before the cycle");
        check(nessession_reset_to_config(s) == 0, "engine: power cycle (CONFIG)");
        check(nessession_load_cart(s, writer_rom) == 0, "engine: power cycle (the writer again)");
        check(nesdebug_breakpoint_list(d, list, 16) == 1 && list[0].id == id, "engine: the breakpoint survived");
        check(nesdebug_label_address(d, "mainloop") >= 0 && (nesdebug_label_address(d, "mainloop") & 0x3FFF) == 0x000F,
              "engine: the label survived");
        check(nesdebug_trace_enabled(d), "engine: Record survived");
        after = nesdebug_trace_count(d);
        check(after >= before, "engine: the old rows survived the new console");
        check(run_until_stop(d, 3000), "engine: the breakpoint fires on the new console");
        nesdebug_cpu_get(d, &c);
        check(c.pc == 0xC011, "engine: at $C011 again");
        check(nesdebug_trace_read(d, rows, 50) >= 8 && is_start(rows[0].pc), "engine: new rows after the cycle");
        check(nesdebug_trace_count(d) > before + 5, "engine: recording went on");
        nesdebug_trace_enable(d, 0);
        nesdebug_trace_clear(d);
    }

    /* ---- resume from a breakpoint at the PC does not re-fire ---- */
    {
        uint8_t first, second;
        check(nesdebug_breakpoint_list(d, list, 16) == 1, "engine: the $C011 breakpoint is set");
        first = ram_byte(d, 0x300);
        check(run_until_stop(d, 3000), "engine: the next loop iteration");
        second = ram_byte(d, 0x300);
        nesdebug_cpu_get(d, &c);
        check(c.pc == 0xC011 && (uint8_t)(second - first) == 1, "engine: one loop between two stops (no re-fire)");
        nesdebug_step(d);
        nesdebug_cpu_get(d, &c);
        check(c.pc == 0xC014, "engine: a step off the breakpoint moves on");
        nesdebug_breakpoint_clear(d);
    }

    /* ---- pause latency and boundaries ---- */
    {
        int bad = 0;
        unsigned long long worst = 0;
        nesdebug_resume(d);
        for (i = 0; i < 12; i++) {
            unsigned long long t0, t1;
            sleep_ms(20 + i * 7);
            t0 = now_ms();
            nesdebug_stop(d);
            wait_stopped(d, 1, 2000);
            t1 = now_ms();
            if (t1 - t0 > worst) worst = t1 - t0;
            nesdebug_cpu_get(d, &c);
            if (!is_start((unsigned)c.pc)) bad++;
            nesdebug_resume(d);
        }
        printf("   pause latency: worst %llu ms\n", worst);
        check(bad == 0, "engine: every pause lands on an instruction boundary");
        check(worst < 500, "engine: pause latency under half a second");
        nesdebug_stop(d);
        wait_stopped(d, 1, 2000);
    }

    /* ---- the stop record is one critical section ---- */
    {
        pthread_t th;
        struct poller p = { d, 0, 0, 0 };
        nesdebug_breakpoint_clear(d);
        nesdebug_breakpoint_add_in(d, NESDEBUG_SPACE_CPU, NESDEBUG_BP_EXEC, 0xC011, 0xC011, "", err, sizeof err);
        pthread_create(&th, NULL, snapshot_poller, &p);
        for (i = 0; i < 80; i++) {
            nesdebug_resume(d);
            wait_stopped(d, 1, 1000);
            if (i % 3 == 0) nesdebug_step(d);
        }
        p.stop = 1;
        pthread_join(th, NULL);
        printf("   snapshot polls: %lu, inconsistent: %d\n", p.loops, p.bad);
        check(p.loops > 100 && p.bad == 0, "engine: a poller never sees a stop half written");
        nesdebug_breakpoint_clear(d);
    }

    /* ---- a polling window across power cycles ---- */
    {
        pthread_t th;
        struct poller p = { d, 0, 0, 0 };
        nesdebug_trace_enable(d, 1);
        pthread_create(&th, NULL, window_poller, &p);
        for (i = 0; i < 4; i++) {
            nessession_reset_to_config(s);
            sleep_ms(30);
            nessession_load_cart(s, writer_rom);
            nesdebug_resume(d);
            sleep_ms(30);
        }
        p.stop = 1;
        pthread_join(th, NULL);
        check(p.loops > 20, "engine: a window polled through four power cycles without a fault");
        nesdebug_trace_enable(d, 0);
        nesdebug_trace_clear(d);
    }

    /* ---- breakpoints and labels across a session restart ---- */
    {
        nesdebug_set_label(d, 0xC020, "tail");
        id = nesdebug_breakpoint_add_in(d, NESDEBUG_SPACE_CPU, NESDEBUG_BP_EXEC, 0xC011, 0xC011, "", err, sizeof err);
        nesdebug_trace_enable(d, 1);
        nesdebug_detach(d);
        nessession_stop(s);
        check(nessession_start(s, o) == 0, "engine: the session restarts");
        d = nessession_debugger(s);
        check(nesdebug_breakpoint_list(d, list, 16) == 1 && list[0].id == id, "engine: the breakpoint survived the restart");
        nesdebug_attach(d);
        check(wait_stopped(d, 1, 3000), "engine: attached after the restart");
        check(nessession_load_cart(s, writer_rom) == 0, "engine: the writer image again");
        check(nesdebug_label_address(d, "tail") >= 0, "engine: the label survived the restart");
        check(nesdebug_trace_enabled(d), "engine: Record survived the restart");
        check(run_until_stop(d, 3000), "engine: the breakpoint fires after the restart");
        nesdebug_cpu_get(d, &c);
        check(c.pc == 0xC011, "engine: at $C011");
        nesdebug_breakpoint_clear(d);
        nesdebug_trace_enable(d, 0);
        nesdebug_trace_clear(d);
    }
    nesdebug_detach(d);
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
    /* on a loaded machine the step can land after nesdebug_step's own wait */
    {
        int waited = 0;
        nesdebug_cpu_get(d, &c2);
        while (c2.pc == c.pc && waited < 2000) {
            sleep_ms(5); waited += 5;
            nesdebug_cpu_get(d, &c2);
        }
    }
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

    /* the FujiNet tab: no FujiNet here, but the state and the trace answer */
    {
        static nesdebug_fuji_packet pk[16];
        check(nesdebug_fujinet_state(d, out, sizeof out) > 0 && strstr(out, "Link") && strstr(out, "Packets"),
              "FujiNet state text");
        check(nesdebug_fujinet_trace(d, pk, 16) >= 0, "the FujiNet trace answers");
        nesdebug_fujinet_trace_clear(d);
        check(nesdebug_fujinet_trace(d, pk, 16) == 0, "and clears");
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

    {
        char writer[700];
        snprintf(writer, sizeof writer, "%s/writer.nes", cfg);
        engine_rom_write(writer);
        engine_tests(s, &o, writer, cfg);
    }

    nessession_stop(s);
    nessession_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
