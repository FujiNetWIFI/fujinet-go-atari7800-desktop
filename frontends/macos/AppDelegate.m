/*
 * A7800AppDelegate -- the window, the menu bar and the session's lifetime.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "AppDelegate.h"

#import "DisplayView.h"
#import "KeyForward.h"
#import "controllers/ControllersWindow.h"
#import "debugger/DebuggerWindow.h"

#include <stdlib.h>
#include <string.h>

#define APP_TITLE @"FujiNet Go Atari 7800"

/* How long a message (a gamepad coming or going, an import) stays over the
 * picture. */
#define TOAST_SECONDS 3.0

@class A7800AppDelegate;

/* The content view sits between AppKit and the session: it forwards keys
 * and the drop, and leaves the display purely about pixels (and the light
 * gun's pointer, which is about where the pixels are). */
@interface A7800ContentView : NSView
@property (nonatomic) a7800session *session;
@property (nonatomic, weak) A7800AppDelegate *owner;
@end

@interface A7800AppDelegate ()
- (void)runSysaction:(int)sa;
- (void)loadMedia:(NSString *)path;
- (void)flipSwitch:(int)sw;
@end

@implementation A7800ContentView

- (BOOL)acceptsFirstResponder { return YES; }

- (void)keyDown:(NSEvent *)e
{
    if ([e isARepeat]) return;
    /* Command shortcuts the menus did not take are not the console's keys. */
    if ([e modifierFlags] & NSEventModifierFlagCommand) return;
    const uint32_t ks = A7800KeysymFromEvent(e);
    if (!ks) return;
    /* Option+L / Option+R flip the difficulty switches. They are the Console
     * menu's key equivalents; this catches them should the menu not, so the
     * L and R they ride on never reach the bindings. */
    if (([e modifierFlags] & NSEventModifierFlagOption) && (ks == 'l' || ks == 'r')) {
        [self.owner flipSwitch:(ks == 'l' ? A7800_SW_LEFT_DIFF : A7800_SW_RIGHT_DIFF)];
        return;
    }
    if (ks == A7800_KEYSYM_F9) { [A7800ControllersWindow toggleWithSession:self.session]; return; }
    if (ks == A7800_KEYSYM_F11) { [[self window] toggleFullScreen:nil]; return; }
    if (ks == A7800_KEYSYM_F12) { [A7800DebuggerWindow toggleForSession:self.session]; return; }

    const int sa = a7800session_key_sysaction(self.session, ks);
    if (sa >= 0) { [self.owner runSysaction:sa]; return; }
    a7800session_key(self.session, ks, 1);
}

- (void)keyUp:(NSEvent *)e
{
    const uint32_t ks = A7800KeysymFromEvent(e);
    if (ks) a7800session_key(self.session, ks, 0);
}

/* Modifier keys arrive as flagsChanged, not keyDown/keyUp; they can be
 * bound like any other key and would otherwise never press. */
- (void)flagsChanged:(NSEvent *)e
{
    int down = 0;
    const uint32_t ks = A7800KeysymFromFlagsChange(e, &down);
    if (ks) a7800session_key(self.session, ks, down);
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
    (void)sender;
    return NSDragOperationCopy;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    NSArray *urls = [[sender draggingPasteboard] readObjectsForClasses:@[[NSURL class]] options:nil];
    if ([urls count] == 0) return NO;
    [self.owner loadMedia:[(NSURL *)urls[0] path]];
    return YES;
}
@end

@implementation A7800AppDelegate {
    a7800session *_session;
    const char *_cartPath;
    NSString *_launchFile;      /* a file Finder opened us with, before the session ran */
    NSWindow *_window;
    A7800DisplayView *_display;
    A7800ContentView *_content;
    NSTimer *_statusTimer;
    NSTimer *_sysactTimer;

    /* the transient message over the picture */
    NSTextField *_toast;
    unsigned _toastGeneration;
    NSUInteger _toastSerial;

    NSMenuItem *_aspectItem;
    NSMenuItem *_leftDiffItem, *_rightDiffItem;

    NSWindow *_settingsWindow;
    BOOL _sessionDirty;
    NSPopUpButton *_padList, *_padPort, *_aspectPopup;
    NSPopUpButton *_portPopup[2];
    NSPopUpButton *_biosPopup[2];   /* the NTSC console's, the PAL console's */
    NSButton *_hscBox;
    NSTimer *_settingsTimer;
    unsigned _padGeneration;

    NSWindow *_logWindow;
    NSTextView *_logView;
    NSTimer *_logTimer;
}

