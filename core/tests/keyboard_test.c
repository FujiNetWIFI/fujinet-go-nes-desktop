/*
 * keyboard_test -- the expansion-port keyboards end to end: a ROM scans the
 * keyboard matrix the way Family BASIC does and leaves what it reads in RAM;
 * keys typed on the host (keyboard mode) and pressed on the on-screen
 * keyboard reach exactly the right row and bit; Scroll Lock gives the keys
 * back to the controllers; the Subor answers the way software detects it;
 * the layouts and keysym tables are consistent; and the Data Recorder
 * records a tape.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

/* What the ROM last read for (row, column): $4017 bits 1-4, active low. */
static uint8_t scan(nessession *s, int row, int column)
{
    nesdebug *d = nessession_debugger(s);
    uint8_t ram[2048];
    nesdebug_attach(d);
    nesdebug_ram_get(d, ram);
    nesdebug_detach(d);
    return (uint8_t)(ram[0x300 + column * 0x10 + row] & 0x1E);
}

static int any_key_down(nessession *s, int rows)
{
    nesdebug *d = nessession_debugger(s);
    uint8_t ram[2048];
    int r;
    nesdebug_attach(d);
    nesdebug_ram_get(d, ram);
    nesdebug_detach(d);
    for (r = 0; r < rows; r++) {
        uint8_t c1 = ram[0x310 + r];
        /* MesenCE's Subor matrix has a "None" placeholder at row 9 / column
         * 1 / bit 1 that always reads down; it is not a key */
        if (rows == 13 && r == 9) c1 |= 0x02;
        if ((ram[0x300 + r] & 0x1E) != 0x1E || (c1 & 0x1E) != 0x1E) {
            printf("  (row %d reads %02X %02X)\n", r, ram[0x300 + r], ram[0x310 + r]);
            return 1;
        }
    }
    return 0;
}

