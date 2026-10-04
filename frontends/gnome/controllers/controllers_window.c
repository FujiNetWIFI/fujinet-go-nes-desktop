/*
 * NESControllersWindow -- both NES controllers side by side, each laid out
 * like the real one (the cross on the left, Select and Start in the middle,
 * B and A on the right, with the turbo buttons above them), the console's
 * RESET and Reset to CONFIG, Map mode, and the ports' controller types and
 * gamepad assignments.
 *
 * Every control is pressed with a raw GtkGestureClick (press AND release),
 * not GtkButton's "clicked": the machine samples the controller once per
 * frame, so a value present only for the instant of a click falls between
 * frames and a polling game never sees it. A button on screen is HELD for
 * as long as the mouse button is down, exactly like the plastic one.
 *
 * The gesture's "cancel" is wired to the same release path as "released":
 * dragging off a button must not leave the machine believing a button is
 * still down forever.
 *
 * What the console sees held -- keyboard, gamepads and this window together
 * (nessession_buttons_held) -- is painted in the accent colour, so the
 * window doubles as an input tester for a new gamepad.
 *
 * The Keyboard page draws the keyboard on the expansion port from the
 * session's layout (key units, so all four frontends show the same picture):
 * clicking a key holds it while the mouse is down, and every held key --
 * typed or clicked -- is lit the same way.
 *
 * SINGLETON, hidden rather than destroyed, so a remap survives closing it.
 *
 * MAP MODE: the Map button arms a two-step rebind -- click any control to
 * pick the target, then press a keyboard key OR a gamepad button. While
 * armed, the same gestures pick the target instead of injecting input, the
 * key handler intercepts the next keystroke, and a timer polls the session's
 * gamepad capture. Rebinding steals the key from whatever held it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controllers_window.h"

#include "../window.h"

#include <string.h>

/* ---- singleton state ------------------------------------------------------ */

static GtkWidget *g_window;
static nessession *g_session;

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad button to bind to that target. */
static int g_map_state = -2;
static GtkWidget *g_map_button;
static GtkWidget *g_map_hint;
static guint g_capture_timer;
static guint g_held_timer;
static GtkWidget *g_type_combo[2];
static GtkWidget *g_pad_box;
static unsigned g_pad_generation = (unsigned)-1;

/* the Keyboard page */
#define KBD_UNIT 36.0           /* pixels per key unit */
#define KBD_GAP 2.0             /* between key caps */
static GtkWidget *g_view_stack;
static GtkWidget *g_kbd_stack;  /* the drawing, or the "no keyboard" placeholder */
static GtkWidget *g_kbd_area;
static GtkWidget *g_kbd_combo;
static int g_kbd_type = -1;     /* what the page currently shows */
static int g_kbd_mouse = -1;    /* the key index the mouse holds */
static gboolean g_kbd_syncing;
static unsigned char g_kbd_lit[128];

typedef struct {
    GtkWidget *button;
    int target;
} control;

static control g_controls[NES_TARGET_COUNT];
static int g_ncontrols;

static void refresh_labels(void);
static void refresh_keyboard(void);

/* ---- pressing ------------------------------------------------------------- */

static void press_target(int target, int down)
{
    if (target >= NES_TARGET_SYSACT(0)) {
        /* System actions fire on release, like a real button: pressing and
         * dragging off should not power-cycle the cartridge. */
        if (!down)
            nessession_sysaction(g_session, target - NES_TARGET_SYSACT(0));
        return;
    }
    nessession_press(g_session, target, down);
}

static void stop_capture(void)
{
    if (g_capture_timer) {
        g_source_remove(g_capture_timer);
        g_capture_timer = 0;
    }
    nessession_gamepad_capture_cancel(g_session);
}