- (instancetype)initWithSession:(a7800session *)session cartPath:(const char *)cartPath
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _cartPath = cartPath;
    return self;
}

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    (void)note;

    /* 320x224 shown at 4:3, three times over: a comfortable first window on
     * any Mac. */
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 960, 720)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_window setTitle:APP_TITLE];
    [_window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
    [_window center];

    _content = [[A7800ContentView alloc] initWithFrame:[[_window contentView] bounds]];
    _content.session = _session;
    _content.owner = self;
    [_content setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [_content registerForDraggedTypes:@[NSPasteboardTypeFileURL]];

    _display = [[A7800DisplayView alloc] initWithSession:_session];
    [_display setFrame:[_content bounds]];
    [_display setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [_display setTvAspect:a7800session_get_int(_session, "aspect", 0) == 0];
    [_display setSmooth:a7800session_get_int(_session, "smooth", 0) != 0];
    [_content addSubview:_display];

    /* The toast: a label over the top of the picture that fades. Hidden when
     * faded, so it never takes the light gun's clicks. */
    _toast = [NSTextField labelWithString:@""];
    _toast.font = [NSFont boldSystemFontOfSize:13];
    _toast.textColor = NSColor.whiteColor;
    _toast.drawsBackground = YES;
    _toast.backgroundColor = [NSColor colorWithWhite:0.0 alpha:0.7];
    _toast.alignment = NSTextAlignmentCenter;
    _toast.alphaValue = 0.0;
    _toast.hidden = YES;
    [_toast setFrame:NSMakeRect(0, [_content bounds].size.height - 34, [_content bounds].size.width, 26)];
    [_toast setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
    [_content addSubview:_toast];

    [_window setContentView:_content];
    [self buildMenu];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_content];
    [_window setDelegate:self];

    a7800session_start_opts opts;
    a7800session_default_opts(_session, &opts);
    if (_cartPath) opts.cart_path = _cartPath;
    if (a7800session_start(_session, &opts) != 0) {
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Could not start"];
        [a setInformativeText:[NSString stringWithUTF8String:a7800session_last_error(_session)]];
        [a runModal];
    }
    _toastGeneration = a7800session_gamepad_generation(_session);
    if (_launchFile) {
        [self loadMedia:_launchFile];
        _launchFile = nil;
    }

    /* The family's launch hooks, for when the app misbehaves before a menu
     * is reachable. */
    if (getenv("A7800_OPEN_CONTROLLERS")) [A7800ControllersWindow toggleWithSession:_session];
    if (getenv("A7800_OPEN_DEBUGGER")) [A7800DebuggerWindow showForSession:_session];
    if (getenv("A7800_OPEN_SETTINGS")) [self showSettings:nil];

    _statusTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES
        block:^(NSTimer *t) { (void)t; [self updateTitle]; }];
    /* System actions the gamepad thread resolved (it cannot touch AppKit),
     * and gamepads coming and going. */
    _sysactTimer = [NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES
        block:^(NSTimer *t) {
            (void)t;
            int sa;
            while (a7800session_sysaction_take(self->_session, &sa)) [self runSysaction:sa];
            const unsigned gen = a7800session_gamepad_generation(self->_session);
            if (gen != self->_toastGeneration) {
                self->_toastGeneration = gen;
                char msg[160];
                if (a7800session_gamepad_last_event(self->_session, msg, sizeof msg) > 0)
                    [self showToast:[NSString stringWithUTF8String:msg]];
            }
        }];
    [self updateTitle];
}

- (void)showToast:(NSString *)text
{
    if (!text) return;
    _toast.stringValue = text;
    _toast.hidden = NO;
    _toast.alphaValue = 1.0;
    const NSUInteger serial = ++_toastSerial;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(TOAST_SECONDS * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        /* a newer message restarts the clock */
        if (serial != self->_toastSerial) return;
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
            ctx.duration = 0.5;
            self->_toast.animator.alphaValue = 0.0;
        } completionHandler:^{
            if (serial == self->_toastSerial) self->_toast.hidden = YES;
        }];
    });
}

- (void)runSysaction:(int)sa
{
    switch (sa) {
    case A7800_SYSACT_REBOOT_CONFIG: a7800session_reboot_to_config(_session); break;
    case A7800_SYSACT_PAUSE:
        /* the debugger window attaches, which stops the machine */
        [A7800DebuggerWindow showForSession:_session];
        break;
    default: break;
    }
    [self updateTitle];
}

