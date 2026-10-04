/*
 * The controllers panel: an on-screen NES controller per player, the
 * console's buttons, the connected gamepads, the expansion-port keyboard
 * and the Map row, in a
 * fixed-size floating panel.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "nessession.h"

@interface NESControllersWindow : NSWindowController <NSWindowDelegate>
+ (void)toggleWithSession:(nessession *)session;
/* Shows the panel (if hidden) for the on-screen keyboard: NES_OPEN_KEYBOARD. */
+ (void)showKeyboardWithSession:(nessession *)session;
+ (BOOL)isVisible;
@end