static gboolean capture_tick(gpointer d)
{
    int button;
    (void)d;
    if (g_map_state < 0) { g_capture_timer = 0; return G_SOURCE_REMOVE; }
    if (nessession_gamepad_capture_poll(g_session, &button)) {
        char stolen[128], msg[200];
        nessession_binding_set_button(g_session, g_map_state, button, stolen, sizeof stolen);
        if (stolen[0])
            g_snprintf(msg, sizeof msg, "Bound %s (was %s)", nes_pad_button_name(button), stolen);
        else
            g_snprintf(msg, sizeof msg, "Bound %s", nes_pad_button_name(button));
        g_capture_timer = 0;
        g_map_state = -1;     /* stay armed: remapping several in a row is normal */
        gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
        refresh_labels();
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void set_map_state(int state)
{
    g_map_state = state;
    if (state == -2) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Map");
        gtk_widget_remove_css_class(g_map_button, "nes-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint), "");
    } else if (state == -1) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Done");
        gtk_widget_add_css_class(g_map_button, "nes-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint), "Click a control to remap");
    } else {
        char msg[128];
        g_snprintf(msg, sizeof msg, "Press a key or gamepad button for %s",
                   nes_target_name(state));
        gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
        nessession_gamepad_capture_begin(g_session);
        if (!g_capture_timer)
            g_capture_timer = g_timeout_add(50, capture_tick, NULL);
    }
    refresh_labels();
}

static void on_pressed(GtkGestureClick *g, int n, double x, double y, gpointer user_data)
{
    int target = GPOINTER_TO_INT(user_data);
    (void)g; (void)n; (void)x; (void)y;

    if (g_map_state == -1) { set_map_state(target); return; }
    if (g_map_state >= 0) return;
    press_target(target, 1);
}

static void on_released(GtkGestureClick *g, int n, double x, double y, gpointer user_data)
{
    int target = GPOINTER_TO_INT(user_data);
    (void)g; (void)n; (void)x; (void)y;
    if (g_map_state != -2) return;
    press_target(target, 0);
}

static void on_cancelled(GtkGesture *g, GdkEventSequence *seq, gpointer user_data)
{
    int target = GPOINTER_TO_INT(user_data);
    (void)g; (void)seq;
    if (g_map_state != -2) return;
    press_target(target, 0);
}

static GtkWidget *control_button(const char *label, int target, int w, int h)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    GtkGesture *g = gtk_gesture_click_new();

    gtk_widget_set_size_request(b, w, h);
    g_signal_connect(g, "pressed", G_CALLBACK(on_pressed), GINT_TO_POINTER(target));
    g_signal_connect(g, "released", G_CALLBACK(on_released), GINT_TO_POINTER(target));
    g_signal_connect(g, "cancel", G_CALLBACK(on_cancelled), GINT_TO_POINTER(target));
    gtk_widget_add_controller(b, GTK_EVENT_CONTROLLER(g));
    gtk_widget_set_focusable(b, FALSE);
    g_object_set_data_full(G_OBJECT(b), "face", g_strdup(label), g_free);

    if (g_ncontrols < NES_TARGET_COUNT) {
        g_controls[g_ncontrols].button = b;
        g_controls[g_ncontrols].target = target;
        g_ncontrols++;
    }
    return b;
}

/* In Map mode each control shows what is bound to it, so choosing what to
 * change does not require remembering the whole table. */
static void refresh_labels(void)
{
    int i;
    for (i = 0; i < g_ncontrols; i++) {
        int t = g_controls[i].target;
        GtkWidget *b = g_controls[i].button;
        if (g_map_state != -2) {
            nes_binding bind = nessession_binding_get(g_session, t);
            char key[32], text[80];
            nessession_keysym_name(bind.keysym, key, sizeof key);
            if (bind.button != NES_PAD_BTN_NONE)
                g_snprintf(text, sizeof text, "%s\n%s", *key ? key : "\xe2\x80\x94",
                           nes_pad_button_name(bind.button));
            else
                g_snprintf(text, sizeof text, "%s", *key ? key : "\xe2\x80\x94");
            gtk_button_set_label(GTK_BUTTON(b), text);
            if (t == g_map_state) gtk_widget_add_css_class(b, "nes-accent");
            else gtk_widget_remove_css_class(b, "nes-accent");
        } else {
            gtk_widget_remove_css_class(b, "nes-accent");
            gtk_button_set_label(GTK_BUTTON(b), g_object_get_data(G_OBJECT(b), "face"));
        }
    }
}

/* What the console sees held, lit in the accent colour. */
static gboolean held_tick(gpointer d)
{
    unsigned held[2];
    int i;
    (void)d;
    if (!g_window || !gtk_widget_get_visible(g_window))
        return G_SOURCE_CONTINUE;
    refresh_keyboard();
    if (g_map_state != -2)
        return G_SOURCE_CONTINUE;
    held[0] = nessession_buttons_held(g_session, 0);
    held[1] = nessession_buttons_held(g_session, 1);
    for (i = 0; i < g_ncontrols; i++) {
        int t = g_controls[i].target;
        gboolean on = FALSE;
        if (t < 2 * NES_ACT_PER_PORT)
            on = (held[t / NES_ACT_PER_PORT] >> (t % NES_ACT_PER_PORT)) & 1;
        if (on) gtk_widget_add_css_class(g_controls[i].button, "nes-accent");
        else gtk_widget_remove_css_class(g_controls[i].button, "nes-accent");
    }
    return G_SOURCE_CONTINUE;
}

/* ---- one controller ------------------------------------------------------- */

static void on_type_changed(GObject *o, GParamSpec *ps, gpointer user_data)
{
    int port = GPOINTER_TO_INT(user_data);
    (void)ps;
    nessession_set_port_type(g_session, port,
        (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(o)));
}

static GtkWidget *build_controller(int port)
{
    GtkWidget *frame = gtk_frame_new(NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    GtkWidget *cross = gtk_grid_new();
    GtkWidget *mid = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *face = gtk_grid_new();
    GtkWidget *title, *typerow, *typelabel;
    const char *names[NES_CTRL_COUNT + 1];
    char label[32];
    int i;

    g_snprintf(label, sizeof label, "Player %d", port + 1);
    title = gtk_label_new(label);
    gtk_widget_add_css_class(title, "heading");

    gtk_grid_set_row_spacing(GTK_GRID(cross), 2);
    gtk_grid_set_column_spacing(GTK_GRID(cross), 2);
    gtk_grid_attach(GTK_GRID(cross), control_button("\xe2\x96\xb2", NES_TARGET_PORT(port, NES_ACT_UP), 44, 40), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(cross), control_button("\xe2\x97\x80", NES_TARGET_PORT(port, NES_ACT_LEFT), 44, 40), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(cross), control_button("\xe2\x96\xb6", NES_TARGET_PORT(port, NES_ACT_RIGHT), 44, 40), 2, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(cross), control_button("\xe2\x96\xbc", NES_TARGET_PORT(port, NES_ACT_DOWN), 44, 40), 1, 2, 1, 1);
    gtk_widget_set_valign(cross, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(mid), control_button("Select", NES_TARGET_PORT(port, NES_ACT_SELECT), 64, 30));
    gtk_box_append(GTK_BOX(mid), control_button("Start", NES_TARGET_PORT(port, NES_ACT_START), 64, 30));
    gtk_widget_set_valign(mid, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(mid, 8);

    gtk_grid_set_row_spacing(GTK_GRID(face), 6);
    gtk_grid_set_column_spacing(GTK_GRID(face), 8);
    gtk_grid_attach(GTK_GRID(face), control_button("Turbo B", NES_TARGET_PORT(port, NES_ACT_TURBO_B), 56, 30), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(face), control_button("Turbo A", NES_TARGET_PORT(port, NES_ACT_TURBO_A), 56, 30), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(face), control_button("B", NES_TARGET_PORT(port, NES_ACT_B), 56, 48), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(face), control_button("A", NES_TARGET_PORT(port, NES_ACT_A), 56, 48), 1, 1, 1, 1);
    gtk_widget_set_valign(face, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(body), cross);
    gtk_box_append(GTK_BOX(body), mid);
    gtk_box_append(GTK_BOX(body), face);
    gtk_widget_set_halign(body, GTK_ALIGN_CENTER);

    typerow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    typelabel = gtk_label_new("Plugged in:");
    gtk_widget_add_css_class(typelabel, "dim-label");
    for (i = 0; i < NES_CTRL_COUNT; i++) names[i] = nes_ctrl_type_name(i);
    names[NES_CTRL_COUNT] = NULL;
    g_type_combo[port] = gtk_drop_down_new_from_strings(names);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(g_type_combo[port]),
                               (guint)nessession_port_type(g_session, port));
    g_signal_connect(g_type_combo[port], "notify::selected",
                     G_CALLBACK(on_type_changed), GINT_TO_POINTER(port));
    gtk_box_append(GTK_BOX(typerow), typelabel);
    gtk_box_append(GTK_BOX(typerow), g_type_combo[port]);
    gtk_widget_set_halign(typerow, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(box), title);
    gtk_box_append(GTK_BOX(box), body);
    gtk_box_append(GTK_BOX(box), typerow);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 10);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_frame_set_child(GTK_FRAME(frame), box);
    return frame;
}

/* ---- gamepads ------------------------------------------------------------- */

static void on_assign_changed(GObject *o, GParamSpec *ps, gpointer user_data)
{
    int idx = GPOINTER_TO_INT(user_data);
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(o));
    (void)ps;
    /* 0 = automatic (connection order), 1 = player 1, 2 = player 2 */
    nessession_gamepad_assign(g_session, idx, sel == 0 ? -1 : (int)sel - 1);
}

