/*
 * The Win32 Controllers window (F9): the two NES-001 controllers and the
 * console's RESET key as the real parts look (the grey shell with the dark
 * face, the black cross, the red B and A, the SELECT / START pills, the small
 * turbo pills of the NES Advantage), an empty socket for a port set to None,
 * all drawn from the session's layout (nessession_controls_layout) so every
 * frontend shows the same parts. Under them: one type selector per port, the
 * gamepads with the player each drives, the Map row, and the expansion-port
 * keyboard (Family BASIC or Subor) drawn from nessession_keyboard_skin_layout.
 *
 * MOUSE: a press on a button or the RESET key holds it for as long as the
 * mouse button is down (the console samples its controllers once per frame, so
 * a value present only for the instant of a click falls between frames); the
 * cross is dragged, the pointer's offset from its centre being the direction
 * (nessession_dpad_from_vector, nessession_pointer_set); a key of the on-screen
 * keyboard is held the same way. The mouse is captured on press; letting go
 * anywhere, losing the capture, hiding, Map being armed or deactivating the
 * window releases whatever the pointer holds.
 *
 * What the console sees held -- keyboard, gamepads and this window together --
 * is polled every 50 ms and drawn as a ring and a darker fill in the held
 * colour, so the window doubles as an input tester.
 *
 * FIXED SIZE: laid out once, in a caption-only tool window with no thick frame
 * and no maximize box, so neither Map mode, a long gamepad name nor a port's
 * controller changing size it.
 *
 * MAP MODE: Map arms a two-step rebind -- click a control (a click on the cross
 * picks the side clicked) to pick the target, then press a host key OR a
 * gamepad button; Esc cancels. While armed the controls show what drives them,
 * the picked target is lit and the rest are drawn at 40%, and typing does not
 * reach the console. Clear takes the picked target's key and button away. "Show
 * bindings" shows the same legends outside Map mode. F9, F11 and F12 are the
 * app's and cannot be bound.
 *
 * The art is drawn at twice the size into a memory bitmap and shrunk with
 * HALFTONE (GDI has no anti-aliasing of its own); the text goes on after at
 * the window's own size.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controller_window.h"

/* GET_X_LPARAM / GET_Y_LPARAM live here, not in windows.h. */
#include <windowsx.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../help/help_compat.h"
#include "../help/help_window.h"
#include "../key_forward.h"
#include "../resource.h"
#include "gdi_art.h"

#define PANEL_CLASS   "FujiNetGoNESControllers"
#define PANEL_STYLE   (WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN)
#define PANEL_EXSTYLE WS_EX_TOOLWINDOW

/* Sizes at 96 dpi; scaled() maps them to the window's. */
#define UNIT96      18      /* pixels per layout unit */
#define MARGIN      12
#define CASE_PAD    10      /* the mat round the art */
#define HEAD_H      22
#define ROW_H       26      /* one gamepad's row */
#define ROW_GAP     3
#define PAD_ROWS    3       /* gamepad rows the window has room for */
#define SEG_AUTO_W  76      /* Automatic */
#define SEG_W       62      /* Player 1, Player 2 */
#define TYPE_H      26      /* a port's type selector */
#define BTN_H       28
#define HINT_H      20
#define KBD_HEAD_H  26
#define MAX_KEYS    192

#define IDT_CAPTURE 1
#define IDT_HELD    2
#define IDT_PADS    3

#define NES_PORTS 2

enum { SB_MAP, SB_CLEAR, SB_DEFAULTS, SB_SHOW, SB_HELP, SB_RESET_CONFIG, SB_STOP, SB_COUNT };

typedef struct {
    unsigned generation;
    int count, assign[PAD_ROWS], effective[PAD_ROWS];
} pads_state;

typedef struct {
    unsigned held[NES_PORTS];       /* nes_action bits the console sees held */
    int reset_held;                 /* the RESET key */
    int view[NES_PORTS];            /* the controller drawn on each port */
    int dragging[NES_PORTS];        /* the pointer's cross drag, as a flag */
    int keyboard;                   /* the expansion keyboard drawn */
    int mode;                       /* keyboard mode */
    unsigned char keys[MAX_KEYS];   /* skin key index held */
} held_state;

static HWND g_panel, g_parent;
static HWND g_combo[NES_PORTS], g_kbd_combo;
static nessession *g_session;
static int g_dpi = 96;

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad button to bind to that target. */
static int g_map_state = -2;
static int g_show_bindings;
static WPARAM g_swallow_vk;     /* the key Map took: its repeats and release too */
static char g_hint[300];
static int g_hover_part = -1;   /* the part under the idle pointer */
static int g_tracking;

/* The layout (static table owned by the core), and where it sits in the client. */
static const nes_control *g_layout;
static int g_numlayout;
static float g_layw, g_layh;
static double g_unit, g_orgx, g_orgy;
static int g_view[NES_PORTS];       /* the controller drawn on each port */

/* What the pointer holds: a pressed part (layout index), a dragged cross, a key. */
static int g_mouse_part = -1;
static int g_mouse_key = -1;        /* skin key index */
static int g_strip_down = -1;       /* strip button under the mouse */

/* The keyboard section. */
static double g_kbd_unit;

/* What was last painted. */
static held_state g_shown;
static pads_state g_pads_shown;
static int g_total_w, g_total_h;

static RECT g_mat_rc, g_gp_rc, g_gp_head_rc, g_gp_row_rc[PAD_ROWS];
static RECT g_btn_rc[SB_COUNT], g_hint_rc;
static RECT g_kbd_head_rc, g_kbd_mode_rc, g_kbd_rc;

static HFONT g_font, g_bold, g_legend, g_tiny, g_micro, g_caption, g_logo, g_keycap, g_keysmall, g_keykana;

static void refresh_held(void);

static int scaled(int val) { return nes_scale(val, g_dpi); }

#define HX(val) gdi_rgb(val)

#define COL_HELD_RING   HX(NESSESSION_HELD_RGB)
#define COL_HELD_FILL   HX(NESSESSION_HELD_FILL_RGB)
#define COL_MAT_TOP     HX(0x7E7B75)    /* the desk: dark enough that the light shells read */
#define COL_MAT_BOT     HX(0x5A5753)
#define COL_MAT_EDGE    HX(0x34322E)
#define COL_SHADOW      HX(0x3E3C38)
#define COL_CAPTION     HX(0xF4F2EA)
#define COL_DIM_TO      HX(0x6A6761)    /* unpicked parts fade toward the mat */

/* An unpicked part in Map mode: 40% of its own colours over the mat's. */
static COLORREF tone(COLORREF col, int dim)
{
    return dim ? gdi_mix(col, COL_DIM_TO, 0.6) : col;
}

/* ---- fonts ------------------------------------------------------------------ */

static void free_fonts(void)
{
    HFONT *const slots[] = { &g_font, &g_bold, &g_legend, &g_tiny, &g_micro, &g_caption, &g_logo, &g_keycap, &g_keysmall, &g_keykana };
    size_t idx;
    for (idx = 0; idx < sizeof slots / sizeof slots[0]; idx++)
        if (*slots[idx]) { DeleteObject(*slots[idx]); *slots[idx] = NULL; }
}

static void make_fonts(void)
{
    int keypx;
    free_fonts();
    g_font = nes_make_font(g_dpi, 0, FW_NORMAL, NULL);
    g_bold = nes_make_font(g_dpi, 0, FW_BOLD, NULL);
    g_legend = gdi_face_font(g_dpi, 9, FW_BOLD, 0);          /* console key and pad legends */
    g_tiny = nes_make_font(g_dpi, 9, FW_NORMAL, NULL);       /* binding legends */
    g_micro = nes_make_font(g_dpi, 8, FW_NORMAL, NULL);      /* the cross's and the pills' legends */
    g_caption = gdi_face_font(g_dpi, 12, FW_BOLD, 0);        /* Player 1 / Player 2 on the mat */
    g_logo = gdi_face_font(g_dpi, 12, FW_BOLD, 1);           /* Nintendo */
    keypx = (int)(g_unit * 1.15 * 96.0 / g_dpi * 0.46);
    if (keypx < 8) keypx = 8;
    if (keypx > 13) keypx = 13;
    g_keycap = gdi_face_font(g_dpi, keypx, FW_BOLD, 0);      /* the keycap legends */
    {
        /* the shifted symbol and the katakana printed small on a Family BASIC cap */
        const int smallpx = keypx > 10 ? 9 : 8;
        g_keysmall = gdi_face_font(g_dpi, smallpx, FW_BOLD, 0);
        g_keykana = CreateFontW(-MulDiv(smallpx, g_dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"MS Gothic");
    }
}

/* ---- art primitives -------------------------------------------------------------- */

/* A dashed rounded outline: the empty socket. */
static void dashed_rrect(HDC hdc, double xpos, double ypos, double width, double height, double radius,
                         COLORREF col, double linew)
{
    LOGBRUSH brush;
    DWORD dashes[2] = { 4, 3 };
    HPEN pen;
    HGDIOBJ oldpen, oldbrush;
    brush.lbStyle = BS_SOLID;
    brush.lbColor = col;
    brush.lbHatch = 0;
    pen = ExtCreatePen(PS_GEOMETRIC | PS_USERSTYLE | PS_ENDCAP_FLAT | PS_JOIN_ROUND,
                       (DWORD)(gdi_len(linew) > 0 ? gdi_len(linew) : 1), &brush, 2, dashes);
    if (!pen) { gdi_rrect(hdc, xpos, ypos, width, height, radius, GDI_NOFILL, col, linew); return; }
    oldpen = SelectObject(hdc, pen);
    oldbrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, gdi_xpos(xpos), gdi_ypos(ypos), gdi_xpos(xpos + width), gdi_ypos(ypos + height),
              gdi_len(radius * 2), gdi_len(radius * 2));
    SelectObject(hdc, oldbrush);
    SelectObject(hdc, oldpen);
    DeleteObject(pen);
}

static void triangle(HDC hdc, double x0, double y0, double x1, double y1, double x2, double y2, COLORREF fill)
{
    POINT pts[3];
    HBRUSH brush = CreateSolidBrush(fill);
    HGDIOBJ oldbrush = SelectObject(hdc, brush);
    HGDIOBJ oldpen = SelectObject(hdc, GetStockObject(NULL_PEN));
    pts[0].x = gdi_xpos(x0); pts[0].y = gdi_ypos(y0);
    pts[1].x = gdi_xpos(x1); pts[1].y = gdi_ypos(y1);
    pts[2].x = gdi_xpos(x2); pts[2].y = gdi_ypos(y2);
    Polygon(hdc, pts, 3);
    SelectObject(hdc, oldpen);
    SelectObject(hdc, oldbrush);
    DeleteObject(brush);
}

