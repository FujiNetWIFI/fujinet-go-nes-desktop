/*
 * keyboard -- the expansion-port keyboards' key tables: which host key
 * presses which key, and the picture the on-screen keyboard draws.
 *
 * Key indices are MesenCE's own (the Buttons enums in
 * Core/NES/Input/FamilyBasicKeyboard.h and SuborKeyboard.h), because those
 * are what its devices read through KeyMapping.CustomKeys.
 *
 * Host keys map BY POSITION where the keyboards share a layout with a PC
 * keyboard, and by legend where they do not: the program on the NES does
 * its own shifting, so a host key is a switch on the matrix, never a
 * character. The Family BASIC keyboard's JIS-only keys take the JIS host
 * keys when the host has them (the Ro, Yen and Kana usages) and fall back
 * to the nearest spare PC key:
 *
 *   :  '       ^  =       @  `       ¥  \        _  Tab
 *   STOP  End/Pause       CLR HOME  Home        GRPH  Left Alt
 *   KANA  Right Alt       DEL  Backspace/Delete INS  Insert
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stddef.h>

#include "hid_keys.h"
#include "keyboard.h"
#include "nessession.h"

/* ---- key indices (MesenCE's enums) ----------------------------------------- */

enum {
    FB_A = 0, FB_0 = 26, FB_RETURN = 36, FB_SPACE, FB_DEL, FB_INS, FB_ESC,
    FB_CTRL, FB_RSHIFT, FB_LSHIFT, FB_RBRACKET, FB_LBRACKET,
    FB_UP, FB_DOWN, FB_LEFT, FB_RIGHT, FB_DOT, FB_COMMA, FB_COLON,
    FB_SEMICOLON, FB_UNDERSCORE, FB_SLASH, FB_MINUS, FB_CARET,
    FB_F1, FB_YEN = FB_F1 + 8, FB_STOP, FB_AT, FB_GRPH, FB_CLRHOME, FB_KANA,
    FB_COUNT
};

enum {
    SB_A = 0, SB_0 = 26, SB_F1 = 36, SB_KP0 = 48, SB_KP_ENTER = 58, SB_KP_DOT,
    SB_KP_PLUS, SB_KP_MULTIPLY, SB_KP_DIVIDE, SB_KP_MINUS, SB_NUMLOCK,
    SB_COMMA, SB_DOT, SB_SEMICOLON, SB_APOSTROPHE, SB_SLASH, SB_BACKSLASH,
    SB_EQUAL, SB_MINUS, SB_GRAVE, SB_LBRACKET, SB_RBRACKET, SB_CAPSLOCK,
    SB_PAUSE, SB_CTRL, SB_SHIFT, SB_ALT, SB_SPACE, SB_BACKSPACE, SB_TAB,
    SB_ESC, SB_ENTER, SB_END, SB_HOME, SB_INS, SB_DELETE, SB_PAGEUP,
    SB_PAGEDOWN, SB_UP, SB_DOWN, SB_LEFT, SB_RIGHT,
    SB_COUNT                       /* 96: Mesen's Unknown1-3 are not keys */
};

/* X11 keysyms the frontends deliver for the keys nessession.h does not name. */
#define KS_CAPS      0xffe5u
#define KS_PAUSE     0xff13u
#define KS_INSERT    0xff63u
#define KS_HOME      0xff50u
#define KS_PAGE_UP   0xff55u
#define KS_DELETE    0xffffu
#define KS_END       0xff57u
#define KS_PAGE_DOWN 0xff56u
#define KS_NUM_LOCK  0xff7fu
#define KS_KP_SUB    0xffadu
#define KS_KP_ADD    0xffabu
/* JIS keys that have no X11 keysym in hid_keys.c: the HID fallback band */
#define KS_JIS_RO    (NESSESSION_KEYSYM_HID_BASE + 0x87u)
#define KS_JIS_KANA  (NESSESSION_KEYSYM_HID_BASE + 0x88u)
#define KS_JIS_YEN   (NESSESSION_KEYSYM_HID_BASE + 0x89u)

/* ---- host keysym -> key index ------------------------------------------------ */

