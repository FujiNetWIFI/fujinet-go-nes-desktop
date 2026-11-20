/*
 * The Controllers window: the two NES-001 pads and the console's RESET key
 * drawn with cairo from the core's layout (nessession_controls_layout,
 * hit-tested with nessession_controls_hit), from photos of the real things
 * (DESIGN-nes.md). A port set to None shows an empty dashed socket. The
 * Keyboard page draws the expansion-port keyboard (Family BASIC or Subor)
 * from nessession_keyboard_skin_layout.
 *
 * Every press is the raw press and release of a gesture, not a button's
 * "clicked": the machine samples its inputs once per frame, so a value present
 * only for the instant of a click falls between two frames. A part on screen
 * is HELD for as long as the mouse button is down; the D-pad follows the
 * pointer while it is dragged (eight 45-degree sectors from its centre) and is
 * let go on release. Everything the pointer holds is let go
 * (nessession_release_pointer) on mouse-up, on the gesture's cancel, on focus
 * loss, when the window hides or maps and when Map is armed.
 *
 * What the console sees held (keyboard, gamepads and this window together) is
 * lit on the picture, so the window doubles as an input tester.
 *
 * SINGLETON, hidden rather than destroyed, so a remap survives closing it. Not
 * resizable: every port-type combination has the same extent.
 *
 * MAP MODE: Map arms a two-step rebind -- click a part (on the D-pad, the
 * direction under the click) to pick the target, then press a keyboard key OR a
 * gamepad button; Esc cancels. Clear unbinds the picked target; Defaults
 * restores the table; Show bindings paints each part's binding on it. While a
 * target is picked the other parts dim to 40%. The two system keys (Reset to
 * CONFIG, stop in the debugger) are not on the picture: the "System keys" list
 * picks them.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controllers_window.h"

#include "../window.h"
#include "../help/help_window.h"
#include "../debugger/dbg_window.h"

#include <math.h>
#include <string.h>

#define SCALE   24.0        /* pixels per layout unit */
#define MARGIN  16.0
#define KBD_SCALE 32.0      /* pixels per key unit */
#define KBD_MARGIN 12.0
#define MAX_PADS 8
#define KEY_SLOTS 128

#define HELD_R  (((NESSESSION_HELD_RGB >> 16) & 255) / 255.0)
#define HELD_G  (((NESSESSION_HELD_RGB >> 8) & 255) / 255.0)
#define HELD_B  ((NESSESSION_HELD_RGB & 255) / 255.0)

/* ---- singleton state ------------------------------------------------------ */

static GtkWidget *g_window;
static GtkWindow *g_parent;     /* the NESWindow: sysactions and F-keys go through it */
static nessession *g_session;
static GtkWidget *g_area;       /* the controllers picture */

static const nes_control *g_layout;
static int g_count;
static int g_view[2] = { -1, -1 };   /* the port types the layout was built for */
static float g_lw, g_lh;
static double g_offx;           /* the picture is centred in the canvas */

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 the picked
 * target waiting for a key or pad button. */
static int g_map_state = -2;
static GtkWidget *g_map_button, *g_clear_button, *g_defaults_button, *g_show_check;
static GtkWidget *g_map_hint, *g_system_dd;
static guint g_capture_timer, g_info_timer;
static gboolean g_sysact_down[NES_SYSACT_COUNT];  /* leading-edge guard: GTK4 has no repeat flag */
static guint g_swallow_code;    /* the key Map took: its repeats and release too */
static GtkWidget *g_type_combo[2];
static GtkWidget *g_pad_box;
static GtkWidget *g_pad_port[MAX_PADS];
static int g_npad_rows;
static unsigned g_pad_generation = (unsigned)-1;
static gboolean g_syncing;
static gboolean g_show_bindings;

static int g_drag_idx = -1;     /* the layout part the drag started on, or -1 */
static guint64 g_last_sig;

/* the Keyboard page */
static GtkWidget *g_view_stack;
static GtkWidget *g_kbd_stack;  /* the drawing, or the "no keyboard" placeholder */
static GtkWidget *g_kbd_area;
static GtkWidget *g_kbd_combo;
static GtkWidget *g_kbd_mode_label;
static int g_kbd_type = -1;     /* what the page currently shows */
static const nes_skin_item *g_skin;
static int g_skin_count;
static float g_skin_w, g_skin_h;
static double g_kbd_offx, g_kbd_offy;
static int g_kbd_mouse = -1;    /* the key index the mouse holds */
static guint64 g_kbd_last_sig;

/* ---- helpers --------------------------------------------------------------- */

static void set_hint(const char *text)
{
    gtk_label_set_text(GTK_LABEL(g_map_hint), text);
}

static void redraw(void)
{
    if (g_area)
        gtk_widget_queue_draw(g_area);
}

static void release_pointer(void)
{
    if (g_session)
        nessession_release_pointer(g_session);
    g_drag_idx = -1;
    redraw();
}

static const nes_control *part(int idx)
{
    return idx >= 0 && idx < g_count ? &g_layout[idx] : NULL;
}

static double px(double unit) { return g_offx + unit * SCALE; }
static double py(double unit) { return MARGIN + unit * SCALE; }

/* The direction bit a vector from the cross's centre points at, for Map: the
 * dominant axis (-1 within the dead centre). */
static int map_direction(double dx, double dy)
{
    if (nessession_dpad_from_vector((float)(dx / SCALE), (float)(dy / SCALE)) == 0)
        return -1;
    if (fabs(dx) >= fabs(dy))
        return dx < 0 ? NES_ACT_LEFT : NES_ACT_RIGHT;
    return dy < 0 ? NES_ACT_UP : NES_ACT_DOWN;
}

/* The Map target a click at canvas point (x, y) picks: a part's target; on the
 * cross the direction under the click. */
static int map_target_at(int idx, double x, double y)
{
    const nes_control *ctl = part(idx);
    int dir;
    if (!ctl)
        return -1;
    if (ctl->kind != NES_CTL_DPAD)
        return ctl->target;
    dir = map_direction(x - px(ctl->x + ctl->w / 2), y - py(ctl->y + ctl->h / 2));
    return dir < 0 ? -1 : NES_TARGET_PORT(ctl->port, dir);
}

/* ---- gamepad capture and Map state ---------------------------------------- */

static void stop_capture(void)
{
    if (g_capture_timer) {
        g_source_remove(g_capture_timer);
        g_capture_timer = 0;
    }
    nessession_gamepad_capture_cancel(g_session);
}

static void set_map_state(int state);

static gboolean capture_tick(gpointer data)
{
    int button;
    (void)data;
    if (g_map_state < 0) { g_capture_timer = 0; return G_SOURCE_REMOVE; }
    if (nessession_gamepad_capture_poll(g_session, &button)) {
        char stolen[128], msg[200];
        stolen[0] = '\0';
        nessession_binding_set_button(g_session, g_map_state, button, stolen, sizeof stolen);
        if (stolen[0])
            g_snprintf(msg, sizeof msg, "Bound %s (was %s)", nes_pad_button_name(button), stolen);
        else
            g_snprintf(msg, sizeof msg, "Bound %s", nes_pad_button_name(button));
        g_capture_timer = 0;
        set_map_state(-1);          /* back to armed: remapping several in a row is normal */
        set_hint(msg);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void set_map_state(int state)
{
    if (state != -2) {
        release_pointer();                  /* a part held into Map would never be released */
        nessession_release_all(g_session);  /* Map swallows key-ups: drop keyboard holds too */
    }
    g_map_state = state;
    if (state == -2) {
        stop_capture();
        g_swallow_code = 0;
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Map");
        gtk_widget_remove_css_class(g_map_button, "nes-accent");
        set_hint("Drag the cross, hold a button or a key.");
    } else if (state == -1) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Done");
        gtk_widget_add_css_class(g_map_button, "nes-accent");
        set_hint("Click a control to remap (on the cross, a direction). Esc backs out");
    } else {
        char msg[128];
        g_snprintf(msg, sizeof msg, "Press a key or gamepad button for %s (Esc cancels)",
                   nes_target_name(state));
        set_hint(msg);
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Done");
        gtk_widget_add_css_class(g_map_button, "nes-accent");
        stop_capture();
        nessession_gamepad_capture_begin(g_session);
        g_capture_timer = g_timeout_add(50, capture_tick, NULL);
    }
    gtk_widget_set_sensitive(g_clear_button, g_map_state >= 0);
    redraw();
}

/* ---- drawing primitives -------------------------------------------------------- */

static void rgb(cairo_t *cr, unsigned hex)
{
    cairo_set_source_rgb(cr, ((hex >> 16) & 255) / 255.0, ((hex >> 8) & 255) / 255.0,
                         (hex & 255) / 255.0);
}

static void rgba(cairo_t *cr, unsigned hex, double alpha)
{
    cairo_set_source_rgba(cr, ((hex >> 16) & 255) / 255.0, ((hex >> 8) & 255) / 255.0,
                          (hex & 255) / 255.0, alpha);
}

static void stop_rgb(cairo_pattern_t *pat, double at, unsigned hex)
{
    cairo_pattern_add_color_stop_rgb(pat, at, ((hex >> 16) & 255) / 255.0,
                                     ((hex >> 8) & 255) / 255.0, (hex & 255) / 255.0);
}

static void rrect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

static void disc(cairo_t *cr, double cx, double cy, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
}

/* Text centred on a point, shrunk to fit its width. */
static void text_fit(cairo_t *cr, const char *text, double cx, double cy, double maxw,
                     double size, cairo_font_slant_t slant, cairo_font_weight_t weight)
{
    cairo_text_extents_t ext;
    cairo_select_font_face(cr, "Sans", slant, weight);
    cairo_set_font_size(cr, size);
    cairo_text_extents(cr, text, &ext);
    if (ext.width > maxw && ext.width > 0) {
        cairo_set_font_size(cr, size * maxw / ext.width);
        cairo_text_extents(cr, text, &ext);
    }
    cairo_move_to(cr, cx - ext.width / 2 - ext.x_bearing, cy - ext.height / 2 - ext.y_bearing);
    cairo_show_text(cr, text);
}

/* The ring and glow of a held or picked part. */
static void held_ring_round(cairo_t *cr, double cx, double cy, double radius)
{
    cairo_set_source_rgba(cr, HELD_R, HELD_G, HELD_B, 0.35);
    disc(cr, cx, cy, radius);
    cairo_fill(cr);
    rgb(cr, NESSESSION_HELD_RGB);
    cairo_set_line_width(cr, 2);
    disc(cr, cx, cy, radius + 1);
    cairo_stroke(cr);
}

static void held_ring_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_set_source_rgba(cr, HELD_R, HELD_G, HELD_B, 0.35);
    rrect(cr, x, y, w, h, r);
    cairo_fill(cr);
    rgb(cr, NESSESSION_HELD_RGB);
    cairo_set_line_width(cr, 2);
    rrect(cr, x + 1, y + 1, w - 2, h - 2, r);
    cairo_stroke(cr);
}