- (void)updateTitle
{
    NSString *state;
    char st[160];
    if (!a7800session_is_running(_session)) {
        state = @"stopped";
    } else {
        a7800session_cart_status(_session, st, sizeof st);
        /* the link's dot: a title bar takes no colour, so the dot is there
         * only while the cartridge's link to FujiNet is up */
        state = [NSString stringWithFormat:@"%@FujiNet: %s",
                 a7800session_cart_link_up(_session) == 1 ? @"● " : @"", st];
        const char *cart = a7800session_cart_path(_session);
        if (cart && cart[0])
            state = [NSString stringWithFormat:@"%@ — %@",
                     [[NSString stringWithUTF8String:cart] lastPathComponent], state];
        /* the console that is running, and whether it has a BIOS */
        const int region = a7800session_running_region(_session);
        const int bios = a7800session_bios(_session, region);
        const BOOL withBios = bios >= 0 && a7800session_bios_available(_session, bios);
        state = [NSString stringWithFormat:@"%@ — %s, %@", state,
                 a7800_region_name(region), withBios ? @"BIOS" : @"no BIOS"];
    }
    /* The title bar is the status bar here: an AppKit window has no natural
     * place for one, and a floating HUD over the picture would be worse. */
    [_window setTitle:[NSString stringWithFormat:@"%@ — %@", APP_TITLE, state]];
}

/* Losing key status with keys held would leave the machine believing they
 * are still down. */
- (void)windowDidResignKey:(NSNotification *)note
{
    if (note.object == _window) a7800session_release_all(_session);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
    (void)app;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    (void)note;
    [_statusTimer invalidate];
    [_sysactTimer invalidate];
    [_display stop];
    a7800session_stop(_session);
    a7800session_free(_session);
}

/* Finder opens files here -- at launch before applicationDidFinishLaunching
 * has started the session, so a file then waits for it. */
- (BOOL)application:(NSApplication *)app openFile:(NSString *)filename
{
    (void)app;
    if (!a7800session_is_running(_session)) {
        _launchFile = [filename copy];
        return YES;
    }
    [self loadMedia:filename];
    return YES;
}

/* ---- menu ------------------------------------------------------------------ */

- (NSMenuItem *)item:(NSMenu *)menu title:(NSString *)title action:(SEL)sel key:(NSString *)key
{
    NSMenuItem *it = [menu addItemWithTitle:title action:sel keyEquivalent:key];
    [it setTarget:self];
    return it;
}

- (void)buildMenu
{
    NSMenu *bar = [[NSMenu alloc] init];

    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    NSMenu *appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:@"About " APP_TITLE action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [self item:appMenu title:@"Settings…" action:@selector(showSettings:) key:@","];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
    [appItem setSubmenu:appMenu];
    [bar addItem:appItem];

    NSMenuItem *machineItem = [[NSMenuItem alloc] init];
    NSMenu *machine = [[NSMenu alloc] initWithTitle:@"Machine"];
    [self item:machine title:@"Open Cartridge…" action:@selector(openCart:) key:@"o"];
    [self item:machine title:@"Eject Cartridge" action:@selector(ejectCart:) key:@""];
    [self item:machine title:@"Import Cartridge to SD…" action:@selector(importToSd:) key:@""];
    [self item:machine title:@"Import BIOS…" action:@selector(importBios:) key:@""];
    [machine addItem:[NSMenuItem separatorItem]];
    [self item:machine title:@"Power Cycle" action:@selector(powerCycle:) key:@""];
    /* Escape is a binding (Reboot to CONFIG), not a key equivalent: a menu
     * equivalent would take the key before the bindings table sees it and
     * could not be remapped. Cmd+R is the menu's own. */
    [self item:machine title:@"Reboot to CONFIG (Esc)" action:@selector(rebootConfig:) key:@"r"];
    [machineItem setSubmenu:machine];
    [bar addItem:machineItem];

    /* F1-F3 are bindings too, shown in the titles only (as equivalents they
     * would fire twice, and could not be remapped). The difficulty switches
     * have no binding of their own, so Option+L / Option+R are the menu's. */
    NSMenuItem *consoleItem = [[NSMenuItem alloc] init];
    NSMenu *console = [[NSMenu alloc] initWithTitle:@"Console"];
    [self item:console title:@"Select (F1)" action:@selector(pressSelect:) key:@""];
    [self item:console title:@"Reset (F2)" action:@selector(pressReset:) key:@""];
    [self item:console title:@"Pause (F3)" action:@selector(pressPause:) key:@""];
    [console addItem:[NSMenuItem separatorItem]];
    _leftDiffItem = [self item:console title:@"Left Difficulty A" action:@selector(toggleLeftDiff:) key:@"l"];
    [_leftDiffItem setKeyEquivalentModifierMask:NSEventModifierFlagOption];
    _rightDiffItem = [self item:console title:@"Right Difficulty A" action:@selector(toggleRightDiff:) key:@"r"];
    [_rightDiffItem setKeyEquivalentModifierMask:NSEventModifierFlagOption];
    [consoleItem setSubmenu:console];
    [bar addItem:consoleItem];

    NSMenuItem *viewItem = [[NSMenuItem alloc] init];
    NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
    [self item:view title:@"Controllers (F9)" action:@selector(toggleControllers:) key:@"j"];
    [self item:view title:@"Debugger (F12)" action:@selector(toggleDebugger:) key:@"d"];
    [view addItem:[NSMenuItem separatorItem]];
    _aspectItem = [self item:view title:@"TV Aspect (4:3)" action:@selector(toggleAspect:) key:@""];
    [_aspectItem setState:(a7800session_get_int(_session, "aspect", 0) == 0 ? NSControlStateValueOn : NSControlStateValueOff)];
    NSMenuItem *sm = [self item:view title:@"Smooth Scaling" action:@selector(toggleSmooth:) key:@""];
    [sm setState:(a7800session_get_int(_session, "smooth", 0) ? NSControlStateValueOn : NSControlStateValueOff)];
    NSMenuItem *fs = [view addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
    [fs setKeyEquivalentModifierMask:NSEventModifierFlagControl | NSEventModifierFlagCommand];
    [viewItem setSubmenu:view];
    [bar addItem:viewItem];

    NSMenuItem *fujiItem = [[NSMenuItem alloc] init];
    NSMenu *fuji = [[NSMenu alloc] initWithTitle:@"FujiNet"];
    [self item:fuji title:@"FujiNet Web UI" action:@selector(openWebUI:) key:@""];
    [self item:fuji title:@"Console Log" action:@selector(showFujiNetLog:) key:@""];
    [fujiItem setSubmenu:fuji];
    [bar addItem:fujiItem];

    [NSApp setMainMenu:bar];
}

/* The difficulty switches can be flipped from the controllers panel and a
 * gamepad too, so the menu reads their position when it opens. */
- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    if (item == _leftDiffItem)
        [item setState:a7800session_switch_get(_session, A7800_SW_LEFT_DIFF) ? NSControlStateValueOn : NSControlStateValueOff];
    else if (item == _rightDiffItem)
        [item setState:a7800session_switch_get(_session, A7800_SW_RIGHT_DIFF) ? NSControlStateValueOn : NSControlStateValueOff];
    return YES;
}

