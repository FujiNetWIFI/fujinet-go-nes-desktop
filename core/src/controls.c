/*
 * controls -- the layout of the on-screen controllers and of the expansion
 * keyboards, for every toolkit.
 *
 * One table (nessession_controls_layout) says what the Controllers window
 * draws and where, in layout units (DESIGN-nes.md's u) from the top-left; the
 * toolkits draw the art from it and ask nessession_controls_hit what a click
 * landed on, so the shapes cannot drift apart between GTK, Qt, AppKit and
 * Win32. It has a view per port, following what is plugged in (the NES-001
 * pad, or an empty socket), over a console strip with the RESET key. Every
 * view has the same extent, so the window never changes size when a port's
 * type does.
 *
 * The second table (nessession_keyboard_skin_layout) is the Family BASIC and
 * Subor keyboards: the keys of nessession_keyboard_layout placed in their case
 * with the plates and the keycap colours of DESIGN-nes.md sections 4 and 5.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <pthread.h>
#include <string.h>

#include "nessession.h"

/* ---- geometry (layout units) ------------------------------------------------------ */

#define CAPTION_H   0.9f
#define PORT_W     16.0f
#define PORT_H      7.0f
#define PORT_GAP    1.0f
#define CONSOLE_W  33.0f
#define CONSOLE_H   4.0f
#define CONSOLE_GAP 0.6f

#define MAX_CONTROLS 80

typedef struct {
    nes_control ctl[MAX_CONTROLS];
    int n;
} layout;

static layout g_layout[NES_CTRL_COUNT][NES_CTRL_COUNT];
static int g_built[NES_CTRL_COUNT][NES_CTRL_COUNT];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void add(layout *lay, int kind, const char *label, const char *legend,
                int port, int target, int device,
                float x, float y, float w, float h)
{
    nes_control *c;
    if (lay->n >= MAX_CONTROLS) return;   /* controls_test counts the parts: it fails if one is dropped */
    c = &lay->ctl[lay->n++];
    c->kind = kind;
    c->label = label;
    c->legend = legend;
    c->port = port;
    c->target = target;
    c->device = device;
    c->x = x; c->y = y; c->w = w; c->h = h;
}

static int view_type(int type)
{
    return type >= 0 && type < NES_CTRL_COUNT ? type : NES_CTRL_STANDARD;
}

/* The NES-001 pad, top view, in its 16 x 7 box (DESIGN-nes.md section 1):
 * the shell, the dark face inset 0.8 on every side, the stripes and logo; the
 * cross in its 4.6 u well on the left (centre (3.3, 3.5), arms 1.3 wide and
 * 3.4 long overall); SELECT and START (1.4 x 0.6 rubber pills) in their well
 * with the red legends above, one 0.7 u gap clear of the cross's well; B and
 * A (r 0.8) side by side on ONE line in their wells, as on the real pad (the
 * stagger in the oblique photo is perspective); and the two small turbo pills
 * above them on one line too, centred on each button, the NES
 * Advantage's, which are the core's TURBO B / TURBO A. */
