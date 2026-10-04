/*
 * The Preferences dialog: a programmatic AdwPreferencesDialog, no .ui file.
 *
 * Controller types, the analog switch and the aspect apply LIVE -- a
 * FujiNet-booted game survives a change of controller, as it would on the
 * real console. The region applies at the next power cycle (Reset to CONFIG
 * or opening a cartridge), as switching a console's region would. The host
 * options are read when the session starts, so those restart the session
 * once, on close, if anything changed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefs.h"

#include "window.h"

typedef struct {
    NESWindow *window;
    nessession *session;
    void (*restart)(NESWindow *window);
    gboolean dirty;
    GtkWidget *pad_group;
    GtkWidget *pad_rows[8];
    int pad_row_count;
    unsigned pad_generation;
    guint pad_timer;
    GtkWidget *port_row[2];
} PrefsState;

typedef struct {
    PrefsState *state;
    const char *key;
    int def;
    int live_port;   /* -1: a restart option; 0/1: a live port-type row;
                        -3: the region; -4: the display aspect */
} RowBinding;

static void binding_free(gpointer p, GClosure *closure)
{
    (void)closure;
    g_free(p);
}

static RowBinding *binding_new(PrefsState *state, const char *key, int def, int live_port)
{
    RowBinding *b = g_new0(RowBinding, 1);
    b->state = state;
    b->key = key;
    b->def = def;
    b->live_port = live_port;
    return b;
}

static void update_port_subtitle(PrefsState *state, int port)
{
    int type = nessession_port_type(state->session, port);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(state->port_row[port]),
        type == NES_CTRL_NONE ? "Unplugged" : "Plugged in");
}

static const char *aspect_name(int i)
{
    static const char *const names[] = { "Television (8:7 pixels)", "Square pixels", NULL };
    return (i >= 0 && i < 2) ? names[i] : NULL;
}

static void combo_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    RowBinding *b = user_data;
    int sel = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    (void)pspec;
    if (b->live_port >= 0) {
        nessession_set_port_type(b->state->session, b->live_port, sel);
        update_port_subtitle(b->state, b->live_port);
        return;
    }
    if (b->live_port == -3) {
        nessession_set_region(b->state->session, sel);
        return;
    }
    if (b->live_port == -4) {
        nessession_set_int(b->state->session, b->key, sel);
        nes_window_apply_aspect(b->state->window, sel);
        return;
    }
    if (nessession_get_int(b->state->session, b->key, b->def) == sel)
        return;
    nessession_set_int(b->state->session, b->key, sel);
    b->state->dirty = TRUE;
}

static void analog_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)row; (void)pspec;
    nessession_set_analog(state->session,
        nessession_get_int(state->session, "analog_joystick", 1));
}

static void switch_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    RowBinding *b = user_data;
    int on = adw_switch_row_get_active(ADW_SWITCH_ROW(row)) ? 1 : 0;
    (void)pspec;
    if (nessession_get_int(b->state->session, b->key, b->def) == on)
        return;
    nessession_set_int(b->state->session, b->key, on);
    if (b->live_port == -2)          /* an analog switch: live */
        analog_changed(row, NULL, b->state);
    else
        b->state->dirty = TRUE;
}

static void volume_changed(GtkRange *range, gpointer user_data)
{
    PrefsState *state = user_data;
    nessession_set_volume(state->session, (int)gtk_range_get_value(range));
}

static GtkWidget *combo_row(PrefsState *state, const char *title,
                            const char *subtitle, const char *key, int def,
                            const char *(*name_fn)(int), int live_port)
{
    GtkWidget *row = adw_combo_row_new();
    GtkStringList *model = gtk_string_list_new(NULL);
    int i;

    for (i = 0; name_fn(i); i++)
        gtk_string_list_append(model, name_fn(i));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
    adw_combo_row_set_selected(ADW_COMBO_ROW(row),
        (guint)nessession_get_int(state->session, key, def));
    g_signal_connect_data(row, "notify::selected", G_CALLBACK(combo_changed),
                          binding_new(state, key, def, live_port), binding_free, 0);
    g_object_unref(model);
    return row;
}

static GtkWidget *switch_row(PrefsState *state, const char *title,
                             const char *subtitle, const char *key, int def,
                             int live)
{
    GtkWidget *row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_switch_row_set_active(ADW_SWITCH_ROW(row),
                              nessession_get_int(state->session, key, def));
    g_signal_connect_data(row, "notify::active", G_CALLBACK(switch_changed),
                          binding_new(state, key, def, live ? -2 : -1), binding_free, 0);
    return row;
}

/* ---- gamepads: one row per pad, with its port assignment ------------------- */

static void pad_assign_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    PrefsState *state = user_data;
    int idx = GPOINTER_TO_INT(g_object_get_data(row, "pad-index"));
    int sel = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    (void)pspec;
    nessession_gamepad_assign(state->session, idx, sel - 1);
}

