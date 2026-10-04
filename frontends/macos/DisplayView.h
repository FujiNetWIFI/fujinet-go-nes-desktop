/*
 * The emulator display.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "nessession.h"

@interface NESDisplayView : NSView
- (instancetype)initWithSession:(nessession *)session;
/* YES: 8:7 pixels, as a TV showed them (setting "aspect" 0); NO: square. */
- (void)setTvAspect:(BOOL)tv;
- (void)setSmooth:(BOOL)smooth;
- (void)stop;
@end
