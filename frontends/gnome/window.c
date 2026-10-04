/*
 * window.c -- the main window: the display, a header-bar menu, keyboard
 * capture, the console's buttons, gamepad hot-plug toasts and the FujiNet
 * status.
 *
 * Keyboard events are translated by hardware keycode (evdev, via the
 * session's HID table) rather than by GDK keyval, so a binding names the
 * physical key whatever Shift is doing and whatever layout is active -- the
 * same path the Qt, Win32 and AppKit frontends take, which is what lets one
 * tested table serve all four.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window.h"

#include "display.h"
#include "fujilog.h"
#include "prefs.h"
#include "debugger/dbg_window.h"
#include "controllers/controllers_window.h"

#include <string.h>

struct _NESWindow {
    AdwApplicationWindow parent_instance;

    nessession *session;
    GtkWidget *display;
    GtkWidget *toast_overlay;
    GtkWidget *status;          /* the FujiNet link indicator */
    GtkWidget *status_dot;
    guint status_id;
    guint sysact_id;
    gboolean sysact_down[NES_SYSACT_COUNT];
    unsigned pad_generation;
    gboolean fullscreen;
};

G_DEFINE_FINAL_TYPE(NESWindow, nes_window, ADW_TYPE_APPLICATION_WINDOW)

void nes_window_toast(NESWindow *self, const char *text)
{
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(self->toast_overlay),
                                adw_toast_new(text));
}

void nes_install_accent_css(void)
{
    static gboolean done;
    GtkCssProvider *css;
    char buf[512];
    if (done) return;
    done = TRUE;
    g_snprintf(buf, sizeof buf,
        ".nes-accent { background: #%06x; color: #000000; }\n"
        ".nes-accent:hover { background: #%06x; }\n"
        ".nes-accent-text { color: #%06x; font-weight: bold; }\n"
        ".nes-dot { border-radius: 6px; min-width: 12px; min-height: 12px; }\n"
        ".nes-dot-on { background: #%06x; }\n"
        ".nes-dot-off { background: #808080; }\n",
        NESSESSION_ACCENT_RGB, NESSESSION_ACCENT_RGB,
        NESSESSION_ACCENT_RGB, NESSESSION_ACCENT_RGB);
    css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, buf);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

/* ---- input ---------------------------------------------------------------- */

static guint32 keysym_of(guint keyval, guint keycode)
{
    /* the hardware key first; a keyval only for keys evdev has no HID
     * usage for (rare: media keys) */
    guint32 k = keycode >= 8 ? nessession_keysym_from_evdev(keycode - 8) : 0;
    return k ? k : keyval;
}

static void run_sysaction(NESWindow *self, int sa)
{
    switch (sa) {
    case NES_SYSACT_RESET_CONFIG:
        nessession_sysaction(self->session, sa);
        nes_window_toast(self, "Back to the FujiNet CONFIG client");
        break;
    case NES_SYSACT_PAUSE:
        /* the debugger window attaches, which stops the machine */
        nes_debugger_show(GTK_WINDOW(self), self->session);
        break;
    default:
        break;
    }
}

static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer user_data)
{
    NESWindow *self = user_data;
    guint32 keysym;
    int sa;
    (void)ctrl;

    /* The window's own keys, deliberately not bindable: they are how you
     * reach the panels that do the binding. */
    if (keyval == GDK_KEY_F9) {
        nes_controllers_window_toggle(GTK_WINDOW(self), self->session);
        return TRUE;
    }
    if (keyval == GDK_KEY_F12) {
        nes_debugger_toggle(GTK_WINDOW(self), self->session);
        return TRUE;
    }
    if (keyval == GDK_KEY_F11) {
        gtk_widget_activate_action(GTK_WIDGET(self), "win.fullscreen", NULL);
        return TRUE;
    }
    if ((state & GDK_CONTROL_MASK) || (state & GDK_ALT_MASK))
        return FALSE;   /* menu accelerators */

    keysym = keysym_of(keyval, keycode);
    /* A system action is checked BEFORE the machine keys, so Escape always
     * gets back to CONFIG whatever else the key table says. Leading edge
     * only: GTK4 has no repeat flag. */
    sa = nessession_key_sysaction(self->session, keysym);
    if (sa >= 0) {
        if (!self->sysact_down[sa]) {
            self->sysact_down[sa] = TRUE;
            run_sysaction(self, sa);
        }
        return TRUE;
    }
    return nessession_key(self->session, keysym, 1) ? TRUE : FALSE;
}

