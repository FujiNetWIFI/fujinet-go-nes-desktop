/*
 * input_test -- the merge of input sources, with no machine running: the
 * keyboard, the Controllers window's pointer and each pad (buttons and stick
 * separately) are sources of their own, so one letting go never drops another's
 * hold; opposed directions cancel; release_all is the keyboard's and the
 * pointer's only; release_keyboard is the keyboard's alone; the RESET key is a
 * plain OR; reserved keys are refused; a pad cleared lets go of exactly what it
 * held.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "nessession.h"
#include "input_merge.h"
#include "test_tmpdir.h"

static int failures;
static void check(int pass, const char *what)
{
    printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

#define B(act) (1u << (act))

int main(void)
{
    char cfg[512], data[512];
    nessession_paths paths;
    nessession *s;
    unsigned held;

    test_tmpdir(cfg, sizeof cfg, "icfg");
    test_tmpdir(data, sizeof data, "idata");
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = "";
    s = nessession_new(&paths);
    if (!s) return 1;

    /* a key and the window on the same input */
    check(nessession_buttons_held(s, 0) == 0, "nothing held at first");
    nessession_key(s, NES_KEYSYM_RIGHT, 1);
    check(nessession_buttons_held(s, 0) == B(NES_ACT_RIGHT), "the key holds right");
    nessession_press(s, NES_TARGET_PORT(0, NES_ACT_RIGHT), 1);
    nessession_key(s, NES_KEYSYM_RIGHT, 0);
    check(nessession_buttons_held(s, 0) == B(NES_ACT_RIGHT), "the key letting go leaves the window's right");
    nessession_press(s, NES_TARGET_PORT(0, NES_ACT_RIGHT), 0);
    check(nessession_buttons_held(s, 0) == 0, "and the window lets go of its own");

    /* a key release does not drop a pad's direction (the old bug) */
    input_press(s, NESSESSION_SRC_STICK(0), NES_TARGET_PORT(0, NES_ACT_UP), 1);
    nessession_key(s, NES_KEYSYM_UP, 1);
    nessession_key(s, NES_KEYSYM_UP, 0);
    check(nessession_buttons_held(s, 0) == B(NES_ACT_UP), "key release keeps a pad's Up");
    check(nessession_buttons_held(s, 1) == 0, "port 2 is untouched");

    /* a key release does not drop a pad's button either */
    input_press(s, NESSESSION_SRC_PAD(0), NES_TARGET_PORT(0, NES_ACT_A), 1);
    nessession_key(s, 'x', 1);
    nessession_key(s, 'x', 0);
    check(nessession_buttons_held(s, 0) == (B(NES_ACT_UP) | B(NES_ACT_A)), "X released keeps the pad's A");
    input_press(s, NESSESSION_SRC_PAD(0), NES_TARGET_PORT(0, NES_ACT_A), 0);

    /* opposed pairs cancel, across sources, and come back */
    nessession_key(s, NES_KEYSYM_DOWN, 1);
    check(nessession_buttons_held(s, 0) == 0, "key Down and pad Up cancel");
    nessession_key(s, NES_KEYSYM_DOWN, 0);
    check(nessession_buttons_held(s, 0) == B(NES_ACT_UP), "and the pad's Up is back");
    nessession_pointer_set(s, 0, B(NES_ACT_LEFT) | B(NES_ACT_DOWN));
    check(nessession_buttons_held(s, 0) == B(NES_ACT_LEFT), "a window drag down-left cancels Up and keeps Left");
    nessession_key(s, NES_KEYSYM_RIGHT, 1);
    check(nessession_buttons_held(s, 0) == 0, "Left and Right cancel as well");
    nessession_key(s, NES_KEYSYM_RIGHT, 0);

    /* pointer vs pad: the drag letting go leaves the pad, the pad letting go leaves the drag */
    nessession_pointer_set(s, 0, 0);
    check(nessession_buttons_held(s, 0) == B(NES_ACT_UP), "the drag ending leaves the pad's Up");
    nessession_pointer_set(s, 0, B(NES_ACT_RIGHT));
    input_press(s, NESSESSION_SRC_STICK(0), NES_TARGET_PORT(0, NES_ACT_UP), 0);
    check(nessession_buttons_held(s, 0) == B(NES_ACT_RIGHT), "the pad letting go leaves the drag");
    nessession_pointer_set(s, 0, 0);
    check(nessession_buttons_held(s, 0) == 0, "all let go");

    /* two pads on one port do not fight: buttons are a plain OR */
    input_press(s, NESSESSION_SRC_PAD(0), NES_TARGET_PORT(1, NES_ACT_B), 1);
    input_press(s, NESSESSION_SRC_PAD(1), NES_TARGET_PORT(1, NES_ACT_B), 1);
    input_press(s, NESSESSION_SRC_PAD(0), NES_TARGET_PORT(1, NES_ACT_B), 0);
    check(nessession_buttons_held(s, 1) == B(NES_ACT_B), "the second pad still holds B");
    input_pad_clear(s, 1);
    check(nessession_buttons_held(s, 1) == 0, "a pad cleared lets go of what it held");

    /* the pad's stick and buttons are two sources: clearing lets go of both */
    input_press(s, NESSESSION_SRC_STICK(3), NES_TARGET_PORT(0, NES_ACT_LEFT), 1);
    input_press(s, NESSESSION_SRC_PAD(3), NES_TARGET_PORT(0, NES_ACT_START), 1);
    check(nessession_buttons_held(s, 0) == (B(NES_ACT_LEFT) | B(NES_ACT_START)), "pad 4's stick and button");
    input_pad_clear(s, 3);
    check(nessession_buttons_held(s, 0) == 0, "input_pad_clear drops both sources");

    /* release_all: keyboard and pointer, never a pad */
    nessession_key(s, 'z', 1);
    nessession_press(s, NES_TARGET_PORT(0, NES_ACT_LEFT), 1);
    input_press(s, NESSESSION_SRC_PAD(2), NES_TARGET_PORT(0, NES_ACT_SELECT), 1);
    input_press(s, NESSESSION_SRC_PAD(2), NES_TARGET_SWITCH(NES_SW_RESET), 1);
    check(nessession_buttons_held(s, 0) == (B(NES_ACT_B) | B(NES_ACT_LEFT) | B(NES_ACT_SELECT)), "z, the window's Left and a pad's Select");
    check(nessession_target_held(s, NES_TARGET_SWITCH(NES_SW_RESET)) == 1, "RESET held by the pad");
    nessession_release_all(s);
    check(nessession_buttons_held(s, 0) == B(NES_ACT_SELECT), "release_all keeps only the pad's hold");
    check(nessession_target_held(s, NES_TARGET_PORT(0, NES_ACT_SELECT)) == 1 &&
          nessession_target_held(s, NES_TARGET_PORT(0, NES_ACT_START)) == 0 &&
          nessession_target_held(s, NES_TARGET_SWITCH(NES_SW_RESET)) == 1 &&
          nessession_target_held(s, NES_TARGET_SYSACT(NES_SYSACT_PAUSE)) == 0, "target_held");
    /* the key's release after release_all is a no-op, not a stale release */
    nessession_key(s, 'z', 0);
    check(nessession_buttons_held(s, 0) == B(NES_ACT_SELECT), "a key released after release_all changes nothing");
    input_pad_clear(s, 2);
    check(nessession_buttons_held(s, 0) == 0 && nessession_target_held(s, NES_TARGET_SWITCH(NES_SW_RESET)) == 0, "pad 3 cleared");

    /* the RESET key: held from the window, lit, and gone with the pointer */
    nessession_press(s, NES_TARGET_SWITCH(NES_SW_RESET), 1);
    check(nessession_target_held(s, NES_TARGET_SWITCH(NES_SW_RESET)) == 1, "the window holds RESET");
    nessession_key(s, NES_KEYSYM_BACKSPACE, 1);
    nessession_press(s, NES_TARGET_SWITCH(NES_SW_RESET), 0);
    check(nessession_target_held(s, NES_TARGET_SWITCH(NES_SW_RESET)) == 1, "the key (Backspace) still holds it");
    nessession_key(s, NES_KEYSYM_BACKSPACE, 0);
    check(nessession_target_held(s, NES_TARGET_SWITCH(NES_SW_RESET)) == 0, "and then it is let go");

    /* release_pointer / release_keyboard */
    nessession_key(s, 'i', 1);
    nessession_press(s, NES_TARGET_PORT(1, NES_ACT_A), 1);
    nessession_release_pointer(s);
    check(nessession_buttons_held(s, 1) == B(NES_ACT_UP), "release_pointer leaves the key");
    nessession_press(s, NES_TARGET_PORT(1, NES_ACT_A), 1);
    nessession_release_keyboard(s);
    check(nessession_buttons_held(s, 1) == B(NES_ACT_A), "release_keyboard leaves the pointer");
    input_press(s, NESSESSION_SRC_PAD(0), NES_TARGET_PORT(1, NES_ACT_START), 1);
    nessession_release_keyboard(s);
    nessession_release_pointer(s);
    check(nessession_buttons_held(s, 1) == B(NES_ACT_START), "and neither touches a pad");
    input_pad_clear(s, 0);
    nessession_key(s, 'i', 0);
    check(nessession_buttons_held(s, 1) == 0, "the 'i' released after release_keyboard is a no-op");

    /* pointer_set replaces the direction bits only; a pressed part keeps its bit */
    nessession_press(s, NES_TARGET_PORT(0, NES_ACT_A), 1);
    nessession_pointer_set(s, 0, B(NES_ACT_UP) | B(NES_ACT_RIGHT) | B(NES_ACT_B));
    held = nessession_buttons_held(s, 0);
    check(held == (B(NES_ACT_A) | B(NES_ACT_UP) | B(NES_ACT_RIGHT)), "pointer_set keeps A and takes only the direction bits");
    nessession_pointer_set(s, 0, 0);
    check(nessession_buttons_held(s, 0) == B(NES_ACT_A), "ending the drag leaves the pressed A");
    nessession_press(s, NES_TARGET_PORT(0, NES_ACT_A), 0);
    nessession_pointer_set(s, 0, B(NES_ACT_UP) | B(NES_ACT_DOWN));
    check(nessession_buttons_held(s, 0) == 0, "opposed bits from the pointer cancel");
    nessession_pointer_set(s, 0, 0);

    /* a long pad_assign line (40 entries) survives a save and a reload */
    {
        char text[8192], got[8192];
        int ent, len = 0;
        text[0] = '\0';
        for (ent = 0; ent < 40; ent++)
            len += snprintf(text + len, sizeof text - (size_t)len, "%s%032x|Pad number %d=%d",
                            ent ? ";" : "", ent + 1, ent, ent & 1);
        nessession_set_str(s, "pad_assign", text);
        nessession_set_int(s, "after_long", 7);
        nessession_free(s);
        s = nessession_new(&paths);
        snprintf(got, sizeof got, "%s", s ? nessession_get_str(s, "pad_assign", "") : "");
        check(len > 1500 && strcmp(got, text) == 0, "a 40-entry pad_assign (over 1500 bytes) round-trips whole");
        check(s && nessession_get_int(s, "after_long", 0) == 7, "and the key after it is intact");
        if (!s) return 1;
    }

    /* reserved keys cannot be bound */
    nessession_binding_set_key(s, NES_TARGET_PORT(0, NES_ACT_START), NES_KEYSYM_F9, NULL, 0);
    nessession_binding_set_key(s, NES_TARGET_PORT(0, NES_ACT_SELECT), NES_KEYSYM_F11, NULL, 0);
    nessession_binding_set_key(s, NES_TARGET_PORT(0, NES_ACT_SELECT), NES_KEYSYM_F12, NULL, 0);
    check(nessession_binding_get(s, NES_TARGET_PORT(0, NES_ACT_START)).keysym == NES_KEYSYM_RETURN &&
          nessession_binding_get(s, NES_TARGET_PORT(0, NES_ACT_SELECT)).keysym == NES_KEYSYM_RSHIFT,
          "F9, F11 and F12 are refused");
    nessession_binding_set_key(s, NES_TARGET_PORT(0, NES_ACT_START), NES_KEYSYM_F10, NULL, 0);
    check(nessession_binding_get(s, NES_TARGET_PORT(0, NES_ACT_START)).keysym == NES_KEYSYM_F10, "F10 is not reserved");
    nessession_bindings_reset(s);

    /* ranges */
    input_press(s, -1, NES_TARGET_PORT(0, NES_ACT_UP), 1);
    input_press(s, NESSESSION_SRC_COUNT, NES_TARGET_PORT(0, NES_ACT_UP), 1);
    input_press(s, NESSESSION_SRC_KEYBOARD, NES_TARGET_SYSACT(0), 1);
    input_set_port(s, NESSESSION_SRC_POINTER, 2, 1);
    check(nessession_buttons_held(s, 0) == 0 && nessession_buttons_held(s, 2) == 0 &&
          nessession_buttons_held(s, -1) == 0, "out-of-range sources, targets and ports are ignored");

    nessession_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
