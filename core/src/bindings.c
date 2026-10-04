/*
 * bindings -- the remappable key/gamepad table, the keyboard translator,
 * and every name the bindings editor and the settings store need.
 *
 * One flat target table (nessession.h's NES_TARGET_* indices): per port
 * the standard controller's ten inputs (the cross, A, B, Select, Start and
 * the two turbo buttons); then the console's RESET button; then the
 * session's own actions.
 * Each target holds at most one keysym and one gamepad button. Rebinding
 * STEALS -- a key drives exactly one target, because one keystroke doing
 * two things is worse than losing the old binding: the second effect is
 * invisible until it matters.
 *
 * Seeds its defaults LAZILY so the table is usable with no session behind
 * it: a unit test with no settings store gets the documented default map.
 * Persisted as one packed "bindings" key holding only the entries that
 * differ from the defaults ("<target>.k:<keysym>" / "<target>.b:<button>").
 *
 * Keysyms are X11's, folded to lower case before lookup so a binding made
 * with 'w' still fires while Shift is held for something else.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hid_keys.h"
#include "session_internal.h"

typedef struct {
    uint32_t keysym;
    int button;
} slot;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static slot s_table[NES_TARGET_COUNT];
static slot s_defaults[NES_TARGET_COUNT];
static int s_seeded;
static struct nessession *s_store;   /* whose settings hold "bindings" */

/* ---- defaults ------------------------------------------------------------- */

static void seed_key(int target, uint32_t keysym)
{
    s_defaults[target].keysym = keysym;
}

static void seed_button(int target, int button)
{
    s_defaults[target].button = button;
}

/* Player 1 on the arrows with Z = B, X = A (the controller's own left-to-
 * right order), A / S the turbo B / A, Right Shift = Select and Return =
 * Start; player 2 on I J K L with N = B, M = A, U = Select, O = Start.
 * Backspace is the console's RESET (Reset Game) and Escape is Reset to
 * CONFIG. F5/F7/F8/F12 are the family's debugger keys and stay free of the
 * machine. */
static void compute_defaults_locked(void)
{
    int t;
    for (t = 0; t < NES_TARGET_COUNT; t++) {
        s_defaults[t].keysym = 0;
        s_defaults[t].button = NES_PAD_BTN_NONE;
    }

    /* player 1 */
    seed_key(NES_TARGET_PORT(0, NES_ACT_UP), NES_KEYSYM_UP);
    seed_key(NES_TARGET_PORT(0, NES_ACT_DOWN), NES_KEYSYM_DOWN);
    seed_key(NES_TARGET_PORT(0, NES_ACT_LEFT), NES_KEYSYM_LEFT);
    seed_key(NES_TARGET_PORT(0, NES_ACT_RIGHT), NES_KEYSYM_RIGHT);
    seed_key(NES_TARGET_PORT(0, NES_ACT_B), 'z');
    seed_key(NES_TARGET_PORT(0, NES_ACT_A), 'x');
    seed_key(NES_TARGET_PORT(0, NES_ACT_TURBO_B), 'a');
    seed_key(NES_TARGET_PORT(0, NES_ACT_TURBO_A), 's');
    seed_key(NES_TARGET_PORT(0, NES_ACT_SELECT), NES_KEYSYM_RSHIFT);
    seed_key(NES_TARGET_PORT(0, NES_ACT_START), NES_KEYSYM_RETURN);

    /* player 2 */
    seed_key(NES_TARGET_PORT(1, NES_ACT_UP), 'i');
    seed_key(NES_TARGET_PORT(1, NES_ACT_DOWN), 'k');
    seed_key(NES_TARGET_PORT(1, NES_ACT_LEFT), 'j');
    seed_key(NES_TARGET_PORT(1, NES_ACT_RIGHT), 'l');
    seed_key(NES_TARGET_PORT(1, NES_ACT_B), 'n');
    seed_key(NES_TARGET_PORT(1, NES_ACT_A), 'm');
    seed_key(NES_TARGET_PORT(1, NES_ACT_SELECT), 'u');
    seed_key(NES_TARGET_PORT(1, NES_ACT_START), 'o');

    /* the console and the session */
    seed_key(NES_TARGET_SWITCH(NES_SW_RESET), NES_KEYSYM_BACKSPACE);
    seed_key(NES_TARGET_SYSACT(NES_SYSACT_RESET_CONFIG), NES_KEYSYM_ESCAPE);

    /* gamepad defaults, uniform on both ports, by position: the NES
     * controller's B and A sit left and right on one row, so South is B and
     * East is A, as on NES-style pads (8BitDo, Nintendo's own). West / North are the turbos;
     * Back and Start are Select and Start. Directions come from the D-pad and
     * the left stick in gamepad_sdl.c, not from bindings. */
    {
        int port;
        for (port = 0; port < 2; port++) {
            seed_button(NES_TARGET_PORT(port, NES_ACT_B), NES_PAD_BTN_SOUTH);
            seed_button(NES_TARGET_PORT(port, NES_ACT_A), NES_PAD_BTN_EAST);
            seed_button(NES_TARGET_PORT(port, NES_ACT_TURBO_B), NES_PAD_BTN_WEST);
            seed_button(NES_TARGET_PORT(port, NES_ACT_TURBO_A), NES_PAD_BTN_NORTH);
            seed_button(NES_TARGET_PORT(port, NES_ACT_SELECT), NES_PAD_BTN_BACK);
            seed_button(NES_TARGET_PORT(port, NES_ACT_START), NES_PAD_BTN_START);
        }
    }
}