/* ---- actions ---------------------------------------------------------------- */

- (void)alert:(NSString *)title text:(NSString *)text
{
    NSAlert *a = [[NSAlert alloc] init];
    [a setMessageText:title];
    if (text) [a setInformativeText:text];
    [a runModal];
}

- (NSString *)lastError
{
    return [NSString stringWithUTF8String:a7800session_last_error(_session)] ?: @"";
}

- (void)openCartAtPath:(NSString *)path
{
    if (a7800session_load_cart(_session, [path fileSystemRepresentation]) != 0)
        [self alert:@"Could not open" text:[self lastError]];
    [self updateTitle];
}

- (void)importRomAtPath:(NSString *)path
{
    char what[160];
    if (a7800session_import_rom(_session, [path fileSystemRepresentation], what, sizeof what) != 0) {
        [self alert:@"Import failed" text:[self lastError]];
        return;
    }
    [self showToast:[NSString stringWithFormat:@"Imported the %s. Choose it in Settings.", what]];
    [self refreshSettings];
}

/* A dropped (or Finder-opened) file: a BIOS or High Score Cart ROM goes to
 * the ROM directory, a cartridge to the cartridge directory and into the
 * slot, anything else to FujiNet's SD folder. */
- (void)loadMedia:(NSString *)path
{
    const char *src = [path fileSystemRepresentation];
    if (a7800session_media_is_rom(src)) {
        [self importRomAtPath:path];
        return;
    }
    char dest[1024];
    if (a7800session_import_media(_session, src, dest, sizeof dest) != 0) {
        [self alert:@"Import failed" text:[self lastError]];
        return;
    }
    if (a7800session_media_is_cartridge(src)) {
        [self openCartAtPath:[NSString stringWithUTF8String:dest]];
        return;
    }
    [self showToast:[NSString stringWithFormat:@"Copied %@ to the SD folder. Mount it from CONFIG.",
                     [[NSString stringWithUTF8String:dest] lastPathComponent]]];
}

- (NSString *)pickCartridge:(NSString *)title
{
    NSOpenPanel *p = [NSOpenPanel openPanel];
    [p setTitle:title];
    [p setAllowedFileTypes:@[@"a78", @"bin", @"zip", @"7z"]];
    if ([p runModal] != NSModalResponseOK) return nil;
    return [[p URL] path];
}

- (void)openCart:(id)sender
{
    (void)sender;
    NSString *path = [self pickCartridge:@"Open Cartridge"];
    if (path) [self openCartAtPath:path];
}

- (void)ejectCart:(id)sender
{
    (void)sender;
    if (a7800session_eject(_session) != 0)
        [self alert:@"Could not eject" text:[self lastError]];
    [self updateTitle];
}