/* What a part shows in Map mode and with Show bindings: its key and pad button
 * (a dash when there is none). */
static void binding_text(int target, char *key, int keysz, char *pad, int padsz)
{
    nes_binding bind = nessession_binding_get(g_session, target);
    key[0] = pad[0] = 0;
    nessession_keysym_name(bind.keysym, key, keysz);
    if (!key[0])
        g_strlcpy(key, "\xe2\x80\x94", keysz);
    if (bind.button != NES_PAD_BTN_NONE)
        g_strlcpy(pad, nes_pad_button_name(bind.button), padsz);
}

static gboolean bindings_on(void)
{
    return g_map_state != -2 || g_show_bindings;
}

/* ---- the pad ------------------------------------------------------------------- */

static void draw_caption(cairo_t *cr, const nes_control *ctl, const GdkRGBA *fg)
{
    const double x = px(ctl->x), y = py(ctl->y), w = ctl->w * SCALE, h = ctl->h * SCALE;
    if (!strncmp(ctl->label, "Player", 6)) {
        cairo_set_source_rgba(cr, fg->red, fg->green, fg->blue, 0.85);
        text_fit(cr, ctl->label, x + w / 2, y + h / 2, w, 13, CAIRO_FONT_SLANT_NORMAL,
                 CAIRO_FONT_WEIGHT_BOLD);
    } else if (!strcmp(ctl->label, "No controller")) {
        cairo_set_source_rgba(cr, fg->red, fg->green, fg->blue, 0.6);
        text_fit(cr, ctl->label, x + w / 2, y + h / 2, w, 14, CAIRO_FONT_SLANT_NORMAL,
                 CAIRO_FONT_WEIGHT_NORMAL);
    } else if (!strcmp(ctl->label, "Nintendo")) {
        rgb(cr, NESSESSION_LEGEND_RGB);
        text_fit(cr, ctl->label, x + w / 2, y + h / 2, w, 15, CAIRO_FONT_SLANT_NORMAL,
                 CAIRO_FONT_WEIGHT_BOLD);
    } else if (!strcmp(ctl->label, "Nintendo ENTERTAINMENT SYSTEM")) {
        cairo_text_extents_t ext;
        rgb(cr, NESSESSION_LEGEND_RGB);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 12);
        cairo_text_extents(cr, ctl->label, &ext);
        cairo_move_to(cr, x, y + h / 2 - ext.height / 2 - ext.y_bearing);
        cairo_show_text(cr, ctl->label);
    } else {                      /* SELECT, START, B, A, T */
        rgb(cr, NESSESSION_LEGEND_RGB);
        text_fit(cr, ctl->label, x + w / 2, y + h / 2, w + 4,
                 strlen(ctl->label) > 1 ? 8 : 12, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    }
}

/* An empty port: a dashed socket outline. */
static void draw_socket(cairo_t *cr, const nes_control *ctl)
{
    static const double dash[2] = { 6, 4 };
    const double x = px(ctl->x), y = py(ctl->y), w = ctl->w * SCALE, h = ctl->h * SCALE;
    rgb(cr, NESSESSION_SOCKET_RGB);
    cairo_set_line_width(cr, 2);
    cairo_set_dash(cr, dash, 2, 0);
    rrect(cr, x + 1, y + 1, w - 2, h - 2, 0.8 * SCALE);
    cairo_stroke(cr);
    cairo_set_dash(cr, NULL, 0, 0);
}

static void draw_console_key(cairo_t *cr, const nes_control *ctl, int lit, double alpha,
                             gboolean dimmed)
{
    const double x = px(ctl->x), y = py(ctl->y), w = ctl->w * SCALE, h = ctl->h * SCALE;
    char key[64], pad[64];
    cairo_pattern_t *pat;

    cairo_push_group(cr);
    rgba(cr, 0x000000, 0.35);
    rrect(cr, x, y + 2, w, h, 3);
    cairo_fill(cr);
    pat = cairo_pattern_create_linear(0, y, 0, y + h);
    stop_rgb(pat, 0, 0xB4B4B8);
    stop_rgb(pat, 1, NESSESSION_KEY_RGB);
    cairo_set_source(cr, pat);
    rrect(cr, x, y, w, h, 3);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(pat);
    rgba(cr, 0xFFFFFF, 0.35);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    if (lit)
        held_ring_rect(cr, x, y, w, h, 3);
    rgb(cr, NESSESSION_KEY_INK_RGB);
    if (!dimmed && bindings_on() && ctl->target >= 0) {
        binding_text(ctl->target, key, sizeof key, pad, sizeof pad);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 8);
        cairo_move_to(cr, x + 5, y + 11);
        cairo_show_text(cr, ctl->label);
        text_fit(cr, key, x + w / 2, y + h * (pad[0] ? 0.55 : 0.68), w - 8, 11,
                 CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        if (pad[0])
            text_fit(cr, pad, x + w / 2, y + h * 0.84, w - 8, 8, CAIRO_FONT_SLANT_NORMAL,
                     CAIRO_FONT_WEIGHT_NORMAL);
    } else {
        text_fit(cr, ctl->label, x + w / 2, y + h / 2, w - 10, 12, CAIRO_FONT_SLANT_NORMAL,
                 CAIRO_FONT_WEIGHT_BOLD);
    }
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, dimmed ? 0.4 : alpha);
    if (dimmed) {                    /* the power LED's pinhole */
        rgb(cr, 0x2A2A2C);
        disc(cr, x - 0.5 * SCALE, y + h / 2, 2.5);
        cairo_fill(cr);
    }
}