/* The pixel rectangle of a layout part. */
static void part_box(const nes_control *ctl, double *xpos, double *ypos, double *width, double *height)
{
    *xpos = g_orgx + (double)ctl->x * g_unit;
    *ypos = g_orgy + (double)ctl->y * g_unit;
    *width = (double)ctl->w * g_unit;
    *height = (double)ctl->h * g_unit;
}

static void part_rect(const nes_control *ctl, RECT *rect)
{
    double xpos, ypos, width, height;
    part_box(ctl, &xpos, &ypos, &width, &height);
    SetRect(rect, (int)floor(xpos), (int)floor(ypos), (int)ceil(xpos + width), (int)ceil(ypos + height));
}

/* ---- what a part shows ---------------------------------------------------------- */

static int binding_mode(void)
{
    return g_map_state != -2 || g_show_bindings;
}

static int pressable(const nes_control *ctl)
{
    return ctl->kind == NES_CTL_DPAD_ARM || ctl->kind == NES_CTL_ROUND_BUTTON || ctl->kind == NES_CTL_PILL ||
           ctl->kind == NES_CTL_BUTTON;
}

/* The port a target belongs to, -1 for the console's. */
static int target_port(int target)
{
    return target >= 0 && target < NES_PORTS * NES_ACT_PER_PORT ? target / NES_ACT_PER_PORT : -1;
}

/* A part shown held: what the console sees. In Map mode the picked target is
 * the one lit instead. */
static int part_lit(const nes_control *ctl)
{
    if (g_map_state != -2) return g_map_state >= 0 && pressable(ctl) && ctl->target == g_map_state;
    if (ctl->target < 0) return 0;
    if (ctl->port >= 0 && target_port(ctl->target) >= 0)
        return (int)((g_shown.held[ctl->port] >> (ctl->target % NES_ACT_PER_PORT)) & 1u);
    return g_shown.reset_held;
}

/* Map mode with a target picked: every other control is drawn at 40%. */
static int part_dim(const nes_control *ctl)
{
    if (g_map_state < 0 || !pressable(ctl)) return 0;
    return ctl->target != g_map_state;
}

/* A device's case in Map mode with a target picked elsewhere. */
static int body_dim(int port)
{
    return g_map_state >= 0 && target_port(g_map_state) != port;
}

/* What drives a target: the key, "-" when unbound. */
static void binding_label(int target, char *out, int outsz)
{
    nes_binding bind;
    out[0] = '\0';
    if (target < 0) return;
    bind = nessession_binding_get(g_session, target);
    if (bind.keysym) {
        nessession_keysym_name(bind.keysym, out, outsz);
        /* the labels are small: "Keypad 5" reads "KP 5" */
        if (strncmp(out, "Keypad ", 7) == 0) {
            memmove(out + 3, out + 7, strlen(out + 7) + 1);
            memcpy(out, "KP ", 3);
        }
    }
    /* "Right Shift" will not fit on a pill: the modifiers and long names shorten */
    {
        static const char *const longer[][2] = {
            { "Left Shift", "L Shift" }, { "Right Shift", "R Shift" }, { "Left Ctrl", "L Ctrl" },
            { "Right Ctrl", "R Ctrl" }, { "Left Alt", "L Alt" }, { "Right Alt", "R Alt" },
            { "Backspace", "Bksp" }, { "Return", "Enter" }, { "Escape", "Esc" }
        };
        size_t idx;
        for (idx = 0; idx < sizeof longer / sizeof longer[0]; idx++)
            if (!strcmp(out, longer[idx][0])) { snprintf(out, (size_t)outsz, "%s", longer[idx][1]); break; }
    }
    if (!out[0]) snprintf(out, (size_t)outsz, "%s", "\xe2\x80\x94");
}

/* ---- the pad ------------------------------------------------------------------ */

static void art_pad_body(HDC hdc, const nes_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = body_dim(ctl->port);
    part_box(ctl, &bx, &by, &bw, &bh);

    if (!strcmp(ctl->label, "NES-001")) {
        gdi_rrect(hdc, bx + u * 0.12, by + u * 0.2, bw, bh, u * 0.6, COL_SHADOW, COL_SHADOW, 0);
        gdi_rrect(hdc, bx, by, bw, bh, u * 0.6, tone(HX(NESSESSION_SHELL_RGB), dim),
                  tone(HX(NESSESSION_SHELL_SHADOW_RGB), dim), 1.2);
    } else if (!strcmp(ctl->label, "Face")) {
        gdi_rrect(hdc, bx, by, bw, bh, u * 0.3, tone(HX(NESSESSION_FACE_RGB), dim), tone(HX(0x2A2A2C), dim), 0.8);
    } else if (!strcmp(ctl->label, "Stripe")) {
        gdi_rrect(hdc, bx, by, bw, bh, u * 0.08, tone(HX(NESSESSION_STRIPE_RGB), dim), tone(HX(NESSESSION_STRIPE_RGB), dim), 0);
    } else if (!strcmp(ctl->label, "DpadWell")) {
        gdi_rrect(hdc, bx, by, bw, bh, u * 0.5, tone(HX(NESSESSION_SHELL_RGB), dim),
                  tone(HX(NESSESSION_SHELL_SHADOW_RGB), dim), 1.0);
    } else if (!strcmp(ctl->label, "SelectWell")) {
        gdi_rrect(hdc, bx, by, bw, bh, u * 0.3, tone(HX(NESSESSION_SHELL_RGB), dim),
                  tone(HX(NESSESSION_SHELL_SHADOW_RGB), dim), 1.0);
    } else if (!strcmp(ctl->label, "ButtonWell")) {
        gdi_rrect(hdc, bx, by, bw, bh, u * 0.45, tone(HX(NESSESSION_SHELL_RGB), dim),
                  tone(HX(NESSESSION_SHELL_SHADOW_RGB), dim), 1.0);
    }
}

/* One arm of the black cross, lit by a held direction; the Right arm (the last
 * one drawn) also puts the dimple at the cross's centre. */
static void art_dpad_arm(HDC hdc, const nes_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = part_dim(ctl), lit = part_lit(ctl);
    COLORREF fill = HX(NESSESSION_DPAD_RGB);
    const COLORREF mark = HX(NESSESSION_DPAD_MARK_RGB);
    part_box(ctl, &bx, &by, &bw, &bh);
    if (lit) fill = gdi_mix(fill, HX(NESSESSION_DPAD_HELD_RGB), 0.55);
    gdi_rrect(hdc, bx, by, bw, bh, u * 0.15, tone(fill, dim), tone(fill, dim), 0);
    if (lit)
        gdi_rrect(hdc, bx, by, bw, bh, u * 0.15, GDI_NOFILL, COL_HELD_RING, u * 0.09);

    /* the embossed triangle near the outer end */
    {
        const COLORREF tri = lit ? HX(0xFFE2B0) : tone(mark, dim);
        const double ctrx = bx + bw / 2, ctry = by + bh / 2, half = u * 0.3, tall = u * 0.4;
        if (!strcmp(ctl->label, "Up"))
            triangle(hdc, ctrx, by + u * 0.2, ctrx - half, by + u * 0.2 + tall, ctrx + half, by + u * 0.2 + tall, tri);
        else if (!strcmp(ctl->label, "Down"))
            triangle(hdc, ctrx, by + bh - u * 0.2, ctrx - half, by + bh - u * 0.2 - tall, ctrx + half,
                     by + bh - u * 0.2 - tall, tri);
        else if (!strcmp(ctl->label, "Left"))
            triangle(hdc, bx + u * 0.2, ctry, bx + u * 0.2 + tall, ctry - half, bx + u * 0.2 + tall, ctry + half, tri);
        else
            triangle(hdc, bx + bw - u * 0.2, ctry, bx + bw - u * 0.2 - tall, ctry - half, bx + bw - u * 0.2 - tall,
                     ctry + half, tri);
        if (!strcmp(ctl->label, "Right"))
            gdi_ellipse(hdc, bx, ctry, u * 0.42, u * 0.42, tone(HX(NESSESSION_DPAD_DIMPLE_RGB), dim),
                        tone(HX(NESSESSION_DPAD_DIMPLE_RGB), dim), 0);
    }
}

/* B and A: a red round button, the highlight at its upper left. */
static void art_round_button(HDC hdc, const nes_control *ctl)
{
    double bx, by, bw, bh, ctrx, ctry;
    const double u = g_unit;
    const int dim = part_dim(ctl), lit = part_lit(ctl), pressed = lit && g_map_state == -2;
    const double rad = (double)ctl->w / 2.0;
    part_box(ctl, &bx, &by, &bw, &bh);
    ctrx = bx + bw / 2;
    ctry = by + bh / 2;
    {
        const COLORREF base = pressed ? COL_HELD_FILL : HX(NESSESSION_BUTTON_RGB);
        gdi_ellipse(hdc, ctrx, ctry, rad * u, rad * u, tone(base, dim), tone(gdi_mix(base, HX(0x000000), 0.35), dim), 1.0);
        if (!pressed)
            gdi_ellipse(hdc, ctrx - u * rad * 0.3, ctry - u * rad * 0.33, u * rad * 0.38, u * rad * 0.28,
                        tone(HX(NESSESSION_BUTTON_LIGHT_RGB), dim), tone(HX(NESSESSION_BUTTON_LIGHT_RGB), dim), 0);
    }
    if (lit)
        gdi_ellipse(hdc, ctrx, ctry, u * (rad + 0.16), u * (rad + 0.16), GDI_NOFILL, COL_HELD_RING, u * 0.1);
}