- (void)importToSd:(id)sender
{
    (void)sender;
    NSString *path = [self pickCartridge:@"Import Cartridge to SD"];
    if (!path) return;
    char dest[1024];
    if (a7800session_import_cart_to_sd(_session, [path fileSystemRepresentation], dest, sizeof dest) != 0) {
        [self alert:@"Import failed" text:[self lastError]];
        return;
    }
    [self showToast:[NSString stringWithFormat:@"%@ is on the SD host. Boot it from CONFIG.",
                     [[NSString stringWithUTF8String:dest] lastPathComponent]]];
}

/* Any file: BIOS images come under every name (7800.u7, a .a78, a .zip),
 * and the session recognises them by size and CRC. */
- (void)importBios:(id)sender
{
    (void)sender;
    NSOpenPanel *p = [NSOpenPanel openPanel];
    [p setTitle:@"Import BIOS (or High Score Cart ROM)"];
    if ([p runModal] != NSModalResponseOK) return;
    [self importRomAtPath:[[p URL] path]];
}

- (void)powerCycle:(id)sender
{
    (void)sender;
    if (a7800session_power_cycle(_session) != 0)
        [self alert:@"Could not power cycle" text:[self lastError]];
    [self updateTitle];
}

- (void)rebootConfig:(id)sender { (void)sender; [self runSysaction:A7800_SYSACT_REBOOT_CONFIG]; }
- (void)pressSelect:(id)sender { (void)sender; a7800session_switch_pulse(_session, A7800_SW_SELECT); }
- (void)pressReset:(id)sender { (void)sender; a7800session_switch_pulse(_session, A7800_SW_RESET); }
- (void)pressPause:(id)sender { (void)sender; a7800session_switch_pulse(_session, A7800_SW_PAUSE); }

- (void)flipSwitch:(int)sw
{
    a7800session_switch_set(_session, sw, !a7800session_switch_get(_session, sw));
}
- (void)toggleLeftDiff:(id)sender { (void)sender; [self flipSwitch:A7800_SW_LEFT_DIFF]; }
- (void)toggleRightDiff:(id)sender { (void)sender; [self flipSwitch:A7800_SW_RIGHT_DIFF]; }

- (void)toggleControllers:(id)sender { (void)sender; [A7800ControllersWindow toggleWithSession:_session]; }
- (void)toggleDebugger:(id)sender { (void)sender; [A7800DebuggerWindow toggleForSession:_session]; }

- (void)setAspect:(int)aspect
{
    a7800session_set_int(_session, "aspect", aspect);
    [_display setTvAspect:aspect == 0];
    [_aspectItem setState:(aspect == 0 ? NSControlStateValueOn : NSControlStateValueOff)];
    if (_aspectPopup) [_aspectPopup selectItemAtIndex:aspect];
}

- (void)toggleAspect:(id)sender
{
    (void)sender;
    [self setAspect:a7800session_get_int(_session, "aspect", 0) == 0 ? 1 : 0];
}

- (void)toggleSmooth:(id)sender
{
    NSMenuItem *item = sender;
    const BOOL on = ([item state] != NSControlStateValueOn);
    [item setState:(on ? NSControlStateValueOn : NSControlStateValueOff)];
    [_display setSmooth:on];
    a7800session_set_int(_session, "smooth", on ? 1 : 0);
}

/* ---- settings ----------------------------------------------------------------
 *
 * Same keys and defaults as the other frontends' Preferences, so a machine
 * configured in one comes up the same in another. The TV system and the
 * BIOS take effect at once (a new console); controllers, the analog stick,
 * the High Score Cart, the picture and the volume apply live; the host
 * options restart the session when the window closes.
 */

- (NSTextField *)sectionLabel:(NSString *)title
{
    NSTextField *label = [NSTextField labelWithString:title];
    label.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    return label;
}

- (NSTextField *)note:(NSString *)text
{
    NSTextField *n = [NSTextField labelWithString:text];
    n.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    n.textColor = NSColor.secondaryLabelColor;
    return n;
}

- (NSPopUpButton *)popUpForKey:(const char *)key fallback:(int)def names:(const char *(*)(int))names
{
    NSPopUpButton *popup = [[NSPopUpButton alloc] init];
    for (int i = 0; names(i); i++) [popup addItemWithTitle:[NSString stringWithUTF8String:names(i)]];
    NSInteger current = a7800session_get_int(_session, key, def);
    if (current < 0 || current >= (NSInteger)popup.numberOfItems) current = def;
    [popup selectItemAtIndex:current];
    popup.identifier = @(key);
    popup.target = self;
    popup.action = @selector(settingChanged:);
    return popup;
}

- (NSButton *)checkBoxForKey:(const char *)key title:(NSString *)title fallback:(int)def
{
    NSButton *box = [NSButton checkboxWithTitle:title target:self action:@selector(settingChanged:)];
    box.state = a7800session_get_int(_session, key, def) ? NSControlStateValueOn : NSControlStateValueOff;
    box.identifier = @(key);
    return box;
}

