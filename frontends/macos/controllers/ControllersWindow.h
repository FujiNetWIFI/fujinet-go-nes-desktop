/*
 * The controllers panel: an on-screen NES controller per player, the
 * console's buttons, the connected gamepads and the Map row, in a
 * fixed-size floating panel.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "nessession.h"

@interface NESControllersWindow : NSWindowController <NSWindowDelegate>
+ (void)toggleWithSession:(nessession *)session;
+ (BOOL)isVisible;
@end