static int fb_index(uint32_t k)
{
    if (k >= 'a' && k <= 'z') return FB_A + (int)(k - 'a');
    if (k >= 'A' && k <= 'Z') return FB_A + (int)(k - 'A');
    if (k >= '0' && k <= '9') return FB_0 + (int)(k - '0');
    if (k >= NES_KEYSYM_F1 && k <= NES_KEYSYM_F8) return FB_F1 + (int)(k - NES_KEYSYM_F1);
    switch (k) {
    case NES_KEYSYM_RETURN: case NES_KEYSYM_KP_ENTER: return FB_RETURN;
    case NES_KEYSYM_SPACE: return FB_SPACE;
    case NES_KEYSYM_BACKSPACE: case KS_DELETE: return FB_DEL;
    case KS_INSERT: return FB_INS;
    case NES_KEYSYM_ESCAPE: return FB_ESC;
    case NES_KEYSYM_LCTRL: case NES_KEYSYM_RCTRL: return FB_CTRL;
    case NES_KEYSYM_RSHIFT: return FB_RSHIFT;
    case NES_KEYSYM_LSHIFT: return FB_LSHIFT;
    case ']': return FB_RBRACKET;
    case '[': return FB_LBRACKET;
    case NES_KEYSYM_UP: return FB_UP;
    case NES_KEYSYM_DOWN: return FB_DOWN;
    case NES_KEYSYM_LEFT: return FB_LEFT;
    case NES_KEYSYM_RIGHT: return FB_RIGHT;
    case '.': return FB_DOT;
    case ',': return FB_COMMA;
    case '\'': return FB_COLON;
    case ';': return FB_SEMICOLON;
    case NES_KEYSYM_TAB: case KS_JIS_RO: return FB_UNDERSCORE;
    case '/': return FB_SLASH;
    case '-': return FB_MINUS;
    case '=': return FB_CARET;
    case '\\': case KS_JIS_YEN: return FB_YEN;
    case KS_END: case KS_PAUSE: return FB_STOP;
    case '`': return FB_AT;
    case NES_KEYSYM_LALT: return FB_GRPH;
    case KS_HOME: return FB_CLRHOME;
    case NES_KEYSYM_RALT: case KS_JIS_KANA: return FB_KANA;
    default: return -1;
    }
}

static int sb_index(uint32_t k)
{
    if (k >= 'a' && k <= 'z') return SB_A + (int)(k - 'a');
    if (k >= 'A' && k <= 'Z') return SB_A + (int)(k - 'A');
    if (k >= '0' && k <= '9') return SB_0 + (int)(k - '0');
    if (k >= NES_KEYSYM_F1 && k <= NES_KEYSYM_F12) return SB_F1 + (int)(k - NES_KEYSYM_F1);
    if (k >= NES_KEYSYM_KP_0 && k <= NES_KEYSYM_KP_9) return SB_KP0 + (int)(k - NES_KEYSYM_KP_0);
    switch (k) {
    case NES_KEYSYM_KP_ENTER: return SB_KP_ENTER;
    case NES_KEYSYM_KP_PERIOD: return SB_KP_DOT;
    case KS_KP_ADD: return SB_KP_PLUS;
    case NES_KEYSYM_KP_MULTIPLY: return SB_KP_MULTIPLY;
    case NES_KEYSYM_KP_DIVIDE: return SB_KP_DIVIDE;
    case KS_KP_SUB: return SB_KP_MINUS;
    case KS_NUM_LOCK: return SB_NUMLOCK;
    case ',': return SB_COMMA;
    case '.': return SB_DOT;
    case ';': return SB_SEMICOLON;
    case '\'': return SB_APOSTROPHE;
    case '/': return SB_SLASH;
    case '\\': case KS_JIS_YEN: return SB_BACKSLASH;
    case '=': return SB_EQUAL;
    case '-': return SB_MINUS;
    case '`': return SB_GRAVE;
    case '[': return SB_LBRACKET;
    case ']': return SB_RBRACKET;
    case KS_CAPS: return SB_CAPSLOCK;
    case KS_PAUSE: return SB_PAUSE;
    case NES_KEYSYM_LCTRL: case NES_KEYSYM_RCTRL: return SB_CTRL;
    case NES_KEYSYM_LSHIFT: case NES_KEYSYM_RSHIFT: return SB_SHIFT;
    case NES_KEYSYM_LALT: case NES_KEYSYM_RALT: return SB_ALT;
    case NES_KEYSYM_SPACE: return SB_SPACE;
    case NES_KEYSYM_BACKSPACE: return SB_BACKSPACE;
    case NES_KEYSYM_TAB: return SB_TAB;
    case NES_KEYSYM_ESCAPE: return SB_ESC;
    case NES_KEYSYM_RETURN: return SB_ENTER;
    case KS_END: return SB_END;
    case KS_HOME: return SB_HOME;
    case KS_INSERT: return SB_INS;
    case KS_DELETE: return SB_DELETE;
    case KS_PAGE_UP: return SB_PAGEUP;
    case KS_PAGE_DOWN: return SB_PAGEDOWN;
    case NES_KEYSYM_UP: return SB_UP;
    case NES_KEYSYM_DOWN: return SB_DOWN;
    case NES_KEYSYM_LEFT: return SB_LEFT;
    case NES_KEYSYM_RIGHT: return SB_RIGHT;
    default: return -1;
    }
}