static gboolean on_key_released(GtkEventControllerKey *ctrl, guint keyval,
                                guint keycode, GdkModifierType state,
                                gpointer user_data)
{
    NESWindow *self = user_data;
    guint32 keysym;
    int sa;
    (void)ctrl; (void)state;

    if (keyval == GDK_KEY_F9 || keyval == GDK_KEY_F11 || keyval == GDK_KEY_F12)
        return TRUE;
    keysym = keysym_of(keyval, keycode);
    sa = nessession_key_sysaction(self->session, keysym);
    if (sa >= 0) {
        self->sysact_down[sa] = FALSE;
        return TRUE;
    }
    return nessession_key(self->session, keysym, 0) ? TRUE : FALSE;
}

/* Losing focus with keys held would leave the machine believing they are
 * still down. */
static void on_focus_leave(GtkEventControllerFocus *ctrl, gpointer user_data)
{
    NESWindow *self = user_data;
    (void)ctrl;
    nessession_release_all(self->session);
    memset(self->sysact_down, 0, sizeof self->sysact_down);
}

/* The gamepad thread cannot call into GTK; it posts system actions and this
 * timer takes them. It also watches for gamepads coming and going, and says
 * so: a pad that drops off Bluetooth mid-game should not be a mystery. */
static gboolean sysact_drain_tick(gpointer user_data)
{
    NESWindow *self = user_data;
    unsigned gen;
    int sa;
    while (nessession_sysaction_take(self->session, &sa))
        run_sysaction(self, sa);

    gen = nessession_gamepad_generation(self->session);
    if (gen != self->pad_generation) {
        char text[160];
        self->pad_generation = gen;
        if (nessession_gamepad_last_event(self->session, text, sizeof text) > 0)
            nes_window_toast(self, text);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- status --------------------------------------------------------------- */

static gboolean update_status(gpointer user_data)
{
    NESWindow *self = user_data;
    char text[200], st[160];
    gboolean on = FALSE;

    if (!nessession_is_running(self->session)) {
        g_snprintf(text, sizeof text, "Stopped");
    } else {
        const char *cart = nessession_cart_path(self->session);
        const char *slash = cart ? strrchr(cart, '/') : NULL;
        int link = nessession_cart_link_up(self->session);
        nessession_cart_status(self->session, st, sizeof st);
        on = link == 1;
        if (cart && *cart)
            g_snprintf(text, sizeof text, "%s â FujiNet %s",
                       slash ? slash + 1 : cart, st);
        else
            g_snprintf(text, sizeof text, "FujiNet %s", st);
    }
    gtk_label_set_text(GTK_LABEL(self->status), text);
    if (on) {
        gtk_widget_add_css_class(self->status_dot, "nes-dot-on");
        gtk_widget_remove_css_class(self->status_dot, "nes-dot-off");
    } else {
        gtk_widget_add_css_class(self->status_dot, "nes-dot-off");
        gtk_widget_remove_css_class(self->status_dot, "nes-dot-on");
    }
    return G_SOURCE_CONTINUE;
}

/* ---- cartridges ------------------------------------------------------------ */

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* A cartridge the FujiNet cartridge cannot run (an unsupported mapper, a
 * headerless dump) says why in a dialog: a toast would vanish before the
 * reason was read, and the reason is the whole point. */
static void show_error(NESWindow *self, const char *heading, const char *body)
{
    AdwDialog *dlg = adw_alert_dialog_new(heading, body);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dlg), "ok", "OK");
    adw_dialog_present(dlg, GTK_WIDGET(self));
}

