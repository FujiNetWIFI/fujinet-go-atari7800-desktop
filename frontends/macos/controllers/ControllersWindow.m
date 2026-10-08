/*
 * A7800ControllersWindow -- the AppKit controllers panel: a ProLine
 * joystick per player (the stick and its two buttons; a 2600 joystick uses
 * Button 1, a light gun's trigger is Button 1 too), the console's switches
 * (Select, Reset and Pause, held while the mouse is; the difficulty
 * switches, which toggle), the session's actions, the gamepads that are
 * connected and which player each drives, and the Map row.
 *
 * Buttons are a PadButton subclass that reports mouseDown and mouseUp
 * rather than an action on click: a joystick button is HELD, and a game
 * reads the controls once a frame, so a value present only for the instant
 * of a click falls between frames. mouseUp arrives even when the pointer has
 * left the button (AppKit tracks the drag for the view that got mouseDown),
 * so dragging off cannot strand the machine with a button held.
 *
 * Whatever holds a control -- the keyboard, a gamepad or a click here -- it
 * lights up in the accent colour, from a7800session_buttons_held and the
 * switches' state.
 *
 * A utility panel of fixed size (no resize mask). Singleton, ordered out
 * rather than closed, so its position survives.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "ControllersWindow.h"

#import "../KeyForward.h"

#include <string.h>

/* -2 idle, -1 armed and waiting for a target, >= 0 waiting for a key or
 * pad button. */
static int g_mapState = -2;
static A7800ControllersWindow *g_singleton;
static a7800session *g_session;

#define MAX_PAD_ROWS 4

static BOOL isDifficulty(int target)
{
    return target == A7800_TARGET_SWITCH(A7800_SW_LEFT_DIFF) ||
           target == A7800_TARGET_SWITCH(A7800_SW_RIGHT_DIFF);
}

/* NOT named `target`: NSControl already has a `target` property. */
@interface PadButton : NSButton
@property (nonatomic) int padTarget;
@property (nonatomic, copy) NSString *face;
@property (nonatomic) BOOL down;
@property (nonatomic) BOOL lit;
@end

@implementation PadButton
- (void)setLitState:(BOOL)lit
{
    if (lit == self.lit) return;
    self.lit = lit;
    self.bezelColor = lit ? A7800AccentColor() : nil;
}

- (void)mouseDown:(NSEvent *)e
{
    (void)e;
    if (g_mapState == -1) {
        g_mapState = self.padTarget;
        [[NSNotificationCenter defaultCenter] postNotificationName:@"A7800PadMapTarget" object:nil];
        return;
    }
    if (g_mapState >= 0 || !self.enabled) return;
    self.down = YES;
    [self setLitState:YES];
    if (self.padTarget >= A7800_TARGET_SYSACT(0)) return;   /* fires on release */
    /* the stick, its buttons and the momentary switches hold while the
     * mouse does; a difficulty switch flips on the press */
    a7800session_press(g_session, self.padTarget, 1);
}

- (void)mouseUp:(NSEvent *)e
{
    (void)e;
    if (!self.down) return;
    self.down = NO;
    [self setLitState:NO];
    if (g_mapState != -2) return;
    if (self.padTarget >= A7800_TARGET_SYSACT(0)) {
        /* System actions fire on release, like a real button: pressing and
         * dragging off must not reboot the machine. Posted, so the
         * application runs them exactly as it runs a gamepad's. */
        a7800session_sysaction_post(g_session, self.padTarget - A7800_TARGET_SYSACT(0));
        return;
    }
    a7800session_press(g_session, self.padTarget, 0);
}
@end

@implementation A7800ControllersWindow {
    NSMutableArray<PadButton *> *_buttons;
    NSButton *_mapButton;
    NSTextField *_hint;
    NSPopUpButton *_typePopup[2];
    NSTextField *_padName[MAX_PAD_ROWS];
    NSPopUpButton *_padAssign[MAX_PAD_ROWS];
    NSTextField *_noPads;
    unsigned _padGeneration;
    NSTimer *_captureTimer;
    NSTimer *_liveTimer;
}