static void add_pad(layout *lay, int port, float ox, float oy)
{
    const int dev = NES_CTRL_STANDARD;
    add(lay, NES_CTL_BODY, "NES-001", NULL, port, -1, dev, ox, oy, PORT_W, PORT_H);
    add(lay, NES_CTL_BODY, "Face", NULL, port, -1, dev, ox + 0.8f, oy + 0.8f, 14.4f, 5.4f);
    add(lay, NES_CTL_BODY, "Stripe", NULL, port, -1, dev, ox + 6.3f, oy + 1.2f, 4.2f, 0.5f);
    add(lay, NES_CTL_BODY, "Stripe", NULL, port, -1, dev, ox + 6.3f, oy + 1.9f, 4.2f, 0.5f);
    add(lay, NES_CTL_BODY, "Stripe", NULL, port, -1, dev, ox + 6.3f, oy + 6.0f, 3.9f, 0.2f);
    /* Centred in the face to the right of the stripes (10.5 .. 15.2). */
    add(lay, NES_CTL_CAPTION, "Nintendo", NULL, port, -1, dev, ox + 11.6f, oy + 1.5f, 2.5f, 0.7f);

    add(lay, NES_CTL_BODY, "DpadWell", NULL, port, -1, dev, ox + 1.0f, oy + 1.2f, 4.6f, 4.6f);
    add(lay, NES_CTL_DPAD, "D-PAD", NULL, port, -1, dev, ox + 1.0f, oy + 1.2f, 4.6f, 4.6f);
    add(lay, NES_CTL_DPAD_ARM, "Up", NULL, port, NES_TARGET_PORT(port, NES_ACT_UP), dev,
        ox + 2.65f, oy + 1.8f, 1.3f, 1.7f);
    add(lay, NES_CTL_DPAD_ARM, "Down", NULL, port, NES_TARGET_PORT(port, NES_ACT_DOWN), dev,
        ox + 2.65f, oy + 3.5f, 1.3f, 1.7f);
    add(lay, NES_CTL_DPAD_ARM, "Left", NULL, port, NES_TARGET_PORT(port, NES_ACT_LEFT), dev,
        ox + 1.6f, oy + 2.85f, 1.7f, 1.3f);
    add(lay, NES_CTL_DPAD_ARM, "Right", NULL, port, NES_TARGET_PORT(port, NES_ACT_RIGHT), dev,
        ox + 3.3f, oy + 2.85f, 1.7f, 1.3f);

    /* SELECT / START well 6.3 .. 10.2: 0.4 edge, pill 1.3, 0.5 gap, pill 1.3, 0.4 edge. */
    add(lay, NES_CTL_BODY, "SelectWell", NULL, port, -1, dev, ox + 6.3f, oy + 4.2f, 3.9f, 1.7f);
    add(lay, NES_CTL_CAPTION, "SELECT", NULL, port, -1, dev, ox + 6.7f, oy + 3.65f, 1.3f, 0.5f);
    add(lay, NES_CTL_CAPTION, "START", NULL, port, -1, dev, ox + 8.5f, oy + 3.65f, 1.3f, 0.5f);
    add(lay, NES_CTL_PILL, "SELECT", "select", port, NES_TARGET_PORT(port, NES_ACT_SELECT), dev,
        ox + 6.7f, oy + 4.75f, 1.3f, 0.6f);
    add(lay, NES_CTL_PILL, "START", "start", port, NES_TARGET_PORT(port, NES_ACT_START), dev,
        ox + 8.5f, oy + 4.75f, 1.3f, 0.6f);

    /* B and A on one line: wells 10.8 .. 12.7 and 12.9 .. 14.8, both y 3.7 .. 5.6. */
    add(lay, NES_CTL_BODY, "ButtonWell", NULL, port, -1, dev, ox + 10.8f, oy + 3.7f, 1.9f, 1.9f);
    add(lay, NES_CTL_BODY, "ButtonWell", NULL, port, -1, dev, ox + 12.9f, oy + 3.7f, 1.9f, 1.9f);
    add(lay, NES_CTL_ROUND_BUTTON, "B", "B button", port, NES_TARGET_PORT(port, NES_ACT_B), dev,
        ox + 10.95f, oy + 3.85f, 1.6f, 1.6f);
    add(lay, NES_CTL_ROUND_BUTTON, "A", "A button", port, NES_TARGET_PORT(port, NES_ACT_A), dev,
        ox + 13.05f, oy + 3.85f, 1.6f, 1.6f);
    add(lay, NES_CTL_CAPTION, "B", NULL, port, -1, dev, ox + 12.05f, oy + 5.65f, 0.6f, 0.5f);
    add(lay, NES_CTL_CAPTION, "A", NULL, port, -1, dev, ox + 14.15f, oy + 5.65f, 0.6f, 0.5f);

    /* Turbo pills on one line, centred over each button (centres 11.75 / 13.85). */
    add(lay, NES_CTL_PILL, "TURBO B", "turbo B", port, NES_TARGET_PORT(port, NES_ACT_TURBO_B), dev,
        ox + 11.15f, oy + 2.7f, 1.2f, 0.6f);
    add(lay, NES_CTL_PILL, "TURBO A", "turbo A", port, NES_TARGET_PORT(port, NES_ACT_TURBO_A), dev,
        ox + 13.25f, oy + 2.7f, 1.2f, 0.6f);
    add(lay, NES_CTL_CAPTION, "T", NULL, port, -1, dev, ox + 12.4f, oy + 2.65f, 0.5f, 0.6f);
    add(lay, NES_CTL_CAPTION, "T", NULL, port, -1, dev, ox + 14.5f, oy + 2.65f, 0.5f, 0.6f);
}