static void open_cart_path(NESWindow *self, const char *path)
{
    char msg[1200];
    if (nessession_load_cart(self->session, path) != 0) {
        show_error(self, "Cannot Open Cartridge", nessession_last_error(self->session));
        return;
    }
    g_snprintf(msg, sizeof msg, "Running %s", base_name(path));
    nes_window_toast(self, msg);
}

/* A dropped file: a cartridge runs; anything else goes where it belongs. */
static void load_media(NESWindow *self, const char *path)
{
    char dest[1024];

    if (nessession_media_is_cartridge(path)) {
        open_cart_path(self, path);
        return;
    }
    if (nessession_import_media(self->session, path, dest, sizeof dest) != 0) {
        nes_window_toast(self, nessession_last_error(self->session));
        return;
    }
    nes_window_toast(self, "Copied to FujiNet's SD folder \xe2\x80\x94 mount it "
                             "from the CONFIG client");
}

static GtkFileDialog *cart_dialog(const char *title)
{
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *carts = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();

    gtk_file_filter_set_name(carts, "NES cartridges (*.nes)");
    gtk_file_filter_add_pattern(carts, "*.nes");
    gtk_file_filter_add_pattern(carts, "*.NES");
    gtk_file_filter_add_pattern(carts, "*.fuji");
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, carts);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dlg, title);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    g_object_unref(carts);
    g_object_unref(all);
    g_object_unref(filters);
    return dlg;
}

static void on_cart_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    NESWindow *self = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    if (!file) return;
    path = g_file_get_path(file);
    if (path) open_cart_path(self, path);
}

static void action_open(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    NESWindow *self = user_data;
    GtkFileDialog *dlg = cart_dialog("Open Cartridge");
    (void)a; (void)p;
    gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_cart_chosen, self);
    g_object_unref(dlg);
}

static void on_sd_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    NESWindow *self = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    char dest[1024], msg[1200];
    if (!file) return;
    path = g_file_get_path(file);
    if (!path) return;
    if (nessession_import_cart_to_sd(self->session, path, dest, sizeof dest) != 0) {
        nes_window_toast(self, nessession_last_error(self->session));
        return;
    }
    g_snprintf(msg, sizeof msg, "%s is on the SD host \xe2\x80\x94 boot it from "
               "the CONFIG client", base_name(dest));
    nes_window_toast(self, msg);
}

static void action_import_sd(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    NESWindow *self = user_data;
    GtkFileDialog *dlg = cart_dialog("Import Cartridge to SD");
    (void)a; (void)p;
    gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_sd_chosen, self);
    g_object_unref(dlg);
}

static void action_eject(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    NESWindow *self = user_data;
    (void)a; (void)p;
    nessession_eject(self->session);
    nes_window_toast(self, "Cartridge ejected \xe2\x80\x94 back to CONFIG");
}

static gboolean on_drop(GtkDropTarget *t, const GValue *value, double x,
                        double y, gpointer user_data)
{
    NESWindow *self = user_data;
    g_autofree char *path = NULL;
    (void)t; (void)x; (void)y;
    if (!G_VALUE_HOLDS(value, G_TYPE_FILE)) return FALSE;
    path = g_file_get_path(G_FILE(g_value_get_object(value)));
    if (!path) return FALSE;
    load_media(self, path);
    return TRUE;
}

/* ---- the console and actions ---------------------------------------------- */

static void action_reset_game(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    NESWindow *self = user_data;
    (void)a; (void)p;
    nessession_reset_game(self->session);
}

static void action_reset_config(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    (void)a; (void)p;
    run_sysaction(user_data, NES_SYSACT_RESET_CONFIG);
}

static void action_controllers(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    NESWindow *self = user_data;
    (void)a; (void)p;
    nes_controllers_window_toggle(GTK_WINDOW(self), self->session);
}

static void action_debugger(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    NESWindow *self = user_data;
    (void)a; (void)p;
    nes_debugger_toggle(GTK_WINDOW(self), self->session);
}