#define KEY_W   60.0
#define KEY_H   34.0
#define GAP      6.0
#define BTN_W   96.0
#define GROUP  24.0
#define DPAD_W (3 * KEY_W + 2 * GAP)
#define BTNS_W (2 * BTN_W + GAP)
#define PAD_W  (DPAD_W + GROUP + BTNS_W)
#define PAD_H  (3 * KEY_H + 2 * GAP)
#define MARGIN  12.0
#define ROW_H   26.0
#define TITLE_H 18.0

- (PadButton *)buttonWithFace:(NSString *)face target:(int)target frame:(NSRect)frame
{
    PadButton *b = [[PadButton alloc] initWithFrame:frame];
    [b setTitle:face];
    [b setBezelStyle:NSBezelStyleRounded];
    b.padTarget = target;
    b.face = face;
    /* Not focusable: clicking a pad button must not steal the key window's
     * first responder. */
    [b setRefusesFirstResponder:YES];
    [_buttons addObject:b];
    return b;
}

/* The pad's cell (col, row) inside a block whose top-left is (x, top);
 * AppKit's y grows upwards, so rows are placed by their bottom edge. */
static NSRect cell(CGFloat x, CGFloat top, int col, int row, CGFloat w)
{
    return NSMakeRect(x + col * (w + GAP), top - (row + 1) * KEY_H - row * GAP, w, KEY_H);
}

- (NSTextField *)sectionTitle:(NSString *)text at:(CGFloat)y in:(NSView *)parent
{
    NSTextField *t = [NSTextField labelWithString:text];
    [t setFont:[NSFont boldSystemFontOfSize:12]];
    [t setFrame:NSMakeRect(MARGIN, y - 16, 200, 16)];
    [parent addSubview:t];
    return t;
}

/* One joystick, laid out downwards from topY. Returns the y below it. */
- (CGFloat)buildController:(int)port intoView:(NSView *)parent topY:(CGFloat)topY
{
    CGFloat y = topY;
    const CGFloat x0 = MARGIN;

    NSTextField *title = [NSTextField labelWithString:(port ? @"Player 2" : @"Player 1")];
    [title setFont:[NSFont boldSystemFontOfSize:12]];
    [title setFrame:NSMakeRect(x0, y - 20, 80, 16)];
    [parent addSubview:title];

    /* What is plugged into the port; Auto says what the game asked for. */
    _typePopup[port] = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(x0 + 90, y - 24, 240, 24)];
    for (int i = 0; a7800_ctrl_type_name(i); i++)
        [_typePopup[port] addItemWithTitle:[NSString stringWithUTF8String:a7800_ctrl_type_name(i)]];
    _typePopup[port].tag = port;
    _typePopup[port].target = self;
    _typePopup[port].action = @selector(typeChanged:);
    [_typePopup[port] setRefusesFirstResponder:YES];
    [parent addSubview:_typePopup[port]];
    y -= ROW_H + GAP;

    /* the stick */
    [parent addSubview:[self buttonWithFace:@"▲" target:A7800_TARGET_PORT(port, A7800_ACT_UP)
                                      frame:cell(x0, y, 1, 0, KEY_W)]];
    [parent addSubview:[self buttonWithFace:@"◀" target:A7800_TARGET_PORT(port, A7800_ACT_LEFT)
                                      frame:cell(x0, y, 0, 1, KEY_W)]];
    [parent addSubview:[self buttonWithFace:@"▶" target:A7800_TARGET_PORT(port, A7800_ACT_RIGHT)
                                      frame:cell(x0, y, 2, 1, KEY_W)]];
    [parent addSubview:[self buttonWithFace:@"▼" target:A7800_TARGET_PORT(port, A7800_ACT_DOWN)
                                      frame:cell(x0, y, 1, 2, KEY_W)]];

    /* the ProLine's two buttons, left and right, level with the stick */
    const CGFloat bx = x0 + DPAD_W + GROUP;
    [parent addSubview:[self buttonWithFace:@"Button 1" target:A7800_TARGET_PORT(port, A7800_ACT_BUTTON1)
                                      frame:cell(bx, y, 0, 1, BTN_W)]];
    [parent addSubview:[self buttonWithFace:@"Button 2" target:A7800_TARGET_PORT(port, A7800_ACT_BUTTON2)
                                      frame:cell(bx, y, 1, 1, BTN_W)]];
    return y - PAD_H;
}

