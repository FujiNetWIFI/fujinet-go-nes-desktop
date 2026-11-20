/*
 * The Controllers panel (F9): the two NES-001 pads (or empty sockets) over the
 * console's RESET key, painted from the session's layout; the gamepads and the
 * player each drives; the expansion keyboard drawn from its skin; and Map mode
 * for rebinding. Hidden at start; a fixed-size floating NSPanel. One panel for
 * the process: every entry point is a class method.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "nessession.h"

@interface NESControllersWindow : NSObject
/* Show the panel, or hide it when it is showing. */
+ (void)toggleWithSession:(nessession *)session;
/* Show the panel (it stays up when it already is). */
+ (void)showWithSession:(nessession *)session;
/* Show the panel on its Keyboard page. */
+ (void)showKeyboardWithSession:(nessession *)session;
+ (BOOL)isVisible;
/* The panel is the key window (it has the keyboard). */
+ (BOOL)isActive;
/* Settings changed the gamepads or the bindings: read them again. */
+ (void)refresh;
@end