static long file_size(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

/* Every key index appears on the layout, and every mapped keysym lands on a
 * real index. */
static void check_tables(int type, int count)
{
    const nes_kbd_key *keys = NULL;
    int n = nessession_keyboard_layout(type, &keys), i, idx, all = 1, ok = 1;
    static const uint32_t probes[] = { 'a', 'z', '0', '9', 0xff0d, 0x20, 0xff08,
        0xff1b, 0xffe1, 0xffe2, 0xffe3, 0xff52, 0xff54, 0xff51, 0xff53, '.',
        ',', '/', '-', ';', '\'', '[', ']', '=', '`', '\\', 0xffbe, 0xff63,
        0xff50, 0xff57 };
    char what[96];
    for (idx = 0; idx < count; idx++) {
        int found = 0;
        for (i = 0; i < n; i++) if (keys[i].index == idx) found = 1;
        if (!found) { all = 0; printf("  (%s: index %d has no key cap)\n", nes_keyboard_name(type), idx); }
    }
    snprintf(what, sizeof what, "%s: every key is on the layout", nes_keyboard_name(type));
    check(all, what);
    for (i = 0; i < (int)(sizeof probes / sizeof probes[0]); i++) {
        idx = nessession_keyboard_index_for_keysym(type, probes[i]);
        if (idx < 0 || idx >= count) { ok = 0; printf("  (keysym 0x%x -> %d)\n", probes[i], idx); }
    }
    snprintf(what, sizeof what, "%s: host keys map onto real keys", nes_keyboard_name(type));
    check(ok, what);
}

int main(void)
{
    char cfg[512], data[512], fb[700], sb[700], tape[800];
    nessession_paths p;
    nessession *s;
    nessession_start_opts o;

    test_tmpdir(cfg, sizeof cfg, "kcfg");
    test_tmpdir(data, sizeof data, "kdata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    snprintf(fb, sizeof fb, "%s/fbscan.nes", cfg);
    snprintf(sb, sizeof sb, "%s/suborscan.nes", cfg);
    test_kbd_rom_write(fb, 9, 1);
    test_kbd_rom_write(sb, 13, 0);

    check(nessession_keyboard_layout(NES_KBD_NONE, NULL) == 0, "no keyboard, no layout");
    check_tables(NES_KBD_FAMILY_BASIC, 72);
    check_tables(NES_KBD_SUBOR, 96);
    check(strcmp(nes_keyboard_name(NES_KBD_FAMILY_BASIC), "Family BASIC Keyboard") == 0, "keyboard names");

    s = nessession_new(&p);
    if (!s) return 1;
    check(nessession_tapes_path(s)[0] != '\0', "a tapes directory");
    nessession_default_opts(s, &o);
    check(o.keyboard == NES_KBD_NONE, "no keyboard by default");
    o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
    o.cart_path = fb;
    o.keyboard = NES_KBD_FAMILY_BASIC;
    check(nessession_start(s, &o) == 0, "starts with the Family BASIC keyboard and the scan ROM");
    sleep_ms(300);

    check(nessession_keyboard(s) == NES_KBD_FAMILY_BASIC, "the keyboard is attached");
    check(nessession_keyboard_mode(s) && nessession_keyboard_captures(s), "keyboard mode is on");
    check(!any_key_down(s, 9), "no key reads down");

    /* A is row 6, column 0, the fourth key: $4017 bit 4 low */
    check(nessession_key(s, 'a', 1) == 1, "A goes to the keyboard");
    sleep_ms(100);
    check(scan(s, 6, 0) == (0x1E & ~0x10), "the program reads A at row 6 / column 0 / bit 4");
    check(nessession_keyboard_held(s, 0), "and A is held");
    check(nessession_key(s, 'a', 0) == 1, "release");
    sleep_ms(100);
    check(!any_key_down(s, 9), "nothing held after the release");

    /* typed keys never reach the controllers; Escape belongs to the keyboard */
    check(nessession_key_sysaction(s, NES_KEYSYM_ESCAPE) == -1, "Escape is the keyboard's ESC, not Reset to CONFIG");
    nessession_key(s, NES_KEYSYM_RETURN, 1);
    sleep_ms(100);
    check(scan(s, 0, 0) == (0x1E & ~0x04), "RETURN reads at row 0 / column 0 / bit 2");
    check((nessession_buttons_held(s, 0) & (1u << NES_ACT_START)) == 0, "and does not press Start");
    nessession_key(s, NES_KEYSYM_RETURN, 0);

    /* two host keys on one NES key: either holds it until both let go */
    nessession_key(s, NES_KEYSYM_BACKSPACE, 1);
    nessession_key(s, 0xffff /* Delete */, 1);
    nessession_key(s, NES_KEYSYM_BACKSPACE, 0);
    sleep_ms(100);
    check(nessession_keyboard_held(s, 38), "DEL stays down while Delete is held");
    nessession_key(s, 0xffff, 0);
    sleep_ms(100);
    check(!nessession_keyboard_held(s, 38), "and lets go with it");

    /* the on-screen keyboard */
    nessession_keyboard_press(s, 25 /* Z */, 1);
    sleep_ms(100);
    check(scan(s, 6, 1) == (0x1E & ~0x04), "a clicked Z reads at row 6 / column 1 / bit 2");
    nessession_keyboard_press(s, 25, 0);

    /* Scroll Lock hands the keys back to the controllers */
    check(nessession_key(s, NES_KEYSYM_SCROLL_LOCK, 1) == 1, "Scroll Lock is consumed");
    nessession_key(s, NES_KEYSYM_SCROLL_LOCK, 0);
    check(!nessession_keyboard_captures(s), "keyboard mode off");
    nessession_key(s, 'x', 1);
    check(nessession_buttons_held(s, 0) & (1u << NES_ACT_A), "X is player 1's A again");
    nessession_key(s, 'x', 0);
    sleep_ms(100);
    check(!any_key_down(s, 9), "and the keyboard reads nothing");
    nessession_key(s, NES_KEYSYM_SCROLL_LOCK, 1);
    nessession_key(s, NES_KEYSYM_SCROLL_LOCK, 0);
    check(nessession_keyboard_captures(s), "Scroll Lock turns it back on");

    /* the Data Recorder: what the program writes to $4016, saved on Stop */
    snprintf(tape, sizeof tape, "%s/test.tape", nessession_tapes_path(s));
    check(nessession_tape_state(s) == NES_TAPE_IDLE, "the deck is idle");
    check(nessession_tape_record(s, tape) == 0, "record");
    check(nessession_tape_state(s) == NES_TAPE_RECORDING, "recording");
    sleep_ms(300);
    check(nessession_tape_stop(s) == 0, "stop");
    check(nessession_tape_state(s) == NES_TAPE_IDLE, "idle again");
    check(file_size(tape) > 0, "the tape was written");
    check(nessession_tape_play(s, tape) == 0, "play it");
    check(nessession_tape_state(s) == NES_TAPE_PLAYING, "playing");

    /* the Subor: live switch, and the read software uses to tell it apart */
    nessession_set_keyboard(s, NES_KBD_SUBOR);
    check(nessession_load_cart(s, sb) == 0, "the Subor scan ROM");
    sleep_ms(300);
    check(nessession_keyboard(s) == NES_KBD_SUBOR && nessession_keyboard_captures(s), "the Subor is attached and captures");
    check(nessession_tape_record(s, tape) == -1, "no Data Recorder with the Subor");
    check(!any_key_down(s, 13), "no Subor key reads down");
    /* A is row 5, column 1, the second key: bit 2 low */
    nessession_key(s, 'a', 1);
    sleep_ms(100);
    check(scan(s, 5, 1) == (0x1E & ~0x04), "the program reads Subor A at row 5 / column 1 / bit 2");
    nessession_key(s, 'a', 0);
    /* F12 is a Subor key, not the debugger */
    nessession_key(s, NES_KEYSYM_F12, 1);
    sleep_ms(100);
    check(nessession_keyboard_held(s, 47), "F12 is the Subor's F12");
    nessession_key(s, NES_KEYSYM_F12, 0);

    nessession_set_keyboard(s, NES_KBD_NONE);
    check(!nessession_keyboard_captures(s), "unplugged: keys are the controllers' again");
    check(nessession_get_int(s, "keyboard", -1) == NES_KBD_NONE, "and the choice is persisted");

    nessession_stop(s);
    nessession_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