/* A row of buttons across the panel's width. */
- (CGFloat)buildRow:(NSView *)parent topY:(CGFloat)y faces:(NSArray<NSString *> *)faces targets:(const int *)targets
{
    const NSUInteger n = faces.count;
    const CGFloat w = (PAD_W - (CGFloat)(n - 1) * GAP) / (CGFloat)n;
    for (NSUInteger i = 0; i < n; i++)
        [parent addSubview:[self buttonWithFace:faces[i] target:targets[i]
                                          frame:NSMakeRect(MARGIN + i * (w + GAP), y - KEY_H, w, KEY_H)]];
    return y - KEY_H - GAP;
}

- (instancetype)init
{
    const CGFloat width = MARGIN + PAD_W + MARGIN;
    const CGFloat height = MARGIN
        + 2 * (ROW_H + GAP + PAD_H + MARGIN)      /* the two joysticks */
        + TITLE_H + 3 * (KEY_H + GAP) + MARGIN    /* the console: switches, difficulty, actions */
        + TITLE_H + MAX_PAD_ROWS * ROW_H + MARGIN /* gamepads */
        + 30 + MARGIN;                            /* map row */

    NSPanel *win = [[NSPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, width, height)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [win setTitle:@"Controllers"];
    /* Floats above the machine's window and stays out of the way. */
    [win setLevel:NSFloatingWindowLevel];
    [win setReleasedWhenClosed:NO];
    [win setBecomesKeyOnlyIfNeeded:NO];

    self = [super initWithWindow:win];
    if (!self) return nil;
    [win setDelegate:self];

    _buttons = [NSMutableArray array];
    NSView *content = [win contentView];
    CGFloat y = height - MARGIN;
    y = [self buildController:0 intoView:content topY:y] - MARGIN;
    y = [self buildController:1 intoView:content topY:y] - MARGIN;

    /* The console's switches, and the session's own actions. */
    [self sectionTitle:@"Console" at:y in:content];
    y -= TITLE_H;
    static const int switches[3] = {
        A7800_TARGET_SWITCH(A7800_SW_SELECT), A7800_TARGET_SWITCH(A7800_SW_RESET),
        A7800_TARGET_SWITCH(A7800_SW_PAUSE),
    };
    y = [self buildRow:content topY:y faces:@[@"Select", @"Reset", @"Pause"] targets:switches];
    static const int difficulty[2] = {
        A7800_TARGET_SWITCH(A7800_SW_LEFT_DIFF), A7800_TARGET_SWITCH(A7800_SW_RIGHT_DIFF),
    };
    y = [self buildRow:content topY:y faces:@[@"Left Difficulty", @"Right Difficulty"] targets:difficulty];
    static const int actions[2] = {
        A7800_TARGET_SYSACT(A7800_SYSACT_REBOOT_CONFIG), A7800_TARGET_SYSACT(A7800_SYSACT_PAUSE),
    };
    y = [self buildRow:content topY:y faces:@[@"Reboot to CONFIG", @"Break (Debugger)"] targets:actions];
    y -= MARGIN - GAP;

    /* The gamepads: which player each one drives. */
    [self sectionTitle:@"Gamepads" at:y in:content];
    y -= TITLE_H;
    _noPads = [NSTextField labelWithString:@"No gamepads connected"];
    [_noPads setTextColor:[NSColor secondaryLabelColor]];
    [_noPads setFrame:NSMakeRect(MARGIN, y - 20, PAD_W, 18)];
    [content addSubview:_noPads];
    for (int i = 0; i < MAX_PAD_ROWS; i++) {
        const CGFloat ry = y - (i + 1) * ROW_H;
        _padName[i] = [NSTextField labelWithString:@""];
        [_padName[i] setFrame:NSMakeRect(MARGIN, ry + 4, PAD_W - 170, 18)];
        [_padName[i] setLineBreakMode:NSLineBreakByTruncatingTail];
        [content addSubview:_padName[i]];
        _padAssign[i] = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(MARGIN + PAD_W - 160, ry, 160, 24)];
        [_padAssign[i] addItemsWithTitles:@[@"Automatic", @"Player 1", @"Player 2"]];
        _padAssign[i].tag = i;
        _padAssign[i].target = self;
        _padAssign[i].action = @selector(padAssignChanged:);
        [_padAssign[i] setRefusesFirstResponder:YES];
        [content addSubview:_padAssign[i]];
    }
    y -= MAX_PAD_ROWS * ROW_H + MARGIN;

    _mapButton = [NSButton buttonWithTitle:@"Map" target:self action:@selector(toggleMap:)];
    [_mapButton setFrame:NSMakeRect(MARGIN, y - 30, 70, 30)];
    [_mapButton setRefusesFirstResponder:YES];
    [content addSubview:_mapButton];

    NSButton *defaults = [NSButton buttonWithTitle:@"Defaults" target:self action:@selector(restoreDefaults:)];
    [defaults setFrame:NSMakeRect(MARGIN + 76, y - 30, 90, 30)];
    [defaults setRefusesFirstResponder:YES];
    [content addSubview:defaults];

    _hint = [NSTextField labelWithString:@""];
    [_hint setFrame:NSMakeRect(MARGIN + 176, y - 24, width - MARGIN - 190, 18)];
    [_hint setTextColor:[NSColor secondaryLabelColor]];
    [_hint setLineBreakMode:NSLineBreakByTruncatingTail];
    [content addSubview:_hint];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(refresh)
                                                 name:@"A7800PadMapTarget" object:nil];
    _padGeneration = (unsigned)-1;
    [self refresh];
    [self refreshPads];
    return self;
}