/* Rebuilt whenever a pad comes or goes (the session's generation counter):
 * one row per pad, its name and which player it drives. */
static void refresh_pads(void)
{
    static const char *const choices[] = { "Automatic", "Player 1", "Player 2", NULL };
    GtkWidget *child;
    int n, i;

    while ((child = gtk_widget_get_first_child(g_pad_box)) != NULL)
        gtk_box_remove(GTK_BOX(g_pad_box), child);

    n = nessession_gamepad_count(g_session);
    if (n == 0) {
        GtkWidget *none = gtk_label_new("No gamepads connected");
        gtk_widget_add_css_class(none, "dim-label");
        gtk_box_append(GTK_BOX(g_pad_box), none);
        return;
    }
    for (i = 0; i < n; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *name, *dd, *eff;
        char text[128], port[32];
        int assign = nessession_gamepad_assignment(g_session, i);
        int effective = nessession_gamepad_effective_port(g_session, i);

        nessession_gamepad_name(g_session, i, text, sizeof text);
        name = gtk_label_new(text);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        dd = gtk_drop_down_new_from_strings(choices);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), assign < 0 ? 0 : (guint)assign + 1);
        g_signal_connect(dd, "notify::selected", G_CALLBACK(on_assign_changed), GINT_TO_POINTER(i));
        if (effective >= 0) g_snprintf(port, sizeof port, "drives player %d", effective + 1);
        else g_snprintf(port, sizeof port, "drives nothing");
        eff = gtk_label_new(port);
        gtk_widget_add_css_class(eff, "dim-label");
        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), eff);
        gtk_box_append(GTK_BOX(row), dd);
        gtk_box_append(GTK_BOX(g_pad_box), row);
    }
}