static void draw_body(cairo_t *cr, const nes_control *ctl)
{
    const double x = px(ctl->x), y = py(ctl->y), w = ctl->w * SCALE, h = ctl->h * SCALE;
    cairo_pattern_t *pat;

    if (!strcmp(ctl->label, "NES-001")) {
        rgba(cr, 0x000000, 0.30);
        rrect(cr, x + 1, y + 3, w, h, 0.6 * SCALE);
        cairo_fill(cr);
        rgb(cr, NESSESSION_SHELL_SHADOW_RGB);
        rrect(cr, x, y, w, h, 0.6 * SCALE);
        cairo_fill(cr);
        pat = cairo_pattern_create_linear(0, y, 0, y + h);
        stop_rgb(pat, 0, 0xE4E2DC);
        stop_rgb(pat, 1, NESSESSION_SHELL_RGB);
        cairo_set_source(cr, pat);
        rrect(cr, x + 1.5, y + 1.5, w - 3, h - 4.5, 0.55 * SCALE);
        cairo_fill(cr);
        cairo_pattern_destroy(pat);
    } else if (!strcmp(ctl->label, "Face")) {
        rgb(cr, NESSESSION_FACE_RGB);
        rrect(cr, x, y, w, h, 0.3 * SCALE);
        cairo_fill(cr);
    } else if (!strcmp(ctl->label, "Stripe")) {
        rgb(cr, NESSESSION_STRIPE_RGB);
        cairo_rectangle(cr, x, y, w, h);
        cairo_fill(cr);
    } else if (!strcmp(ctl->label, "DpadWell") || !strcmp(ctl->label, "SelectWell") ||
               !strcmp(ctl->label, "ButtonWell")) {
        const double r = !strcmp(ctl->label, "DpadWell") ? 0.5 * SCALE : 0.35 * SCALE;
        rgb(cr, NESSESSION_SHELL_RGB);
        rrect(cr, x, y, w, h, r);
        cairo_fill_preserve(cr);
        rgb(cr, NESSESSION_SHELL_SHADOW_RGB);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
    } else if (!strcmp(ctl->label, "Socket")) {
        draw_socket(cr, ctl);
    } else if (!strcmp(ctl->label, "Console")) {
        rgba(cr, 0x000000, 0.25);
        rrect(cr, x, y + 2, w, h, 0.5 * SCALE);
        cairo_fill(cr);
        rgb(cr, NESSESSION_CONSOLE_RGB);
        rrect(cr, x, y, w, h, 0.5 * SCALE);
        cairo_fill(cr);
    } else if (!strcmp(ctl->label, "Base")) {
        cairo_save(cr);
        rrect(cr, px(0), py(ctl->y - 2.0), g_lw * SCALE, 4.0 * SCALE, 0.5 * SCALE);
        cairo_clip(cr);
        rgb(cr, NESSESSION_CONSOLE_BASE_RGB);
        cairo_rectangle(cr, x, y, w, h);
        cairo_fill(cr);
        cairo_restore(cr);
    }
}

/* One arm of the cross: black, an embossed triangle, shifted a little toward
 * the press and tinted while held. */
static void draw_arm(cairo_t *cr, const nes_control *ctl, int lit, double alpha)
{
    const double shift = lit ? 0.08 * SCALE : 0;
    double x = px(ctl->x), y = py(ctl->y);
    const double w = ctl->w * SCALE, h = ctl->h * SCALE;
    const double cx0 = px(ctl->x + ctl->w / 2), cy0 = py(ctl->y + ctl->h / 2);
    double tx, ty;
    char key[64], pad[64];
    int dir = !strcmp(ctl->label, "Up") ? 0 : !strcmp(ctl->label, "Down") ? 1
            : !strcmp(ctl->label, "Left") ? 2 : 3;

    if (dir == 0) y -= shift; else if (dir == 1) y += shift;
    else if (dir == 2) x -= shift; else x += shift;
    cairo_push_group(cr);
    rgb(cr, NESSESSION_DPAD_RGB);
    rrect(cr, x, y, w, h, 0.18 * SCALE);
    cairo_fill(cr);
    if (lit) {
        cairo_set_source_rgba(cr, ((NESSESSION_DPAD_HELD_RGB >> 16) & 255) / 255.0,
                              ((NESSESSION_DPAD_HELD_RGB >> 8) & 255) / 255.0,
                              (NESSESSION_DPAD_HELD_RGB & 255) / 255.0, 0.55);
        rrect(cr, x, y, w, h, 0.18 * SCALE);
        cairo_fill(cr);
    }
    tx = cx0 + (dir == 2 ? -0.3 : dir == 3 ? 0.3 : 0) * SCALE;
    ty = cy0 + (dir == 0 ? -0.35 : dir == 1 ? 0.35 : 0) * SCALE;
    if (bindings_on()) {
        binding_text(ctl->target, key, sizeof key, pad, sizeof pad);
        rgb(cr, 0xEDEDE6);
        /* on the outer half of a side arm, clear of the centre */
        text_fit(cr, key, cx0 + (dir == 2 ? -0.2 : dir == 3 ? 0.2 : 0) * SCALE, cy0,
                 dir < 2 ? w - 3 : w * 0.62, 9, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    } else {
        const double s = 0.22 * SCALE;
        rgb(cr, lit ? 0x2A1A08 : NESSESSION_DPAD_MARK_RGB);
        if (dir == 0) { cairo_move_to(cr, tx, ty - s); cairo_line_to(cr, tx - s, ty + s * 0.7); cairo_line_to(cr, tx + s, ty + s * 0.7); }
        else if (dir == 1) { cairo_move_to(cr, tx, ty + s); cairo_line_to(cr, tx - s, ty - s * 0.7); cairo_line_to(cr, tx + s, ty - s * 0.7); }
        else if (dir == 2) { cairo_move_to(cr, tx - s, ty); cairo_line_to(cr, tx + s * 0.7, ty - s); cairo_line_to(cr, tx + s * 0.7, ty + s); }
        else { cairo_move_to(cr, tx + s, ty); cairo_line_to(cr, tx - s * 0.7, ty - s); cairo_line_to(cr, tx - s * 0.7, ty + s); }
        cairo_close_path(cr);
        cairo_fill(cr);
    }
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, alpha);
}

/* B and A: the red domes in their wells. */
static void draw_round(cairo_t *cr, const nes_control *ctl, int lit, double alpha)
{
    const double cx = px(ctl->x + ctl->w / 2), cy = py(ctl->y + ctl->h / 2), r = ctl->w * SCALE / 2;
    cairo_pattern_t *pat;
    char key[64], pad[64];

    cairo_push_group(cr);
    rgba(cr, 0x000000, 0.3);
    disc(cr, cx + 1, cy + 2, r);
    cairo_fill(cr);
    pat = cairo_pattern_create_radial(cx - r * 0.35, cy - r * 0.4, r * 0.1, cx, cy, r);
    stop_rgb(pat, 0, lit ? NESSESSION_BUTTON_LIGHT_RGB : NESSESSION_BUTTON_LIGHT_RGB);
    stop_rgb(pat, 1, lit ? NESSESSION_HELD_FILL_RGB : NESSESSION_BUTTON_RGB);
    cairo_set_source(cr, pat);
    disc(cr, cx, cy, r);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);
    if (lit)
        held_ring_round(cr, cx, cy, r);
    else {
        rgba(cr, 0xFFFFFF, 0.45);
        disc(cr, cx - r * 0.35, cy - r * 0.4, r * 0.16);
        cairo_fill(cr);
    }
    if (bindings_on()) {
        binding_text(ctl->target, key, sizeof key, pad, sizeof pad);
        rgb(cr, 0xFFFFFF);
        text_fit(cr, key, cx, cy, r * 1.6, 10, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    }
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, alpha);
}

/* SELECT / START (dark rubber) and the turbo pills (light grey). */
static void draw_pill(cairo_t *cr, const nes_control *ctl, int lit, double alpha)
{
    const double x = px(ctl->x), y = py(ctl->y), w = ctl->w * SCALE, h = ctl->h * SCALE;
    const gboolean turbo = !strncmp(ctl->label, "TURBO", 5);
    char key[64], pad[64];

    cairo_push_group(cr);
    rgba(cr, 0x000000, 0.3);
    rrect(cr, x, y + 1.5, w, h, h / 2);
    cairo_fill(cr);
    rgb(cr, turbo ? NESSESSION_SHELL_RGB : NESSESSION_RUBBER_RGB);
    rrect(cr, x, y, w, h, h / 2);
    cairo_fill_preserve(cr);
    if (turbo) {
        rgb(cr, NESSESSION_SHELL_SHADOW_RGB);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
    } else {
        rgba(cr, 0xFFFFFF, 0.12);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
    }
    if (lit)
        held_ring_rect(cr, x, y, w, h, h / 2);
    if (bindings_on()) {
        binding_text(ctl->target, key, sizeof key, pad, sizeof pad);
        rgb(cr, turbo ? 0x2A2A2A : 0xEDEDE6);
        text_fit(cr, key, x + w / 2, y + h / 2, w - 6, 8, CAIRO_FONT_SLANT_NORMAL,
                 CAIRO_FONT_WEIGHT_BOLD);
    }
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, alpha);
}

