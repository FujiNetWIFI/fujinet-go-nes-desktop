/*
 * NESControllersWindow -- both NES controllers on screen, held buttons lit,
 * Map mode for rebinding any control to a key or a gamepad button, and the
 * ports' controller types and gamepad assignments; and a Keyboard page with
 * the keyboard on the expansion port.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "nessession.h"

/* Toggles visibility: shows the singleton window (creating it on first
 * call), or hides it if already showing. `parent` is only used the first
 * time, to set the transient-for relationship. */
void nes_controllers_window_toggle(GtkWindow *parent, nessession *session);
/* Shows the window on its Keyboard page (the expansion-port keyboard). */
void nes_controllers_window_show_keyboard(GtkWindow *parent, nessession *session);
gboolean nes_controllers_window_is_visible(void);