static void action_fullscreen(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    NESWindow *self = user_data;
    (void)a; (void)p;
    self->fullscreen = !self->fullscreen;
    if (self->fullscreen) gtk_window_fullscreen(GTK_WINDOW(self));
    else gtk_window_unfullscreen(GTK_WINDOW(self));
}

static void action_fujinet_config(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    NESWindow *self = user_data;
    (void)a; (void)p;
    if (!nessession_fujinet_running(self->session)) {
        nes_window_toast(self, "FujiNet is not running");
        return;
    }
    nes_fujiconfig_show(GTK_WINDOW(self), self->session);
}

static void action_fujinet_log(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    NESWindow *self = user_data;
    (void)a; (void)p;
    nes_fujilog_show(GTK_WINDOW(self), self->session);
}

/* Stop, re-read the settings store, start. Preferences hands this to
 * nes_prefs_show() as its close callback. */
static void restart_session(NESWindow *self)
{
    nessession_start_opts o;
    nessession_settings_flush(self->session);
    nessession_default_opts(self->session, &o);
    nessession_stop(self->session);
    if (nessession_start(self->session, &o) != 0) {
        nes_window_toast(self, nessession_last_error(self->session));
        return;
    }
    nes_window_toast(self, "Machine options applied (session restarted)");
}

static void action_prefs(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    NESWindow *self = user_data;
    (void)a; (void)p;
    nes_prefs_show(self, self->session, restart_session);
}

static void action_about(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    NESWindow *self = user_data;
    AdwDialog *about;
    (void)a; (void)p;
    about = adw_about_dialog_new();
    adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(about), "FujiNet Go NES");
    adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(about), nes_icon_name());
    adw_about_dialog_set_version(ADW_ABOUT_DIALOG(about), NES_VERSION_STRING);
    adw_about_dialog_set_developer_name(ADW_ABOUT_DIALOG(about), "Thomas Cherryhomes");
    adw_about_dialog_set_website(ADW_ABOUT_DIALOG(about), "https://fujinet.online/");
    adw_about_dialog_set_issue_url(ADW_ABOUT_DIALOG(about),
        "https://github.com/FujiNetWIFI/fujinet-go-nes-desktop/issues");
    adw_about_dialog_set_license_type(ADW_ABOUT_DIALOG(about), GTK_LICENSE_GPL_3_0);
    adw_about_dialog_set_comments(ADW_ABOUT_DIALOG(about),
        "An NES with a built-in FujiNet. The emulator is MesenCE "
        "(GPL-3.0-or-later), from Mesen by Sour and contributors, with the "
        "FujiNet NES cartridge.");
    adw_dialog_present(about, GTK_WIDGET(self));
}

static void action_aspect(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    NESWindow *self = user_data;
    gboolean tv = !g_variant_get_boolean(g_action_get_state(G_ACTION(a)));
    (void)p;
    g_simple_action_set_state(a, g_variant_new_boolean(tv));
    nes_display_set_tv_aspect(NES_DISPLAY(self->display), tv);
    /* "aspect": 0 = the television's 8:7 pixels, 1 = square pixels */
    nessession_set_int(self->session, "aspect", tv ? 0 : 1);
}

static void action_smooth(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    NESWindow *self = user_data;
    gboolean sm = !g_variant_get_boolean(g_action_get_state(G_ACTION(a)));
    (void)p;
    g_simple_action_set_state(a, g_variant_new_boolean(sm));
    nes_display_set_smooth(NES_DISPLAY(self->display), sm);
    nessession_set_int(self->session, "smooth", sm ? 1 : 0);
}

void nes_window_apply_aspect(NESWindow *self, int aspect)
{
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(self), "tv-aspect");
    nes_display_set_tv_aspect(NES_DISPLAY(self->display), aspect == 0);
    if (a)
        g_simple_action_set_state(G_SIMPLE_ACTION(a), g_variant_new_boolean(aspect == 0));
}

