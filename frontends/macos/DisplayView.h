/*
 * The emulator display.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "a7800session.h"

@interface A7800DisplayView : NSView
- (instancetype)initWithSession:(a7800session *)session;
/* YES: the picture at 4:3, as a TV showed it (setting "aspect" 0); NO:
 * square pixels. */
- (void)setTvAspect:(BOOL)tv;
- (void)setSmooth:(BOOL)smooth;
- (void)stop;
@end