/* SELECT / START (dark rubber) and the two turbo pills (light, like the wells). */
static void art_pill(HDC hdc, const nes_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = part_dim(ctl), lit = part_lit(ctl), pressed = lit && g_map_state == -2;
    const int turbo = ctl->label[0] == 'T';
    COLORREF fill = turbo ? HX(NESSESSION_SHELL_RGB) : HX(NESSESSION_RUBBER_RGB);
    part_box(ctl, &bx, &by, &bw, &bh);
    if (pressed) fill = turbo ? gdi_mix(fill, COL_HELD_RING, 0.6) : COL_HELD_FILL;
    gdi_rrect(hdc, bx, by, bw, bh, bh / 2, tone(fill, dim),
              tone(turbo ? HX(NESSESSION_SHELL_SHADOW_RGB) : HX(0x151516), dim), 1.0);
    if (lit)
        gdi_rrect(hdc, bx - u * 0.12, by - u * 0.12, bw + u * 0.24, bh + u * 0.24, bh / 2 + u * 0.12, GDI_NOFILL,
                  COL_HELD_RING, u * 0.1);
}

/* An empty port: the socket's dashed outline. */
static void art_socket(HDC hdc, const nes_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = body_dim(ctl->port);
    part_box(ctl, &bx, &by, &bw, &bh);
    gdi_rrect(hdc, bx, by, bw, bh, u * 0.6, tone(HX(0x68655F), dim), tone(HX(0x68655F), dim), 0);
    dashed_rrect(hdc, bx + u * 0.15, by + u * 0.15, bw - u * 0.3, bh - u * 0.3, u * 0.5,
                 tone(HX(NESSESSION_SOCKET_RGB), dim), 0.12);
}

/* ---- the console ----------------------------------------------------------------- */

static void art_console_body(HDC hdc, const nes_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = body_dim(-1);
    part_box(ctl, &bx, &by, &bw, &bh);
    if (!strcmp(ctl->label, "Console")) {
        gdi_rrect(hdc, bx + u * 0.1, by + u * 0.16, bw, bh, u * 0.5, COL_SHADOW, COL_SHADOW, 0);
        gdi_rrect(hdc, bx, by, bw, bh, u * 0.5, tone(HX(NESSESSION_CONSOLE_RGB), dim),
                  tone(HX(NESSESSION_SHELL_SHADOW_RGB), dim), 1.2);
    } else if (!strcmp(ctl->label, "Base")) {
        /* the darker lower band: rounded at the bottom, square where it meets the slab */
        gdi_rrect(hdc, bx, by, bw, bh, u * 0.5, tone(HX(NESSESSION_CONSOLE_BASE_RGB), dim),
                  tone(HX(NESSESSION_SHELL_SHADOW_RGB), dim), 1.2);
        gdi_rrect(hdc, bx + u * 0.04, by - u * 0.04, bw - u * 0.08, u * 0.7, 0, tone(HX(NESSESSION_CONSOLE_BASE_RGB), dim),
                  tone(HX(NESSESSION_CONSOLE_BASE_RGB), dim), 0);
        gdi_line(hdc, bx + u * 0.2, by, bx + bw - u * 0.2, by, tone(HX(0x77777A), dim), 0.08);
    }
}

/* A flat console key (RESET), or POWER dimmed: it is not a target. */
static void art_console_key(HDC hdc, const nes_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int power = ctl->kind == NES_CTL_BODY;
    const int dim = power ? body_dim(-1) : part_dim(ctl);
    const int lit = !power && part_lit(ctl), pressed = lit && g_map_state == -2;
    COLORREF fill = HX(NESSESSION_KEY_RGB);
    part_box(ctl, &bx, &by, &bw, &bh);
    if (power) fill = gdi_mix(fill, HX(NESSESSION_CONSOLE_BASE_RGB), 0.5);
    if (pressed) fill = gdi_mix(fill, COL_HELD_RING, 0.4);
    gdi_rrect(hdc, bx, by, bw, bh, u * 0.15, tone(fill, dim), tone(HX(0x55555A), dim), 1.0);
    if (!pressed)
        gdi_line(hdc, bx + u * 0.15, by + u * 0.1, bx + bw - u * 0.15, by + u * 0.1, tone(HX(0xB4B4B8), dim), 0.08);
    if (lit)
        gdi_rrect(hdc, bx - u * 0.12, by - u * 0.12, bw + u * 0.24, bh + u * 0.24, u * 0.25, GDI_NOFILL, COL_HELD_RING,
                  u * 0.1);
}

/* The mat and every device in the layout, in layout order (so a body is drawn
 * before the parts on it). */
static void art_controls(HDC hdc)
{
    int idx;
    gdi_grad_rrect(hdc, g_mat_rc.left, g_mat_rc.top, g_mat_rc.right - g_mat_rc.left, g_mat_rc.bottom - g_mat_rc.top,
                   scaled(14), COL_MAT_TOP, COL_MAT_BOT, COL_MAT_EDGE, 1.5);
    for (idx = 0; idx < g_numlayout; idx++) {
        const nes_control *ctl = &g_layout[idx];
        switch (ctl->kind) {
        case NES_CTL_BODY:
            if (!strcmp(ctl->label, "Socket")) art_socket(hdc, ctl);
            else if (!strcmp(ctl->label, "POWER")) art_console_key(hdc, ctl);
            else if (ctl->port < 0) art_console_body(hdc, ctl);
            else art_pad_body(hdc, ctl);
            break;
        case NES_CTL_DPAD_ARM: art_dpad_arm(hdc, ctl); break;
        case NES_CTL_ROUND_BUTTON: art_round_button(hdc, ctl); break;
        case NES_CTL_PILL: art_pill(hdc, ctl); break;
        case NES_CTL_BUTTON: art_console_key(hdc, ctl); break;
        default: break;
        }
    }
}

/* ---- the keyboard ----------------------------------------------------------------- */

static void kbd_origin(float width, double *orgx)
{
    *orgx = g_kbd_rc.left + ((g_kbd_rc.right - g_kbd_rc.left) - (double)width * g_kbd_unit) / 2.0;
}

static void art_keyboard(HDC hdc)
{
    const int type = nessession_keyboard(g_session);
    int num = 0, idx;
    float width = 0, height = 0;
    const nes_skin_item *items = nessession_keyboard_skin_layout(type, &num, &width, &height);
    double orgx;
    const double u = g_kbd_unit;

    if (!items || num == 0) {
        gdi_rrect(hdc, g_kbd_rc.left, g_kbd_rc.top, g_kbd_rc.right - g_kbd_rc.left, g_kbd_rc.bottom - g_kbd_rc.top,
                  scaled(8), gdi_mix(GetSysColor(COLOR_BTNFACE), 0x000000, 0.06), GetSysColor(COLOR_BTNSHADOW), 1.0);
        return;
    }
    kbd_origin(width, &orgx);
    for (idx = 0; idx < num; idx++) {
        const nes_skin_item *item = &items[idx];
        const double bx = orgx + (double)item->x * u, by = g_kbd_rc.top + (double)item->y * u;
        const double bw = (double)item->w * u, bh = (double)item->h * u;
        if (item->kind == NES_SKIN_BODY) {
            if (!strcmp(item->label, "Case")) {
                gdi_rrect(hdc, bx + u * 0.1, by + u * 0.15, bw, bh, u * 0.6, COL_SHADOW, COL_SHADOW, 0);
                gdi_rrect(hdc, bx, by, bw, bh, u * 0.6, HX(item->fill_rgb), gdi_mix(HX(item->fill_rgb), 0x000000, 0.35), 1.2);
            } else if (!strcmp(item->label, "Vent")) {
                /* a group of thin vertical slits */
                const int slits = 14;
                int slit;
                for (slit = 0; slit < slits; slit++) {
                    const double sx = bx + (bw - u * 0.1) * slit / (slits - 1);
                    gdi_rrect(hdc, sx, by, u * 0.1, bh, u * 0.04, HX(item->fill_rgb), HX(item->fill_rgb), 0);
                }
            } else if (!strcmp(item->label, "Well")) {
                gdi_rrect(hdc, bx, by, bw, bh, u * 0.2, HX(item->fill_rgb), HX(item->ink_rgb), 0.04);
            } else if (!strcmp(item->label, "Logo")) {
                /* the Subor's red mark: a rounded square with the fuji peak cut in */
                gdi_rrect(hdc, bx, by, bw, bh, u * 0.2, HX(item->fill_rgb), HX(item->fill_rgb), 0);
                triangle(hdc, bx + bw / 2, by + bh * 0.2, bx + bw * 0.2, by + bh * 0.8, bx + bw * 0.8, by + bh * 0.8,
                         HX(NESSESSION_SUBOR_CASE_RGB));
            } else {   /* Slot, Name, Plate */
                gdi_rrect(hdc, bx, by, bw, bh, u * 0.15, HX(item->fill_rgb), gdi_mix(HX(item->fill_rgb), 0x000000, 0.3), 1.0);
            }
        } else {
            const int lit = item->key >= 0 && item->key < MAX_KEYS && g_shown.keys[item->key];
            COLORREF fill = HX(item->fill_rgb);
            const double inset = u * 0.04;
            const double kx = bx + inset, ky = by + inset, kw = bw - inset * 2, kh = bh - inset * 2;
            if (lit) fill = gdi_mix(fill, COL_HELD_RING, 0.55);
            /* a bevelled cap: dark body, lighter top face set back from the lower edge */
            gdi_rrect(hdc, kx, ky + u * 0.06, kw, kh, u * 0.12, gdi_mix(fill, 0x000000, 0.2), gdi_mix(fill, 0x000000, 0.4), 1.0);
            gdi_rrect(hdc, kx + u * 0.06, ky, kw - u * 0.12, kh - u * 0.16, u * 0.1, gdi_mix(fill, 0xFFFFFF, lit ? 0.0 : 0.1),
                      gdi_mix(fill, 0x000000, 0.12), 0.5);
            if (lit)
                gdi_rrect(hdc, kx, ky, kw, kh, u * 0.12, GDI_NOFILL, COL_HELD_RING, u * 0.1);
        }
    }
}