static uint32_t fold(uint32_t keysym)
{
    if (keysym >= 'A' && keysym <= 'Z') return keysym + 32;
    /* the numeric keypad's digits mean the digits: one binding drives both */
    if (keysym >= NES_KEYSYM_KP_0 && keysym <= NES_KEYSYM_KP_9)
        return '0' + (keysym - NES_KEYSYM_KP_0);
    if (keysym == NES_KEYSYM_KP_ENTER) return NES_KEYSYM_RETURN;
    if (keysym == NES_KEYSYM_KP_MULTIPLY) return '*';
    if (keysym == NES_KEYSYM_KP_DIVIDE) return '/';
    if (keysym == NES_KEYSYM_KP_PERIOD) return '.';
    return keysym;
}

/* ---- persistence ---------------------------------------------------------- */

static void apply_persisted_locked(const char *packed)
{
    const char *p = packed;
    while (p && *p) {
        int target = -1; char kind = 0; long value = 0;
        const char *end = strchr(p, ' ');
        if (sscanf(p, "%d.%c:%ld", &target, &kind, &value) == 3
            && target >= 0 && target < NES_TARGET_COUNT) {
            if (kind == 'k') s_table[target].keysym = (uint32_t)value;
            else if (kind == 'b') s_table[target].button = (int)value;
        }
        if (!end) break;
        p = end + 1;
    }
}

static void pack_locked(char *out, size_t outsz)
{
    size_t len = 0;
    int t;
    out[0] = '\0';
    for (t = 0; t < NES_TARGET_COUNT; t++) {
        if (s_table[t].keysym != s_defaults[t].keysym)
            len += (size_t)snprintf(out + len, outsz - len, "%s%d.k:%u",
                                    len ? " " : "", t, s_table[t].keysym);
        if (len >= outsz) break;
        if (s_table[t].button != s_defaults[t].button)
            len += (size_t)snprintf(out + len, outsz - len, "%s%d.b:%d",
                                    len ? " " : "", t, s_table[t].button);
        if (len >= outsz) break;
    }
}

static void persist_locked(void)
{
    char packed[8192];
    if (!s_store) return;
    pack_locked(packed, sizeof packed);
    nessession_set_str(s_store, "bindings", packed);
}

static void ensure_seeded_locked(void)
{
    if (s_seeded) return;
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
}

void bindings_init(struct nessession *s)
{
    pthread_mutex_lock(&s_lock);
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
    s_store = s;
    if (s)
        apply_persisted_locked(nessession_get_str(s, "bindings", ""));
    pthread_mutex_unlock(&s_lock);
}

/* ---- lookups -------------------------------------------------------------- */

static int target_for_key_locked(uint32_t keysym)
{
    int t;
    if (!keysym) return -1;
    for (t = 0; t < NES_TARGET_COUNT; t++)
        if (s_table[t].keysym == keysym) return t;
    return -1;
}

