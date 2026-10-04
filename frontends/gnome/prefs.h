/*
 * Preferences dialog for the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "nessession.h"

G_BEGIN_DECLS

typedef struct _NESWindow NESWindow;

/* Shows the preferences dialog. Controller types, the analog switch and the
 * aspect apply live, the region at the next power cycle; the host options
 * (FujiNet, audio, gamepads) are read when the session starts, so the dialog
 * restarts the session on close if one of those changed. */
void nes_prefs_show(NESWindow *parent, nessession *session,
                      void (*restart)(NESWindow *parent));

G_END_DECLS