/* A controller port: Auto says what it resolved to for the running game. */
- (void)refreshPortTitles
{
    for (int port = 0; port < 2; port++) {
        if (!_portPopup[port]) continue;
        const char *now = a7800_ctrl_type_name(a7800session_port_detected(_session, port));
        NSString *title = [NSString stringWithFormat:@"Auto (now: %s)", now ? now : "?"];
        NSMenuItem *autoItem = [_portPopup[port] itemAtIndex:A7800_CTRL_AUTO];
        if (![autoItem.title isEqualToString:title]) autoItem.title = title;
    }
}

/* A console's BIOS: None, or one of MAME's for that console, each usable
 * once imported. The item tags are the session's BIOS indices. */
- (void)fillBiosPopup:(NSPopUpButton *)popup region:(int)region
{
    [popup removeAllItems];
    [popup addItemWithTitle:@"None (the cartridge starts directly)"];
    popup.lastItem.tag = -1;
    for (int i = 0; i < a7800session_bios_count(); i++) {
        const a7800_bios_info *b = a7800session_bios_info(i);
        if (!b || b->region != region) continue;
        const BOOL have = a7800session_bios_available(_session, i) != 0;
        [popup addItemWithTitle:(have ? [NSString stringWithUTF8String:b->desc]
                                      : [NSString stringWithFormat:@"%s (not imported)", b->desc])];
        popup.lastItem.tag = i;
        popup.lastItem.enabled = have;
    }
    if (![popup selectItemWithTag:a7800session_bios(_session, region)]) [popup selectItemAtIndex:0];
}

- (NSPopUpButton *)biosPopupForRegion:(int)region
{
    NSPopUpButton *popup = [[NSPopUpButton alloc] init];
    /* the not-yet-imported entries stay greyed out */
    popup.autoenablesItems = NO;
    popup.identifier = region == A7800_REGION_PAL ? @"bios_pal" : @"bios_ntsc";
    popup.target = self;
    popup.action = @selector(settingChanged:);
    [self fillBiosPopup:popup region:region];
    return popup;
}

/* What another window (an import, the controllers panel) may have changed. */
- (void)refreshSettings
{
    if (!_settingsWindow) return;
    [self fillBiosPopup:_biosPopup[0] region:A7800_REGION_NTSC];
    [self fillBiosPopup:_biosPopup[1] region:A7800_REGION_PAL];
    _hscBox.enabled = a7800session_hsc_available(_session) != 0;
    _hscBox.state = a7800session_hsc(_session) ? NSControlStateValueOn : NSControlStateValueOff;
    for (int port = 0; port < 2; port++)
        [_portPopup[port] selectItemAtIndex:a7800session_port_type(_session, port)];
    [self refreshPortTitles];
}

- (void)refreshPadList
{
    const NSInteger sel = _padList.indexOfSelectedItem;
    [_padList removeAllItems];
    const int n = a7800session_gamepad_count(_session);
    if (n == 0) [_padList addItemWithTitle:@"(no gamepads connected)"];
    for (int i = 0; i < n; i++) {
        char name[128];
        const int eff = a7800session_gamepad_effective_port(_session, i);
        a7800session_gamepad_name(_session, i, name, sizeof name);
        /* Menu items with equal titles collapse; the index keeps them apart. */
        [_padList addItemWithTitle:(eff >= 0
            ? [NSString stringWithFormat:@"%d: %s  [player %d]", i + 1, name, eff + 1]
            : [NSString stringWithFormat:@"%d: %s  [unused]", i + 1, name])];
    }
    if (sel >= 0 && sel < n) [_padList selectItemAtIndex:sel];
    [self padSelected:nil];
}

- (void)padSelected:(id)sender
{
    (void)sender;
    const NSInteger sel = _padList.indexOfSelectedItem;
    if (sel >= 0 && sel < a7800session_gamepad_count(_session))
        [_padPort selectItemAtIndex:a7800session_gamepad_assignment(_session, (int)sel) + 1];
}

- (void)padPortChanged:(id)sender
{
    (void)sender;
    const NSInteger sel = _padList.indexOfSelectedItem;
    if (sel >= 0 && sel < a7800session_gamepad_count(_session))
        a7800session_gamepad_assign(_session, (int)sel, (int)_padPort.indexOfSelectedItem - 1);
    [self refreshPadList];
}

- (void)volumeChanged:(NSSlider *)sender
{
    a7800session_set_volume(_session, (int)sender.integerValue);
}

- (void)aspectChanged:(NSPopUpButton *)sender
{
    [self setAspect:(int)sender.indexOfSelectedItem];
}

