/*
 * session_test -- the frontend contract end to end, through the public API
 * only: settings round-trip and persist across sessions, paths resolve
 * inside the given tree, CONFIG boots through the cartridge's loader and
 * paints, a cartridge opens and runs, Reset Game keeps it, Reset to CONFIG
 * ejects it, an unmappable image is refused, and stop/start survive.
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

static int wait_frames(nessession *s, uint64_t *serial, int count, int timeout_ms)
{
    static uint32_t px[NESSESSION_FB_WIDTH * NESSESSION_FB_MAX_HEIGHT];
    int got = 0, waited = 0, h;
    while (waited < timeout_ms) {
        if (nessession_copy_frame(s, px, &h, serial)) {
            if (++got >= count) return 1;
        }
        sleep_ms(2); waited += 2;
    }
    return 0;
}

/* Poll the cartridge status until it contains `want`, or time out. */
static int wait_status(nessession *s, const char *want, int timeout_ms)
{
    char st[160];
    int waited = 0;
    while (waited < timeout_ms) {
        nessession_cart_status(s, st, sizeof st);
        if (strstr(st, want)) return 1;
        sleep_ms(20); waited += 20;
    }
    printf("  (status: %s)\n", st);
    return 0;
}

/* The test ROM counts in $10; peek at it through the debugger (which stops
 * the machine while attached) and let it run on. */
static uint8_t counter(nessession *s)
{
    nesdebug *d = nessession_debugger(s);
    uint8_t v = 0;
    nesdebug_attach(d);
    nesdebug_read(d, 0x10, &v, 1);
    nesdebug_detach(d);
    return v;
}

