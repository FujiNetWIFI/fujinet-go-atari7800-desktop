/*
 * The application delegate: the window, the menu bar, and the session.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "a7800session.h"

@interface A7800AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
- (instancetype)initWithSession:(a7800session *)session cartPath:(const char *)cartPath;
@end