static gboolean pad_tick(gpointer d)
{
    unsigned gen;
    (void)d;
    if (!g_window || !gtk_widget_get_visible(g_window)) return G_SOURCE_CONTINUE;
    gen = nessession_gamepad_generation(g_session);
    if (gen != g_pad_generation) {
        g_pad_generation = gen;
        refresh_pads();
    }
    return G_SOURCE_CONTINUE;
}

/* ---- the expansion-port keyboard ----------------------------------------- */

static void layout_extent(int type, double *w, double *h)
{
    const nes_kbd_key *keys = NULL;
    int n = nessession_keyboard_layout(type, &keys), i;
    *w = *h = 0;
    for (i = 0; i < n; i++) {
        if (keys[i].x + keys[i].w > *w) *w = keys[i].x + keys[i].w;
        if (keys[i].y + keys[i].h > *h) *h = keys[i].y + keys[i].h;
    }
}

static void kbd_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer d)
{
    const nes_kbd_key *keys = NULL;
    int n = nessession_keyboard_layout(g_kbd_type, &keys), i;
    double ew, eh, ox, oy;
    GdkRGBA fg;
    PangoLayout *pl = gtk_widget_create_pango_layout(GTK_WIDGET(area), NULL);
    PangoFontDescription *font = pango_font_description_from_string("Sans Bold 8");
    (void)d;

    gtk_widget_get_color(GTK_WIDGET(area), &fg);
    layout_extent(g_kbd_type, &ew, &eh);
    ox = (width - ew * KBD_UNIT) / 2;
    oy = (height - eh * KBD_UNIT) / 2;
    pango_layout_set_font_description(pl, font);
    pango_layout_set_alignment(pl, PANGO_ALIGN_CENTER);
    pango_layout_set_wrap(pl, PANGO_WRAP_WORD);

    for (i = 0; i < n; i++) {
        const nes_kbd_key *k = &keys[i];
        double x = ox + k->x * KBD_UNIT + KBD_GAP / 2, y = oy + k->y * KBD_UNIT + KBD_GAP / 2;
        double w = k->w * KBD_UNIT - KBD_GAP, h = k->h * KBD_UNIT - KBD_GAP;
        gboolean lit = k->index >= 0 && k->index < (int)sizeof g_kbd_lit && g_kbd_lit[k->index];
        int tw, th;

        cairo_new_path(cr);
        cairo_rectangle(cr, x, y, w, h);
        if (lit)
            cairo_set_source_rgb(cr, ((NESSESSION_ACCENT_RGB >> 16) & 0xFF) / 255.0,
                                 ((NESSESSION_ACCENT_RGB >> 8) & 0xFF) / 255.0,
                                 (NESSESSION_ACCENT_RGB & 0xFF) / 255.0);
        else
            cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.08);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.35);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);

        pango_layout_set_width(pl, (int)((w - 4) * PANGO_SCALE));
        pango_layout_set_text(pl, k->label, -1);
        pango_layout_get_pixel_size(pl, &tw, &th);
        if (lit) cairo_set_source_rgb(cr, 0, 0, 0);
        else gdk_cairo_set_source_rgba(cr, &fg);
        cairo_move_to(cr, x + 2, y + (h - th) / 2);
        pango_cairo_show_layout(cr, pl);
    }
    pango_font_description_free(font);
    g_object_unref(pl);
}

