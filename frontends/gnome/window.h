/*
 * The main application window.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "nessession.h"

#define NES_TYPE_WINDOW (nes_window_get_type())
G_DECLARE_FINAL_TYPE(NESWindow, nes_window, NES, WINDOW,
                     AdwApplicationWindow)

GtkWidget *nes_window_new(AdwApplication *app, nessession *session);
/* main.c: the icon name to use (the installed id, or the in-tree art). */
const char *nes_icon_name(void);
void nes_window_toast(NESWindow *self, const char *text);
/* The display's shape: 0 = the television's 8:7 pixels, 1 = square. */
void nes_window_apply_aspect(NESWindow *self, int aspect);

/* The accent colour (NESSESSION_ACCENT_RGB) as a CSS class every window
 * can use: ".nes-accent" paints a widget's background in it, and
 * ".nes-accent-text" its label. Installed once by the main window. */
void nes_install_accent_css(void);