/* An empty port: the dashed socket outline and its words, in the pad's box. */
static void add_none(layout *lay, int port, float ox, float oy)
{
    const int dev = NES_CTRL_NONE;
    add(lay, NES_CTL_BODY, "Socket", NULL, port, -1, dev, ox, oy, PORT_W, PORT_H);
    add(lay, NES_CTL_CAPTION, "No controller", NULL, port, -1, dev, ox + 2.0f, oy + 2.7f, 12.0f, 1.6f);
}

/* The console strip, 33 x 4: the light slab over the darker base band, the
 * red "Nintendo ENTERTAINMENT SYSTEM" at the left, and at the right the two
 * flat keys of the NES-001's front, POWER (drawn dimmed and no target, the
 * emulator is on) and RESET (momentary). */
static void add_console(layout *lay, float oy)
{
    add(lay, NES_CTL_BODY, "Console", NULL, -1, -1, -1, 0.0f, oy, CONSOLE_W, CONSOLE_H);
    add(lay, NES_CTL_BODY, "Base", NULL, -1, -1, -1, 0.0f, oy + 2.0f, CONSOLE_W, 2.0f);
    add(lay, NES_CTL_CAPTION, "Nintendo ENTERTAINMENT SYSTEM", NULL, -1, -1, -1,
        1.0f, oy + 0.6f, 16.0f, 1.0f);
    add(lay, NES_CTL_BODY, "POWER", "not interactive", -1, -1, -1, 22.0f, oy + 1.2f, 4.5f, 1.6f);
    add(lay, NES_CTL_BUTTON, "RESET", "reset", -1,
        NES_TARGET_SWITCH(NES_SW_RESET), -1, 27.5f, oy + 1.2f, 4.5f, 1.6f);
}

static void build(layout *lay, int left, int right)
{
    static const char *const k_caption[2] = { "Player 1", "Player 2" };
    int port;
    memset(lay, 0, sizeof *lay);
    for (port = 0; port < 2; port++) {
        const float ox = (float)port * (PORT_W + PORT_GAP);
        const float oy = CAPTION_H;
        add(lay, NES_CTL_CAPTION, k_caption[port], NULL, port, -1, -1, ox, 0.0f, PORT_W, CAPTION_H);
        if ((port ? right : left) == NES_CTRL_NONE) add_none(lay, port, ox, oy);
        else add_pad(lay, port, ox, oy);
    }
    add_console(lay, CAPTION_H + PORT_H + CONSOLE_GAP);
}

const nes_control *nessession_controls_layout(int type0, int type1, int *n,
                                              float *width, float *height)
{
    const int lt = view_type(type0), rt = view_type(type1);
    layout *lay;

    pthread_mutex_lock(&g_lock);
    if (!g_built[lt][rt]) {
        build(&g_layout[lt][rt], lt, rt);
        g_built[lt][rt] = 1;
    }
    lay = &g_layout[lt][rt];
    pthread_mutex_unlock(&g_lock);
    if (n) *n = lay->n;
    if (width) *width = NESSESSION_CONTROLS_W;
    if (height) *height = NESSESSION_CONTROLS_H;
    return lay->ctl;
}

static int is_pressable(int kind)
{
    return kind == NES_CTL_DPAD || kind == NES_CTL_ROUND_BUTTON
        || kind == NES_CTL_PILL || kind == NES_CTL_BUTTON;
}