static int kbd_hit(double px, double py)
{
    const nes_kbd_key *keys = NULL;
    int n = nessession_keyboard_layout(g_kbd_type, &keys), i;
    double ew, eh, x, y;
    layout_extent(g_kbd_type, &ew, &eh);
    x = (px - (gtk_widget_get_width(g_kbd_area) - ew * KBD_UNIT) / 2) / KBD_UNIT;
    y = (py - (gtk_widget_get_height(g_kbd_area) - eh * KBD_UNIT) / 2) / KBD_UNIT;
    for (i = 0; i < n; i++)
        if (x >= keys[i].x && x < keys[i].x + keys[i].w &&
            y >= keys[i].y && y < keys[i].y + keys[i].h)
            return keys[i].index;
    return -1;
}

static void kbd_release_mouse(void)
{
    if (g_kbd_mouse >= 0) {
        nessession_keyboard_press(g_session, g_kbd_mouse, 0);
        g_kbd_mouse = -1;
        refresh_keyboard();
    }
}

static void on_kbd_pressed(GtkGestureClick *g, int n, double x, double y, gpointer d)
{
    int idx = kbd_hit(x, y);
    (void)g; (void)n; (void)d;
    kbd_release_mouse();
    if (idx < 0) return;
    g_kbd_mouse = idx;
    nessession_keyboard_press(g_session, idx, 1);
    refresh_keyboard();
}