- (void)settingChanged:(id)sender
{
    NSControl *control = sender;
    const char *key = [control.identifier UTF8String];
    int value;

    if (!strcmp(key, "bios_ntsc") || !strcmp(key, "bios_pal")) {
        const NSInteger tag = ((NSPopUpButton *)control).selectedItem.tag;
        a7800session_set_bios(_session, key[5] == 'p' ? A7800_REGION_PAL : A7800_REGION_NTSC, (int)tag);
        [self updateTitle];
        return;
    }
    if ([control isKindOfClass:[NSPopUpButton class]])
        value = (int)((NSPopUpButton *)control).indexOfSelectedItem;
    else
        value = ((NSButton *)control).state == NSControlStateValueOn ? 1 : 0;

    if (!strcmp(key, "port0_type") || !strcmp(key, "port1_type")) {
        a7800session_set_port_type(_session, key[4] == '1', value);
        return;
    }
    if (!strcmp(key, "region")) {
        a7800session_set_region(_session, value);
        [self updateTitle];
        return;
    }
    if (!strcmp(key, "hsc")) {
        a7800session_set_hsc(_session, value);
        return;
    }
    if (!strcmp(key, "analog_joystick")) {
        a7800session_set_analog(_session, value);
        return;
    }
    a7800session_set_int(_session, key, value);
    _sessionDirty = YES;
}

- (void)showSettings:(id)sender
{
    (void)sender;
    if (_settingsWindow) {
        [self refreshSettings];
        [_settingsWindow makeKeyAndOrderFront:nil];
        return;
    }

    _padList = [[NSPopUpButton alloc] init];
    _padList.target = self;
    _padList.action = @selector(padSelected:);
    _padPort = [[NSPopUpButton alloc] init];
    [_padPort addItemsWithTitles:@[@"Automatic", @"Player 1", @"Player 2"]];
    _padPort.target = self;
    _padPort.action = @selector(padPortChanged:);

    _aspectPopup = [[NSPopUpButton alloc] init];
    [_aspectPopup addItemsWithTitles:@[@"4:3 TV", @"Square pixels"]];
    [_aspectPopup selectItemAtIndex:(a7800session_get_int(_session, "aspect", 0) ? 1 : 0)];
    _aspectPopup.target = self;
    _aspectPopup.action = @selector(aspectChanged:);

    _biosPopup[0] = [self biosPopupForRegion:A7800_REGION_NTSC];
    _biosPopup[1] = [self biosPopupForRegion:A7800_REGION_PAL];
    _hscBox = [self checkBoxForKey:"hsc" title:@"High Score Cart (games keep their high scores)" fallback:0];
    _hscBox.enabled = a7800session_hsc_available(_session) != 0;
    for (int port = 0; port < 2; port++)
        _portPopup[port] = [self popUpForKey:(port ? "port1_type" : "port0_type")
                                    fallback:A7800_CTRL_AUTO names:a7800_ctrl_type_name];
    [self refreshPortTitles];

    NSSlider *volume = [NSSlider sliderWithValue:a7800session_get_int(_session, "volume", 100)
                                        minValue:0 maxValue:100 target:self action:@selector(volumeChanged:)];
    [volume.widthAnchor constraintEqualToConstant:220].active = YES;

    NSStackView *padRow = [NSStackView stackViewWithViews:@[_padList, _padPort]];
    padRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;

    NSArray<NSArray<NSView *> *> *rows = @[
        @[ [self sectionLabel:@"Machine"], [self note:@"the TV system and the BIOS take effect at once (a new console)"] ],
        @[ [NSTextField labelWithString:@"TV system"],
           [self popUpForKey:"region" fallback:A7800_REGION_AUTO names:a7800_region_name] ],
        @[ [NSTextField labelWithString:@"NTSC BIOS"], _biosPopup[0] ],
        @[ [NSTextField labelWithString:@"PAL BIOS"], _biosPopup[1] ],
        @[ [NSTextField labelWithString:@""], [self note:@"No BIOS is needed. Machine ▸ Import BIOS… adds your own."] ],
        @[ [NSTextField labelWithString:@""], _hscBox ],
        @[ [NSTextField labelWithString:@"Picture"], _aspectPopup ],
        @[ [self sectionLabel:@"Controllers"], [self note:@"applied immediately"] ],
        @[ [NSTextField labelWithString:@"Player 1"], _portPopup[0] ],
        @[ [NSTextField labelWithString:@"Player 2"], _portPopup[1] ],
        @[ [NSTextField labelWithString:@""],
           [self checkBoxForKey:"analog_joystick" title:@"Analog sticks drive the joystick" fallback:1] ],
        @[ [NSTextField labelWithString:@"Gamepads"], padRow ],
        @[ [NSTextField labelWithString:@"Volume"], volume ],
        @[ [self sectionLabel:@"Host"], [self note:@"applied by restarting the session"] ],
        @[ [NSTextField labelWithString:@""], [self checkBoxForKey:"enable_fujinet" title:@"Enable FujiNet" fallback:1] ],
        @[ [NSTextField labelWithString:@""], [self checkBoxForKey:"enable_audio" title:@"Audio" fallback:1] ],
        @[ [NSTextField labelWithString:@""], [self checkBoxForKey:"enable_gamepad" title:@"Gamepads" fallback:1] ],
    ];

    NSGridView *grid = [NSGridView gridViewWithViews:rows];
    grid.rowSpacing = 8;
    grid.columnSpacing = 12;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;

    NSStackView *root = [NSStackView stackViewWithViews:@[grid]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.edgeInsets = NSEdgeInsetsMake(16, 16, 16, 16);

    _settingsWindow = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 600, 620)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _settingsWindow.title = @"Settings";
    _settingsWindow.releasedWhenClosed = NO;
    _settingsWindow.delegate = self;
    _settingsWindow.contentView = root;
    [_settingsWindow center];

    _padGeneration = a7800session_gamepad_generation(_session);
    [self refreshPadList];
    [self startSettingsTimer];
    [_settingsWindow makeKeyAndOrderFront:nil];
}

