/*
 * The Win32 Controllers window (F9): both NES-001 pads and the console's
 * RESET key drawn as the real parts look, the port selectors, the gamepad
 * rows, the Map row and the expansion-port keyboard (Family BASIC or Subor),
 * in a fixed-size tool window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <windows.h>

#include "nessession.h"

/* Show / hide (F9). Created on first use, hidden not destroyed after. */
void nes_controller_window_toggle(HWND parent, nessession *session);

/* Show without toggling (Settings' Open Controllers). */
void nes_controller_window_show(HWND parent, nessession *session);

/* Show the window (NES_OPEN_KEYBOARD): its Keyboard section holds the
 * on-screen keyboard for the expansion port. */
void nes_controller_window_show_keyboard(HWND parent, nessession *session);

int nes_controller_window_visible(void);

/* Called when the gamepad set, a port's controller type or the bindings
 * changed, so the window refreshes. */
void nes_controller_window_gamepads_changed(void);

/* Give the window first refusal on a message from the main loop. Returns 1
 * if consumed. */
int nes_controller_pretranslate(MSG *msg);