static const GActionEntry win_actions[] = {
    { "open", action_open, NULL, NULL, NULL, { 0 } },
    { "eject", action_eject, NULL, NULL, NULL, { 0 } },
    { "import-sd", action_import_sd, NULL, NULL, NULL, { 0 } },
    { "reset-game", action_reset_game, NULL, NULL, NULL, { 0 } },
    { "reset-config", action_reset_config, NULL, NULL, NULL, { 0 } },
    { "controllers", action_controllers, NULL, NULL, NULL, { 0 } },
    { "debugger", action_debugger, NULL, NULL, NULL, { 0 } },
    { "fullscreen", action_fullscreen, NULL, NULL, NULL, { 0 } },
    { "tv-aspect", action_aspect, NULL, "true", NULL, { 0 } },
    { "smooth", action_smooth, NULL, "false", NULL, { 0 } },
    { "fujinet-config", action_fujinet_config, NULL, NULL, NULL, { 0 } },
    { "fujinet-log", action_fujinet_log, NULL, NULL, NULL, { 0 } },
    { "prefs", action_prefs, NULL, NULL, NULL, { 0 } },
    { "about", action_about, NULL, NULL, NULL, { 0 } },
};

/* ---- construction --------------------------------------------------------- */

static GMenu *build_menu(void)
{
    GMenu *menu = g_menu_new();
    GMenu *cart = g_menu_new();
    GMenu *sw = g_menu_new();
    GMenu *view = g_menu_new();
    GMenu *fuji = g_menu_new();
    GMenu *app = g_menu_new();

    g_menu_append(cart, "_Open Cartridge...", "win.open");
    g_menu_append(cart, "_Eject Cartridge", "win.eject");
    g_menu_append(cart, "_Import Cartridge to SD...", "win.import-sd");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(cart));

    g_menu_append(sw, "_Reset Game (Backspace)", "win.reset-game");
    g_menu_append(sw, "Reset to _CONFIG (Esc)", "win.reset-config");
    g_menu_append_section(menu, "Console", G_MENU_MODEL(sw));

    g_menu_append(view, "_Controllers (F9)", "win.controllers");
    g_menu_append(view, "_Debugger (F12)", "win.debugger");
    g_menu_append(view, "_TV Aspect (8:7 pixels)", "win.tv-aspect");
    g_menu_append(view, "_Smooth Scaling", "win.smooth");
    g_menu_append(view, "_Fullscreen (F11)", "win.fullscreen");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(view));

    g_menu_append(fuji, "FujiNet _Web UI", "win.fujinet-config");
    g_menu_append(fuji, "Console _Log", "win.fujinet-log");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(fuji));

    g_menu_append(app, "_Preferences", "win.prefs");
    g_menu_append(app, "_About FujiNet Go NES", "win.about");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(app));

    g_object_unref(cart);
    g_object_unref(sw);
    g_object_unref(view);
    g_object_unref(fuji);
    g_object_unref(app);
    return menu;
}

static void nes_window_dispose(GObject *object)
{
    NESWindow *self = NES_WINDOW(object);
    if (self->status_id) {
        g_source_remove(self->status_id);
        self->status_id = 0;
    }
    if (self->sysact_id) {
        g_source_remove(self->sysact_id);
        self->sysact_id = 0;
    }
    G_OBJECT_CLASS(nes_window_parent_class)->dispose(object);
}

static void nes_window_class_init(NESWindowClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = nes_window_dispose;
}

static void nes_window_init(NESWindow *self)
{
    (void)self;
}

