/*
 * The Controllers panel (F9): the two ports' controllers (a ProLine
 * joystick, a 2600 CX40, the XG-1 light gun or an empty socket, each by what
 * the port has) over the console's keys and difficulty slides, the gamepads
 * with their assignments, and Map mode for rebinding. Hidden at start; a
 * fixed-size floating NSPanel. One panel for the process: every entry point
 * is a class method.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "a7800session.h"

@interface A7800ControllersWindow : NSObject
/* Show the panel, or hide it when it is showing. */
+ (void)toggleWithSession:(a7800session *)session;
/* Show the panel (it stays up when it already is). */
+ (void)showWithSession:(a7800session *)session;
+ (BOOL)isVisible;
/* The panel is the key window (it has the keyboard). */
+ (BOOL)isActive;
/* Settings changed the gamepads or the bindings: read them again. */
+ (void)refresh;
@end
