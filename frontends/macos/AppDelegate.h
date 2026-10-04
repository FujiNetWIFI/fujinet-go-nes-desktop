/*
 * The application delegate: the window, the menu bar, and the session.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "nessession.h"

@interface NESAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
- (instancetype)initWithSession:(nessession *)session cartPath:(const char *)cartPath;
@end