int nessession_target_for_key(nessession *s, uint32_t keysym)
{
    int t;
    (void)s;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    t = target_for_key_locked(fold(keysym));
    pthread_mutex_unlock(&s_lock);
    return t;
}

/* Gamepad buttons are scoped to the pad's port: the same button index is
 * free to mean different things on different ports, so only that port's
 * targets and the machine-wide switches/actions are searched. */
int nessession_target_for_button(nessession *s, int port, int button)
{
    int t, found = -1;
    (void)s;
    if (button == NES_PAD_BTN_NONE) return -1;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    for (t = 0; t < NES_ACT_PER_PORT; t++)
        if (s_table[NES_TARGET_PORT(port, t)].button == button) {
            found = NES_TARGET_PORT(port, t);
            break;
        }
    if (found < 0)
        for (t = 2 * NES_ACT_PER_PORT; t < NES_TARGET_COUNT; t++)
            if (s_table[t].button == button) { found = t; break; }
    pthread_mutex_unlock(&s_lock);
    return found;
}

nes_binding nessession_binding_get(nessession *s, int target)
{
    nes_binding b = { 0, NES_PAD_BTN_NONE };
    (void)s;
    if (target < 0 || target >= NES_TARGET_COUNT) return b;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    b.keysym = s_table[target].keysym;
    b.button = s_table[target].button;
    pthread_mutex_unlock(&s_lock);
    return b;
}

/* A key may legitimately drive several targets (two players' controls can
 * be put on one key by hand). An explicit rebinding is the user saying
 * "this key does exactly this", so it displaces every holder -- and names
 * them all, comma-separated, so nothing is lost silently. */
static void describe_stolen(const int *victims, int nvictims, char *stolen,
                            int stolensz)
{
    int i, len = 0;
    if (!stolen || stolensz <= 0) return;
    stolen[0] = '\0';
    for (i = 0; i < nvictims && len < stolensz; i++)
        len += snprintf(stolen + len, (size_t)(stolensz - len), "%s%s",
                        i ? ", " : "", nes_target_name(victims[i]));
}

void nessession_binding_set_key(nessession *s, int target, uint32_t keysym,
                                  char *stolen, int stolensz)
{
    int victims[NES_TARGET_COUNT], nvictims = 0, t;
    (void)s;
    if (target < 0 || target >= NES_TARGET_COUNT) return;
    keysym = fold(keysym);
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    if (keysym) {
        for (t = 0; t < NES_TARGET_COUNT; t++)
            if (t != target && s_table[t].keysym == keysym) {
                s_table[t].keysym = 0;
                victims[nvictims++] = t;
            }
    }
    s_table[target].keysym = keysym;
    persist_locked();
    pthread_mutex_unlock(&s_lock);
    describe_stolen(victims, nvictims, stolen, stolensz);
}

void nessession_binding_set_button(nessession *s, int target, int button,
                                     char *stolen, int stolensz)
{
    int victims[NES_TARGET_COUNT], nvictims = 0, t;
    int port = target / NES_ACT_PER_PORT;   /* 2 = switches/sysactions */
    (void)s;
    if (target < 0 || target >= NES_TARGET_COUNT) return;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    if (button != NES_PAD_BTN_NONE) {
        for (t = 0; t < NES_TARGET_COUNT; t++) {
            int tport = t / NES_ACT_PER_PORT;
            /* steal only within the same scope: this port, or the
             * machine-wide targets, which every pad reaches */
            if (t != target && s_table[t].button == button
                && (tport == port || tport >= 2 || port >= 2)) {
                s_table[t].button = NES_PAD_BTN_NONE;
                victims[nvictims++] = t;
            }
        }
    }
    s_table[target].button = button;
    persist_locked();
    pthread_mutex_unlock(&s_lock);
    describe_stolen(victims, nvictims, stolen, stolensz);
}

void nessession_bindings_reset(nessession *s)
{
    (void)s;
    pthread_mutex_lock(&s_lock);
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
    if (s_store) nessession_set_str(s_store, "bindings", "");
    pthread_mutex_unlock(&s_lock);
}

/* ---- names ---------------------------------------------------------------- */