/* ---- the controls ------------------------------------------------------------ */

- (void)typeChanged:(NSPopUpButton *)sender
{
    a7800session_set_port_type(g_session, (int)sender.tag, (int)sender.indexOfSelectedItem);
    [self refreshTypes];
}

- (void)padAssignChanged:(NSPopUpButton *)sender
{
    a7800session_gamepad_assign(g_session, (int)sender.tag, (int)sender.indexOfSelectedItem - 1);
    [self refreshPads];
}

- (void)refreshPads
{
    _padGeneration = a7800session_gamepad_generation(g_session);
    const int n = a7800session_gamepad_count(g_session);
    _noPads.hidden = n > 0;
    for (int i = 0; i < MAX_PAD_ROWS; i++) {
        const BOOL shown = i < n;
        _padName[i].hidden = !shown;
        _padAssign[i].hidden = !shown;
        if (!shown) continue;
        char name[128];
        a7800session_gamepad_name(g_session, i, name, sizeof name);
        const int eff = a7800session_gamepad_effective_port(g_session, i);
        _padName[i].stringValue = eff >= 0
            ? [NSString stringWithFormat:@"%s — player %d", name, eff + 1]
            : [NSString stringWithFormat:@"%s — unused", name];
        [_padAssign[i] selectItemAtIndex:a7800session_gamepad_assignment(g_session, i) + 1];
    }
    for (int port = 0; port < 2; port++)
        [_typePopup[port] selectItemAtIndex:a7800session_port_type(g_session, port)];
    [self refreshTypes];
}

/* What each port has now (Auto resolved for the running game), in the
 * popup's Auto entry, and which of the joystick's controls mean anything
 * with it: a 2600 joystick has one button, a light gun only its trigger. */
- (void)refreshTypes
{
    for (int port = 0; port < 2; port++) {
        const int detected = a7800session_port_detected(g_session, port);
        const char *now = a7800_ctrl_type_name(detected);
        NSString *title = [NSString stringWithFormat:@"Auto (now: %s)", now ? now : "?"];
        NSMenuItem *autoItem = [_typePopup[port] itemAtIndex:A7800_CTRL_AUTO];
        if (![autoItem.title isEqualToString:title]) autoItem.title = title;

        int type = a7800session_port_type(g_session, port);
        if (type == A7800_CTRL_AUTO) type = detected;
        const BOOL stick = type == A7800_CTRL_PROLINE || type == A7800_CTRL_JOY2600;
        for (PadButton *b in _buttons) {
            if (b.padTarget >= A7800_TARGET_SWITCH(0) || b.padTarget / A7800_ACT_PER_PORT != port) continue;
            const int act = b.padTarget % A7800_ACT_PER_PORT;
            BOOL use;
            if (act == A7800_ACT_BUTTON1) use = type != A7800_CTRL_NONE;
            else if (act == A7800_ACT_BUTTON2) use = type == A7800_CTRL_PROLINE;
            else use = stick;
            /* Map mode can bind anything, whatever is plugged in */
            b.enabled = use || g_mapState != -2;
        }
    }
}