static void text_keyboard(HDC hdc)
{
    const int type = nessession_keyboard(g_session);
    int num = 0, idx;
    float width = 0, height = 0;
    const nes_skin_item *items = nessession_keyboard_skin_layout(type, &num, &width, &height);
    double orgx;
    const double u = g_kbd_unit;

    if (!items || num == 0) {
        RECT rect = g_kbd_rc;
        SelectObject(hdc, g_font);
        SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
        gdi_text_line(hdc, "No keyboard on the expansion port. Choose the Family BASIC or Subor keyboard above.",
                      &rect, DT_CENTER);
        return;
    }
    kbd_origin(width, &orgx);
    for (idx = 0; idx < num; idx++) {
        const nes_skin_item *item = &items[idx];
        RECT rect;
        SetRect(&rect, (int)floor(orgx + (double)item->x * u), (int)floor(g_kbd_rc.top + (double)item->y * u),
                (int)ceil(orgx + (double)(item->x + item->w) * u), (int)ceil(g_kbd_rc.top + (double)(item->y + item->h) * u));
        if (item->kind == NES_SKIN_BODY) {
            if (item->text) {
                SelectObject(hdc, g_legend);
                SetTextColor(hdc, HX(item->ink_rgb));
                gdi_text_line(hdc, item->text, &rect, DT_CENTER);
            }
        } else {
            const int lit = item->key >= 0 && item->key < MAX_KEYS && g_shown.keys[item->key];
            const COLORREF ink = lit ? RGB(255, 255, 255) : HX(item->ink_rgb);
            RECT face = rect;
            char shifted[24] = "", kana[24] = "";
            const char *bar = item->text ? strchr(item->text, '|') : NULL;
            face.bottom -= (int)(u * 0.24);
            if (bar) {
                snprintf(shifted, sizeof shifted, "%.*s", (int)(bar - item->text) < 23 ? (int)(bar - item->text) : 23, item->text);
                snprintf(kana, sizeof kana, "%s", bar + 1);
            }
            SetTextColor(hdc, ink);
            if (shifted[0] || kana[0]) {
                /* the main legend left and low; the shifted symbol top right, the katakana bottom right */
                RECT main = face, corner = face;
                main.left += (int)(u * 0.2);
                main.right -= (int)(u * 0.3);
                SelectObject(hdc, g_keycap);
                gdi_text_out(hdc, item->label, &main, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
                corner.right -= (int)(u * 0.08);
                corner.left = corner.right - (int)(u * 0.5);
                if (shifted[0]) {
                    RECT top = corner;
                    top.bottom = face.top + (face.bottom - face.top) / 2;
                    SelectObject(hdc, g_keysmall);
                    gdi_text_out(hdc, shifted, &top, DT_RIGHT | DT_TOP | DT_SINGLELINE);
                }
                if (kana[0]) {
                    RECT low = corner;
                    low.top = face.top + (face.bottom - face.top) / 2;
                    SelectObject(hdc, g_keykana);
                    gdi_text_out(hdc, kana, &low, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE);
                }
            } else if (item->cap_class == NES_CAP_PALE && strchr(item->label, ' ')) {
                /* CLR HOME on two lines */
                char first[24];
                const char *space = strchr(item->label, ' ');
                RECT upper = face, lower = face;
                snprintf(first, sizeof first, "%.*s", (int)(space - item->label), item->label);
                upper.bottom = (face.top + face.bottom) / 2 + 1;
                lower.top = upper.bottom - 1;
                SelectObject(hdc, g_keysmall);
                gdi_text_line(hdc, first, &upper, DT_CENTER);
                gdi_text_line(hdc, space + 1, &lower, DT_CENTER);
            } else {
                SelectObject(hdc, g_keycap);
                gdi_text_line(hdc, item->label, &face, DT_CENTER);
            }
        }
    }
}

/* ---- the text over the art ------------------------------------------------------------- */

static void text_parts(HDC hdc)
{
    int idx;
    SetBkMode(hdc, TRANSPARENT);
    for (idx = 0; idx < g_numlayout; idx++) {
        const nes_control *ctl = &g_layout[idx];
        RECT rect;
        const double u = g_unit;
        part_rect(ctl, &rect);
        switch (ctl->kind) {
        case NES_CTL_CAPTION:
            if (ctl->port >= 0 && ctl->device < 0) {          /* Player 1 / Player 2 */
                SelectObject(hdc, g_caption);
                SetTextColor(hdc, COL_CAPTION);
                gdi_text_line(hdc, ctl->label, &rect, DT_CENTER);
            } else if (ctl->port < 0) {                       /* the console's name */
                SelectObject(hdc, g_legend);
                SetTextColor(hdc, tone(HX(NESSESSION_LEGEND_RGB), body_dim(-1)));
                gdi_text_line(hdc, ctl->label, &rect, DT_LEFT);
            } else if (ctl->device == NES_CTRL_NONE) {
                SelectObject(hdc, g_font);
                SetTextColor(hdc, tone(HX(0xC8C6C0), body_dim(ctl->port)));
                gdi_text_line(hdc, ctl->label, &rect, DT_CENTER);
            } else if (!strcmp(ctl->label, "Nintendo")) {
                SelectObject(hdc, g_logo);
                SetTextColor(hdc, tone(HX(NESSESSION_LEGEND_RGB), body_dim(ctl->port)));
                InflateRect(&rect, (int)(u * 0.3), 0);
                gdi_text_line(hdc, ctl->label, &rect, DT_CENTER);
            } else {                                          /* SELECT START B A T */
                if (binding_mode() && (!strcmp(ctl->label, "SELECT") || !strcmp(ctl->label, "START"))) {
                    /* the pill is too small for a key name: it goes where the red legend is */
                    const int target = NES_TARGET_PORT(ctl->port, ctl->label[1] == 'E' ? NES_ACT_SELECT : NES_ACT_START);
                    char legend[96];
                    InflateRect(&rect, (int)(u * 0.15), 0);
                    binding_label(target, legend, sizeof legend);
                    SelectObject(hdc, g_micro);
                    SetTextColor(hdc, g_map_state == target ? COL_HELD_RING
                                      : tone(RGB(0xFF, 0xFF, 0xFF), g_map_state >= 0 && g_map_state != target));
                    gdi_text_line(hdc, legend, &rect, DT_CENTER | DT_END_ELLIPSIS);
                    break;
                }
                SelectObject(hdc, g_legend);
                SetTextColor(hdc, tone(HX(NESSESSION_LEGEND_RGB), body_dim(ctl->port)));
                InflateRect(&rect, (int)(u * 0.2), 0);
                gdi_text_line(hdc, ctl->label, &rect, DT_CENTER);
            }
            break;
        case NES_CTL_BODY:
            if (!strcmp(ctl->label, "POWER")) {
                SelectObject(hdc, g_legend);
                SetTextColor(hdc, tone(gdi_mix(HX(NESSESSION_KEY_INK_RGB), HX(NESSESSION_KEY_RGB), 0.4), body_dim(-1)));
                gdi_text_line(hdc, ctl->label, &rect, DT_CENTER);
            }
            break;
        case NES_CTL_DPAD_ARM:
        case NES_CTL_ROUND_BUTTON:
        case NES_CTL_PILL:
            if (binding_mode() && ctl->target >= 0 &&
                !(ctl->kind == NES_CTL_PILL && ctl->label[0] != 'T')) {   /* SELECT / START: above the well */
                char legend[96];
                RECT box = rect;
                const int dim = part_dim(ctl);
                const int turbo = ctl->kind == NES_CTL_PILL;
                if (ctl->kind == NES_CTL_PILL) InflateRect(&box, (int)(u * 0.1), (int)(u * 0.2));
                else if (ctl->kind == NES_CTL_ROUND_BUTTON) InflateRect(&box, (int)(u * 0.15), 0);
                binding_label(ctl->target, legend, sizeof legend);
                SelectObject(hdc, ctl->kind == NES_CTL_ROUND_BUTTON ? g_tiny : g_micro);
                SetTextColor(hdc, g_map_state == ctl->target ? COL_HELD_RING
                                  : tone(turbo ? HX(0x2A2A2C) : RGB(0xFF, 0xFF, 0xFF), dim));
                gdi_text_line(hdc, legend, &box, DT_CENTER | DT_END_ELLIPSIS);
            }
            break;
        case NES_CTL_BUTTON: {
            const int lit = part_lit(ctl), dim = part_dim(ctl);
            if (binding_mode()) {
                char legend[96];
                binding_label(ctl->target, legend, sizeof legend);
                SelectObject(hdc, g_tiny);
                SetTextColor(hdc, g_map_state == ctl->target ? COL_HELD_RING : tone(HX(NESSESSION_KEY_INK_RGB), dim));
                gdi_text_line(hdc, legend, &rect, DT_CENTER | DT_END_ELLIPSIS);
            } else {
                SelectObject(hdc, g_legend);
                SetTextColor(hdc, lit ? RGB(255, 255, 255) : tone(HX(NESSESSION_KEY_INK_RGB), dim));
                gdi_text_line(hdc, ctl->label, &rect, DT_CENTER);
            }
            break;
        }
        default:
            break;
        }
    }
}

/* ---- the strips: gamepads, Map, hint, keyboard heading ------------------------------------ */

static void draw_face(HDC hdc, const RECT *bounds, int pushed, int accent, int inactive)
{
    RECT rect = *bounds;
    if (accent) {
        HBRUSH back = CreateSolidBrush(HX(NESSESSION_ACCENT_RGB));
        HBRUSH edge = CreateSolidBrush(gdi_mix(HX(NESSESSION_ACCENT_RGB), 0x000000, 0.4));
        FillRect(hdc, &rect, back);
        FrameRect(hdc, &rect, edge);
        DeleteObject(back);
        DeleteObject(edge);
        SetTextColor(hdc, RGB(255, 255, 255));
    } else {
        DrawFrameControl(hdc, &rect, DFC_BUTTON,
                         DFCS_BUTTONPUSH | (pushed ? DFCS_PUSHED : 0) | (inactive ? DFCS_INACTIVE : 0));
        SetTextColor(hdc, GetSysColor(inactive ? COLOR_GRAYTEXT : COLOR_BTNTEXT));
    }
}

static const char *strip_label(int btn)
{
    switch (btn) {
    case SB_MAP: return g_map_state == -2 ? "Map" : "Done";
    case SB_CLEAR: return "Clear";
    case SB_DEFAULTS: return "Defaults";
    case SB_SHOW: return "Show bindings";
    case SB_RESET_CONFIG: return "Reset to CONFIG";
    case SB_STOP: return "Stop in debugger";
    default: return "Help";
    }
}

/* The choice at the right of a gamepad's row: segment 0 is Automatic, n the
 * player n. */
static void segment_rect(const RECT *row, int seg, RECT *rect)
{
    const int gap = scaled(4);
    const int total = scaled(SEG_AUTO_W) + NES_PORTS * scaled(SEG_W) + NES_PORTS * gap;
    int left = row->right - total, idx;
    for (idx = 0; idx < seg; idx++)
        left += (idx == 0 ? scaled(SEG_AUTO_W) : scaled(SEG_W)) + gap;
    SetRect(rect, left, row->top, left + (seg == 0 ? scaled(SEG_AUTO_W) : scaled(SEG_W)), row->bottom - 1);
}

/* One line per gamepad: its name and the player it drives now, then the
 * Automatic / Player 1 / Player 2 choice, the current one in the accent. */
static void paint_gamepads(HDC hdc)
{
    static const char *const labels[NES_PORTS + 1] = { "Automatic", "Player 1", "Player 2" };
    const int num = nessession_gamepad_count(g_session);
    int idx, seg;

    SelectObject(hdc, g_bold);
    SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
    gdi_text_line(hdc, "Gamepads", &g_gp_head_rc, DT_LEFT);
    SelectObject(hdc, g_font);
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    if (num == 0) {
        gdi_text_line(hdc, "No gamepads connected - plug one in", &g_gp_row_rc[0], DT_LEFT);
        return;
    }
    if (num > PAD_ROWS) {
        char more[48];
        snprintf(more, sizeof more, "%d more not shown", num - PAD_ROWS);
        gdi_text_line(hdc, more, &g_gp_head_rc, DT_RIGHT);
    }
    for (idx = 0; idx < num && idx < PAD_ROWS; idx++) {
        const int assign = nessession_gamepad_assignment(g_session, idx);
        const int effective = nessession_gamepad_effective_port(g_session, idx);
        const int chosen = assign < 0 || assign >= NES_PORTS ? 0 : assign + 1;
        char name[128], line[200];
        RECT name_rc, first;

        segment_rect(&g_gp_row_rc[idx], 0, &first);
        name_rc = g_gp_row_rc[idx];
        name_rc.right = first.left - scaled(6);
        nessession_gamepad_name(g_session, idx, name, sizeof name);
        if (effective >= 0) snprintf(line, sizeof line, "%s  (player %d)", name, effective + 1);
        else snprintf(line, sizeof line, "%s  (no player)", name);
        SelectObject(hdc, g_font);
        SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
        gdi_text_line(hdc, line, &name_rc, DT_LEFT | DT_END_ELLIPSIS);
        for (seg = 0; seg <= NES_PORTS; seg++) {
            RECT rect;
            segment_rect(&g_gp_row_rc[idx], seg, &rect);
            draw_face(hdc, &rect, 0, seg == chosen, 0);
            gdi_text_line(hdc, labels[seg], &rect, DT_CENTER);
        }
    }
}

/* What the hint line says about the part under an idle pointer. */
static void hover_text(int part, char *out, int outsz)
{
    const nes_control *ctl;
    char key[64];
    nes_binding bind;
    out[0] = '\0';
    if (part < 0 || part >= g_numlayout) return;
    ctl = &g_layout[part];
    if (ctl->kind == NES_CTL_DPAD) {
        snprintf(out, (size_t)outsz, "Player %d cross: drag it to press a direction", ctl->port + 1);
        return;
    }
    if (ctl->target < 0) return;
    bind = nessession_binding_get(g_session, ctl->target);
    key[0] = '\0';
    if (bind.keysym) nessession_keysym_name(bind.keysym, key, sizeof key);
    snprintf(out, (size_t)outsz, "%s   Key: %s   Gamepad: %s", nes_target_name(ctl->target), key[0] ? key : "none",
             bind.button != NES_PAD_BTN_NONE ? nes_pad_button_name(bind.button) : "none");
}

static void paint_strip(HDC hdc)
{
    int btn;
    char hover[200];
    SelectObject(hdc, g_font);
    for (btn = 0; btn < SB_COUNT; btn++) {
        const int armed = (btn == SB_MAP && g_map_state != -2) || (btn == SB_SHOW && g_show_bindings);
        const int inactive = btn == SB_CLEAR && g_map_state < 0;
        draw_face(hdc, &g_btn_rc[btn], btn == g_strip_down && !inactive, armed, inactive);
        gdi_text_line(hdc, strip_label(btn), &g_btn_rc[btn], DT_CENTER);
    }
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    hover_text(g_map_state >= 0 ? -1 : g_hover_part, hover, sizeof hover);
    gdi_text_line(hdc, hover[0] ? hover : g_hint, &g_hint_rc, DT_LEFT | DT_END_ELLIPSIS);
}

static void paint_keyboard_head(HDC hdc)
{
    char line[160];
    SelectObject(hdc, g_bold);
    SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
    gdi_text_line(hdc, "Keyboard", &g_kbd_head_rc, DT_LEFT);
    SelectObject(hdc, g_font);
    if (nessession_keyboard(g_session) != NES_KBD_NONE) {
        snprintf(line, sizeof line, "%s (Scroll Lock)",
                 nessession_keyboard_mode(g_session) ? "Typing goes to the keyboard" : "Keys drive the controllers");
        SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
        gdi_text_line(hdc, line, &g_kbd_mode_rc, DT_LEFT | DT_END_ELLIPSIS);
    }
}

/* The art of one area of the window is drawn by one of these. */
static void paint_controls_art(HDC hdc) { art_controls(hdc); }
static void paint_keyboard_art(HDC hdc) { art_keyboard(hdc); }

/* The whole window: the controllers' art supersampled, their text, the
 * strips, the keyboard. Only what the update region needs is drawn. */
static void paint_rect(HDC hdc, const RECT *inval)
{
    RECT art;
    FillRect(hdc, inval, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(hdc, TRANSPARENT);

    IntersectRect(&art, inval, &g_mat_rc);
    if (!IsRectEmpty(&art)) {
        gdi_paint_art(hdc, &art, GetSysColor(COLOR_BTNFACE), paint_controls_art);
        SelectObject(hdc, g_font);
        text_parts(hdc);
    }
    IntersectRect(&art, inval, &g_kbd_rc);
    if (!IsRectEmpty(&art)) {
        gdi_paint_art(hdc, &art, GetSysColor(COLOR_BTNFACE), paint_keyboard_art);
        SelectObject(hdc, g_font);
        text_keyboard(hdc);
    }
    SelectObject(hdc, g_font);
    paint_gamepads(hdc);
    paint_strip(hdc);
    paint_keyboard_head(hdc);
}

/* ---- pressing ------------------------------------------------------------------- */

static void invalidate_art(void)
{
    if (g_panel) InvalidateRect(g_panel, &g_mat_rc, FALSE);
}

static void invalidate_keyboard(void)
{
    if (g_panel) InvalidateRect(g_panel, &g_kbd_rc, FALSE);
}

/* Lets go of what the pointer holds in this window (only that: the main
 * window releases its own keys). */
static void mouse_release(void)
{
    const int had_key = g_mouse_key >= 0;
    if (had_key && g_session) nessession_keyboard_press(g_session, g_mouse_key, 0);
    g_mouse_part = -1;
    g_mouse_key = -1;
    if (g_session) nessession_release_pointer(g_session);
    if (g_panel) {
        refresh_held();
        invalidate_art();
        if (had_key) invalidate_keyboard();
    }
}

static void set_hint(const char *text)
{
    snprintf(g_hint, sizeof g_hint, "%s", text);
    g_hover_part = -1;
    if (g_panel) InvalidateRect(g_panel, &g_hint_rc, FALSE);
}

#define IDLE_HINT "Press a button or key to hold it; drag the cross; Map rebinds."

static void set_map_state(int state)
{
    g_map_state = state;
    if (state != -2) {
        mouse_release();                    /* a held part must not outlive Map being armed */
        nessession_release_all(g_session);  /* Map swallows key-ups, so drop keyboard holds too */
    }
    if (state < 0) {
        if (g_panel) KillTimer(g_panel, IDT_CAPTURE);
        nessession_gamepad_capture_cancel(g_session);
    }
    if (state == -2) {
        set_hint(IDLE_HINT);
    } else if (state == -1) {
        set_hint("Click a button, a key or a side of the cross to remap it. Esc finishes.");
    } else {
        char msg[200];
        snprintf(msg, sizeof msg, "Press a key or a gamepad button for %s. Esc cancels.", nes_target_name(state));
        set_hint(msg);
        nessession_gamepad_capture_begin(g_session);
        if (g_panel) SetTimer(g_panel, IDT_CAPTURE, 50, NULL);
    }
    if (g_panel) {
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

/* After a rebind: what it is now, and whom it was taken from. */
static void bound_hint(int target, const char *what, const char *was)
{
    char msg[300], name[96];
    snprintf(name, sizeof name, "%s", nes_target_name(target));   /* a static buffer */
    if (was && was[0]) snprintf(msg, sizeof msg, "Bound %s to %s (was %s)", name, what, was);
    else snprintf(msg, sizeof msg, "Bound %s to %s", name, what);
    set_hint(msg);
}

/* ---- layout --------------------------------------------------------------------- */

static void fetch_layout(void)
{
    int port;
    for (port = 0; port < NES_PORTS; port++) g_view[port] = nessession_port_type(g_session, port);
    g_layout = nessession_controls_layout(g_view[0], g_view[1], &g_numlayout, &g_layw, &g_layh);
}

/* The widest and tallest keyboard skin, in key units: one scale for both, so
 * the window never resizes when another keyboard is plugged in. */
static double g_kbd_avail;      /* the width the keyboard section has, in pixels */

/* The key unit that fits the plugged-in keyboard's case to the section's width. */
static void update_kbd_unit(int type)
{
    int num = 0;
    float width = 0, height = 0;
    g_kbd_unit = g_unit;
    if (nessession_keyboard_skin_layout(type, &num, &width, &height) && width > 0)
        g_kbd_unit = g_kbd_avail / (double)width;
    if (g_kbd_unit > g_unit * 1.6) g_kbd_unit = g_unit * 1.6;
}

/* The section is as tall as the tallest case at its own fitted scale, so the
 * window never resizes when another keyboard is plugged in. */
static double fetch_keyboard_extent(void)
{
    int type;
    double tallest = g_unit;
    for (type = NES_KBD_NONE + 1; type < NES_KBD_COUNT; type++) {
        int num = 0;
        float width = 0, height = 0;
        nessession_keyboard_skin_layout(type, &num, &width, &height);
        if (width > 0) {
            double unit = g_kbd_avail / (double)width;
            if (unit > g_unit * 1.6) unit = g_unit * 1.6;
            if (unit * height > tallest) tallest = unit * height;
        }
    }
    return tallest;
}

/* The mat with the controllers in it, the type selectors, the gamepad rows,
 * the Map rows and the keyboard: the window is this size at this DPI, always. */
static void layout_static(void)
{
    static const int widths[SB_COUNT] = { 56, 56, 76, 108, 56, 120, 124 };
    const int left = scaled(MARGIN);
    int ypos = scaled(MARGIN), idx, xpos, content_w, port, kbd_region_h;

    fetch_layout();
    g_unit = (double)UNIT96 * g_dpi / 96.0;
    content_w = (int)ceil((double)g_layw * g_unit) + 2 * scaled(CASE_PAD);
    g_total_w = 2 * left + content_w;
    g_kbd_avail = content_w;
    update_kbd_unit(nessession_keyboard(g_session));
    kbd_region_h = (int)ceil(fetch_keyboard_extent());

    g_orgx = left + scaled(CASE_PAD);
    g_orgy = ypos + scaled(CASE_PAD);
    SetRect(&g_mat_rc, left, ypos, left + content_w, (int)ceil(g_orgy + (double)g_layh * g_unit) + scaled(CASE_PAD));
    ypos = g_mat_rc.bottom + scaled(8);

    /* a type selector under each pad, as wide as its view */
    for (port = 0; port < NES_PORTS; port++) {
        if (!g_combo[port]) continue;
        MoveWindow(g_combo[port], (int)(g_orgx + port * 17.0 * g_unit), ypos, (int)(16.0 * g_unit),
                   scaled(TYPE_H) + scaled(220), TRUE);
        SendMessageA(g_combo[port], WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageA(g_combo[port], CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)scaled(TYPE_H - 6));
    }
    ypos += scaled(TYPE_H) + scaled(10);

    SetRect(&g_gp_head_rc, left, ypos, left + content_w, ypos + scaled(HEAD_H));
    ypos += scaled(HEAD_H);
    for (idx = 0; idx < PAD_ROWS; idx++) {
        SetRect(&g_gp_row_rc[idx], left, ypos, left + content_w, ypos + scaled(ROW_H));
        ypos += scaled(ROW_H + ROW_GAP);
    }
    SetRect(&g_gp_rc, left, g_gp_head_rc.top, left + content_w, ypos);
    ypos += scaled(6);

    xpos = left;
    for (idx = 0; idx < SB_HELP; idx++) {
        SetRect(&g_btn_rc[idx], xpos, ypos, xpos + scaled(widths[idx]), ypos + scaled(BTN_H));
        xpos += scaled(widths[idx]) + scaled(6);
    }
    SetRect(&g_btn_rc[SB_HELP], left + content_w - scaled(widths[SB_HELP]), ypos, left + content_w, ypos + scaled(BTN_H));
    ypos += scaled(BTN_H) + scaled(6);
    xpos = left;
    for (idx = SB_RESET_CONFIG; idx < SB_COUNT; idx++) {
        SetRect(&g_btn_rc[idx], xpos, ypos, xpos + scaled(widths[idx]), ypos + scaled(BTN_H));
        xpos += scaled(widths[idx]) + scaled(6);
    }
    ypos += scaled(BTN_H) + scaled(6);
    SetRect(&g_hint_rc, left, ypos, left + content_w, ypos + scaled(HINT_H));
    ypos += scaled(HINT_H) + scaled(10);

    /* the keyboard: heading row with the expansion-port selector, then the case */
    SetRect(&g_kbd_head_rc, left, ypos, left + scaled(80), ypos + scaled(KBD_HEAD_H));
    if (g_kbd_combo) {
        MoveWindow(g_kbd_combo, left + scaled(84), ypos, scaled(200), scaled(KBD_HEAD_H) + scaled(160), TRUE);
        SendMessageA(g_kbd_combo, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageA(g_kbd_combo, CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)scaled(TYPE_H - 6));
    }
    SetRect(&g_kbd_mode_rc, left + scaled(84 + 208), ypos, left + content_w, ypos + scaled(KBD_HEAD_H));
    ypos += scaled(KBD_HEAD_H) + scaled(6);
    SetRect(&g_kbd_rc, left, ypos, left + content_w, ypos + kbd_region_h);
    g_total_h = g_kbd_rc.bottom + scaled(MARGIN);
}

/* "Standard" / "None" for each port. */
static void sync_combos(void)
{
    int port;
    for (port = 0; port < NES_PORTS; port++) {
        const int sel = nessession_port_type(g_session, port);
        if (g_combo[port] && (int)SendMessageA(g_combo[port], CB_GETCURSEL, 0, 0) != sel)
            SendMessageA(g_combo[port], CB_SETCURSEL, (WPARAM)sel, 0);
    }
    if (g_kbd_combo) {
        const int kbd = nessession_keyboard(g_session);
        if ((int)SendMessageA(g_kbd_combo, CB_GETCURSEL, 0, 0) != kbd)
            SendMessageA(g_kbd_combo, CB_SETCURSEL, (WPARAM)kbd, 0);
    }
}

static void rebuild(void)
{
    mouse_release();
    if (g_panel && GetCapture() == g_panel) ReleaseCapture();
    layout_static();
    make_fonts();
    memset(&g_shown, 0, sizeof g_shown);
    if (g_panel) {
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

/* ---- polling -------------------------------------------------------------------- */

/* What the console sees held, the controller on each port, the keyboard:
 * redrawn when any of it changed. */
static void refresh_held(void)
{
    held_state now;
    int port, idx, count = 0;
    if (!g_panel || !g_session) return;
    memset(&now, 0, sizeof now);
    for (port = 0; port < NES_PORTS; port++) {
        now.held[port] = nessession_buttons_held(g_session, port);
        now.view[port] = nessession_port_type(g_session, port);
    }
    now.reset_held = nessession_target_held(g_session, NES_TARGET_SWITCH(NES_SW_RESET));
    if (g_mouse_part >= 0 && g_mouse_part < g_numlayout && g_layout[g_mouse_part].kind == NES_CTL_DPAD)
        now.dragging[g_layout[g_mouse_part].port] = 1;
    now.keyboard = nessession_keyboard(g_session);
    now.mode = nessession_keyboard_mode(g_session);
    if (now.keyboard != NES_KBD_NONE) {
        int num = 0;
        const nes_skin_item *items = nessession_keyboard_skin_layout(now.keyboard, &num, NULL, NULL);
        for (idx = 0; items && idx < num; idx++)
            if (items[idx].kind == NES_SKIN_KEYCAP && items[idx].key + 1 > count) count = items[idx].key + 1;
    }
    if (count > MAX_KEYS) count = MAX_KEYS;
    for (idx = 0; idx < count; idx++) now.keys[idx] = (unsigned char)(nessession_keyboard_held(g_session, idx) != 0);

    if (memcmp(&now, &g_shown, sizeof now) != 0) {
        const int views_changed = now.view[0] != g_shown.view[0] || now.view[1] != g_shown.view[1];
        const int kbd_changed = memcmp(now.keys, g_shown.keys, sizeof now.keys) != 0 || now.keyboard != g_shown.keyboard;
        const int head_changed = now.mode != g_shown.mode || now.keyboard != g_shown.keyboard;
        if (now.keyboard != g_shown.keyboard) update_kbd_unit(now.keyboard);
        g_shown = now;
        if (views_changed) {
            /* a port's controller changed under the pointer: whatever it held is gone with it */
            if (g_mouse_part >= 0) {
                g_mouse_part = -1;
                nessession_release_pointer(g_session);
                if (GetCapture() == g_panel && g_mouse_key < 0) ReleaseCapture();
            }
            fetch_layout();
        }
        sync_combos();
        invalidate_art();
        if (kbd_changed) invalidate_keyboard();
        if (head_changed) {
            InvalidateRect(g_panel, &g_kbd_head_rc, FALSE);
            InvalidateRect(g_panel, &g_kbd_mode_rc, FALSE);
        }
    }
}

/* Hot-plugged pads and auto-assignment changes. */
static void refresh_pads(void)
{
    pads_state now;
    int idx;
    if (!g_session) return;
    memset(&now, 0, sizeof now);
    now.generation = nessession_gamepad_generation(g_session);
    now.count = nessession_gamepad_count(g_session);
    for (idx = 0; idx < PAD_ROWS && idx < now.count; idx++) {
        now.assign[idx] = nessession_gamepad_assignment(g_session, idx);
        now.effective[idx] = nessession_gamepad_effective_port(g_session, idx);
    }
    if (memcmp(&now, &g_pads_shown, sizeof now) != 0) {
        g_pads_shown = now;
        if (g_panel) InvalidateRect(g_panel, &g_gp_rc, FALSE);
    }
}

/* ---- keys ----------------------------------------------------------------------- */

static void hide_panel(void)
{
    set_map_state(-2);
    mouse_release();
    if (GetCapture() == g_panel) ReleaseCapture();
    g_swallow_vk = 0;
    ShowWindow(g_panel, SW_HIDE);
}

/* The app's own keys, never the machine's or Map's. */
static int app_key(WPARAM vk)
{
    return vk == VK_F9 || vk == VK_F11 || vk == VK_F12;
}

/* A key while Map is armed: 1 if Map took it. Waiting for a target, a key is
 * not typing; waiting for a binding, the key is the binding (not F9, F11 or
 * F12, which are the app's). */
static int map_key(UINT msg, WPARAM vk, LPARAM lp)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;

    if (g_swallow_vk && vk == g_swallow_vk) {
        if (!down) g_swallow_vk = 0;
        return 1;
    }
    if (g_map_state == -2) return 0;
    if (app_key(vk) || vk == VK_F10) return 0;
    if (msg == WM_SYSKEYDOWN && vk == VK_F4) return 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) return 0;       /* Ctrl chords are the app's */
    if (!down || (lp & (1 << 30))) return 1;
    if (vk == VK_ESCAPE) {
        set_map_state(g_map_state >= 0 ? -1 : -2);
        g_swallow_vk = vk;
        return 1;
    }
    if (g_map_state >= 0) {
        const int target = g_map_state;
        const uint32_t keysym = nes_keysym_from_msg(vk, lp);
        char was[128], name[48];
        if (!keysym) return 1;          /* nothing to call it: keep waiting */
        if (nessession_key_reserved(keysym)) {
            set_hint("F9, F11 and F12 are the app's and cannot be bound.");
            g_swallow_vk = vk;
            return 1;
        }
        was[0] = '\0';
        nessession_binding_set_key(g_session, target, keysym, was, sizeof was);
        nessession_keysym_name(keysym, name, sizeof name);
        g_swallow_vk = vk;
        set_map_state(-1);              /* stay armed: remapping several in a row is normal */
        bound_hint(target, name, was);
    }
    return 1;
}

/* A key with the window focused: 1 if taken. Anything Map does not take goes to
 * the main window's key path, so a key plays exactly as typed there (keyboard
 * mode included). */
static int panel_key(UINT msg, WPARAM vk, LPARAM lp)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    if (map_key(msg, vk, lp)) return 1;
    if (vk == VK_F9 && !nessession_keyboard_captures(g_session)) {
        if (down && !(lp & (1 << 30))) hide_panel();
        return 1;
    }
    if (msg == WM_SYSKEYDOWN && (vk == VK_F4 || vk == VK_F10) && !nessession_keyboard_captures(g_session)) return 0;
    if (g_parent) {
        SendMessageA(g_parent, msg, vk, lp);
        return 1;
    }
    return 0;
}

/* Exactly the laid-out client size, whatever the frame is at this DPI. */
static void fit_client(void)
{
    RECT wnd, client;
    GetWindowRect(g_panel, &wnd);
    GetClientRect(g_panel, &client);
    if (client.right == g_total_w && client.bottom == g_total_h) return;
    SetWindowPos(g_panel, NULL, 0, 0, (wnd.right - wnd.left) - client.right + g_total_w,
                 (wnd.bottom - wnd.top) - client.bottom + g_total_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* The window's outer size for the laid-out client. */
static void window_size(HWND hwnd, SIZE *dims)
{
    RECT wnd, client;
    if (hwnd && GetWindowRect(hwnd, &wnd) && GetClientRect(hwnd, &client) && client.right > 0) {
        dims->cx = (wnd.right - wnd.left) - client.right + g_total_w;
        dims->cy = (wnd.bottom - wnd.top) - client.bottom + g_total_h;
    } else {
        SetRect(&wnd, 0, 0, g_total_w, g_total_h);
        AdjustWindowRectEx(&wnd, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
        dims->cx = wnd.right - wnd.left;
        dims->cy = wnd.bottom - wnd.top;
    }
}

static void rescale(int dpi)
{
    g_dpi = dpi;
    rebuild();
}

/* ---- the mouse ------------------------------------------------------------------ */

/* The layout part under a client point, or -1. */
static int part_at(POINT pnt)
{
    if (!g_numlayout) return -1;
    return nessession_controls_hit(g_view[0], g_view[1], (float)((pnt.x - g_orgx) / g_unit),
                                   (float)((pnt.y - g_orgy) / g_unit));
}

/* The skin item under a client point, or -1. */
static int key_at(POINT pnt)
{
    const int type = nessession_keyboard(g_session);
    int num = 0;
    float width = 0, height = 0;
    double orgx;
    if (type == NES_KBD_NONE || !PtInRect(&g_kbd_rc, pnt)) return -1;
    if (!nessession_keyboard_skin_layout(type, &num, &width, &height)) return -1;
    kbd_origin(width, &orgx);
    return nessession_keyboard_skin_hit(type, (float)((pnt.x - orgx) / g_kbd_unit),
                                        (float)((pnt.y - g_kbd_rc.top) / g_kbd_unit));
}

static int key_index_at(POINT pnt)
{
    const int item = key_at(pnt);
    int num = 0;
    const nes_skin_item *items;
    if (item < 0) return -1;
    items = nessession_keyboard_skin_layout(nessession_keyboard(g_session), &num, NULL, NULL);
    return items && item < num ? items[item].key : -1;
}

/* The cross part's centre, in pixels. */
static void dpad_centre(int part, double *ctrx, double *ctry)
{
    double xpos, ypos, width, height;
    part_box(&g_layout[part], &xpos, &ypos, &width, &height);
    *ctrx = xpos + width / 2;
    *ctry = ypos + height / 2;
}

/* Where the pointer is on the cross, as the directions the console is given. */
static void dpad_follow(int part, POINT pnt)
{
    double ctrx, ctry;
    dpad_centre(part, &ctrx, &ctry);
    nessession_pointer_set(g_session, g_layout[part].port,
                           nessession_dpad_from_vector((float)((pnt.x - ctrx) / g_unit), (float)((pnt.y - ctry) / g_unit)));
    refresh_held();
    invalidate_art();
}

/* What a Map click on a part picks: for the cross, the side of it clicked. */
static int map_target_for(const nes_control *ctl, POINT pnt, int part)
{
    if (ctl->kind == NES_CTL_DPAD) {
        double ctrx, ctry, delx, dely;
        dpad_centre(part, &ctrx, &ctry);
        delx = pnt.x - ctrx;
        dely = pnt.y - ctry;
        if (fabs(delx) > fabs(dely)) return NES_TARGET_PORT(ctl->port, delx < 0 ? NES_ACT_LEFT : NES_ACT_RIGHT);
        return NES_TARGET_PORT(ctl->port, dely < 0 ? NES_ACT_UP : NES_ACT_DOWN);
    }
    return ctl->target;
}

static void strip_click(HWND hwnd, int btn)
{
    switch (btn) {
    case SB_MAP: set_map_state(g_map_state == -2 ? -1 : -2); break;
    case SB_CLEAR:
        if (g_map_state >= 0) {
            nessession_binding_clear(g_session, g_map_state);
            set_hint("Cleared. Press a key or a gamepad button to bind it again");
            nessession_gamepad_capture_begin(g_session);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;
    case SB_DEFAULTS: {
        const int count = nessession_gamepad_count(g_session);
        int pad;
        nessession_bindings_reset(g_session);
        for (pad = 0; pad < count; pad++) nessession_gamepad_assign(g_session, pad, -1);
        refresh_pads();
        set_hint("Every key and button is back to its default");
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }
    case SB_SHOW:
        g_show_bindings = !g_show_bindings;
        nessession_set_controls_show_bindings(g_session, g_show_bindings);
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case SB_RESET_CONFIG: nessession_sysaction_post(g_session, NES_SYSACT_RESET_CONFIG); break;
    case SB_STOP: nessession_sysaction_post(g_session, NES_SYSACT_PAUSE); break;
    default: nes_help_show(hwnd, "controllers"); break;
    }
}

static void mouse_down(HWND hwnd, POINT pnt)
{
    int part, row, seg, btn;
    SetFocus(hwnd);
    if (g_hover_part >= 0) {
        g_hover_part = -1;
        InvalidateRect(hwnd, &g_hint_rc, FALSE);
    }

    for (btn = 0; btn < SB_COUNT; btn++)
        if (PtInRect(&g_btn_rc[btn], pnt)) {
            /* fires on release inside the button, like a real one */
            if (btn == SB_CLEAR && g_map_state < 0) return;
            g_strip_down = btn;
            SetCapture(hwnd);
            InvalidateRect(hwnd, &g_btn_rc[btn], FALSE);
            return;
        }
    for (row = 0; row < PAD_ROWS && row < nessession_gamepad_count(g_session); row++)
        for (seg = 0; seg <= NES_PORTS; seg++) {
            RECT rect;
            segment_rect(&g_gp_row_rc[row], seg, &rect);
            if (PtInRect(&rect, pnt)) {
                /* 0 = automatic (connection order), 1..2 = that player */
                nessession_gamepad_assign(g_session, row, seg - 1);
                refresh_pads();
                InvalidateRect(hwnd, &g_gp_rc, FALSE);
                return;
            }
        }

    if (g_map_state == -2 && PtInRect(&g_kbd_rc, pnt)) {
        const int key = key_index_at(pnt);
        if (key >= 0) {
            mouse_release();
            g_mouse_key = key;
            nessession_keyboard_press(g_session, key, 1);
            SetCapture(hwnd);
            refresh_held();
            invalidate_keyboard();
        }
        return;
    }

    part = part_at(pnt);
    if (part < 0) return;
    {
        const nes_control *ctl = &g_layout[part];
        if (g_map_state != -2) {
            /* armed: picks (or changes) the target; on the cross, the side clicked */
            const int target = map_target_for(ctl, pnt, part);
            if (target >= 0) set_map_state(target);
            return;
        }
        mouse_release();
        g_mouse_part = part;
        if (ctl->kind == NES_CTL_DPAD) dpad_follow(part, pnt);
        else nessession_press(g_session, ctl->target, 1);
        SetCapture(hwnd);
        refresh_held();
        invalidate_art();
    }
}

static void mouse_move(HWND hwnd, POINT pnt)
{
    if (g_strip_down >= 0) return;
    if (g_mouse_part >= 0) {
        if (g_layout[g_mouse_part].kind == NES_CTL_DPAD) {
            dpad_follow(g_mouse_part, pnt);         /* a cross is dragged, never "dragged off" */
            return;
        }
        /* dragged off what it holds: let go, as a button does */
        if (part_at(pnt) != g_mouse_part) {
            mouse_release();
            if (GetCapture() == hwnd) ReleaseCapture();
        }
        return;
    }
    if (g_mouse_key >= 0) {
        if (key_index_at(pnt) != g_mouse_key) {
            mouse_release();
            if (GetCapture() == hwnd) ReleaseCapture();
        }
        return;
    }
    {   /* idle: the hint line says what drives the part under the pointer */
        const int part = part_at(pnt);
        if (!g_tracking) {
            TRACKMOUSEEVENT track;
            memset(&track, 0, sizeof track);
            track.cbSize = sizeof track;
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            g_tracking = TrackMouseEvent(&track) != 0;
        }
        if (part != g_hover_part) {
            g_hover_part = part;
            InvalidateRect(hwnd, &g_hint_rc, FALSE);
        }
    }
}

static void mouse_up(HWND hwnd, POINT pnt)
{
    if (g_strip_down >= 0) {
        const int btn = g_strip_down;
        g_strip_down = -1;
        InvalidateRect(hwnd, &g_btn_rc[btn], FALSE);
        if (GetCapture() == hwnd) ReleaseCapture();
        if (PtInRect(&g_btn_rc[btn], pnt)) strip_click(hwnd, btn);
        return;
    }
    if (g_mouse_part >= 0 || g_mouse_key >= 0) mouse_release();
    if (GetCapture() == hwnd) ReleaseCapture();
}

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC hdc = BeginPaint(hwnd, &paint);
        /* Off-screen first: the held highlights repaint at 20 Hz. */
        RECT inval = paint.rcPaint;
        const int width = inval.right - inval.left, height = inval.bottom - inval.top;
        if (width > 0 && height > 0) {
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, width, height);
            HGDIOBJ oldbmp = SelectObject(mem, bmp);
            HGDIOBJ oldfont = SelectObject(mem, g_font);
            SetViewportOrgEx(mem, -inval.left, -inval.top, NULL);
            paint_rect(mem, &inval);
            SetViewportOrgEx(mem, 0, 0, NULL);
            BitBlt(hdc, inval.left, inval.top, width, height, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldfont);
            SelectObject(mem, oldbmp);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        mouse_down(hwnd, pnt);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        mouse_move(hwnd, pnt);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tracking = 0;
        if (g_hover_part >= 0) {
            g_hover_part = -1;
            InvalidateRect(hwnd, &g_hint_rc, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        /* released wherever the mouse ended up: capture means this arrives
         * even if the pointer left the part */
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        mouse_up(hwnd, pnt);
        return 0;
    }
    case WM_CAPTURECHANGED:
        /* lost the mouse some other way (Alt+Tab mid-press): let go */
        if ((HWND)lparam != hwnd) {
            if (g_strip_down >= 0) {
                InvalidateRect(hwnd, &g_btn_rc[g_strip_down], FALSE);
                g_strip_down = -1;
            }
            if (g_mouse_part >= 0 || g_mouse_key >= 0) mouse_release();
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wparam) == IDC_CTRL_TYPE0 || LOWORD(wparam) == IDC_CTRL_TYPE1) {
            const int port = LOWORD(wparam) == IDC_CTRL_TYPE1;
            if (HIWORD(wparam) == CBN_SELCHANGE) {
                const int sel = (int)SendMessageA(g_combo[port], CB_GETCURSEL, 0, 0);
                if (sel >= 0) nessession_set_port_type(g_session, port, sel);
                SetFocus(hwnd);             /* keys go to the console again, not to the list */
                refresh_held();
            } else if (HIWORD(wparam) == CBN_CLOSEUP) {
                SetFocus(hwnd);
            }
            return 0;
        }
        if (LOWORD(wparam) == IDC_CTRL_KBD) {
            if (HIWORD(wparam) == CBN_SELCHANGE) {
                const int sel = (int)SendMessageA(g_kbd_combo, CB_GETCURSEL, 0, 0);
                if (sel >= 0) nessession_set_keyboard(g_session, sel);
                SetFocus(hwnd);
                refresh_held();
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (HIWORD(wparam) == CBN_CLOSEUP) {
                SetFocus(hwnd);
            }
            return 0;
        }
        break;

    case WM_TIMER:
        if (wparam == IDT_CAPTURE) {
            int button;
            if (g_map_state < 0) {
                KillTimer(hwnd, IDT_CAPTURE);
            } else if (nessession_gamepad_capture_poll(g_session, &button)) {
                char was[128];
                const int target = g_map_state;
                was[0] = '\0';
                nessession_binding_set_button(g_session, target, button, was, sizeof was);
                set_map_state(-1);          /* stay armed: remapping several in a row is normal */
                bound_hint(target, nes_pad_button_name(button), was);
                InvalidateRect(hwnd, NULL, FALSE);
            }
        } else if (wparam == IDT_HELD) {
            refresh_held();
        } else if (wparam == IDT_PADS) {
            refresh_pads();
        }
        return 0;

    /* WM_SYSKEY* too: a bound Alt is the console's */
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
        if (panel_key(msg, wparam, lparam)) return 0;
        break;
    case WM_SYSCHAR:
        /* Alt+letter: the main window's menu mnemonics, as when it has the focus */
        if (wparam != ' ' && g_parent) { SendMessageA(g_parent, msg, wparam, lparam); return 0; }
        break;
    case WM_SYSCOMMAND:
        if ((wparam & 0xFFF0) == SC_KEYMENU && nessession_keyboard_captures(g_session)) return 0;
        break;
    case WM_ENTERMENULOOP:
        nessession_release_all(g_session);
        break;

    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) {
            const HWND other = (HWND)lparam;
            mouse_release();                /* nessession_release_pointer: the pointer's holds go */
            if (GetCapture() == hwnd) ReleaseCapture();
            g_strip_down = -1;
            g_swallow_vk = 0;               /* its release goes elsewhere */
            /* Leaving the app: nothing typed here stays held (within it, the
             * window that gets the key-up lets go). */
            nessession_release_keyboard(g_session);   /* keys typed here, whichever window is next */
            if (!other || GetWindowThreadProcessId(other, NULL) != GetCurrentThreadId())
                nessession_release_all(g_session);
        }
        break;                              /* DefWindowProc gives the window the focus */
    case WM_SHOWWINDOW:
        if (wparam) {
            refresh_held();
            SetTimer(hwnd, IDT_HELD, 50, NULL);
            SetTimer(hwnd, IDT_PADS, 250, NULL);
        } else {
            KillTimer(hwnd, IDT_HELD);
            KillTimer(hwnd, IDT_PADS);
            set_map_state(-2);              /* hidden with Map armed (main window minimised) */
            g_swallow_vk = 0;
            mouse_release();
        }
        break;
    case WM_GETMINMAXINFO:
        /* Fixed: the size limits say so too, so neither maximizing nor a
         * compositor (Wine passes them on) makes it any other size. */
        if (g_total_w) {
            MINMAXINFO *limits = (MINMAXINFO *)lparam;
            SIZE dims;
            window_size(hwnd, &dims);
            limits->ptMinTrackSize.x = limits->ptMaxTrackSize.x = limits->ptMaxSize.x = dims.cx;
            limits->ptMinTrackSize.y = limits->ptMaxTrackSize.y = limits->ptMaxSize.y = dims.cy;
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        const RECT *suggested = (const RECT *)lparam;
        rescale(HIWORD(wparam));
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        fit_client();
        return 0;
    }
    case WM_CLOSE:
        /* Hide, do not destroy: a remap and the position survive closing it. */
        hide_panel();
        return 0;
    case WM_DESTROY:
        /* with the main window: the next toggle starts afresh */
        mouse_release();
        set_map_state(-2);
        g_panel = NULL;
        g_combo[0] = g_combo[1] = NULL;
        g_kbd_combo = NULL;
        g_numlayout = 0;
        free_fonts();
        break;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static int create_panel(void)
{
    static int registered;
    HINSTANCE inst = GetModuleHandleA(NULL);
    RECT want;
    int port, type;

    g_show_bindings = nessession_controls_show_bindings(g_session) > 0;

    if (!registered) {
        WNDCLASSEXA wcls;
        memset(&wcls, 0, sizeof wcls);
        wcls.cbSize = sizeof wcls;
        wcls.lpfnWndProc = panel_proc;      /* no CS_DBLCLKS: fast clicks stay press/release pairs */
        wcls.hInstance = inst;
        wcls.hCursor = LoadCursor(NULL, IDC_ARROW);
        wcls.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wcls.lpszClassName = PANEL_CLASS;
        registered = RegisterClassExA(&wcls) != 0;
    }
    g_dpi = nes_window_dpi(g_parent);
    layout_static();
    make_fonts();

    SetRect(&want, 0, 0, g_total_w, g_total_h);
    /* A tool window with a caption and no thick frame or maximize box: it is
     * exactly the size of its contents, and stays off the taskbar. */
    AdjustWindowRectEx(&want, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
    g_panel = CreateWindowExA(PANEL_EXSTYLE, PANEL_CLASS, "Controllers (F9)", PANEL_STYLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left, want.bottom - want.top,
                              g_parent, NULL, inst, NULL);
    if (!g_panel) return 0;
    for (port = 0; port < NES_PORTS; port++) {
        g_combo[port] = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                                        0, 0, 100, 100, g_panel, (HMENU)(INT_PTR)(IDC_CTRL_TYPE0 + port), inst, NULL);
        if (!g_combo[port]) continue;
        for (type = 0; type < NES_CTRL_COUNT; type++) {
            const char *name = nes_ctrl_type_name(type);
            SendMessageA(g_combo[port], CB_ADDSTRING, 0, (LPARAM)(name ? name : "?"));
        }
    }
    g_kbd_combo = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                                  0, 0, 100, 100, g_panel, (HMENU)(INT_PTR)IDC_CTRL_KBD, inst, NULL);
    if (g_kbd_combo) {
        for (type = 0; nes_keyboard_name(type); type++)
            SendMessageA(g_kbd_combo, CB_ADDSTRING, 0, (LPARAM)nes_keyboard_name(type));
    }
    nes_help_set_context(g_panel, "controllers", 0);    /* F1 is the keyboard's here */
    rescale(nes_window_dpi(g_panel));    /* placed on another monitor than the main window */
    fit_client();
    sync_combos();
    memset(&g_pads_shown, 0, sizeof g_pads_shown);
    g_hover_part = -1;
    set_map_state(-2);
    return 1;
}

static void show_panel(void)
{
    refresh_pads();
    ShowWindow(g_panel, SW_SHOW);
    SetForegroundWindow(g_panel);
}

void nes_controller_window_toggle(HWND parent, nessession *session)
{
    g_session = session;
    if (parent) g_parent = parent;
    if (!g_panel && !create_panel()) return;
    if (IsWindowVisible(g_panel)) hide_panel();
    else show_panel();
}

void nes_controller_window_show(HWND parent, nessession *session)
{
    g_session = session;
    if (parent) g_parent = parent;
    if (!g_panel && !create_panel()) return;
    if (!IsWindowVisible(g_panel)) show_panel();
    else SetForegroundWindow(g_panel);
}

void nes_controller_window_show_keyboard(HWND parent, nessession *session)
{
    /* One window holds the controllers and the keyboard, so "on the
     * keyboard" is just shown -- with keys going to the panel. */
    nes_controller_window_show(parent, session);
    if (g_panel) SetFocus(g_panel);
}

int nes_controller_window_visible(void)
{
    return g_panel && IsWindowVisible(g_panel);
}

void nes_controller_window_gamepads_changed(void)
{
    if (!g_panel) return;
    sync_combos();
    if (IsWindowVisible(g_panel)) {
        refresh_pads();
        refresh_held();
        InvalidateRect(g_panel, NULL, FALSE);
    }
}

int nes_controller_pretranslate(MSG *msg)
{
    /* Keys reach the window proc directly (the only focusable children are
     * the selectors, which give the focus back); nothing to steal. */
    (void)msg;
    return 0;
}