static void draw_all(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
    GdkRGBA fg;
    int i;
    (void)data; (void)height;

    gtk_widget_get_color(GTK_WIDGET(area), &fg);
    g_offx = (width - g_lw * SCALE) / 2;
    for (i = 0; i < g_count; i++) {
        const nes_control *ctl = &g_layout[i];
        const int held = ctl->target >= 0 && nessession_target_held(g_session, ctl->target);
        const int picked = g_map_state >= 0 && ctl->target == g_map_state;
        const double alpha = g_map_state >= 0 && !picked ? 0.4 : 1.0;

        switch (ctl->kind) {
        case NES_CTL_CAPTION:
            draw_caption(cr, ctl, &fg);
            break;
        case NES_CTL_BODY:
            if (!strcmp(ctl->label, "POWER"))
                draw_console_key(cr, ctl, 0, 1.0, TRUE);
            else
                draw_body(cr, ctl);
            break;
        case NES_CTL_DPAD:
            break;
        case NES_CTL_DPAD_ARM:
            draw_arm(cr, ctl, held || picked, alpha);
            /* the dimple at the cross's centre, once all four arms are down */
            if (!strcmp(ctl->label, "Right") && !bindings_on()) {
                rgb(cr, NESSESSION_DPAD_DIMPLE_RGB);
                disc(cr, px(ctl->x), py(ctl->y + ctl->h / 2), 0.3 * SCALE);
                cairo_fill(cr);
            }
            break;
        case NES_CTL_ROUND_BUTTON:
            draw_round(cr, ctl, held || picked, alpha);
            break;
        case NES_CTL_PILL:
            draw_pill(cr, ctl, held || picked, alpha);
            break;
        case NES_CTL_BUTTON:
            draw_console_key(cr, ctl, held || picked, alpha, FALSE);
            break;
        }
    }
}

/* ---- sizing ---------------------------------------------------------------- */

/* The layout for the ports' types now. Its extent is the same for every
 * combination, so only the contents change. */
static void relayout(void)
{
    g_view[0] = nessession_port_type(g_session, 0);
    g_view[1] = nessession_port_type(g_session, 1);
    g_layout = nessession_controls_layout(g_view[0], g_view[1], &g_count, &g_lw, &g_lh);
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(g_area), (int)(g_lw * SCALE + 2 * MARGIN));
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g_area), (int)(g_lh * SCALE + 2 * MARGIN));
}

/* A signature of everything that decides how the canvas looks. */
static guint64 paint_signature(void)
{
    guint64 sig = (guint64)(g_view[0] * 8 + g_view[1]);
    int port;
    for (port = 0; port < 2; port++)
        sig = sig * 1099511628211ull + nessession_buttons_held(g_session, port);
    sig = sig * 1099511628211ull + (guint64)(nessession_target_held(g_session,
                                       NES_TARGET_SWITCH(NES_SW_RESET)) + 1);
    return sig;
}

/* Every frame while the picture is on screen: follow what the console sees
 * held (keyboard, gamepads and this window together), and the layout when a
 * port's type changes. */
static gboolean tick_cb(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
    guint64 sig;
    (void)widget; (void)clock; (void)data;
    if (nessession_port_type(g_session, 0) != g_view[0] ||
        nessession_port_type(g_session, 1) != g_view[1]) {
        release_pointer();
        relayout();
    }
    sig = paint_signature();
    if (sig != g_last_sig) {
        g_last_sig = sig;
        redraw();
    }
    return G_SOURCE_CONTINUE;
}

/* ---- the pointer -------------------------------------------------------------- */

static void dpad_to_pointer(const nes_control *ctl, double x, double y)
{
    const double cx = px(ctl->x + ctl->w / 2), cy = py(ctl->y + ctl->h / 2);
    nessession_pointer_set(g_session, ctl->port,
        nessession_dpad_from_vector((float)((x - cx) / SCALE), (float)((y - cy) / SCALE)));
}

static void on_drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer data)
{
    const int idx = nessession_controls_hit(g_view[0], g_view[1], (float)((x - g_offx) / SCALE),
                                            (float)((y - MARGIN) / SCALE));
    const nes_control *ctl = part(idx);
    (void)gesture; (void)data;
    if (!ctl)
        return;
    if (g_map_state != -2) {
        int target = map_target_at(idx, x, y);
        if (target >= 0)
            set_map_state(target);
        return;
    }
    release_pointer();
    g_drag_idx = idx;
    if (ctl->kind == NES_CTL_DPAD)
        dpad_to_pointer(ctl, x, y);
    else
        nessession_press(g_session, ctl->target, 1);
    redraw();
}

static void on_drag_update(GtkGestureDrag *gesture, double dx, double dy, gpointer data)
{
    const nes_control *ctl = part(g_drag_idx);
    double sx, sy;
    (void)data;
    if (!ctl || ctl->kind != NES_CTL_DPAD || g_map_state != -2)
        return;
    gtk_gesture_drag_get_start_point(gesture, &sx, &sy);
    dpad_to_pointer(ctl, sx + dx, sy + dy);
}

static void on_drag_end(GtkGestureDrag *gesture, double dx, double dy, gpointer data)
{
    (void)gesture; (void)dx; (void)dy; (void)data;
    release_pointer();       /* the pointer source holds the cross and every button */
}

static void on_cancelled(GtkGesture *gesture, GdkEventSequence *seq, gpointer data)
{
    (void)gesture; (void)seq; (void)data;
    release_pointer();
}

/* ---- the ports' types ------------------------------------------------------ */

static void on_type_changed(GObject *obj, GParamSpec *spec, gpointer user_data)
{
    const int port = GPOINTER_TO_INT(user_data);
    (void)spec;
    if (g_syncing)
        return;
    nessession_set_port_type(g_session, port,
        (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(obj)));
}

static void refresh_types(void)
{
    int port;
    for (port = 0; port < 2; port++) {
        const int type = nessession_port_type(g_session, port);
        if ((int)gtk_drop_down_get_selected(GTK_DROP_DOWN(g_type_combo[port])) != type) {
            g_syncing = TRUE;
            gtk_drop_down_set_selected(GTK_DROP_DOWN(g_type_combo[port]), (guint)type);
            g_syncing = FALSE;
        }
    }
}

static GtkWidget *build_type_half(int port)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new("Plugged in:");
    const char *names[NES_CTRL_COUNT + 1];
    int i;

    gtk_widget_add_css_class(label, "dim-label");
    for (i = 0; i < NES_CTRL_COUNT; i++)
        names[i] = nes_ctrl_type_name(i);
    names[NES_CTRL_COUNT] = NULL;
    g_type_combo[port] = gtk_drop_down_new_from_strings(names);
    gtk_widget_set_focus_on_click(g_type_combo[port], FALSE);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(g_type_combo[port]),
                               (guint)nessession_port_type(g_session, port));
    g_signal_connect(g_type_combo[port], "notify::selected",
                     G_CALLBACK(on_type_changed), GINT_TO_POINTER(port));
    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), g_type_combo[port]);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    return box;
}

/* ---- gamepads ------------------------------------------------------------- */

static void drives_text(int idx, char *text, int size)
{
    const int effective = nessession_gamepad_effective_port(g_session, idx);
    if (effective >= 0) g_snprintf(text, size, "drives player %d", effective + 1);
    else g_snprintf(text, size, "drives nothing");
}

static void refresh_pad_ports(void)
{
    int i;
    for (i = 0; i < g_npad_rows; i++) {
        char text[40];
        drives_text(i, text, sizeof text);
        if (strcmp(gtk_label_get_text(GTK_LABEL(g_pad_port[i])), text) != 0)
            gtk_label_set_text(GTK_LABEL(g_pad_port[i]), text);
    }
}

static void on_assign_changed(GObject *obj, GParamSpec *spec, gpointer user_data)
{
    const int idx = GPOINTER_TO_INT(user_data);
    const guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    (void)spec;
    if (g_syncing)
        return;
    /* 0 = automatic (connection order), 1 = player 1, 2 = player 2 */
    nessession_gamepad_assign(g_session, idx, sel == 0 ? -1 : (int)sel - 1);
    refresh_pad_ports();
}

/* Rebuilt whenever a pad comes or goes (the session's generation counter): one
 * row per pad, its name, the player it drives and the player it is set to. */
static void refresh_pads(void)
{
    static const char *const choices[] = { "Automatic", "Player 1", "Player 2", NULL };
    GtkWidget *child;
    int count, i;

    while ((child = gtk_widget_get_first_child(g_pad_box)) != NULL)
        gtk_box_remove(GTK_BOX(g_pad_box), child);
    g_npad_rows = 0;

    count = nessession_gamepad_count(g_session);
    if (count == 0) {
        GtkWidget *none = gtk_label_new("No gamepads connected");
        gtk_widget_add_css_class(none, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(none), 0);
        gtk_box_append(GTK_BOX(g_pad_box), none);
        return;
    }
    for (i = 0; i < count; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *name, *dropdown, *effective;
        char text[128], drives[40];
        const int assign = nessession_gamepad_assignment(g_session, i);

        nessession_gamepad_name(g_session, i, text, sizeof text);
        name = gtk_label_new(text);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(name), 24);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 40);
        gtk_widget_set_tooltip_text(name, text);
        dropdown = gtk_drop_down_new_from_strings(choices);
        g_syncing = TRUE;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown),
                                   assign < 0 || assign > 1 ? 0 : (guint)assign + 1);
        g_syncing = FALSE;
        gtk_widget_set_focus_on_click(dropdown, FALSE);
        g_signal_connect(dropdown, "notify::selected", G_CALLBACK(on_assign_changed),
                         GINT_TO_POINTER(i));
        drives_text(i, drives, sizeof drives);
        effective = gtk_label_new(drives);
        gtk_widget_add_css_class(effective, "dim-label");
        gtk_label_set_width_chars(GTK_LABEL(effective), 16);
        gtk_label_set_xalign(GTK_LABEL(effective), 0);
        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), effective);
        gtk_box_append(GTK_BOX(row), dropdown);
        gtk_box_append(GTK_BOX(g_pad_box), row);
        if (g_npad_rows < MAX_PADS)
            g_pad_port[g_npad_rows++] = effective;
    }
}