int nessession_controls_hit(int type0, int type1, float x, float y)
{
    int n, i;
    const nes_control *c = nessession_controls_layout(type0, type1, &n, NULL, NULL);

    /* the topmost: later entries are drawn over earlier ones */
    for (i = n - 1; i >= 0; i--) {
        float grow = 0.0f;
        if (!is_pressable(c[i].kind)) continue;
        if (c[i].kind == NES_CTL_PILL) grow = 0.25f;   /* a 0.6 u rubber pill is a small target */
        if (x < c[i].x - grow || x >= c[i].x + c[i].w + grow
            || y < c[i].y - grow || y >= c[i].y + c[i].h + grow)
            continue;
        if (c[i].kind == NES_CTL_ROUND_BUTTON) {
            const float rx = c[i].w * 0.5f, ry = c[i].h * 0.5f;
            const float dx = (x - (c[i].x + rx)) / rx, dy = (y - (c[i].y + ry)) / ry;
            if (dx * dx + dy * dy > 1.0f) continue;
        }
        return i;
    }
    return -1;
}

unsigned nessession_dpad_from_vector(float dx, float dy)
{
    const float ax = fabsf(dx), ay = fabsf(dy);
    unsigned mask;
    /* tan(22.5 degrees): the edge of a 45-degree sector */
    const float edge = 0.41421356f;

    if (ax * ax + ay * ay < 0.4f * 0.4f) return 0;
    if (ay <= ax * edge)       mask = dx < 0.f ? 1u << NES_ACT_LEFT : 1u << NES_ACT_RIGHT;
    else if (ax <= ay * edge)  mask = dy < 0.f ? 1u << NES_ACT_UP : 1u << NES_ACT_DOWN;
    else {
        mask = dx < 0.f ? 1u << NES_ACT_LEFT : 1u << NES_ACT_RIGHT;
        mask |= dy < 0.f ? 1u << NES_ACT_UP : 1u << NES_ACT_DOWN;
    }
    return mask;
}

/* ---- the keyboard skins ------------------------------------------------------------- */

#define MAX_SKIN 192

typedef struct {
    nes_skin_item item[MAX_SKIN];
    int n;
    float w, h;
} skin;

static skin g_skin[NES_KBD_COUNT];
static int g_skin_built[NES_KBD_COUNT];

static void skin_add(skin *sk, int kind, const char *label, const char *text, int key,
                     int cap_class, uint32_t fill, uint32_t ink,
                     float x, float y, float w, float h)
{
    nes_skin_item *it;
    if (sk->n >= MAX_SKIN) return;
    it = &sk->item[sk->n++];
    it->kind = kind;
    it->label = label;
    it->text = text;
    it->key = key;
    it->cap_class = cap_class;
    it->fill_rgb = fill;
    it->ink_rgb = ink;
    it->x = x; it->y = y; it->w = w; it->h = h;
}

/* The key indices of keyboard.c's two enums that the colour rules name. The
 * enums are private to keyboard.c; the labels are stable, so the classes are
 * chosen from them (a legend is the key's identity on the printed cap). */
static int fb_is_red(const char *label)
{
    static const char *const red[] = {
        "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "RETURN", "SHIFT", "SPACE",
        "\xE2\x86\x91", "\xE2\x86\x90", "\xE2\x86\x92", "\xE2\x86\x93", NULL
    };
    int i;
    for (i = 0; red[i]; i++)
        if (!strcmp(label, red[i])) return 1;
    return 0;
}

static int sb_is_dark(const char *label)
{
    static const char *const dark[] = {
        "Esc", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
        "Pause", "Bksp", "Ins", "Home", "PgUp", "Del", "End", "PgDn", "Num", "Tab",
        "Caps", "Enter", "Shift", "Ctrl", "Alt", "Ent",
        "\xE2\x86\x91", "\xE2\x86\x90", "\xE2\x86\x93", "\xE2\x86\x92", NULL
    };
    int i;
    for (i = 0; dark[i]; i++)
        if (!strcmp(label, dark[i])) return 1;
    return 0;
}

/* The keypad's / * - + are dark; its digits (the same labels as the main
 * row's) and "." are cream. They are told apart by position: the keypad is
 * the block right of x = 17.5 u in the layout. */
static int sb_is_keypad_operator(const nes_kbd_key *key)
{
    return key->x >= 17.5f && (!strcmp(key->label, "/") || !strcmp(key->label, "*")
                               || !strcmp(key->label, "-") || !strcmp(key->label, "+"));
}