int main(void)
{
    char cfg[512], data[512], rom[700], bad[700];
    nessession_paths p;
    nessession *s;
    nessession_start_opts o;
    uint64_t serial = 0;
    int h;

    test_tmpdir(cfg, sizeof cfg, "cfg");
    test_tmpdir(data, sizeof data, "data");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data;
    p.fujinet_lib = "";     /* no runtime: the cart runs link-down */

    snprintf(rom, sizeof rom, "%s/counter.nes", cfg);
    check(test_rom_write(rom, 0) > 0, "wrote an NROM test image");
    snprintf(bad, sizeof bad, "%s/mmc5.nes", cfg);
    check(test_rom_write(bad, 5) > 0, "wrote a mapper-5 test image");

    s = nessession_new(&p);
    check(s != NULL, "session created");
    if (!s) return 1;

    check(strcmp(nessession_config_path(s), cfg) == 0, "config path is the given tree");
    check(strncmp(nessession_carts_path(s), data, strlen(data)) == 0, "carts dir is under the data tree");
    check(strncmp(nessession_sd_path(s), data, strlen(data)) == 0, "SD path is under the data tree");

    nessession_set_int(s, "answer", 42);
    nessession_set_str(s, "greeting", "hello");
    check(nessession_get_int(s, "answer", 0) == 42, "int setting round-trips");
    check(strcmp(nessession_get_str(s, "greeting", ""), "hello") == 0, "string setting round-trips");
    check(nessession_get_int(s, "nope", 7) == 7, "missing setting yields its default");

    nessession_default_opts(s, &o);
    check(o.enable_fujinet == 1 && o.enable_audio == 1, "default opts enable FujiNet and audio");
    check(o.port_type[0] == NES_CTRL_STANDARD && o.region == NES_REGION_AUTO,
          "default opts: NES controllers, region auto");
    check(o.cart_path == NULL, "no remembered cartridge: CONFIG");

    o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
    check(nessession_start(s, &o) == 0, "session starts (CONFIG, no FujiNet)");
    if (!nessession_is_running(s)) { printf("error: %s\n", nessession_last_error(s)); return 1; }

    check(wait_frames(s, &serial, 30, 5000), "frames arrive");
    check(wait_status(s, "link down", 5000), "CONFIG went through the loader and runs link-down");
    check(!nessession_cart_booted_game(s), "CONFIG is not a booted game");
    {
        static uint32_t px[NESSESSION_FB_WIDTH * NESSESSION_FB_MAX_HEIGHT];
        uint64_t z = 0; int distinct = 0, i, tries;
        check(nessession_copy_frame(s, px, &h, &z) == 1, "a forced copy (serial 0) always copies");
        check(h == 240, "frame is 240 lines");
        /* CONFIG draws text on a plain background: wait for more than one colour */
        /* up to 15 s: a loaded CI runner can take a while to the first paint */
        for (tries = 0; tries < 150; tries++) {
            uint32_t first = px[0];
            distinct = 0;
            for (i = 0; i < NESSESSION_FB_WIDTH * h; i++) if (px[i] != first) distinct++;
            if (distinct > 100) break;
            sleep_ms(100);
            z = 0;
            nessession_copy_frame(s, px, &h, &z);
        }
        check(distinct > 100, "the CONFIG client painted something");
    }
    check(nessession_refresh_rate(s) == 60, "CONFIG is an NTSC image: 60 Hz");

    /* keyboard: the default map binds X to player 1's A */
    check(nessession_key(s, 'x', 1) == 1, "X is bound (player 1 A)");
    check(nessession_key(s, 'x', 0) == 1, "and released");
    check(nessession_key(s, 0xffc8 /* F11 */, 1) == 0, "an unbound key is ignored");
    check(nessession_key_sysaction(s, NES_KEYSYM_ESCAPE) == NES_SYSACT_RESET_CONFIG,
          "Escape is the Reset to CONFIG system action");
    check(nessession_target_for_key(s, NES_KEYSYM_BACKSPACE) == NES_TARGET_SWITCH(NES_SW_RESET),
          "Backspace is the console's RESET");
    check(wait_frames(s, &serial, 10, 3000), "the machine keeps running after input");

    /* an image the cartridge cannot map */
    {
        char why[256];
        check(nessession_check_cart(bad, why, sizeof why) == 0, "mapper 5 is refused");
        check(strstr(why, "Mapper 5") != NULL, "with the mapper named");
        check(nessession_load_cart(s, bad) == -1, "load_cart refuses it");
        check(!nessession_cart_booted_game(s) && nessession_cart_path(s)[0] == '\0',
              "and CONFIG keeps running");
    }

    /* open a cartridge: it goes straight into the SRAMs and runs */
    check(nessession_check_cart(rom, NULL, 0) == 1, "the NROM image is accepted");
    check(nessession_load_cart(s, rom) == 0, "load_cart");
    check(strcmp(nessession_cart_path(s), rom) == 0, "the cart path is remembered");
    check(strcmp(nessession_get_str(s, "cart", ""), rom) == 0, "and persisted");
    check(wait_status(s, "game running", 3000), "the cartridge reports a game (mailbox closed)");
    {
        uint8_t a = counter(s);
        sleep_ms(200);
        check(counter(s) != a, "the image is running (its counter moves)");
    }

    /* Reset Game: the same image restarts; it is still the cartridge's */
    check(nessession_reset_game(s) == 0, "reset game");
    serial = 0;
    check(wait_frames(s, &serial, 5, 3000), "frames after reset");
    check(nessession_cart_booted_game(s), "the game is still in the cartridge after a reset");
    check(strcmp(nessession_cart_path(s), rom) == 0, "and still the open cartridge");

    /* Reset to CONFIG: a power cycle that ejects it */
    check(nessession_reset_to_config(s) == 0, "reset to CONFIG");
    check(nessession_cart_path(s)[0] == '\0', "the cartridge is ejected");
    check(strcmp(nessession_get_str(s, "cart", "x"), "") == 0, "and forgotten");
    serial = 0;
    check(wait_frames(s, &serial, 10, 5000), "frames flow from the new console");
    check(wait_status(s, "link down", 5000), "CONFIG is back");
    check(!nessession_cart_booted_game(s), "no game in the cartridge");
    check(nessession_cart_link_up(s) == 0, "with no runtime the cart reports link down");

    /* live controller type change, then back */
    nessession_set_port_type(s, 1, NES_CTRL_NONE);
    check(nessession_port_type(s, 1) == NES_CTRL_NONE, "player 2 unplugged");
    nessession_set_port_type(s, 1, NES_CTRL_STANDARD);
    check(wait_frames(s, &serial, 10, 3000), "the machine keeps running after a controller swap");

    /* a remembered cartridge boots at the next start */
    check(nessession_load_cart(s, rom) == 0, "open the cartridge again");
    nessession_stop(s);
    check(!nessession_is_running(s), "session stops");
    nessession_free(s);

    s = nessession_new(&p);
    check(s && nessession_get_int(s, "answer", 0) == 42, "settings persisted across sessions");
    if (s) {
        nessession_default_opts(s, &o);
        check(o.cart_path && strcmp(o.cart_path, rom) == 0, "the cartridge is remembered");
        o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
        check(nessession_start(s, &o) == 0, "restart");
        check(wait_status(s, "game running", 3000), "and it boots the remembered cartridge");
        nessession_stop(s);
        nessession_free(s);
    }

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
