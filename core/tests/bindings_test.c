/*
 * bindings_test -- the remappable table and the keyboard translator, pure:
 * defaults, steal-and-describe, persistence through the settings store, and
 * names. No machine is started.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "nessession.h"
#include "test_tmpdir.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

int main(void)
{
    char cfg[512], data[512], stolen[64], name[64];
    nessession_paths p;
    nessession *s;
    nes_binding b;

    test_tmpdir(cfg, sizeof cfg, "bcfg");
    test_tmpdir(data, sizeof data, "bdata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    s = nessession_new(&p);
    if (!s) return 1;

    /* defaults */
    b = nessession_binding_get(s, NES_TARGET_PORT(0, NES_ACT_A));
    check(b.keysym == 'x' && b.button == NES_PAD_BTN_EAST, "player 1 A defaults to X / East");
    b = nessession_binding_get(s, NES_TARGET_PORT(0, NES_ACT_B));
    check(b.keysym == 'z' && b.button == NES_PAD_BTN_SOUTH, "player 1 B defaults to Z / South");
    b = nessession_binding_get(s, NES_TARGET_PORT(1, NES_ACT_UP));
    check(b.keysym == 'i', "player 2 Up defaults to I");
    b = nessession_binding_get(s, NES_TARGET_SWITCH(NES_SW_RESET));
    check(b.keysym == NES_KEYSYM_BACKSPACE, "Reset Game defaults to Backspace");
    check(nessession_target_for_key(s, 'X') == NES_TARGET_PORT(0, NES_ACT_A), "lookup folds case");
    check(nessession_target_for_button(s, 1, NES_PAD_BTN_SOUTH) == NES_TARGET_PORT(1, NES_ACT_B), "button lookup is scoped to the port");
    check(nessession_target_for_button(s, 0, NES_PAD_BTN_START) == NES_TARGET_PORT(0, NES_ACT_START), "Start is Start");

    /* steal */
    nessession_binding_set_key(s, NES_TARGET_PORT(1, NES_ACT_A), 'x', stolen, sizeof stolen);
    check(strstr(stolen, "Player 1: A") != NULL, "rebinding X reports the holder it displaced");
    b = nessession_binding_get(s, NES_TARGET_PORT(0, NES_ACT_A));
    check(b.keysym == 0, "the old holder lost the key");
    check(nessession_target_for_key(s, 'x') == NES_TARGET_PORT(1, NES_ACT_A), "the new holder has it");

    /* names */
    nessession_keysym_name(NES_KEYSYM_F12, name, sizeof name);
    check(strcmp(name, "F12") == 0, "F12 names itself");
    nessession_keysym_name('a', name, sizeof name);
    check(strcmp(name, "A") == 0, "letters name upper-case");
    check(strcmp(nes_pad_button_name(NES_PAD_BTN_DPAD_LEFT), "D-pad Left") == 0, "pad button names");
    check(strcmp(nes_target_name(NES_TARGET_SYSACT(NES_SYSACT_RESET_CONFIG)), "Reset to CONFIG") == 0, "system action names");
    check(strcmp(nes_target_name(NES_TARGET_PORT(1, NES_ACT_SELECT)), "Player 2: Select") == 0, "target names");
    check(strcmp(nes_target_short_name(NES_TARGET_PORT(0, NES_ACT_TURBO_A)), "Turbo A") == 0, "short names");

    /* native key codes: evdev KEY_1 is 2, Windows scancode 0x02 is '1', macOS 0x12 is '1' */
    check(nessession_keysym_from_evdev(2) == '1', "evdev KEY_1 -> '1'");
    check(nessession_keysym_from_win_scancode(0x02, 0) == '1', "Windows scancode 02 -> '1'");
    check(nessession_keysym_from_macos_keycode(0x12) == '1', "macOS keycode 0x12 -> '1'");
    check(nessession_keysym_from_evdev(103) == NES_KEYSYM_UP, "evdev KEY_UP -> Up");
    check(nessession_keysym_from_win_scancode(0x48, 1) == NES_KEYSYM_UP, "Windows E0 48 -> Up");
    check(nessession_keysym_from_macos_keycode(0x7E) == NES_KEYSYM_UP, "macOS 0x7E -> Up");

    /* persistence */
    nessession_free(s);
    s = nessession_new(&p);
    check(s && nessession_target_for_key(s, 'x') == NES_TARGET_PORT(1, NES_ACT_A), "the rebinding persisted");
    nessession_bindings_reset(s);
    check(nessession_target_for_key(s, 'x') == NES_TARGET_PORT(0, NES_ACT_A), "reset restores the defaults");
    nessession_free(s);

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