static void on_kbd_released(GtkGestureClick *g, int n, double x, double y, gpointer d)
{
    (void)g; (void)n; (void)x; (void)y; (void)d;
    kbd_release_mouse();
}

static void on_kbd_cancelled(GtkGesture *g, GdkEventSequence *seq, gpointer d)
{
    (void)g; (void)seq; (void)d;
    kbd_release_mouse();
}

static void on_kbd_type_changed(GObject *o, GParamSpec *ps, gpointer d)
{
    (void)ps; (void)d;
    if (g_kbd_syncing) return;
    kbd_release_mouse();
    nessession_set_keyboard(g_session, (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(o)));
    refresh_keyboard();
}

/* Follows the session: the type can change in Preferences, and keys are
 * held from the host keyboard too. Redraws only when something changed. */
static void refresh_keyboard(void)
{
    int type = nessession_keyboard(g_session), i, changed = 0;
    if (!g_kbd_area) return;
    if (type != g_kbd_type) {
        double w, h;
        g_kbd_mouse = -1;       /* changing keyboards let go of everything */
        g_kbd_type = type;
        g_kbd_syncing = TRUE;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(g_kbd_combo), (guint)type);
        g_kbd_syncing = FALSE;
        layout_extent(type, &w, &h);
        gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(g_kbd_area), (int)(w * KBD_UNIT + 0.999));
        gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g_kbd_area), (int)(h * KBD_UNIT + 0.999));
        gtk_stack_set_visible_child_name(GTK_STACK(g_kbd_stack),
                                         type == NES_KBD_NONE ? "none" : "keys");
        memset(g_kbd_lit, 0, sizeof g_kbd_lit);
        changed = 1;
    }
    if (type != NES_KBD_NONE)
        for (i = 0; i < (int)sizeof g_kbd_lit; i++) {
            unsigned char on = nessession_keyboard_held(g_session, i) ? 1 : 0;
            if (on != g_kbd_lit[i]) { g_kbd_lit[i] = on; changed = 1; }
        }
    if (changed) gtk_widget_queue_draw(g_kbd_area);
}

static GtkWidget *build_keyboard_page(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new("Expansion port:");
    GtkWidget *placeholder, *hint;
    GtkGesture *g;
    const char *names[NES_KBD_COUNT + 1];
    int i;

    for (i = 0; i < NES_KBD_COUNT; i++) names[i] = nes_keyboard_name(i);
    names[NES_KBD_COUNT] = NULL;
    gtk_widget_add_css_class(label, "dim-label");
    g_kbd_combo = gtk_drop_down_new_from_strings(names);
    g_signal_connect(g_kbd_combo, "notify::selected", G_CALLBACK(on_kbd_type_changed), NULL);
    gtk_box_append(GTK_BOX(row), label);
    gtk_box_append(GTK_BOX(row), g_kbd_combo);
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);

    g_kbd_area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_kbd_area), kbd_draw, NULL, NULL);
    g = gtk_gesture_click_new();
    g_signal_connect(g, "pressed", G_CALLBACK(on_kbd_pressed), NULL);
    g_signal_connect(g, "released", G_CALLBACK(on_kbd_released), NULL);
    g_signal_connect(g, "cancel", G_CALLBACK(on_kbd_cancelled), NULL);
    gtk_widget_add_controller(g_kbd_area, GTK_EVENT_CONTROLLER(g));

    placeholder = gtk_label_new("No keyboard attached \xe2\x80\x94 choose one in Preferences");
    gtk_widget_add_css_class(placeholder, "dim-label");
    gtk_widget_set_vexpand(placeholder, TRUE);

    g_kbd_stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(g_kbd_stack), g_kbd_area, "keys");
    gtk_stack_add_named(GTK_STACK(g_kbd_stack), placeholder, "none");
    gtk_widget_set_vexpand(g_kbd_stack, TRUE);

    hint = gtk_label_new("Click a key to press it. Scroll Lock switches typing between "
                         "the keyboard and the controllers.");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);

    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), g_kbd_stack);
    gtk_box_append(GTK_BOX(box), hint);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);

    g_kbd_type = -1;
    refresh_keyboard();
    return box;
}