GtkWidget *nes_window_new(AdwApplication *app, nessession *session)
{
    NESWindow *self = g_object_new(NES_TYPE_WINDOW, "application", app, NULL);
    GtkWidget *box, *header, *menu_button, *toolbar, *status_box;
    GtkEventController *keys, *focus;
    g_autoptr(GMenu) menu = NULL;

    self->session = session;
    nes_install_accent_css();

    gtk_window_set_title(GTK_WINDOW(self), "FujiNet Go NES");
    gtk_window_set_icon_name(GTK_WINDOW(self), nes_icon_name());
    /* 292x240 at 3x: the PPU's line at the television's 8:7 width, three
     * times over. */
    gtk_window_set_default_size(GTK_WINDOW(self), 876, 720 + 46);

    g_action_map_add_action_entries(G_ACTION_MAP(self), win_actions,
                                    G_N_ELEMENTS(win_actions), self);
    {
        static const char *const prefs_accels[] = { "<Control>comma", NULL };
        static const char *const open_accels[] = { "<Control>o", NULL };
        static const char *const reboot_accels[] = { "<Control>r", NULL };
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.prefs", prefs_accels);
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.open", open_accels);
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.reset-config", reboot_accels);
    }

    header = adw_header_bar_new();
    menu = build_menu();
    menu_button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), G_MENU_MODEL(menu));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu_button);

    status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    self->status_dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(self->status_dot, "nes-dot");
    gtk_widget_add_css_class(self->status_dot, "nes-dot-off");
    gtk_widget_set_valign(self->status_dot, GTK_ALIGN_CENTER);
    self->status = gtk_label_new("Starting...");
    gtk_widget_add_css_class(self->status, "dim-label");
    gtk_box_append(GTK_BOX(status_box), self->status_dot);
    gtk_box_append(GTK_BOX(status_box), self->status);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), status_box);

    self->display = nes_display_new(session);
    nes_display_set_tv_aspect(NES_DISPLAY(self->display),
        nessession_get_int(session, "aspect", 0) == 0);
    nes_display_set_smooth(NES_DISPLAY(self->display),
        nessession_get_int(session, "smooth", 0) != 0);
    {
        GAction *a = g_action_map_lookup_action(G_ACTION_MAP(self), "tv-aspect");
        g_simple_action_set_state(G_SIMPLE_ACTION(a),
            g_variant_new_boolean(nessession_get_int(session, "aspect", 0) == 0));
        a = g_action_map_lookup_action(G_ACTION_MAP(self), "smooth");
        g_simple_action_set_state(G_SIMPLE_ACTION(a),
            g_variant_new_boolean(nessession_get_int(session, "smooth", 0) != 0));
    }

    {
        GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_FILE, GDK_ACTION_COPY);
        g_signal_connect(drop, "drop", G_CALLBACK(on_drop), self);
        gtk_widget_add_controller(self->display, GTK_EVENT_CONTROLLER(drop));
    }

    self->toast_overlay = adw_toast_overlay_new();
    adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(self->toast_overlay), self->display);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), self->toast_overlay);
    gtk_box_append(GTK_BOX(box), toolbar);
    gtk_widget_set_vexpand(toolbar, TRUE);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(self), box);

    /* Capture on the WINDOW so input works no matter what has focus. */
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), self);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), self);
    gtk_widget_add_controller(GTK_WIDGET(self), keys);

    focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), self);
    gtk_widget_add_controller(GTK_WIDGET(self), focus);

    self->status_id = g_timeout_add_seconds(1, update_status, self);
    /* The current pad set is the baseline: no toast for pads that were
     * already plugged in at launch. */
    self->pad_generation = nessession_gamepad_generation(session);
    self->sysact_id = g_timeout_add(250, sysact_drain_tick, self);
    update_status(self);

    /* NES_OPEN_CONTROLLERS=1 / NES_OPEN_DEBUGGER=1 / NES_OPEN_SETTINGS=1
     * open those windows at launch, following the family's convention: the
     * way in when the app misbehaves before the menu is reachable. */
    {
        const char *env = g_getenv("NES_OPEN_CONTROLLERS");
        if (env && *env && *env != '0')
            nes_controllers_window_toggle(GTK_WINDOW(self), session);
        env = g_getenv("NES_OPEN_DEBUGGER");
        if (env && *env && *env != '0')
            nes_debugger_show(GTK_WINDOW(self), session);
        env = g_getenv("NES_OPEN_SETTINGS");
        if (env && *env && *env != '0')
            nes_prefs_show(self, session, restart_session);
    }
    return GTK_WIDGET(self);
}