/* A button's face: the difficulty switches say where they are. */
- (NSString *)faceFor:(PadButton *)b
{
    if (b.padTarget == A7800_TARGET_SWITCH(A7800_SW_LEFT_DIFF))
        return [NSString stringWithFormat:@"%@: %@", b.face,
                a7800session_switch_get(g_session, A7800_SW_LEFT_DIFF) ? @"A" : @"B"];
    if (b.padTarget == A7800_TARGET_SWITCH(A7800_SW_RIGHT_DIFF))
        return [NSString stringWithFormat:@"%@: %@", b.face,
                a7800session_switch_get(g_session, A7800_SW_RIGHT_DIFF) ? @"A" : @"B"];
    return b.face;
}

/* The live part, twenty times a second while the panel is up: what is held,
 * where the difficulty switches are, and whether a gamepad came or went. */
- (void)tick
{
    if (a7800session_gamepad_generation(g_session) != _padGeneration) [self refreshPads];
    [self refreshTypes];
    if (g_mapState != -2) return;
    const unsigned held[2] = { a7800session_buttons_held(g_session, 0), a7800session_buttons_held(g_session, 1) };
    for (PadButton *b in _buttons) {
        if (isDifficulty(b.padTarget)) {
            NSString *face = [self faceFor:b];
            if (![b.title isEqualToString:face]) [b setTitle:face];
            continue;
        }
        if (b.down || b.padTarget >= A7800_TARGET_SYSACT(0)) continue;
        if (b.padTarget >= A7800_TARGET_SWITCH(0)) {
            /* Select, Reset, Pause: held from the keyboard or a gamepad too */
            [b setLitState:a7800session_switch_get(g_session, b.padTarget - A7800_TARGET_SWITCH(0)) != 0];
            continue;
        }
        const int port = b.padTarget / A7800_ACT_PER_PORT;
        const int act = b.padTarget % A7800_ACT_PER_PORT;
        [b setLitState:(held[port] & (1u << act)) != 0];
    }
}

/* ---- Map mode ---------------------------------------------------------------- */

- (void)toggleMap:(id)sender
{
    (void)sender;
    _hint.stringValue = @"";
    [self setMapState:(g_mapState == -2) ? -1 : -2];
}

- (void)restoreDefaults:(id)sender
{
    (void)sender;
    a7800session_bindings_reset(g_session);
    _hint.stringValue = @"Default bindings restored";
    [self refresh];
}

- (void)setMapState:(int)state
{
    g_mapState = state;
    [_captureTimer invalidate];
    _captureTimer = nil;
    if (state >= 0) {
        a7800session_gamepad_capture_begin(g_session);
        _captureTimer = [NSTimer scheduledTimerWithTimeInterval:0.05 repeats:YES block:^(NSTimer *t) {
            (void)t;
            [self pollCapture];
        }];
    } else {
        a7800session_gamepad_capture_cancel(g_session);
        if (state == -2) _hint.stringValue = @"";
    }
    [self refresh];
}

- (void)pollCapture
{
    int button;
    if (g_mapState < 0) return;
    if (a7800session_gamepad_capture_poll(g_session, &button)) {
        char stolen[128];
        const int target = g_mapState;
        a7800session_binding_set_button(g_session, target, button, stolen, sizeof stolen);
        [self setMapState:-1];   /* stay armed: remapping several in a row is normal */
        _hint.stringValue = stolen[0]
            ? [NSString stringWithFormat:@"%s: %s (taken from %s)", a7800_target_name(target),
                                         a7800_pad_button_name(button), stolen]
            : [NSString stringWithFormat:@"%s: %s", a7800_target_name(target), a7800_pad_button_name(button)];
    }
}

/* In Map mode every button shows its binding; otherwise its face. */
- (void)refresh
{
    [_mapButton setTitle:(g_mapState == -2 ? @"Map" : @"Done")];
    _mapButton.bezelColor = g_mapState == -2 ? nil : A7800AccentColor();
    if (g_mapState == -1 && _hint.stringValue.length == 0)
        [_hint setStringValue:@"Click a control to remap"];
    else if (g_mapState >= 0)
        [_hint setStringValue:[NSString stringWithFormat:@"Press a key or gamepad button for %s",
                               a7800_target_name(g_mapState)]];

    [self refreshTypes];
    for (PadButton *b in _buttons) {
        if (g_mapState != -2) {
            const a7800_binding bind = a7800session_binding_get(g_session, b.padTarget);
            char key[32];
            a7800session_keysym_name(bind.keysym, key, sizeof key);
            NSString *text = key[0] ? [NSString stringWithUTF8String:key] : @"—";
            if (bind.button != A7800_PAD_BTN_NONE)
                text = [text stringByAppendingFormat:@" / %s", a7800_pad_button_name(bind.button)];
            [b setTitle:text];
            [b setToolTip:[NSString stringWithUTF8String:a7800_target_name(b.padTarget)]];
            [b setLitState:(b.padTarget == g_mapState)];
        } else {
            [b setTitle:[self faceFor:b]];
            [b setToolTip:nil];
            if (!b.down) [b setLitState:NO];
        }
    }
}

