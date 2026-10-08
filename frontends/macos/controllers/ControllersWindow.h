/*
 * The controllers panel: an on-screen ProLine joystick per player, the
 * console's switches, the session's actions, the connected gamepads and the
 * Map row, in a fixed-size floating panel.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "a7800session.h"

@interface A7800ControllersWindow : NSWindowController <NSWindowDelegate>
+ (void)toggleWithSession:(a7800session *)session;
+ (BOOL)isVisible;
@end