static const char *const act_names[NES_ACT_PER_PORT] = {
    "Up", "Down", "Left", "Right", "A", "B", "Select", "Start",
    "Turbo A", "Turbo B",
};
static const char *const act_short[NES_ACT_PER_PORT] = {
    "Up", "Down", "Left", "Right", "A", "B", "Select", "Start",
    "Turbo A", "Turbo B",
};
static const char *const switch_names[NES_SW_COUNT] = {
    "Reset Game",
};
static const char *const sysact_names[NES_SYSACT_COUNT] = {
    "Reset to CONFIG", "Pause",
};

const char *nes_target_name(int target)
{
    static char buf[64];
    if (target < 0 || target >= NES_TARGET_COUNT) return "";
    if (target < 2 * NES_ACT_PER_PORT) {
        snprintf(buf, sizeof buf, "Player %d: %s", target < NES_ACT_PER_PORT ? 1 : 2,
                 act_names[target % NES_ACT_PER_PORT]);
        return buf;
    }
    target -= 2 * NES_ACT_PER_PORT;
    if (target < NES_SW_COUNT) return switch_names[target];
    return sysact_names[target - NES_SW_COUNT];
}

const char *nes_target_short_name(int target)
{
    if (target < 0 || target >= NES_TARGET_COUNT) return "";
    if (target < 2 * NES_ACT_PER_PORT) return act_short[target % NES_ACT_PER_PORT];
    target -= 2 * NES_ACT_PER_PORT;
    if (target < NES_SW_COUNT) return switch_names[target];
    return sysact_names[target - NES_SW_COUNT];
}

static const char *const pad_button_names[NES_PAD_BTN_COUNT] = {
    "A", "B", "X", "Y", "Back", "Guide", "Start", "Left Stick", "Right Stick",
    "Left Shoulder", "Right Shoulder", "D-pad Up", "D-pad Down", "D-pad Left",
    "D-pad Right", "Left Trigger", "Right Trigger",
};

const char *nes_pad_button_name(int button)
{
    static char buf[32];
    if (NES_PAD_BTN_IS_NAMED(button)) return pad_button_names[button];
    if (NES_PAD_BTN_IS_RAW(button)) {
        snprintf(buf, sizeof buf, "Button %d", button - NES_PAD_BTN_RAW_BASE);
        return buf;
    }
    if (NES_PAD_BTN_IS_HAT(button)) {
        static const char *const dirs[NES_PAD_HAT_DIRS] =
            { "Up", "Up-Right", "Right", "Down-Right", "Down", "Down-Left",
              "Left", "Up-Left" };
        int h = (button - NES_PAD_HAT_BASE) / NES_PAD_HAT_DIRS;
        int d = (button - NES_PAD_HAT_BASE) % NES_PAD_HAT_DIRS;
        snprintf(buf, sizeof buf, "Hat %d %s", h, dirs[d]);
        return buf;
    }
    return "";
}