int nessession_keyboard_index_for_keysym(int type, uint32_t keysym)
{
    switch (type) {
    case NES_KBD_FAMILY_BASIC: return fb_index(keysym);
    case NES_KBD_SUBOR: return sb_index(keysym);
    default: return -1;
    }
}

int keyboard_key_count(int type)
{
    switch (type) {
    case NES_KBD_FAMILY_BASIC: return FB_COUNT;
    case NES_KBD_SUBOR: return SB_COUNT;
    default: return 0;
    }
}

/* ---- the on-screen layouts ---------------------------------------------------
 * Built once from row descriptions: each row is a list of (label, index,
 * width) laid left to right from a starting x; index -1 is a gap. */

typedef struct { const char *label; int index; float w; } cap;
#define GAP(w) { NULL, -1, (w) }
#define END    { NULL, -2, 0 }

#define MAX_KEYS 128
static nes_kbd_key fb_keys[MAX_KEYS], sb_keys[MAX_KEYS];
static int fb_n, sb_n;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void row(nes_kbd_key *out, int *n, float x, float y, const cap *caps)
{
    int i;
    for (i = 0; caps[i].index != -2; i++) {
        if (caps[i].index >= 0 && *n < MAX_KEYS) {
            nes_kbd_key *k = &out[(*n)++];
            k->label = caps[i].label;
            k->index = caps[i].index;
            k->x = x;
            k->y = y;
            k->w = caps[i].w;
            k->h = 1.0f;
        }
        x += caps[i].w;
    }
}

static void tall(nes_kbd_key *out, int *n, const char *label, int index,
                 float x, float y, float w, float h)
{
    nes_kbd_key *k = &out[(*n)++];
    k->label = label; k->index = index; k->x = x; k->y = y; k->w = w; k->h = h;
}

/* Nintendo HVC-007: function keys across the top, a JIS-style main block
 * with CTR/SHIFT/GRPH and KANA, and CLR HOME / INS / DEL over a cursor
 * diamond on the right. */