/* ---- the expansion-port keyboard ----------------------------------------- */

static void kbd_extent(int type, int *width, int *height)
{
    float w, h;
    *width = *height = 0;
    if (nessession_keyboard_skin_layout(type, NULL, &w, &h)) {
        *width = (int)(w * KBD_SCALE + 2 * KBD_MARGIN + 0.999);
        *height = (int)(h * KBD_SCALE + 2 * KBD_MARGIN + 0.999);
    }
}

/* The cursor keys' legends are arrows (U+2190..2193): drawn, so they do not
 * depend on the font having them. Returns whether the label was one. */
static gboolean kbd_draw_arrow(cairo_t *cr, const char *label, double cx, double cy, double size)
{
    static const double dir_x[4] = { -1, 0, 1, 0 }, dir_y[4] = { 0, -1, 0, 1 };
    const unsigned char *bytes = (const unsigned char *)label;
    int dir;
    double px_, py_;
    if (!(bytes[0] == 0xE2 && bytes[1] == 0x86 && bytes[2] >= 0x90 && bytes[2] <= 0x93 && !bytes[3]))
        return FALSE;
    dir = bytes[2] - 0x90;
    px_ = -dir_y[dir];            /* perpendicular */
    py_ = dir_x[dir];
    cairo_move_to(cr, cx + dir_x[dir] * size, cy + dir_y[dir] * size);
    cairo_line_to(cr, cx - dir_x[dir] * size * 0.7 + px_ * size, cy - dir_y[dir] * size * 0.7 + py_ * size);
    cairo_line_to(cr, cx - dir_x[dir] * size * 0.7 - px_ * size, cy - dir_y[dir] * size * 0.7 - py_ * size);
    cairo_close_path(cr);
    cairo_fill(cr);
    return TRUE;
}

/* Text through Pango (so katakana falls back to a font that has it): the
 * ink-box of the text is anchored at (ax, ay) by fractions (0 left/top .. 1
 * right/bottom) and shrunk to maxw. */
static void pango_put(cairo_t *cr, const char *text, double ax, double ay, double fx, double fy,
                      double size, double maxw, gboolean bold)
{
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_new();
    PangoRectangle ink;
    double scale = 1;
    pango_font_description_set_family(font, "Sans");
    pango_font_description_set_weight(font, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    if (maxw > 0 && ink.width > maxw)
        scale = maxw / ink.width;
    cairo_save(cr);
    cairo_translate(cr, ax, ay);
    cairo_scale(cr, scale, scale);
    cairo_move_to(cr, -ink.x - ink.width * fx, -ink.y - ink.height * fy);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);
    pango_font_description_free(font);
    g_object_unref(layout);
}

static unsigned shade(unsigned hex, double factor)
{
    unsigned red = (hex >> 16) & 255, green = (hex >> 8) & 255, blue = hex & 255;
    red = (unsigned)fmin(255, red * factor);
    green = (unsigned)fmin(255, green * factor);
    blue = (unsigned)fmin(255, blue * factor);
    return (red << 16) | (green << 8) | blue;
}

/* One keycap: darker body, lighter bevelled face, the legend(s). */
static void kbd_draw_cap(cairo_t *cr, const nes_skin_item *item, double x, double y,
                         double w, double h, gboolean lit)
{
    const double unit = KBD_SCALE;
    const double inset = 0.04 * unit;
    const double kx = x + inset, ky = y + inset, kw = w - 2 * inset, kh = h - 2 * inset;
    const double face_x = kx + 0.07 * unit, face_w = kw - 0.14 * unit, face_h = kh - 0.16 * unit;
    cairo_pattern_t *pat;
    char *shifted = NULL, *kana = NULL;

    rgba(cr, 0x000000, 0.45);
    rrect(cr, kx, ky + 1.5, kw, kh, 0.12 * unit);
    cairo_fill(cr);
    rgb(cr, shade(item->fill_rgb, 0.72));
    rrect(cr, kx, ky, kw, kh, 0.12 * unit);
    cairo_fill(cr);
    pat = cairo_pattern_create_linear(0, ky, 0, ky + face_h);
    stop_rgb(pat, 0, shade(item->fill_rgb, 1.10));
    stop_rgb(pat, 1, item->fill_rgb);
    cairo_set_source(cr, pat);
    rrect(cr, face_x, ky, face_w, face_h, 0.10 * unit);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);

    rgb(cr, item->ink_rgb);
    if (kbd_draw_arrow(cr, item->label, kx + kw / 2, ky + face_h / 2, 0.2 * unit)) {
        /* drawn */
    } else if (strchr(item->label, ' ')) {          /* CLR HOME: two lines */
        char first[16], *space;
        g_strlcpy(first, item->label, sizeof first);
        space = strchr(first, ' ');
        *space = '\0';
        pango_put(cr, first, kx + kw / 2, ky + face_h * 0.36, 0.5, 0.5, 0.30 * unit, kw - 4, FALSE);
        pango_put(cr, space + 1 - first + item->label, kx + kw / 2, ky + face_h * 0.68, 0.5, 0.5,
                  0.30 * unit, kw - 4, FALSE);
    } else if (item->text && *item->text) {
        const char *bar = strchr(item->text, '|');
        if (bar) {
            shifted = g_strndup(item->text, (gsize)(bar - item->text));
            kana = g_strdup(bar + 1);
        }
        pango_put(cr, item->label, face_x + 0.10 * unit, ky + face_h * 0.62, 0, 0.5,
                  0.42 * unit, face_w * 0.55, FALSE);
        if (shifted && *shifted)
            pango_put(cr, shifted, face_x + face_w - 0.06 * unit, ky + face_h * 0.30, 1, 0.5,
                      0.28 * unit, face_w * 0.4, FALSE);
        if (kana && *kana)
            pango_put(cr, kana, face_x + face_w - 0.06 * unit, ky + face_h * 0.78, 1, 0.5,
                      0.28 * unit, face_w * 0.4, FALSE);
        g_free(shifted);
        g_free(kana);
    } else {
        pango_put(cr, item->label, kx + kw / 2, ky + face_h / 2, 0.5, 0.5,
                  strlen(item->label) > 2 ? 0.34 * unit : 0.42 * unit, kw - 6, FALSE);
    }
    if (lit)
        held_ring_rect(cr, kx, ky, kw, kh, 0.12 * unit);
}

static void kbd_draw_plate(cairo_t *cr, const nes_skin_item *item, double x, double y,
                           double w, double h)
{
    rgb(cr, item->fill_rgb);
    rrect(cr, x, y, w, h, 0.12 * KBD_SCALE);
    cairo_fill(cr);
    if (item->text) {
        rgb(cr, item->ink_rgb ? item->ink_rgb : NESSESSION_FB_GOLD_RGB);
        text_fit(cr, item->text, x + w / 2, y + h / 2, w - 8, 11, CAIRO_FONT_SLANT_NORMAL,
                 CAIRO_FONT_WEIGHT_BOLD);
    }
}