/* ---- keyboard ------------------------------------------------------------- */

static gboolean on_key_pressed(GtkEventControllerKey *c, guint keyval,
                               guint code, GdkModifierType st, gpointer d)
{
    guint32 keysym = code >= 8 ? nessession_keysym_from_evdev(code - 8) : 0;
    (void)c; (void)st; (void)d;
    if (!keysym) keysym = keyval;

    if (g_map_state >= 0) {
        char stolen[128], msg[200], name[32];
        nessession_binding_set_key(g_session, g_map_state, keysym, stolen, sizeof stolen);
        nessession_keysym_name(keysym, name, sizeof name);
        set_map_state(-1);   /* stay armed: remapping several in a row is normal */
        if (stolen[0])
            g_snprintf(msg, sizeof msg, "Bound %s (was %s)", name, stolen);
        else
            g_snprintf(msg, sizeof msg, "Bound %s", name);
        gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
        return TRUE;
    }
    if (g_map_state == -1) return TRUE;
    /* typing goes to the expansion-port keyboard: F9 and Escape included */
    if (nessession_keyboard_captures(g_session)) {
        nessession_key(g_session, keysym, 1);
        return TRUE;
    }
    if (keyval == GDK_KEY_F9) {
        nes_controllers_window_toggle(NULL, g_session);
        return TRUE;
    }
    /* Otherwise behave exactly like the main window, so playing works
     * whichever window has focus. */
    {
        int sa = nessession_key_sysaction(g_session, keysym);
        if (sa >= 0) { nessession_sysaction(g_session, sa); return TRUE; }
    }
    return nessession_key(g_session, keysym, 1) ? TRUE : FALSE;
}

static gboolean on_key_released(GtkEventControllerKey *c, guint keyval,
                                guint code, GdkModifierType st, gpointer d)
{
    guint32 keysym = code >= 8 ? nessession_keysym_from_evdev(code - 8) : 0;
    (void)c; (void)st; (void)d;
    if (!keysym) keysym = keyval;
    if (g_map_state != -2) return TRUE;
    return nessession_key(g_session, keysym, 0) ? TRUE : FALSE;
}

/* ---- the window ----------------------------------------------------------- */

static void on_map_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    set_map_state(g_map_state == -2 ? -1 : -2);
}

static void on_defaults_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    nessession_bindings_reset(g_session);
    refresh_labels();
}

static gboolean on_close(GtkWindow *w, gpointer d)
{
    (void)d;
    set_map_state(-2);
    kbd_release_mouse();
    gtk_widget_set_visible(GTK_WIDGET(w), FALSE);
    return TRUE;
}

