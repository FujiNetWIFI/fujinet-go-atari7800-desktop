/*
 * The debugger's Video tab (AppKit): a thin view over core/include/a7800vidtab.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import <Cocoa/Cocoa.h>

#include "a7800session.h"

@interface A7800VideoTab : NSObject

/* The tab's content view; this object owns the controller, which is freed with it. */
@property (nonatomic, readonly) NSView *view;

- (instancetype)initWithSession:(a7800session *)session;
/* Take the machine's state and redraw what changed (force: everything). Cheap to call often. */
- (void)refreshForce:(BOOL)force;

@end
