/*
 * gamepad_test -- the gamepad layer's pure helpers (no hardware), and that
 * the SDL gamepad subsystem starts and stops cleanly with nothing attached.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "nessession.h"
#include "gamepad_sdl.h"
#include "test_tmpdir.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

int main(void)
{
    int assign[4];

    /* stick hysteresis */
    check(nes_dir_from_stick(0.f, 0.f, 0, 0.35f, 0.15f) == 0, "centered stick is no direction");
    check(nes_dir_from_stick(0.5f, 0.f, 0, 0.35f, 0.15f) == NES_DIR_RIGHT, "half right engages right");
    check(nes_dir_from_stick(0.2f, 0.f, 0, 0.35f, 0.15f) == 0, "a fifth right does not engage");
    check(nes_dir_from_stick(0.2f, 0.f, NES_DIR_RIGHT, 0.35f, 0.15f) == NES_DIR_RIGHT, "but holds once engaged");
    check(nes_dir_from_stick(0.1f, 0.f, NES_DIR_RIGHT, 0.35f, 0.15f) == 0, "and releases below exit");
    check(nes_dir_from_stick(-0.6f, -0.6f, 0, 0.35f, 0.15f) == (NES_DIR_LEFT | NES_DIR_UP), "diagonals are two bits");
    check(nes_dir_from_stick(0.6f, 0.05f, 0, 0.35f, 0.15f) == NES_DIR_RIGHT, "a pure horizontal picks up no vertical noise");

    /* port assignment */
    assign[0] = -1; assign[1] = -1; assign[2] = -1;
    check(nes_pad_for_port(assign, 2, 0) == 0 && nes_pad_for_port(assign, 2, 1) == 1, "connection order: first pad left, second right");
    check(nes_pad_for_port(assign, 1, 1) == -1, "one pad leaves the right port empty");
    assign[0] = 1; assign[1] = -1;
    check(nes_pad_for_port(assign, 2, 1) == 0 && nes_pad_for_port(assign, 2, 0) == 1, "an explicit assignment wins and the other pad fills the gap");
    assign[0] = 1; assign[1] = 1;
    check(nes_pad_for_port(assign, 2, 1) == 0 && nes_pad_for_port(assign, 2, 0) == -1, "two pads on one port: the first wins, the other port stays empty");

    /* SDL button tables round-trip */
    {
        int b, ok = 1;
        for (b = 0; b < NES_PAD_BTN_LEFT_TRIGGER; b++) {
            int sdl = nes_sdl_button_from_pad(b);
            if (sdl < 0 || nes_pad_button_from_sdl((uint8_t)sdl) != b) ok = 0;
        }
        check(ok, "every named button round-trips through the SDL tables");
        check(nes_pad_button_from_sdl(200) == NES_PAD_BTN_NONE, "an unknown SDL button is NONE");
    }

    /* the subsystem with no hardware: a session with gamepads enabled
     * starts, reports zero pads, and stops */
    {
        char cfg[512], data[512];
        nessession_paths p;
        nessession *s;
        nessession_start_opts o;
        test_tmpdir(cfg, sizeof cfg, "gcfg");
        test_tmpdir(data, sizeof data, "gdata");
        memset(&p, 0, sizeof p);
        p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
        s = nessession_new(&p);
        nessession_default_opts(s, &o);
        o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 1;
        check(nessession_start(s, &o) == 0, "session starts with the gamepad thread");
        check(nessession_gamepad_count(s) >= 0, "gamepad count answers");
        check(nessession_gamepad_effective_port(s, 99) == -1, "an absent pad has no port");
        nessession_gamepad_capture_begin(s);
        check(nessession_gamepad_capture_poll(s, NULL) == 0, "nothing captured with no pad");
        nessession_gamepad_capture_cancel(s);
        nessession_stop(s);
        nessession_free(s);
        check(1, "session with gamepads stops cleanly");
    }

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
