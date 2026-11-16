/*
 * The debugger's Video tab (AppKit): a thin view over core/include/nesvidtab.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import <Cocoa/Cocoa.h>

#include "nessession.h"

@interface NESVideoTab : NSObject

/* The tab's content view; this object owns the controller, which is freed with it. */
@property(nonatomic, readonly) NSView *view;

/* The session: the controller asks its debugger whether the machine is parked and adds the
 * Break on writes breakpoints through it. */
- (instancetype)initWithSession:(nessession *)session;
/* Take the machine's state and redraw what changed (force: everything). Cheap to call often. */
- (void)refreshForce:(BOOL)force;

@end
