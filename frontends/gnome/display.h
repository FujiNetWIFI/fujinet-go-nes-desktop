/*
 * The emulator display: a GtkWidget that pulls frames from the session on
 * the compositor's own frame clock.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "nessession.h"

#define NES_TYPE_DISPLAY (nes_display_get_type())
G_DECLARE_FINAL_TYPE(NESDisplay, nes_display, NES, DISPLAY, GtkWidget)

GtkWidget *nes_display_new(nessession *session);
/* The 8:7 pixels a television showed (292 x 240), or square pixels. */
void nes_display_set_tv_aspect(NESDisplay *self, gboolean tv);
void nes_display_set_smooth(NESDisplay *self, gboolean smooth);
