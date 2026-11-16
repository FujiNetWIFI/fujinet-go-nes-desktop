/*
 * Debugger window for the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "nessession.h"

G_BEGIN_DECLS

/* Shows (creating on first use) the debugger window for the session.
 * Showing attaches the debugger engine, which stops the machine; closing the
 * window detaches it and lets the machine run on. */
void nes_debugger_show(GtkWindow *parent, nessession *session);

/* Shows it if hidden or absent, closes it if showing: the main window's F12
 * and menu item. */
void nes_debugger_toggle(GtkWindow *parent, nessession *session);

G_END_DECLS
