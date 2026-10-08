/*
 * FujiNet Go Atari 7800 -- the macOS (AppKit) frontend.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import <Cocoa/Cocoa.h>

#import "AppDelegate.h"

#include "a7800session.h"

int main(int argc, const char **argv)
{
    @autoreleasepool {
        /* The runtime is packed beside the executable (see CMakeLists.txt);
         * tell the session where, so a fresh install provisions its tree
         * from the bundle rather than hunting. */
        a7800session_paths paths = { NULL, NULL, NULL, NULL };
        NSString *exeDir = [[[NSBundle mainBundle] executablePath] stringByDeletingLastPathComponent];
        NSString *runtime = [exeDir stringByAppendingPathComponent:@"fujinet"];
        if ([[NSFileManager defaultManager] fileExistsAtPath:runtime])
            paths.fujinet_runtime_src = [runtime fileSystemRepresentation];

        a7800session *session = a7800session_new(&paths);
        if (!session) {
            NSLog(@"Could not create the session (unusable config or data directories?)");
            return 1;
        }

        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        A7800AppDelegate *delegate = [[A7800AppDelegate alloc]
            initWithSession:session cartPath:(argc > 1 && argv[1][0] != '-' ? argv[1] : NULL)];
        [app setDelegate:delegate];
        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    return 0;
}