static void kbd_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
    int i;
    (void)area; (void)data;
    if (!g_skin)
        return;
    g_kbd_offx = (width - g_skin_w * KBD_SCALE) / 2;
    g_kbd_offy = (height - g_skin_h * KBD_SCALE) / 2;

    for (i = 0; i < g_skin_count; i++) {
        const nes_skin_item *item = &g_skin[i];
        const double x = g_kbd_offx + item->x * KBD_SCALE, y = g_kbd_offy + item->y * KBD_SCALE;
        const double w = item->w * KBD_SCALE, h = item->h * KBD_SCALE;

        if (item->kind == NES_SKIN_BODY) {
            if (!strcmp(item->label, "Case")) {
                rgba(cr, 0x000000, 0.25);
                rrect(cr, x, y + 3, w, h, 0.5 * KBD_SCALE);
                cairo_fill(cr);
                rgb(cr, item->fill_rgb);
                rrect(cr, x, y, w, h, 0.5 * KBD_SCALE);
                cairo_fill(cr);
                rgba(cr, 0x000000, 0.18);
                cairo_set_line_width(cr, 1);
                rrect(cr, x + 0.5, y + 0.5, w - 1, h - 1, 0.5 * KBD_SCALE);
                cairo_stroke(cr);
            } else if (!strcmp(item->label, "Vent")) {
                const int slits = 14;
                int slit;
                rgb(cr, item->fill_rgb);
                for (slit = 0; slit < slits; slit++) {
                    const double sx = x + (slit + 0.25) * w / slits;
                    cairo_rectangle(cr, sx, y, w / slits * 0.45, h);
                    cairo_fill(cr);
                }
            } else if (!strcmp(item->label, "Well")) {
                rgb(cr, item->fill_rgb);
                rrect(cr, x, y, w, h, 0.12 * KBD_SCALE);
                cairo_fill_preserve(cr);
                rgba(cr, item->ink_rgb, 0.5);
                cairo_set_line_width(cr, 1);
                cairo_stroke(cr);
            } else if (!strcmp(item->label, "Slot")) {
                rgb(cr, item->fill_rgb);
                rrect(cr, x, y, w, h, 0.1 * KBD_SCALE);
                cairo_fill(cr);
            } else if (!strcmp(item->label, "Logo")) {
                rgb(cr, item->fill_rgb);
                disc(cr, x + w / 2, y + h / 2, h * 0.45);
                cairo_fill(cr);
                rgb(cr, NESSESSION_SUBOR_CASE_RGB);
                cairo_set_line_width(cr, 2);
                disc(cr, x + w / 2, y + h / 2, h * 0.22);
                cairo_stroke(cr);
            } else {
                kbd_draw_plate(cr, item, x, y, w, h);
            }
        } else {
            kbd_draw_cap(cr, item, x, y, w, h, nessession_keyboard_held(g_session, item->key) != 0);
        }
    }
}

static void kbd_release_mouse(void)
{
    if (g_kbd_mouse >= 0) {
        nessession_keyboard_press(g_session, g_kbd_mouse, 0);
        g_kbd_mouse = -1;
        gtk_widget_queue_draw(g_kbd_area);
    }
}

static void on_kbd_pressed(GtkGestureClick *gesture, int presses, double x, double y,
                           gpointer data)
{
    const int idx = g_skin ? nessession_keyboard_skin_hit(g_kbd_type,
                                 (float)((x - g_kbd_offx) / KBD_SCALE),
                                 (float)((y - g_kbd_offy) / KBD_SCALE)) : -1;
    (void)gesture; (void)presses; (void)data;
    kbd_release_mouse();
    if (idx < 0 || g_skin[idx].kind != NES_SKIN_KEYCAP)
        return;
    g_kbd_mouse = g_skin[idx].key;
    nessession_keyboard_press(g_session, g_kbd_mouse, 1);
    gtk_widget_queue_draw(g_kbd_area);
}

static void on_kbd_released(GtkGestureClick *gesture, int presses, double x, double y,
                            gpointer data)
{
    (void)gesture; (void)presses; (void)x; (void)y; (void)data;
    kbd_release_mouse();
}

static void on_kbd_cancelled(GtkGesture *gesture, GdkEventSequence *seq, gpointer data)
{
    (void)gesture; (void)seq; (void)data;
    kbd_release_mouse();
}

/* Follows the session: the type can change in Preferences, and keys are held
 * from the host keyboard too. Redraws only when something changed. */
static void refresh_keyboard(void)
{
    int type, i;
    guint64 sig;
    if (!g_kbd_area)
        return;
    type = nessession_keyboard(g_session);
    if (type != g_kbd_type) {
        g_kbd_mouse = -1;           /* changing keyboards let go of everything */
        g_kbd_type = type;
        g_skin = nessession_keyboard_skin_layout(type, &g_skin_count, &g_skin_w, &g_skin_h);
        g_syncing = TRUE;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(g_kbd_combo), (guint)type);
        g_syncing = FALSE;
        gtk_stack_set_visible_child_name(GTK_STACK(g_kbd_stack),
                                         g_skin ? "keys" : "none");
    }
    {
        const gboolean captures = nessession_keyboard_captures(g_session) != 0;
        const char *text = !g_skin ? ""
            : captures ? "Keyboard mode is on: typing goes to the keyboard. Scroll Lock switches it off."
                       : "Keyboard mode is off: typing drives the controllers. Scroll Lock switches it on.";
        if (strcmp(gtk_label_get_text(GTK_LABEL(g_kbd_mode_label)), text) != 0)
            gtk_label_set_text(GTK_LABEL(g_kbd_mode_label), text);
    }
    sig = (guint64)(type + 1);
    if (g_skin)
        for (i = 0; i < g_skin_count; i++)
            if (g_skin[i].kind == NES_SKIN_KEYCAP)
                sig = sig * 1099511628211ull + (guint64)(nessession_keyboard_held(g_session,
                                                                 g_skin[i].key) ? i + 7 : 3);
    if (sig != g_kbd_last_sig) {
        g_kbd_last_sig = sig;
        gtk_widget_queue_draw(g_kbd_area);
    }
}

static gboolean kbd_tick_cb(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
    (void)widget; (void)clock; (void)data;
    refresh_keyboard();
    return G_SOURCE_CONTINUE;
}

static void on_kbd_type_changed(GObject *obj, GParamSpec *spec, gpointer data)
{
    (void)spec; (void)data;
    if (g_syncing)
        return;
    kbd_release_mouse();
    nessession_set_keyboard(g_session, (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(obj)));
    refresh_keyboard();
}

static GtkWidget *build_keyboard_page(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new("Expansion port:");
    GtkWidget *placeholder;
    GtkGesture *click;
    const char *names[NES_KBD_COUNT + 1];
    int i, fbw, fbh, suw, suh;

    for (i = 0; i < NES_KBD_COUNT; i++) names[i] = nes_keyboard_name(i);
    names[NES_KBD_COUNT] = NULL;
    gtk_widget_add_css_class(label, "dim-label");
    g_kbd_combo = gtk_drop_down_new_from_strings(names);
    gtk_widget_set_focus_on_click(g_kbd_combo, FALSE);
    g_signal_connect(g_kbd_combo, "notify::selected", G_CALLBACK(on_kbd_type_changed), NULL);
    gtk_box_append(GTK_BOX(row), label);
    gtk_box_append(GTK_BOX(row), g_kbd_combo);
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);

    g_kbd_area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_kbd_area), kbd_draw, NULL, NULL);
    /* big enough for either keyboard, so the window never changes size */
    kbd_extent(NES_KBD_FAMILY_BASIC, &fbw, &fbh);
    kbd_extent(NES_KBD_SUBOR, &suw, &suh);
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(g_kbd_area), fbw > suw ? fbw : suw);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g_kbd_area), fbh > suh ? fbh : suh);
    gtk_widget_set_halign(g_kbd_area, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(g_kbd_area, GTK_ALIGN_CENTER);
    click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(on_kbd_pressed), NULL);
    g_signal_connect(click, "released", G_CALLBACK(on_kbd_released), NULL);
    g_signal_connect(click, "cancel", G_CALLBACK(on_kbd_cancelled), NULL);
    gtk_widget_add_controller(g_kbd_area, GTK_EVENT_CONTROLLER(click));
    gtk_widget_add_tick_callback(g_kbd_area, kbd_tick_cb, NULL, NULL);

    placeholder = gtk_label_new("No keyboard attached \xe2\x80\x94 choose one above or in Preferences");
    gtk_widget_add_css_class(placeholder, "dim-label");
    gtk_widget_set_vexpand(placeholder, TRUE);

    g_kbd_stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(g_kbd_stack), g_kbd_area, "keys");
    gtk_stack_add_named(GTK_STACK(g_kbd_stack), placeholder, "none");
    gtk_stack_set_hhomogeneous(GTK_STACK(g_kbd_stack), TRUE);
    gtk_stack_set_vhomogeneous(GTK_STACK(g_kbd_stack), TRUE);
    gtk_widget_set_vexpand(g_kbd_stack, TRUE);

    g_kbd_mode_label = gtk_label_new("");
    gtk_widget_add_css_class(g_kbd_mode_label, "dim-label");

    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), g_kbd_stack);
    gtk_box_append(GTK_BOX(box), g_kbd_mode_label);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);

    g_kbd_type = -1;
    refresh_keyboard();
    return box;
}

/* ---- timers --------------------------------------------------------------- */

/* Every 250 ms while the window is on screen: hot-plugged pads and the ports'
 * types. */
static gboolean info_tick(gpointer data)
{
    unsigned gen;
    (void)data;
    if (!g_window || !gtk_widget_get_visible(g_window)) return G_SOURCE_CONTINUE;
    refresh_types();
    gen = nessession_gamepad_generation(g_session);
    if (gen != g_pad_generation) {
        g_pad_generation = gen;
        refresh_pads();
    } else {
        refresh_pad_ports();
    }
    return G_SOURCE_CONTINUE;
}