/* The HVC-007 as photographed (reference/nes/famibasic-user.png, DESIGN-nes.md
 * section 4): every key's position in key units from the case's top-left
 * corner, and its printed legends: `text` is "shifted|kana" (either may be
 * empty). The debugger keeps keyboard.c's plain grid; only the skin follows the
 * photo. Keys not listed fall back to the grid. */
typedef struct { const char *label; int occurrence; float x, y, w, h; const char *text; } fb_photo_key;

#define FB_FROW 2.3f
#define FB_R1 3.25f
#define FB_R2 4.25f
#define FB_R3 5.25f
#define FB_R4 6.25f
#define FB_R5 7.25f
static const fb_photo_key fb_photo[] = {
    {"F1", 0, 1.3f, FB_FROW, 1.9f, 0.95f, NULL}, {"F2", 0, 3.3f, FB_FROW, 1.9f, 0.95f, NULL},
    {"F3", 0, 5.3f, FB_FROW, 1.9f, 0.95f, NULL}, {"F4", 0, 7.3f, FB_FROW, 1.9f, 0.95f, NULL},
    {"F5", 0, 9.3f, FB_FROW, 1.9f, 0.95f, NULL}, {"F6", 0, 11.3f, FB_FROW, 1.9f, 0.95f, NULL},
    {"F7", 0, 13.3f, FB_FROW, 1.9f, 0.95f, NULL}, {"F8", 0, 15.3f, FB_FROW, 1.9f, 0.95f, NULL},

    {"1", 0, 1.7f, FB_R1, 1.0f, 1.0f, "!|\xE3\x82\xA2"},   {"2", 0, 2.7f, FB_R1, 1.0f, 1.0f, "\"|\xE3\x82\xA4"},
    {"3", 0, 3.7f, FB_R1, 1.0f, 1.0f, "#|\xE3\x82\xA6"},   {"4", 0, 4.7f, FB_R1, 1.0f, 1.0f, "$|\xE3\x82\xA8"},
    {"5", 0, 5.7f, FB_R1, 1.0f, 1.0f, "%|\xE3\x82\xAA"},   {"6", 0, 6.7f, FB_R1, 1.0f, 1.0f, "&|\xE3\x83\x8A"},
    {"7", 0, 7.7f, FB_R1, 1.0f, 1.0f, "'|\xE3\x83\x8B"},   {"8", 0, 8.7f, FB_R1, 1.0f, 1.0f, "(|\xE3\x83\x8C"},
    {"9", 0, 9.7f, FB_R1, 1.0f, 1.0f, ")|\xE3\x83\x8D"},   {"0", 0, 10.7f, FB_R1, 1.0f, 1.0f, "|\xE3\x83\x8E"},
    {"-", 0, 11.7f, FB_R1, 1.0f, 1.0f, "=|\xE3\x83\xA9"},  {"^", 0, 12.7f, FB_R1, 1.0f, 1.0f, "|\xE3\x83\xAA"},
    {"\xC2\xA5", 0, 13.7f, FB_R1, 1.0f, 1.0f, "|\xE3\x83\xAB"},
    {"STOP", 0, 14.8f, FB_R1, 0.95f, 1.0f, NULL},

    {"ESC", 0, 1.1f, FB_R2, 1.1f, 1.0f, NULL},
    {"Q", 0, 2.25f, FB_R2, 1.0f, 1.0f, "|\xE3\x82\xAB"},   {"W", 0, 3.25f, FB_R2, 1.0f, 1.0f, "|\xE3\x82\xAD"},
    {"E", 0, 4.25f, FB_R2, 1.0f, 1.0f, "|\xE3\x82\xAF"},   {"R", 0, 5.25f, FB_R2, 1.0f, 1.0f, "|\xE3\x82\xB1"},
    {"T", 0, 6.25f, FB_R2, 1.0f, 1.0f, "|\xE3\x82\xB3"},   {"Y", 0, 7.25f, FB_R2, 1.0f, 1.0f, "|\xE3\x83\x8F"},
    {"U", 0, 8.25f, FB_R2, 1.0f, 1.0f, "|\xE3\x83\x92"},   {"I", 0, 9.25f, FB_R2, 1.0f, 1.0f, "|\xE3\x83\x95"},
    {"O", 0, 10.25f, FB_R2, 1.0f, 1.0f, "|\xE3\x83\x98"},  {"P", 0, 11.25f, FB_R2, 1.0f, 1.0f, "|\xE3\x83\x9B"},
    {"@", 0, 12.25f, FB_R2, 1.0f, 1.0f, "|\xE3\x83\xAC"},  {"[", 0, 13.25f, FB_R2, 1.0f, 1.0f, "\xE3\x80\x8C|\xE3\x83\xAD"},
    {"RETURN", 0, 14.3f, FB_R2, 1.9f, 1.0f, NULL},

    {"CTR", 0, 1.45f, FB_R3, 1.0f, 1.0f, NULL},
    {"A", 0, 2.5f, FB_R3, 1.0f, 1.0f, "|\xE3\x82\xB5"},    {"S", 0, 3.5f, FB_R3, 1.0f, 1.0f, "|\xE3\x82\xB7"},
    {"D", 0, 4.5f, FB_R3, 1.0f, 1.0f, "|\xE3\x82\xB9"},    {"F", 0, 5.5f, FB_R3, 1.0f, 1.0f, "|\xE3\x82\xBB"},
    {"G", 0, 6.5f, FB_R3, 1.0f, 1.0f, "|\xE3\x82\xBD"},    {"H", 0, 7.5f, FB_R3, 1.0f, 1.0f, "|\xE3\x83\x9E"},
    {"J", 0, 8.5f, FB_R3, 1.0f, 1.0f, "|\xE3\x83\x9F"},    {"K", 0, 9.5f, FB_R3, 1.0f, 1.0f, "|\xE3\x83\xA0"},
    {"L", 0, 10.5f, FB_R3, 1.0f, 1.0f, "|\xE3\x83\xA1"},   {";", 0, 11.5f, FB_R3, 1.0f, 1.0f, "+|\xE3\x83\xA2"},
    {":", 0, 12.5f, FB_R3, 1.0f, 1.0f, "*|\xE3\x83\xBC"},  {"]", 0, 13.5f, FB_R3, 1.0f, 1.0f, "\xE3\x80\x8D|\xE3\x80\x82"},
    {"KANA", 0, 14.5f, FB_R3, 1.0f, 1.0f, NULL},

    {"SHIFT", 0, 1.05f, FB_R4, 1.9f, 1.0f, NULL},
    {"Z", 0, 3.0f, FB_R4, 1.0f, 1.0f, "|\xE3\x82\xBF"},    {"X", 0, 4.0f, FB_R4, 1.0f, 1.0f, "|\xE3\x83\x81"},
    {"C", 0, 5.0f, FB_R4, 1.0f, 1.0f, "|\xE3\x83\x84"},    {"V", 0, 6.0f, FB_R4, 1.0f, 1.0f, "|\xE3\x83\x86"},
    {"B", 0, 7.0f, FB_R4, 1.0f, 1.0f, "|\xE3\x83\x88"},    {"N", 0, 8.0f, FB_R4, 1.0f, 1.0f, "|\xE3\x83\xA4"},
    {"M", 0, 9.0f, FB_R4, 1.0f, 1.0f, "|\xE3\x83\xA6"},    {",", 0, 10.0f, FB_R4, 1.0f, 1.0f, "<|\xE3\x83\xA8"},
    {".", 0, 11.0f, FB_R4, 1.0f, 1.0f, ">|\xE3\x83\xAF"},  {"/", 0, 12.0f, FB_R4, 1.0f, 1.0f, "?|\xE3\x83\xB2"},
    {"_", 0, 13.0f, FB_R4, 1.0f, 1.0f, "|\xE3\x83\xB3"},
    {"SHIFT", 1, 14.05f, FB_R4, 1.9f, 1.0f, NULL},

    {"GRPH", 0, 3.95f, FB_R5, 1.0f, 1.0f, NULL},
    {"SPACE", 0, 5.0f, FB_R5, 7.9f, 1.0f, NULL},

    {"CLR HOME", 0, 16.7f, 3.7f, 1.1f, 0.95f, NULL},
    {"INS", 0, 17.95f, 3.7f, 1.1f, 0.95f, NULL},
    {"DEL", 0, 19.2f, 3.7f, 1.1f, 0.95f, NULL},
    {"\xE2\x86\x91", 0, 17.65f, 4.75f, 1.7f, 0.95f, NULL},
    {"\xE2\x86\x90", 0, 16.3f, 5.75f, 2.0f, 0.95f, NULL},
    {"\xE2\x86\x92", 0, 18.4f, 5.75f, 2.0f, 0.95f, NULL},
    {"\xE2\x86\x93", 0, 17.65f, 6.75f, 1.7f, 0.95f, NULL},
    {NULL, 0, 0, 0, 0, 0, NULL}
};