static void build_family_basic(void)
{
    static const cap r0[] = { GAP(1.5f), {"F1",FB_F1,1.5f}, {"F2",FB_F1+1,1.5f},
        {"F3",FB_F1+2,1.5f}, {"F4",FB_F1+3,1.5f}, {"F5",FB_F1+4,1.5f},
        {"F6",FB_F1+5,1.5f}, {"F7",FB_F1+6,1.5f}, {"F8",FB_F1+7,1.5f}, END };
    static const cap r1[] = { {"ESC",FB_ESC,1}, {"1",FB_0+1,1}, {"2",FB_0+2,1},
        {"3",FB_0+3,1}, {"4",FB_0+4,1}, {"5",FB_0+5,1}, {"6",FB_0+6,1},
        {"7",FB_0+7,1}, {"8",FB_0+8,1}, {"9",FB_0+9,1}, {"0",FB_0,1},
        {"-",FB_MINUS,1}, {"^",FB_CARET,1}, {"\xC2\xA5",FB_YEN,1},
        {"STOP",FB_STOP,1.5f}, GAP(0.5f), {"CLR HOME",FB_CLRHOME,1.5f},
        {"INS",FB_INS,1}, {"DEL",FB_DEL,1}, END };
    static const cap r2[] = { {"CTR",FB_CTRL,1.5f}, {"Q",FB_A+16,1}, {"W",FB_A+22,1},
        {"E",FB_A+4,1}, {"R",FB_A+17,1}, {"T",FB_A+19,1}, {"Y",FB_A+24,1},
        {"U",FB_A+20,1}, {"I",FB_A+8,1}, {"O",FB_A+14,1}, {"P",FB_A+15,1},
        {"@",FB_AT,1}, {"[",FB_LBRACKET,1}, {"RETURN",FB_RETURN,2},
        GAP(1.5f), {"\xE2\x86\x91",FB_UP,1}, END };
    static const cap r3[] = { GAP(1.75f), {"A",FB_A,1}, {"S",FB_A+18,1},
        {"D",FB_A+3,1}, {"F",FB_A+5,1}, {"G",FB_A+6,1}, {"H",FB_A+7,1},
        {"J",FB_A+9,1}, {"K",FB_A+10,1}, {"L",FB_A+11,1}, {";",FB_SEMICOLON,1},
        {":",FB_COLON,1}, {"]",FB_RBRACKET,1}, {"KANA",FB_KANA,1.25f},
        GAP(1.0f), {"\xE2\x86\x90",FB_LEFT,1}, GAP(1.0f), {"\xE2\x86\x92",FB_RIGHT,1}, END };
    static const cap r4[] = { {"SHIFT",FB_LSHIFT,2.25f}, {"Z",FB_A+25,1},
        {"X",FB_A+23,1}, {"C",FB_A+2,1}, {"V",FB_A+21,1}, {"B",FB_A+1,1},
        {"N",FB_A+13,1}, {"M",FB_A+12,1}, {",",FB_COMMA,1}, {".",FB_DOT,1},
        {"/",FB_SLASH,1}, {"_",FB_UNDERSCORE,1}, {"SHIFT",FB_RSHIFT,1.75f},
        GAP(1.0f), {"\xE2\x86\x93",FB_DOWN,1}, END };
    static const cap r5[] = { GAP(1.5f), {"GRPH",FB_GRPH,1.5f},
        {"SPACE",FB_SPACE,8}, END };
    row(fb_keys, &fb_n, 0, 0, r0);
    row(fb_keys, &fb_n, 0, 1, r1);
    row(fb_keys, &fb_n, 0, 2, r2);
    row(fb_keys, &fb_n, 0, 3, r3);
    row(fb_keys, &fb_n, 0, 4, r4);
    row(fb_keys, &fb_n, 0, 5, r5);
}

/* Subor: a PC/AT layout -- main block, the navigation cluster and arrows,
 * and a numeric keypad. One Shift/Ctrl/Alt on the matrix, drawn twice. */