/* Gamepads coming and going, and what Auto resolves to, while the window is
 * up. The timer goes with the window: reopening starts it again. */
- (void)startSettingsTimer
{
    if (_settingsTimer) return;
    _settingsTimer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t) {
        (void)t;
        const unsigned gen = a7800session_gamepad_generation(self->_session);
        if (gen != self->_padGeneration) { self->_padGeneration = gen; [self refreshPadList]; }
        [self refreshPortTitles];
    }];
}

- (void)windowWillClose:(NSNotification *)note
{
    if (note.object == _logWindow) {
        [_logTimer invalidate];
        _logTimer = nil;
        return;
    }
    if (note.object != _settingsWindow) return;
    [_settingsTimer invalidate];
    _settingsTimer = nil;
    if (!_sessionDirty) return;
    _sessionDirty = NO;

    a7800session_start_opts o;
    a7800session_settings_flush(_session);
    a7800session_default_opts(_session, &o);
    a7800session_stop(_session);
    if (a7800session_start(_session, &o) != 0)
        [self alert:@"Could not start" text:[self lastError]];
    [self updateTitle];
}

- (void)windowDidBecomeKey:(NSNotification *)note
{
    if (note.object == _settingsWindow) [self startSettingsTimer];
}

/* ---- FujiNet console log ------------------------------------------------------- */

- (void)refreshLog:(NSTimer *)timer
{
    (void)timer;
    static char buf[128 * 1024];
    const int n = a7800session_fujinet_copy_log(_session, buf, sizeof buf);
    NSScrollView *scroll = (NSScrollView *)_logView.enclosingScrollView;
    const BOOL atEnd = !scroll || (NSMaxY(scroll.contentView.documentVisibleRect) >=
                                   NSMaxY(((NSView *)scroll.documentView).frame) - 4.0);
    NSString *text = n > 0 ? [NSString stringWithUTF8String:buf] : nil;
    [_logView setString:(text ?: @"(no FujiNet output yet)")];
    if (atEnd) [_logView scrollRangeToVisible:NSMakeRange(_logView.string.length, 0)];
}

- (void)showFujiNetLog:(id)sender
{
    (void)sender;
    if (_logWindow) {
        [_logWindow makeKeyAndOrderFront:nil];
        if (!_logTimer)
            _logTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self
                                                       selector:@selector(refreshLog:) userInfo:nil repeats:YES];
        return;
    }

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 860, 600)];
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers = NO;

    _logView = [[NSTextView alloc] initWithFrame:scroll.bounds];
    _logView.editable = NO;
    _logView.richText = NO;
    _logView.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    _logView.autoresizingMask = NSViewWidthSizable;
    scroll.documentView = _logView;

    _logWindow = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 860, 600)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _logWindow.title = @"FujiNet Console Log";
    _logWindow.releasedWhenClosed = NO;
    _logWindow.delegate = self;
    _logWindow.contentView = scroll;
    [_logWindow center];

    _logTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self
                                               selector:@selector(refreshLog:) userInfo:nil repeats:YES];
    [self refreshLog:nil];
    [_logWindow makeKeyAndOrderFront:nil];
}

- (void)openWebUI:(id)sender
{
    (void)sender;
    if (!a7800session_fujinet_running(_session)) {
        [self alert:@"FujiNet is not running" text:nil];
        return;
    }
    [[NSWorkspace sharedWorkspace] openURL:
        [NSURL URLWithString:[NSString stringWithUTF8String:a7800session_fujinet_webui_url(_session)]]];
}
@end