/* ---- keyboard ------------------------------------------------------------- */

static gboolean on_key(guint keyval, guint code, GdkModifierType state, gboolean down)
{
    guint32 keysym = code >= 8 ? nessession_keysym_from_evdev(code - 8) : 0;
    int sysact;

    /* an open drop-down (a type or a gamepad's player) has the arrows and Return */
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(g_window));
    if (focus && gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER))
        return FALSE;
    if (!keysym) keysym = keyval;

    /* F9, F11, F12 and Ctrl chords are never Map's: they go where they always do */
    if (g_map_state != -2 && !(keyval == GDK_KEY_F9 || keyval == GDK_KEY_F11 ||
                               keyval == GDK_KEY_F12 || (state & GDK_CONTROL_MASK))) {
        if (g_swallow_code && code == g_swallow_code) {
            if (!down) g_swallow_code = 0;
            return TRUE;
        }
        if (!down)
            return TRUE;
        if (keyval == GDK_KEY_Escape) {
            set_map_state(g_map_state >= 0 ? -1 : -2);
            g_swallow_code = code;      /* after: leaving Map clears it */
            return TRUE;
        }
        if (g_map_state < 0)
            return TRUE;           /* armed, nothing picked: keys do nothing */
        {
            char stolen[128], msg[200], name[32];
            if (nessession_key_reserved(keysym)) {
                set_hint("F9, F11 and F12 can't be bound");
                g_swallow_code = code;
                return TRUE;
            }
            stolen[0] = '\0';
            nessession_binding_set_key(g_session, g_map_state, keysym, stolen, sizeof stolen);
            nessession_keysym_name(keysym, name, sizeof name);
            g_swallow_code = code;
            set_map_state(-1);     /* back to armed */
            if (stolen[0])
                g_snprintf(msg, sizeof msg, "Bound %s (was %s)", name, stolen);
            else
                g_snprintf(msg, sizeof msg, "Bound %s", name);
            set_hint(msg);
            return TRUE;
        }
    }

    /* Otherwise behave exactly like the main window, so playing works
     * whichever window has focus. Typing goes to the expansion-port keyboard
     * while keyboard mode is on: F9 and Escape included. */
    if (nessession_keyboard_captures(g_session)) {
        if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK))
            return FALSE;
        nessession_key(g_session, keysym, down);
        return TRUE;
    }
    if (keyval == GDK_KEY_F9 || keyval == GDK_KEY_F11 || keyval == GDK_KEY_F12) {
        if (down) {
            if (keyval == GDK_KEY_F9)
                nes_controllers_window_toggle(g_parent, g_session);
            else if (keyval == GDK_KEY_F12 && g_parent)
                nes_debugger_toggle(g_parent, g_session);
            else if (keyval == GDK_KEY_F11 && g_parent)
                gtk_widget_activate_action(GTK_WIDGET(g_parent), "win.fullscreen", NULL);
        }
        return TRUE;
    }
    if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK))
        return FALSE;
    sysact = nessession_key_sysaction(g_session, keysym);
    if (sysact >= 0) {
        if (!down)
            g_sysact_down[sysact] = FALSE;
        else if (!g_sysact_down[sysact]) {
            g_sysact_down[sysact] = TRUE;
            if (g_parent)
                nes_window_run_sysaction(NES_WINDOW(g_parent), sysact);
        }
        return TRUE;
    }
    return nessession_key(g_session, keysym, down) ? TRUE : FALSE;
}

static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                               guint code, GdkModifierType state, gpointer data)
{
    (void)ctrl; (void)data;
    return on_key(keyval, code, state, TRUE);
}

static void on_key_released(GtkEventControllerKey *ctrl, guint keyval,
                            guint code, GdkModifierType state, gpointer data)
{
    (void)ctrl; (void)data;
    on_key(keyval, code, state, FALSE);
}

/* Keys and mouse buttons held into a focus change must not stay down: the
 * release goes to another window. Keys typed here are the keyboard source
 * (released the way the main window releases its own). */
static void let_go(void)
{
    release_pointer();
    kbd_release_mouse();
    nessession_release_keyboard(g_session);
    memset(g_sysact_down, 0, sizeof g_sysact_down);
}

static void on_focus_leave(GtkEventControllerFocus *ctrl, gpointer data)
{
    (void)ctrl; (void)data;
    let_go();
    g_swallow_code = 0;
}

static void on_active_changed(GObject *obj, GParamSpec *spec, gpointer data)
{
    (void)spec; (void)data;
    if (!gtk_window_is_active(GTK_WINDOW(obj)))
        let_go();
}

/* ---- the window ----------------------------------------------------------- */

static void on_map_clicked(GtkButton *button, gpointer data)
{
    (void)button; (void)data;
    set_map_state(g_map_state == -2 ? -1 : -2);
}

static void on_clear_clicked(GtkButton *button, gpointer data)
{
    char msg[160];
    (void)button; (void)data;
    if (g_map_state < 0) return;
    nessession_binding_clear(g_session, g_map_state);
    g_snprintf(msg, sizeof msg, "Cleared %s", nes_target_name(g_map_state));
    set_hint(msg);
    redraw();
}

static void on_defaults_clicked(GtkButton *button, gpointer data)
{
    int i;
    const int count = nessession_gamepad_count(g_session);
    (void)button; (void)data;
    nessession_bindings_reset(g_session);
    for (i = 0; i < count; i++)
        nessession_gamepad_assign(g_session, i, -1);
    refresh_pads();
    set_hint("Back to the defaults");
    redraw();
}

static void on_show_toggled(GtkCheckButton *button, gpointer data)
{
    (void)data;
    g_show_bindings = gtk_check_button_get_active(button);
    nessession_set_controls_show_bindings(g_session, g_show_bindings);
    redraw();
}

static void on_reset_config_clicked(GtkButton *button, gpointer data)
{
    (void)button; (void)data;
    if (g_parent)
        nes_window_run_sysaction(NES_WINDOW(g_parent), NES_SYSACT_RESET_CONFIG);
    else
        nessession_sysaction(g_session, NES_SYSACT_RESET_CONFIG);
}

static void on_stop_clicked(GtkButton *button, gpointer data)
{
    (void)button; (void)data;
    if (g_parent)
        nes_window_run_sysaction(NES_WINDOW(g_parent), NES_SYSACT_PAUSE);
    else
        nessession_sysaction(g_session, NES_SYSACT_PAUSE);
}

/* The system keys are not on the picture: picking one here arms Map for it. */
static void on_system_changed(GObject *obj, GParamSpec *spec, gpointer data)
{
    const guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    (void)spec; (void)data;
    if (g_syncing || sel == 0 || sel == GTK_INVALID_LIST_POSITION)
        return;
    set_map_state(NES_TARGET_SYSACT((int)sel - 1));
    g_syncing = TRUE;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(obj), 0);
    g_syncing = FALSE;
}

static void hide_window(void)
{
    set_map_state(-2);
    let_go();
    gtk_widget_set_visible(g_window, FALSE);
}

static gboolean on_close(GtkWindow *window, gpointer data)
{
    (void)window; (void)data;
    hide_window();
    return TRUE;
}

/* The info timer runs only while the window is on screen: closing only hides
 * it, and a hidden window has nobody to update. */
static void on_window_map(GtkWidget *widget, gpointer data)
{
    (void)widget; (void)data;
    release_pointer();
    if (!g_info_timer) g_info_timer = g_timeout_add(250, info_tick, NULL);
}

static void on_window_unmap(GtkWidget *widget, gpointer data)
{
    (void)widget; (void)data;
    let_go();
    if (g_info_timer) g_source_remove(g_info_timer);
    g_info_timer = 0;
}

static void on_page_changed(GObject *obj, GParamSpec *spec, gpointer data)
{
    (void)obj; (void)spec; (void)data;
    /* Map belongs to the controllers page */
    if (g_map_state != -2)
        set_map_state(-2);
    let_go();
}

static void on_destroy(GtkWidget *widget, gpointer data)
{
    (void)widget; (void)data;
    if (g_capture_timer) g_source_remove(g_capture_timer);
    if (g_info_timer) g_source_remove(g_info_timer);
    g_capture_timer = g_info_timer = 0;
    g_window = NULL;
    g_area = NULL;
    g_kbd_area = NULL;
    g_npad_rows = 0;
    g_map_state = -2;
    g_view[0] = g_view[1] = -1;
    g_kbd_type = -1;
}

static GtkWidget *plain_button(const char *label, GCallback callback)
{
    GtkWidget *button = gtk_button_new_with_label(label);
    gtk_widget_set_focus_on_click(button, FALSE);
    g_signal_connect(button, "clicked", callback, NULL);
    return button;
}

