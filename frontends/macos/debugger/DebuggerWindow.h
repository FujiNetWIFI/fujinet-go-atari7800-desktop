/*
 * The AppKit debugger window over MAME's debugger engine (a7800debug.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "a7800session.h"

@interface A7800DebuggerWindow : NSObject <NSWindowDelegate, NSTextFieldDelegate>
/* Shows (creating on first use) the debugger for the session; attaching the
 * engine stops the machine, as the other frontends do. */
+ (void)showForSession:(a7800session *)session;
/* F12: shows it, or closes it (detaching, so the machine runs on). */
+ (void)toggleForSession:(a7800session *)session;
@end