/* ---- the keyboard ------------------------------------------------------------- */

/* Keyboard here behaves exactly as in the main window, so typing drives the
 * machine whichever window is key -- except in Map mode, where the next key
 * pressed is the binding. */
- (void)keyDown:(NSEvent *)e
{
    if ([e isARepeat]) return;
    const uint32_t ks = A7800KeysymFromEvent(e);
    if (g_mapState >= 0) {
        if (ks) {
            char stolen[128], name[32];
            const int target = g_mapState;
            a7800session_binding_set_key(g_session, target, ks, stolen, sizeof stolen);
            a7800session_keysym_name(ks, name, sizeof name);
            [self setMapState:-1];   /* stay armed: remapping several in a row is normal */
            _hint.stringValue = stolen[0]
                ? [NSString stringWithFormat:@"%s: %s (taken from %s)", a7800_target_name(target), name, stolen]
                : [NSString stringWithFormat:@"%s: %s", a7800_target_name(target), name];
        }
        return;
    }
    if (g_mapState == -1 || !ks) return;
    /* Command shortcuts the menus did not take are not the console's keys. */
    if ([e modifierFlags] & NSEventModifierFlagCommand) return;
    if (ks == A7800_KEYSYM_F9) { [A7800ControllersWindow toggleWithSession:g_session]; return; }

    const int sa = a7800session_key_sysaction(g_session, ks);
    if (sa >= 0) { a7800session_sysaction_post(g_session, sa); return; }
    a7800session_key(g_session, ks, 1);
}

- (void)keyUp:(NSEvent *)e
{
    if (g_mapState != -2) return;
    const uint32_t ks = A7800KeysymFromEvent(e);
    if (ks) a7800session_key(g_session, ks, 0);
}

/* Modifiers bind too. */
- (void)flagsChanged:(NSEvent *)e
{
    int down = 0;
    const uint32_t ks = A7800KeysymFromFlagsChange(e, &down);
    if (!ks) return;
    if (g_mapState >= 0) {
        if (!down) return;
        char stolen[128], name[32];
        const int target = g_mapState;
        a7800session_binding_set_key(g_session, target, ks, stolen, sizeof stolen);
        a7800session_keysym_name(ks, name, sizeof name);
        [self setMapState:-1];
        _hint.stringValue = stolen[0]
            ? [NSString stringWithFormat:@"%s: %s (taken from %s)", a7800_target_name(target), name, stolen]
            : [NSString stringWithFormat:@"%s: %s", a7800_target_name(target), name];
        return;
    }
    if (g_mapState == -2) a7800session_key(g_session, ks, down);
}

/* ---- lifetime ------------------------------------------------------------------ */

- (void)hidePanel
{
    [self setMapState:-2];
    [_liveTimer invalidate];
    _liveTimer = nil;
    a7800session_release_all(g_session);
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [self hidePanel];
}

+ (void)toggleWithSession:(a7800session *)session
{
    g_session = session;
    if (!g_singleton) g_singleton = [[A7800ControllersWindow alloc] init];

    if ([[g_singleton window] isVisible]) {
        [g_singleton hidePanel];
        [[g_singleton window] orderOut:nil];
    } else {
        [g_singleton refreshPads];
        [g_singleton refresh];
        g_singleton->_liveTimer = [NSTimer scheduledTimerWithTimeInterval:0.05 repeats:YES block:^(NSTimer *t) {
            (void)t;
            [g_singleton tick];
        }];
        [[g_singleton window] makeKeyAndOrderFront:nil];
    }
}

+ (BOOL)isVisible
{
    return g_singleton && [[g_singleton window] isVisible];
}
@end