static void rebuild_pad_rows(PrefsState *state)
{
    static const char *const choices[] = { "Automatic", "Player 1", "Player 2", NULL };
    int i, n = nessession_gamepad_count(state->session);
    char name[128], sub[64];

    for (i = 0; i < state->pad_row_count; i++)
        adw_preferences_group_remove(ADW_PREFERENCES_GROUP(state->pad_group), state->pad_rows[i]);
    state->pad_row_count = 0;

    if (n == 0) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "No gamepads connected");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "Plug one in: it is picked up as it appears");
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(state->pad_group), row);
        state->pad_rows[state->pad_row_count++] = row;
        return;
    }
    for (i = 0; i < n && i < 8; i++) {
        GtkWidget *row = adw_combo_row_new();
        GtkStringList *model = gtk_string_list_new(choices);
        int eff = nessession_gamepad_effective_port(state->session, i);
        nessession_gamepad_name(state->session, i, name, sizeof name);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
        if (eff >= 0)
            g_snprintf(sub, sizeof sub, "Driving player %d", eff + 1);
        else
            g_snprintf(sub, sizeof sub, "Driving nothing");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
        adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
        adw_combo_row_set_selected(ADW_COMBO_ROW(row),
            (guint)(nessession_gamepad_assignment(state->session, i) + 1));
        g_object_set_data(G_OBJECT(row), "pad-index", GINT_TO_POINTER(i));
        g_signal_connect(row, "notify::selected", G_CALLBACK(pad_assign_changed), state);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(state->pad_group), row);
        state->pad_rows[state->pad_row_count++] = row;
        g_object_unref(model);
    }
}

static gboolean pad_tick(gpointer user_data)
{
    PrefsState *state = user_data;
    unsigned gen = nessession_gamepad_generation(state->session);
    if (gen != state->pad_generation) {
        state->pad_generation = gen;
        rebuild_pad_rows(state);
    }
    update_port_subtitle(state, 0);
    update_port_subtitle(state, 1);
    return G_SOURCE_CONTINUE;
}

static void prefs_closed(AdwDialog *dialog, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)dialog;
    if (state->pad_timer) g_source_remove(state->pad_timer);
    if (state->dirty && state->restart)
        state->restart(state->window);
    g_free(state);
}

void nes_prefs_show(NESWindow *parent, nessession *session,
                      void (*restart)(NESWindow *parent))
{
    PrefsState *state = g_new0(PrefsState, 1);
    AdwPreferencesDialog *dialog = ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *machine = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *ports = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *analog = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *pads = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *audio = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *host = ADW_PREFERENCES_GROUP(adw_preferences_group_new());

    state->window = parent;
    state->session = session;
    state->restart = restart;
    state->pad_group = GTK_WIDGET(pads);

    adw_dialog_set_title(ADW_DIALOG(dialog), "Preferences");
    adw_dialog_set_content_width(ADW_DIALOG(dialog), 560);
    adw_preferences_page_set_title(page, "Machine");
    adw_preferences_page_set_icon_name(page, "applications-games-symbolic");

    adw_preferences_group_set_title(machine, "Machine");
    adw_preferences_group_add(machine,
        combo_row(state, "Region", "Auto follows the image's header. Takes effect at "
                  "the next power cycle (Reset to CONFIG, or opening a cartridge)",
                  "region", NES_REGION_AUTO, nes_region_name, -3));
    adw_preferences_group_add(machine,
        combo_row(state, "Aspect", "How the 256 x 240 picture is shaped",
                  "aspect", 0, aspect_name, -4));

    adw_preferences_group_set_title(ports, "Controllers");
    adw_preferences_group_set_description(ports,
        "What is plugged into each port. Applies immediately, even to a game "
        "booted over FujiNet.");
    state->port_row[0] = combo_row(state, "Player 1", "", "port0_type", NES_CTRL_STANDARD,
                                   nes_ctrl_type_name, 0);
    state->port_row[1] = combo_row(state, "Player 2", "", "port1_type", NES_CTRL_STANDARD,
                                   nes_ctrl_type_name, 1);
    adw_preferences_group_add(ports, state->port_row[0]);
    adw_preferences_group_add(ports, state->port_row[1]);
    update_port_subtitle(state, 0);
    update_port_subtitle(state, 1);

    adw_preferences_group_set_title(analog, "Analog sticks");
    adw_preferences_group_add(analog,
        switch_row(state, "Left stick drives the D-pad",
                   "Off: only a gamepad's D-pad moves the cross", "analog_joystick", 1, 1));

    adw_preferences_group_set_title(pads, "Gamepads");
    adw_preferences_group_set_description(pads,
        "Assigned to ports in connection order unless chosen here");
    rebuild_pad_rows(state);
    state->pad_generation = nessession_gamepad_generation(session);

    adw_preferences_group_set_title(audio, "Audio");
    {
        GtkWidget *row = adw_action_row_new();
        GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 5);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Volume");
        gtk_range_set_value(GTK_RANGE(scale), nessession_get_int(session, "volume", 100));
        gtk_widget_set_size_request(scale, 200, -1);
        gtk_widget_set_valign(scale, GTK_ALIGN_CENTER);
        g_signal_connect(scale, "value-changed", G_CALLBACK(volume_changed), state);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), scale);
        adw_preferences_group_add(audio, row);
    }

    adw_preferences_group_set_title(host, "Host");
    adw_preferences_group_set_description(host, "Applied by restarting the session");
    adw_preferences_group_add(host,
        switch_row(state, "Enable FujiNet",
                   "Run the in-process FujiNet the cartridge dials into. Off "
                   "means no network and a link-down CONFIG.",
                   "enable_fujinet", 1, 0));
    adw_preferences_group_add(host,
        switch_row(state, "Audio", "Open the system audio device", "enable_audio", 1, 0));
    adw_preferences_group_add(host,
        switch_row(state, "Gamepads", "Poll USB/Bluetooth gamepads", "enable_gamepad", 1, 0));

    adw_preferences_page_add(page, machine);
    adw_preferences_page_add(page, ports);
    adw_preferences_page_add(page, analog);
    adw_preferences_page_add(page, pads);
    adw_preferences_page_add(page, audio);
    adw_preferences_page_add(page, host);
    adw_preferences_dialog_add(dialog, page);
    g_signal_connect(dialog, "closed", G_CALLBACK(prefs_closed), state);
    state->pad_timer = g_timeout_add(1000, pad_tick, state);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent));
}