int nessession_keysym_name(uint32_t keysym, char *dst, int dstsz)
{
    const char *name = NULL;
    if (!dst || dstsz <= 0) return 0;
    switch (keysym) {
    case 0: name = ""; break;
    case NES_KEYSYM_UP: name = "Up"; break;
    case NES_KEYSYM_DOWN: name = "Down"; break;
    case NES_KEYSYM_LEFT: name = "Left"; break;
    case NES_KEYSYM_RIGHT: name = "Right"; break;
    case NES_KEYSYM_ESCAPE: name = "Escape"; break;
    case NES_KEYSYM_RETURN: name = "Return"; break;
    case NES_KEYSYM_BACKSPACE: name = "Backspace"; break;
    case NES_KEYSYM_TAB: name = "Tab"; break;
    case NES_KEYSYM_SPACE: name = "Space"; break;
    case NES_KEYSYM_LSHIFT: name = "Left Shift"; break;
    case NES_KEYSYM_RSHIFT: name = "Right Shift"; break;
    case NES_KEYSYM_LCTRL: name = "Left Ctrl"; break;
    case NES_KEYSYM_RCTRL: name = "Right Ctrl"; break;
    case NES_KEYSYM_LALT: name = "Left Alt"; break;
    case NES_KEYSYM_RALT: name = "Right Alt"; break;
    case NES_KEYSYM_KP_ENTER: name = "Keypad Enter"; break;
    case NES_KEYSYM_KP_MULTIPLY: name = "Keypad *"; break;
    case NES_KEYSYM_KP_DIVIDE: name = "Keypad /"; break;
    case NES_KEYSYM_KP_PERIOD: name = "Keypad ."; break;
    case 0xffff: name = "Delete"; break;
    case 0xff63: name = "Insert"; break;
    case 0xff50: name = "Home"; break;
    case 0xff57: name = "End"; break;
    case 0xff55: name = "Page Up"; break;
    case 0xff56: name = "Page Down"; break;
    case 0xffe5: name = "Caps Lock"; break;
    default: break;
    }
    if (name) return snprintf(dst, (size_t)dstsz, "%s", name);
    if (keysym >= NES_KEYSYM_F1 && keysym <= NES_KEYSYM_F12)
        return snprintf(dst, (size_t)dstsz, "F%u", keysym - NES_KEYSYM_F1 + 1);
    if (keysym >= NES_KEYSYM_KP_0 && keysym <= NES_KEYSYM_KP_9)
        return snprintf(dst, (size_t)dstsz, "Keypad %u", keysym - NES_KEYSYM_KP_0);
    if (keysym >= 0x21 && keysym <= 0x7e) {
        char c = (char)keysym;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        return snprintf(dst, (size_t)dstsz, "%c", c);
    }
    if (keysym >= NESSESSION_KEYSYM_HID_BASE
        && keysym <= NESSESSION_KEYSYM_HID_BASE + NESSESSION_HID_USAGE_MAX) {
        const char *hid = nes_hid_usage_name(keysym - NESSESSION_KEYSYM_HID_BASE);
        if (hid) return snprintf(dst, (size_t)dstsz, "%s", hid);
        return snprintf(dst, (size_t)dstsz, "HID 0x%02X",
                        (unsigned)(keysym - NESSESSION_KEYSYM_HID_BASE));
    }
    return snprintf(dst, (size_t)dstsz, "Key 0x%X", keysym);
}

/* ---- the keyboard translator --------------------------------------------- */

int nessession_key_sysaction(nessession *s, uint32_t keysym)
{
    int t;
    /* while typing into the NES keyboard, Escape is its ESC key */
    if (s && nessession_keyboard_captures(s)) return -1;
    t = nessession_target_for_key(s, keysym);
    if (t < NES_TARGET_SYSACT(0)) return -1;
    return t - NES_TARGET_SYSACT(0);
}

int nessession_key(nessession *s, uint32_t keysym, int down)
{
    uint32_t folded = fold(keysym);
    int t;

    if (!s) return 0;
    /* keyboard mode (and Scroll Lock, its toggle) first */
    if (session_keyboard_key(s, keysym, down)) return 1;
    if (down) {
        int hit = 0;
        /* every target the key drives; a system action is the frontend's
         * to fire and is not a machine input */
        pthread_mutex_lock(&s_lock);
        ensure_seeded_locked();
        for (t = 0; t < NES_TARGET_SYSACT(0); t++) {
            if (s_table[t].keysym != folded) continue;
            hit = 1;
            if (s->held_keysym[t] == folded) continue;  /* auto-repeat */
            s->held_keysym[t] = folded;
            pthread_mutex_unlock(&s_lock);
            nessession_press(s, t, 1);
            pthread_mutex_lock(&s_lock);
        }
        pthread_mutex_unlock(&s_lock);
        return hit;
    }
    {
        int hit = 0;
        /* remembered by the key, so a release clears exactly what its press
         * asserted even if the binding changed in between */
        for (t = 0; t < NES_TARGET_COUNT; t++) {
            if (s->held_keysym[t] == folded) {
                s->held_keysym[t] = 0;
                nessession_press(s, t, 0);
                hit = 1;
            }
        }
        return hit;
    }
}

void nessession_release_all(nessession *s)
{
    int t;
    if (!s) return;
    session_keyboard_release_all(s);
    for (t = 0; t < NES_TARGET_COUNT; t++) {
        if (s->held_keysym[t]) {
            s->held_keysym[t] = 0;
            nessession_press(s, t, 0);
        }
    }
}