static void build_subor(void)
{
    static const cap r0[] = { {"Esc",SB_ESC,1}, GAP(1), {"F1",SB_F1,1},
        {"F2",SB_F1+1,1}, {"F3",SB_F1+2,1}, {"F4",SB_F1+3,1}, GAP(0.5f),
        {"F5",SB_F1+4,1}, {"F6",SB_F1+5,1}, {"F7",SB_F1+6,1}, {"F8",SB_F1+7,1},
        GAP(0.5f), {"F9",SB_F1+8,1}, {"F10",SB_F1+9,1}, {"F11",SB_F1+10,1},
        {"F12",SB_F1+11,1}, GAP(2.25f), {"Pause",SB_PAUSE,1}, END };
    static const cap r1[] = { {"`",SB_GRAVE,1}, {"1",SB_0+1,1}, {"2",SB_0+2,1},
        {"3",SB_0+3,1}, {"4",SB_0+4,1}, {"5",SB_0+5,1}, {"6",SB_0+6,1},
        {"7",SB_0+7,1}, {"8",SB_0+8,1}, {"9",SB_0+9,1}, {"0",SB_0,1},
        {"-",SB_MINUS,1}, {"=",SB_EQUAL,1}, {"Bksp",SB_BACKSPACE,2}, GAP(0.25f),
        {"Ins",SB_INS,1}, {"Home",SB_HOME,1}, {"PgUp",SB_PAGEUP,1}, GAP(0.25f),
        {"Num",SB_NUMLOCK,1}, {"/",SB_KP_DIVIDE,1}, {"*",SB_KP_MULTIPLY,1},
        {"-",SB_KP_MINUS,1}, END };
    static const cap r2[] = { {"Tab",SB_TAB,1.5f}, {"Q",SB_A+16,1}, {"W",SB_A+22,1},
        {"E",SB_A+4,1}, {"R",SB_A+17,1}, {"T",SB_A+19,1}, {"Y",SB_A+24,1},
        {"U",SB_A+20,1}, {"I",SB_A+8,1}, {"O",SB_A+14,1}, {"P",SB_A+15,1},
        {"[",SB_LBRACKET,1}, {"]",SB_RBRACKET,1}, {"\\",SB_BACKSLASH,1.5f},
        GAP(0.25f), {"Del",SB_DELETE,1}, {"End",SB_END,1}, {"PgDn",SB_PAGEDOWN,1},
        GAP(0.25f), {"7",SB_KP0+7,1}, {"8",SB_KP0+8,1}, {"9",SB_KP0+9,1}, END };
    static const cap r3[] = { {"Caps",SB_CAPSLOCK,1.75f}, {"A",SB_A,1},
        {"S",SB_A+18,1}, {"D",SB_A+3,1}, {"F",SB_A+5,1}, {"G",SB_A+6,1},
        {"H",SB_A+7,1}, {"J",SB_A+9,1}, {"K",SB_A+10,1}, {"L",SB_A+11,1},
        {";",SB_SEMICOLON,1}, {"'",SB_APOSTROPHE,1}, {"Enter",SB_ENTER,2.25f},
        GAP(3.5f), {"4",SB_KP0+4,1}, {"5",SB_KP0+5,1}, {"6",SB_KP0+6,1}, END };
    static const cap r4[] = { {"Shift",SB_SHIFT,2.25f}, {"Z",SB_A+25,1},
        {"X",SB_A+23,1}, {"C",SB_A+2,1}, {"V",SB_A+21,1}, {"B",SB_A+1,1},
        {"N",SB_A+13,1}, {"M",SB_A+12,1}, {",",SB_COMMA,1}, {".",SB_DOT,1},
        {"/",SB_SLASH,1}, {"Shift",SB_SHIFT,2.75f}, GAP(1.25f),
        {"\xE2\x86\x91",SB_UP,1}, GAP(1.25f), {"1",SB_KP0+1,1},
        {"2",SB_KP0+2,1}, {"3",SB_KP0+3,1}, END };
    static const cap r5[] = { {"Ctrl",SB_CTRL,1.5f}, GAP(1), {"Alt",SB_ALT,1.5f},
        {"Space",SB_SPACE,7}, {"Alt",SB_ALT,1.5f}, GAP(1), {"Ctrl",SB_CTRL,1.5f},
        GAP(0.25f), {"\xE2\x86\x90",SB_LEFT,1}, {"\xE2\x86\x93",SB_DOWN,1},
        {"\xE2\x86\x92",SB_RIGHT,1}, GAP(0.25f), {"0",SB_KP0,2},
        {".",SB_KP_DOT,1}, END };
    row(sb_keys, &sb_n, 0, 0, r0);
    row(sb_keys, &sb_n, 0, 1, r1);
    row(sb_keys, &sb_n, 0, 2, r2);
    row(sb_keys, &sb_n, 0, 3, r3);
    row(sb_keys, &sb_n, 0, 4, r4);
    row(sb_keys, &sb_n, 0, 5, r5);
    /* the keypad's two-row keys, right of 9 and of 3 */
    tall(sb_keys, &sb_n, "+", SB_KP_PLUS, 21.5f, 2, 1, 2);
    tall(sb_keys, &sb_n, "Ent", SB_KP_ENTER, 21.5f, 4, 1, 2);
}

static void build(void)
{
    build_family_basic();
    build_subor();
}

int nessession_keyboard_layout(int type, const nes_kbd_key **keys)
{
    pthread_once(&once, build);
    switch (type) {
    case NES_KBD_FAMILY_BASIC: if (keys) *keys = fb_keys; return fb_n;
    case NES_KBD_SUBOR: if (keys) *keys = sb_keys; return sb_n;
    default: if (keys) *keys = NULL; return 0;
    }
}

const char *nes_keyboard_name(int type)
{
    static const char *const names[NES_KBD_COUNT] =
        { "None", "Family BASIC Keyboard", "Subor Keyboard" };
    return (type >= 0 && type < NES_KBD_COUNT) ? names[type] : NULL;
}
