/*
 * The Win32 Controllers window: both NES controllers on screen, the
 * console's buttons and the Map row, in a fixed-size tool window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <windows.h>

#include "nessession.h"

/* Show / hide (F9). Created on first use, hidden not destroyed after. */
void nes_controller_window_toggle(HWND parent, nessession *session);

/* Called when the gamepad set changed, so the per-port pad lines refresh. */
void nes_controller_window_gamepads_changed(void);

/* Give the window first refusal on a message from the main loop. Returns 1
 * if consumed. */
int nes_controller_pretranslate(MSG *msg);