static GtkWidget *build_controllers_page(void)
{
    GtkWidget *root, *maprow, *typerow, *sysrow, *padtitle, *padscroll;
    GtkStringList *system;
    GtkGesture *drag;
    int i;

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(root, 8);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);

    g_area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_area), draw_all, NULL, NULL);
    gtk_widget_set_halign(g_area, GTK_ALIGN_CENTER);
    gtk_widget_set_focusable(g_area, FALSE);
    drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), NULL);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), NULL);
    g_signal_connect(drag, "cancel", G_CALLBACK(on_cancelled), NULL);
    gtk_widget_add_controller(g_area, GTK_EVENT_CONTROLLER(drag));
    gtk_widget_add_tick_callback(g_area, tick_cb, NULL, NULL);
    gtk_box_append(GTK_BOX(root), g_area);
    relayout();

    /* each port's type, under its picture */
    typerow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_set_homogeneous(GTK_BOX(typerow), TRUE);
    gtk_widget_set_size_request(typerow, (int)(g_lw * SCALE + 2 * MARGIN), -1);
    gtk_widget_set_halign(typerow, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(typerow), build_type_half(0));
    gtk_box_append(GTK_BOX(typerow), build_type_half(1));
    gtk_box_append(GTK_BOX(root), typerow);

    maprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_map_button = plain_button("Map", G_CALLBACK(on_map_clicked));
    gtk_widget_set_size_request(g_map_button, 72, -1);
    g_clear_button = plain_button("Clear", G_CALLBACK(on_clear_clicked));
    g_defaults_button = plain_button("Defaults", G_CALLBACK(on_defaults_clicked));
    g_show_check = gtk_check_button_new_with_label("Show bindings");
    g_show_bindings = nessession_controls_show_bindings(g_session) != 0;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_show_check), g_show_bindings);
    gtk_widget_set_focus_on_click(g_show_check, FALSE);
    g_signal_connect(g_show_check, "toggled", G_CALLBACK(on_show_toggled), NULL);
    system = gtk_string_list_new(NULL);
    gtk_string_list_append(system, "System keys\xe2\x80\xa6");
    for (i = 0; i < NES_SYSACT_COUNT; i++)
        gtk_string_list_append(system, nes_target_name(NES_TARGET_SYSACT(i)));
    g_system_dd = gtk_drop_down_new(G_LIST_MODEL(system), NULL);
    gtk_widget_set_focus_on_click(g_system_dd, FALSE);
    gtk_widget_set_halign(g_system_dd, GTK_ALIGN_END);
    gtk_widget_set_hexpand(g_system_dd, TRUE);
    gtk_widget_set_tooltip_text(g_system_dd,
        "Reset to CONFIG and the debugger stop are not on the picture: pick one to remap it");
    g_signal_connect(g_system_dd, "notify::selected", G_CALLBACK(on_system_changed), NULL);
    gtk_box_append(GTK_BOX(maprow), g_map_button);
    gtk_box_append(GTK_BOX(maprow), g_clear_button);
    gtk_box_append(GTK_BOX(maprow), g_defaults_button);
    gtk_box_append(GTK_BOX(maprow), g_show_check);
    gtk_box_append(GTK_BOX(maprow), g_system_dd);
    gtk_box_append(GTK_BOX(root), maprow);

    g_map_hint = gtk_label_new("");
    gtk_widget_add_css_class(g_map_hint, "dim-label");
    gtk_widget_set_hexpand(g_map_hint, TRUE);
    /* a long hint must not widen the window */
    gtk_label_set_ellipsize(GTK_LABEL(g_map_hint), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(g_map_hint), 24);
    gtk_label_set_max_width_chars(GTK_LABEL(g_map_hint), 90);
    gtk_label_set_xalign(GTK_LABEL(g_map_hint), 0);
    gtk_box_append(GTK_BOX(root), g_map_hint);

    sysrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(sysrow), plain_button("Reset to CONFIG", G_CALLBACK(on_reset_config_clicked)));
    gtk_box_append(GTK_BOX(sysrow), plain_button("Stop in debugger", G_CALLBACK(on_stop_clicked)));
    gtk_box_append(GTK_BOX(root), sysrow);

    padtitle = gtk_label_new("Gamepads");
    gtk_widget_add_css_class(padtitle, "heading");
    gtk_label_set_xalign(GTK_LABEL(padtitle), 0);
    gtk_box_append(GTK_BOX(root), padtitle);
    g_pad_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    /* two pad rows tall whatever is plugged in (more scroll), so a pad coming
     * or going never resizes the window */
    padscroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(padscroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(padscroll), FALSE);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(padscroll), 84);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(padscroll), 84);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(padscroll), g_pad_box);
    gtk_box_append(GTK_BOX(root), padscroll);
    g_object_unref(system);
    return root;
}

static void build_window(GtkWindow *parent)
{
    GtkWidget *toolbar, *header, *switcher;
    GtkEventController *keys, *focus;
    AdwViewStackPage *page;

    g_parent = parent;
    g_window = adw_window_new();
    gtk_window_set_title(GTK_WINDOW(g_window), "Controllers (F9)");
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(g_window), parent);
        /* Same app-id as the main window, so the shell groups and icons it. */
        gtk_window_set_application(GTK_WINDOW(g_window), gtk_window_get_application(parent));
    }
    gtk_window_set_destroy_with_parent(GTK_WINDOW(g_window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(g_window), FALSE);
    g_signal_connect(g_window, "close-request", G_CALLBACK(on_close), NULL);
    g_signal_connect(g_window, "destroy", G_CALLBACK(on_destroy), NULL);
    g_signal_connect(g_window, "notify::is-active", G_CALLBACK(on_active_changed), NULL);

    g_view_stack = adw_view_stack_new();
    page = adw_view_stack_add_titled(ADW_VIEW_STACK(g_view_stack), build_controllers_page(),
                                     "controllers", "Controllers");
    adw_view_stack_page_set_icon_name(page, "input-gaming-symbolic");
    page = adw_view_stack_add_titled(ADW_VIEW_STACK(g_view_stack), build_keyboard_page(),
                                     "keyboard", "Keyboard");
    adw_view_stack_page_set_icon_name(page, "input-keyboard-symbolic");
    g_signal_connect(g_view_stack, "notify::visible-child", G_CALLBACK(on_page_changed), NULL);

    header = adw_header_bar_new();
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), nes_help_button("controllers"));
    nes_help_set_context(GTK_WINDOW(g_window), "controllers");
    switcher = adw_view_switcher_new();
    adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(switcher), ADW_VIEW_STACK(g_view_stack));
    adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(switcher), ADW_VIEW_SWITCHER_POLICY_WIDE);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), switcher);
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), g_view_stack);
    adw_window_set_content(ADW_WINDOW(g_window), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), NULL);
    gtk_widget_add_controller(g_window, keys);

    focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), NULL);
    gtk_widget_add_controller(g_window, focus);

    g_signal_connect(g_window, "map", G_CALLBACK(on_window_map), NULL);
    g_signal_connect(g_window, "unmap", G_CALLBACK(on_window_unmap), NULL);
    set_map_state(-2);
}

static void show_window(void)
{
    if (g_view[0] != nessession_port_type(g_session, 0) ||
        g_view[1] != nessession_port_type(g_session, 1))
        relayout();
    refresh_types();
    g_pad_generation = nessession_gamepad_generation(g_session);
    refresh_pads();
    refresh_keyboard();
    gtk_window_present(GTK_WINDOW(g_window));
}

void nes_controllers_window_toggle(GtkWindow *parent, nessession *session)
{
    g_session = session;
    if (!g_window)
        build_window(parent);
    else if (parent)
        g_parent = parent;

    if (gtk_widget_get_visible(g_window))
        hide_window();
    else
        show_window();
}

void nes_controllers_window_show_keyboard(GtkWindow *parent, nessession *session)
{
    g_session = session;
    if (!g_window)
        build_window(parent);
    else if (parent)
        g_parent = parent;
    adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(g_view_stack), "keyboard");
    show_window();
}

void nes_controllers_window_show(GtkWindow *parent, nessession *session)
{
    g_session = session;
    if (!g_window)
        build_window(parent);
    else if (parent)
        g_parent = parent;
    adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(g_view_stack), "controllers");
    show_window();
}

gboolean nes_controllers_window_is_visible(void)
{
    return g_window && gtk_widget_get_visible(g_window);
}

/* Bindings changed outside the window (Preferences' Reset Controls): redraw it
 * and rebuild what the pad rows offer. */
void nes_controllers_window_refresh(void)
{
    if (!g_window)
        return;
    g_pad_generation = (unsigned)-1;
    redraw();
}
