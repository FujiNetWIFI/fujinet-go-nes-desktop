/*
 * The FujiNet console log window, and the FujiNet configuration window.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "nessession.h"

G_BEGIN_DECLS

/* Shows (raising an existing one) the FujiNet console log window. */
void nes_fujilog_show(GtkWindow *parent, nessession *session);
/* Shows the FujiNet web UI: embedded with WebKitGTK when built with it,
 * otherwise in the system browser. */
void nes_fujiconfig_show(GtkWindow *parent, nessession *session);

G_END_DECLS