static void build_window(GtkWindow *parent)
{
    GtkWidget *root, *ports, *system, *maprow, *toolbar, *header, *defaults, *systitle, *padtitle;
    GtkWidget *switcher;
    AdwViewStackPage *page;
    GtkEventController *keys;

    g_window = adw_window_new();
    gtk_window_set_title(GTK_WINDOW(g_window), "Controllers");
    gtk_window_set_transient_for(GTK_WINDOW(g_window), parent);
    /* Same app-id as the main window, so the shell groups and icons it. */
    gtk_window_set_application(GTK_WINDOW(g_window), gtk_window_get_application(parent));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(g_window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(g_window), FALSE);
    g_signal_connect(g_window, "close-request", G_CALLBACK(on_close), NULL);

    g_ncontrols = 0;

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);

    ports = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_append(GTK_BOX(ports), build_controller(0));
    gtk_box_append(GTK_BOX(ports), build_controller(1));
    gtk_box_append(GTK_BOX(root), ports);

    systitle = gtk_label_new("Console");
    gtk_widget_add_css_class(systitle, "heading");
    gtk_box_append(GTK_BOX(root), systitle);
    system = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(system, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(system), control_button("Reset Game",
                   NES_TARGET_SWITCH(NES_SW_RESET), 120, 36));
    gtk_box_append(GTK_BOX(system), control_button("Reset to CONFIG",
                   NES_TARGET_SYSACT(NES_SYSACT_RESET_CONFIG), 140, 36));
    gtk_box_append(GTK_BOX(root), system);

    padtitle = gtk_label_new("Gamepads");
    gtk_widget_add_css_class(padtitle, "heading");
    gtk_box_append(GTK_BOX(root), padtitle);
    g_pad_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(root), g_pad_box);

    maprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_map_button = gtk_button_new_with_label("Map");
    g_signal_connect(g_map_button, "clicked", G_CALLBACK(on_map_clicked), NULL);
    defaults = gtk_button_new_with_label("Defaults");
    g_signal_connect(defaults, "clicked", G_CALLBACK(on_defaults_clicked), NULL);
    g_map_hint = gtk_label_new("");
    gtk_widget_add_css_class(g_map_hint, "dim-label");
    gtk_widget_set_hexpand(g_map_hint, TRUE);
    gtk_box_append(GTK_BOX(maprow), g_map_button);
    gtk_box_append(GTK_BOX(maprow), defaults);
    gtk_box_append(GTK_BOX(maprow), g_map_hint);
    gtk_box_append(GTK_BOX(root), maprow);

    g_view_stack = adw_view_stack_new();
    page = adw_view_stack_add_titled(ADW_VIEW_STACK(g_view_stack), root, "controllers", "Controllers");
    adw_view_stack_page_set_icon_name(page, "input-gaming-symbolic");
    page = adw_view_stack_add_titled(ADW_VIEW_STACK(g_view_stack), build_keyboard_page(),
                                     "keyboard", "Keyboard");
    adw_view_stack_page_set_icon_name(page, "input-keyboard-symbolic");

    header = adw_header_bar_new();
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

    g_held_timer = g_timeout_add(33, held_tick, NULL);
    g_timeout_add(250, pad_tick, NULL);
    set_map_state(-2);
}

static void show_window(void)
{
    int port;
    for (port = 0; port < 2; port++)
        gtk_drop_down_set_selected(GTK_DROP_DOWN(g_type_combo[port]),
                                   (guint)nessession_port_type(g_session, port));
    g_pad_generation = (unsigned)-1;
    pad_tick(NULL);
    refresh_keyboard();
    gtk_window_present(GTK_WINDOW(g_window));
    pad_tick(NULL);
}

void nes_controllers_window_toggle(GtkWindow *parent, nessession *session)
{
    g_session = session;
    if (!g_window)
        build_window(parent);

    if (gtk_widget_get_visible(g_window)) {
        set_map_state(-2);
        kbd_release_mouse();
        gtk_widget_set_visible(g_window, FALSE);
    } else {
        show_window();
    }
}

void nes_controllers_window_show_keyboard(GtkWindow *parent, nessession *session)
{
    g_session = session;
    if (!g_window)
        build_window(parent);
    adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(g_view_stack), "keyboard");
    show_window();
}

gboolean nes_controllers_window_is_visible(void)
{
    return g_window && gtk_widget_get_visible(g_window);
}