static const fb_photo_key *fb_photo_find(const char *label, int occurrence)
{
    int i;
    for (i = 0; fb_photo[i].label; i++)
        if (!strcmp(fb_photo[i].label, label) && fb_photo[i].occurrence == occurrence)
            return &fb_photo[i];
    return NULL;
}

static int fb_is_pale(const char *label)
{
    return !strcmp(label, "CLR HOME") || !strcmp(label, "INS") || !strcmp(label, "DEL");
}

static void build_skin(skin *sk, int type)
{
    const nes_kbd_key *keys;
    int count = nessession_keyboard_layout(type, &keys), i;
    float maxx = 0.0f, maxy = 0.0f, offx, offy, casew, caseh;

    memset(sk, 0, sizeof *sk);
    for (i = 0; i < count; i++) {
        if (keys[i].x + keys[i].w > maxx) maxx = keys[i].x + keys[i].w;
        if (keys[i].y + keys[i].h > maxy) maxy = keys[i].y + keys[i].h;
    }

    if (type == NES_KBD_FAMILY_BASIC) {
        /* the HVC-007 (famibasic-user.png): off-white case, two groups of vent
         * slits and the HVC-007 plate along the top, the keys in recessed wells
         * that follow the rows, the FAMILY COMPUTER plate under them */
        static const float wells[][4] = {
            {1.2f, 2.2f, 16.0f, 1.1f},  {1.6f, 3.2f, 14.2f, 1.1f}, {1.0f, 4.2f, 15.3f, 1.1f},
            {1.35f, 5.2f, 14.25f, 1.1f}, {0.95f, 6.2f, 15.15f, 1.1f}, {3.85f, 7.2f, 9.15f, 1.1f},
            {16.2f, 3.6f, 4.3f, 4.2f}
        };
        size_t wi;
        offx = 0.0f; offy = 0.0f;
        casew = 21.6f;
        caseh = 10.6f;
        skin_add(sk, NES_SKIN_BODY, "Case", NULL, -1, 0, NESSESSION_FB_CASE_RGB, 0,
                 0.0f, 0.0f, casew, caseh);
        skin_add(sk, NES_SKIN_BODY, "Vent", NULL, -1, 0, NESSESSION_FB_VENT_RGB, 0,
                 3.1f, 0.1f, 3.4f, 0.5f);
        skin_add(sk, NES_SKIN_BODY, "Vent", NULL, -1, 0, NESSESSION_FB_VENT_RGB, 0,
                 14.6f, 0.1f, 3.4f, 0.5f);
        for (wi = 0; wi < sizeof wells / sizeof wells[0]; wi++)
            skin_add(sk, NES_SKIN_BODY, "Well", NULL, -1, 0, NESSESSION_FB_WELL_RGB,
                     NESSESSION_FB_WELL_EDGE_RGB, wells[wi][0], wells[wi][1], wells[wi][2], wells[wi][3]);
        skin_add(sk, NES_SKIN_BODY, "Name", "HVC-007", -1, 0, NESSESSION_FB_PLATE_RGB,
                 NESSESSION_FB_NAME_INK_RGB, 17.9f, 1.6f, 2.75f, 0.7f);
        skin_add(sk, NES_SKIN_BODY, "Plate", "FAMILY COMPUTER", -1, 0, NESSESSION_FB_PLATE_RGB,
                 NESSESSION_FB_GOLD_RGB, 6.6f, 9.4f, 7.8f, 0.7f);
    } else {
        /* the Subor: aged-cream case with a SUBOR plate above the keys and a
         * red logo at the top left */
        offx = 0.8f; offy = 2.0f;
        casew = maxx + 2.0f * offx;
        caseh = offy + maxy + 0.8f;
        skin_add(sk, NES_SKIN_BODY, "Case", NULL, -1, 0, NESSESSION_SUBOR_CASE_RGB, 0,
                 0.0f, 0.0f, casew, caseh);
        skin_add(sk, NES_SKIN_BODY, "Logo", NULL, -1, 0, 0xB02A28, 0, offx, 0.5f, 1.2f, 1.0f);
        skin_add(sk, NES_SKIN_BODY, "Plate", "SUBOR", -1, 0, 0x5C5A55, 0xE2DCC8,
                 offx + 1.8f, 0.5f, 4.0f, 1.0f);
    }

    for (i = 0; i < count; i++) {
        int cls, strong;
        uint32_t fill, ink;
        const char *legend = NULL;
        float kx = offx + keys[i].x, ky = offy + keys[i].y, kw = keys[i].w, kh = keys[i].h;
        if (type == NES_KBD_FAMILY_BASIC) {
            /* the second SHIFT in the grid is the right-hand one */
            int occurrence = 0, j;
            const fb_photo_key *photo;
            for (j = 0; j < i; j++)
                if (!strcmp(keys[j].label, keys[i].label)) occurrence++;
            photo = fb_photo_find(keys[i].label, occurrence);
            if (photo) { kx = photo->x; ky = photo->y; kw = photo->w; kh = photo->h; legend = photo->text; }
            strong = fb_is_red(keys[i].label);
            if (fb_is_pale(keys[i].label)) {
                cls = NES_CAP_PALE; fill = NESSESSION_FB_PALE_RGB; ink = NESSESSION_FB_PALE_INK_RGB;
            } else {
                cls = strong ? NES_CAP_RED : NES_CAP_GREY;
                fill = strong ? NESSESSION_FB_RED_RGB : NESSESSION_FB_GREY_RGB;
                ink = strong ? NESSESSION_FB_RED_INK_RGB : NESSESSION_FB_GREY_INK_RGB;
            }
        } else {
            strong = sb_is_dark(keys[i].label) || sb_is_keypad_operator(&keys[i]);
            cls = strong ? NES_CAP_DARK : NES_CAP_CREAM;
            fill = strong ? NESSESSION_SUBOR_DARK_RGB : NESSESSION_SUBOR_CREAM_RGB;
            ink = NESSESSION_SUBOR_INK_RGB;
        }
        skin_add(sk, NES_SKIN_KEYCAP, keys[i].label, legend, keys[i].index, cls, fill, ink,
                 kx, ky, kw, kh);
    }
    sk->w = casew;
    sk->h = caseh;
}

const nes_skin_item *nessession_keyboard_skin_layout(int type, int *n,
                                                     float *width, float *height)
{
    skin *sk;
    if (type != NES_KBD_FAMILY_BASIC && type != NES_KBD_SUBOR) {
        if (n) *n = 0;
        if (width) *width = 0.0f;
        if (height) *height = 0.0f;
        return NULL;
    }
    pthread_mutex_lock(&g_lock);
    if (!g_skin_built[type]) {
        build_skin(&g_skin[type], type);
        g_skin_built[type] = 1;
    }
    sk = &g_skin[type];
    pthread_mutex_unlock(&g_lock);
    if (n) *n = sk->n;
    if (width) *width = sk->w;
    if (height) *height = sk->h;
    return sk->item;
}

int nessession_keyboard_skin_hit(int type, float x, float y)
{
    int n, i;
    const nes_skin_item *it = nessession_keyboard_skin_layout(type, &n, NULL, NULL);
    for (i = n - 1; i >= 0; i--)
        if (it[i].kind == NES_SKIN_KEYCAP && x >= it[i].x && x < it[i].x + it[i].w
            && y >= it[i].y && y < it[i].y + it[i].h)
            return i;
    return -1;
}
