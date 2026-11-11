/*
 * Debugger window (AppKit) over MAME's own debugger engine (its 6502
 * disassembler, breakpoints, watchpoints and console), via
 * core/include/a7800debug.h and a7800memtools.h. Mirrors the family's other
 * debuggers (the SMS, Astrocade, Apple II and Intellivision macOS ones, and
 * the GTK, Qt and Win32 windows), tab for tab: Console, Disassembly, Memory,
 * Breakpoints, Watches, Cart & I/O, Trace, History, Video, Sound, Symbols,
 * FujiNet. The registers sit in a strip under the tabs; any tab can be popped
 * out into a window of its own (Pop Out / Dock).
 *
 * Opening the window attaches the engine, which stops the machine; closing
 * it (F12, the close button, the menu) detaches, and the machine runs on.
 *
 * A 100 ms NSTimer polls a7800debug_generation() and refreshes only the tab on
 * show, and only when the generation changed (a stop, a resume, a step, a
 * command, an edit). While the machine runs, that tab's live values refresh
 * twice a second (every fifth tick). After a step the window waits a moment
 * (~40 ms) for the stop to settle before it reads anything back; a longer run
 * (Frame, Step Out) is picked up by the poll when it stops. A new
 * a7800debug_stop_serial() (not a mere refresh) puts the Disassembly back on
 * the PC and the Memory tab on it too, and the status line says why it stopped.
 *
 * Keys work anywhere in the window (a local key monitor, so the console's
 * field takes them too): F5 stop/run, F7 step, F8 step over, Shift+F8 step
 * out, F12 close.
 *
 * A7800_DEBUGGER_TAB picks the tab shown first: an index (0 Console,
 * 1 Disassembly, 2 Memory, 3 Breakpoints, 4 Watches, 5 Cart & I/O, 6 Trace,
 * 7 History, 8 Video, 9 Sound, 10 Symbols, 11 FujiNet) or the start of a tab's
 * name ("cart", "prompt" for the console, "maria" for Video, "io" for
 * Cart & I/O). For captures: A7800_WATCH (a comma list), A7800_WATCH_BREAK,
 * A7800_MEM_GOTO, A7800_HISTORY=1 and A7800_POPOUT=<tab>.
 *
 * Highlights: the PC's line is the 7800 accent orange with white text, an
 * enabled breakpoint's marker red and a disabled one's grey, labels and
 * comments grey; the Run button is the accent while stopped.
 *
 * The Video tab (VideoTab.m, over a7800vidtab.h) shows the picture in four
 * views with overlays, hover/pin inspection, the registers, the colours, the
 * per-row MARIA records and the timing. The FujiNet tab shows every FujiBus
 * packet between the console and FujiNet, both ways, with hex, beside the
 * cartridge's mailbox (a7800debug_fujinet_*).
 *
 * NOT COMPILED OR RUN HERE (no Cocoa SDK on the Linux dev box): ported from
 * the SMS macOS debugger, which the macOS CI runner compiled; checked on that
 * runner.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "../KeyForward.h"
#import "../help/HelpWindow.h"
#import "VideoTab.h"

#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "a7800debug.h"
#include "a7800memtools.h"

#define DISASM_WINDOW 48
#define MAX_BPS 64
#define NUM_REGS 6
#define NUM_FLAGS 6
#define MEM_ROWS 40            /* the hex dump: 16 bytes a row */
#define MEM_BYTES (MEM_ROWS * 16)
#define MAX_REGIONS 64
#define FUJI_PACKETS_MAX 512   /* FujiNet packets shown */
#define TAB_CONSOLE 0          /* the tabs, in order */
#define TAB_DISASM 1
#define TAB_MEMORY 2
#define TAB_BREAKS 3
#define TAB_WATCHES 4
#define TAB_CART 5
#define TAB_TRACE 6
#define TAB_HIST 7
#define TAB_VIDEO 8
#define TAB_SOUND 9
#define TAB_SYMBOLS 10
#define TAB_FUJINET 11
#define NUM_TABS 12
#define HIST_RING 65536        /* the engine's trace ring */
#define HIST_SHOWN 2000        /* the newest matching lines a tab shows */
#define TRACE_TICKS 4          /* 4 x 100 ms: the Trace tab's live refresh */
#define LIVE_TICKS 5           /* 5 x 100 ms: the live refresh while running */

static A7800DebuggerWindow *g_debugger;

/* The open panels' filters: a type per extension (a dynamic one for an
 * extension nothing has registered, which still matches by extension). */
static NSArray<UTType *> *typesForExtensions(NSArray<NSString *> *exts)
{
    NSMutableArray<UTType *> *types = [NSMutableArray array];
    for (NSString *ext in exts) {
        UTType *type = [UTType typeWithFilenameExtension:ext];
        if (type) [types addObject:type];
    }
    return types;
}

/* Toggled-on look for a push button: accent bezel and a white title. Off
 * restores the plain bezel and title. Call again after setting a new title
 * on a button that is on (a plain title drops the attributed one). */
static void setButtonAccent(NSButton *button, BOOL on)
{
    if (!button) return;
    NSString *title = [button.title copy];
    if (on) {
        NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
        para.alignment = NSTextAlignmentCenter;
        button.bezelColor = A7800AccentColor();
        button.attributedTitle = [[NSAttributedString alloc]
            initWithString:title
                attributes:@{
                    NSFontAttributeName: button.font ?: [NSFont systemFontOfSize:[NSFont systemFontSize]],
                    NSForegroundColorAttributeName: NSColor.whiteColor,
                    NSParagraphStyleAttributeName: para
                }];
    } else {
        button.bezelColor = nil;
        button.title = title;       /* drops the attributed title */
    }
}

/* An enabled breakpoint's marker: the same red in every toolkit's debugger. */
static NSColor *breakpointRed(void)
{
    return [NSColor colorWithSRGBRed:0xE0 / 255.0 green:0x1B / 255.0 blue:0x24 / 255.0 alpha:1.0];
}

/* A monospace text view for rows: a click reports its line, column and click
 * count; the wheel browses by row. The disassembly and the memory dump. */
@interface DasmTextView : NSTextView
@property (nonatomic, copy) void (^onClick)(int line, int column, NSInteger clicks);
@property (nonatomic, copy) void (^onScroll)(int rows);
@end

@implementation DasmTextView {
    CGFloat _wheel;     /* the wheel's fraction not yet scrolled */
}

- (void)mouseDown:(NSEvent *)event
{
    NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger index = [self characterIndexForInsertionAtPoint:point];
    NSString *text = self.string;
    if (index > text.length) index = text.length;
    int line = 0;
    NSUInteger lineStart = 0;
    for (NSUInteger i = 0; i < index; i++)
        if ([text characterAtIndex:i] == '\n') { line++; lineStart = i + 1; }
    if (self.onClick) self.onClick(line, (int)(index - lineStart), event.clickCount);
}

- (void)scrollWheel:(NSEvent *)event
{
    /* a notch is a few rows; a trackpad's pixels are many */
    _wheel += event.hasPreciseScrollingDeltas ? -event.scrollingDeltaY / 8.0 : -event.scrollingDeltaY * 3.0;
    const int rows = (int)_wheel;
    if (rows == 0) return;
    _wheel -= rows;
    if (self.onScroll) self.onScroll(rows);
}
@end

/* The Symbols list: a click reports the clicked line (the wheel scrolls it
 * as any text view does). */
@interface SymbolTextView : NSTextView
@property (nonatomic, copy) void (^onClickLine)(int line);
@end

@implementation SymbolTextView
- (void)mouseDown:(NSEvent *)event
{
    NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger index = [self characterIndexForInsertionAtPoint:point];
    NSString *text = self.string;
    if (index > text.length) index = text.length;
    int line = 0;
    for (NSUInteger i = 0; i < index; i++)
        if ([text characterAtIndex:i] == '\n') line++;
    if (self.onClickLine) self.onClickLine(line);
}
@end

@interface A7800DebuggerWindow () <NSTabViewDelegate>
- (instancetype)initWithSession:(a7800session *)session;
- (void)show;
- (void)refreshNow;
@end

@implementation A7800DebuggerWindow {
    a7800session *_session;
    a7800debug *_dbg;
    NSWindow *_window;
    NSTimer *_tick;
    id _keyMonitor;
    unsigned _seenGen;
    BOOL _wasStopped;
    int _liveTicks;
    NSTabView *_tabs;

    NSButton *_runBtn;
    NSTextField *_status;
    NSTextField *_breakAt;

    NSTextView *_conOut;
    NSTextField *_conIn;
    NSMutableArray<NSString *> *_history;
    NSInteger _historyPos;

    NSTextField *_reg[NUM_REGS];
    NSButton *_flag[NUM_FLAGS];
    NSTextField *_beam;
    DasmTextView *_mem;
    NSTextField *_memGoto, *_memWrite;
    int _memBase;               /* the dump's first address */
    BOOL _memHl;                /* a range is highlighted: Go to, a hit, a watch */
    int _memHlLo, _memHlHi;
    BOOL _holdMemOnce;          /* A7800_MEM_GOTO: the first stop leaves the dump alone */
    NSPopUpButton *_srchKind;
    NSTextField *_srchEntry, *_srchNote;

    uint64_t _seenStop;         /* a7800debug_stop_serial() last acted on */
    BOOL _scrolledAway;         /* the wheel moved the disassembly off the PC */
    int _traceTicks;

    NSStackView *_wtList;
    NSString *_wtSig;
    NSTextField *_wtEntry;

    NSTextView *_cartText;
    NSTextView *_soundText;
    NSButton *_hRec[2];         /* Trace (0) and History (1) share one ring */
    NSPopUpButton *_hFilter[2];
    NSTextField *_hRange[2], *_hNote[2];
    NSTextView *_hText[2];

    NSTextField *_symFilter, *_symCount;
    SymbolTextView *_symList;
    NSMutableArray<NSNumber *> *_symAddrs;      /* the address of each line shown */

    NSTabViewItem *_items[NUM_TABS];    /* each tab's item, in order */
    NSWindow *_popped[NUM_TABS];        /* its own window while popped out */

    NSButton *_followPc;
    NSTextField *_jump;
    DasmTextView *_disasm;
    uint16_t _disasmAddr;
    uint16_t _lineAddr[DISASM_WINDOW];
    int _lineCount;

    A7800VideoTab *_video;            /* the Video tab (VideoTab.m) */
    BOOL _videoShown;               /* it was on show at the last refresh */

    NSPopUpButton *_bpType;
    NSTextField *_bpStart, *_bpEnd, *_bpCond, *_bpErr, *_bpStop;
    NSStackView *_bpList;
    NSString *_bpSig;

    NSTextField *_fujiNote;
    NSTextView *_fujiText, *_fujiState;
    uint64_t _fujiLast;
    int _fujiCount;             /* what the log shows: -1 forces a rebuild */
}

#define NUM_FILE_KINDS 3
static const char *const kFileKinds[NUM_FILE_KINDS] = { "dis", "ram", "mem" };
static NSString *const kFileTitles[NUM_FILE_KINDS] = {
    @"Disassembly ($4000-$FFFF)…", @"Console RAM ($1800-$27FF)…", @"Memory (the whole 64K)…"
};
static NSString *const kFileNames[NUM_FILE_KINDS] = { @"disassembly.asm", @"ram.bin", @"memory.bin" };

/* A7800_DEBUGGER_TAB takes an index or the start of one of these. */
static const char *const kTabNames[NUM_TABS] = {
    "console", "disassembly", "memory", "breakpoints", "watches", "cart", "trace", "history", "video", "sound",
    "symbols", "fujinet"
};
static NSString *const kTabTitles[NUM_TABS] = {
    @"Console", @"Disassembly", @"Memory", @"Breakpoints", @"Watches", @"Cart & I/O", @"Trace", @"History",
    @"Video", @"Sound", @"Symbols", @"FujiNet"
};
/* Help's contexts (a7800help.h), one per tab */
static NSString *const kHelpTabs[NUM_TABS] = {
    @"console", @"disassembly", @"memory", @"breakpoints", @"watches", @"cart", @"trace", @"history", @"video",
    @"sound", @"symbols", @"fujinet"
};

/* A tab by index or by the start of its name; -1 for none. */
static int tabFromText(const char *tab)
{
    if (!tab || !*tab) return -1;
    if (tab[0] >= '0' && tab[0] <= '9') return atoi(tab);
    if (!strncasecmp(tab, "prompt", strlen(tab)) && strlen(tab) > 1) return TAB_CONSOLE;
    if (!strncasecmp(tab, "maria", strlen(tab)) && strlen(tab) > 1) return TAB_VIDEO;
    if (!strncasecmp(tab, "io", 2) || !strncasecmp(tab, "i/o", 3)) return TAB_CART;
    for (int i = 0; i < NUM_TABS; i++)
        if (!strncasecmp(tab, kTabNames[i], strlen(tab))) return i;
    return -1;
}

/* ---- helpers --------------------------------------------------------------- */

/* The engine's text as a string: control bytes dropped (a console's CRs and
 * escapes), UTF-8 if it is, otherwise Latin-1. */
static NSString *cstr(const char *text)
{
    if (!text) return @"";
    const size_t length = strlen(text);
    char *copy = malloc(length + 1);
    if (!copy) return @"";
    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        const unsigned char c = (unsigned char)text[i];
        if (c >= 0x20 || c == '\n' || c == '\t') copy[used++] = (char)c;
    }
    copy[used] = '\0';
    NSString *result = [NSString stringWithUTF8String:copy];
    if (!result) result = [[NSString alloc] initWithBytes:copy length:used encoding:NSISOLatin1StringEncoding];
    free(copy);
    return result ?: @"";
}

/* Pads a column to width (never cuts a longer one). */
static NSString *padTo(NSString *text, NSUInteger width)
{
    return text.length >= width ? text : [text stringByPaddingToLength:width withString:@" " startingAtIndex:0];
}

/* The pad's held actions, by name. */
static void appendHeld(NSMutableString *text, unsigned held)
{
    static const char *const names[A7800_ACT_PER_PORT] = { "Up", "Down", "Left", "Right", "Button 1", "Button 2" };
    BOOL any = NO;
    for (int action = 0; action < A7800_ACT_PER_PORT; action++)
        if (held & (1u << action)) {
            [text appendFormat:@" %s", names[action]];
            any = YES;
        }
    if (!any) [text appendString:@" (nothing held)"];
}

/* SWCHA's nibble (a low bit is a held direction). */
static void appendStick(NSMutableString *text, uint8_t nibble)
{
    static const char *const directions[4] = { "up", "down", "left", "right" };
    BOOL any = NO;
    for (int bit = 0; bit < 4; bit++)
        if (!(nibble & (1u << bit))) {
            [text appendFormat:@" %s", directions[bit]];
            any = YES;
        }
    if (!any) [text appendString:@" centred"];
}

/* $hex, 0xhex, hexh, #dec, or bare hex. */
static BOOL parseNum(NSString *text, long *out)
{
    NSString *t = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    int base = 16;
    if ([t hasPrefix:@"$"]) t = [t substringFromIndex:1];
    else if ([t.lowercaseString hasPrefix:@"0x"]) t = [t substringFromIndex:2];
    else if ([t hasPrefix:@"#"]) { t = [t substringFromIndex:1]; base = 10; }
    else if (t.length > 1 && [t.lowercaseString hasSuffix:@"h"]) t = [t substringToIndex:t.length - 1];
    if (t.length == 0) return NO;
    char *end;
    long value = strtol(t.UTF8String, &end, base);
    if (*end) return NO;
    *out = value;
    return YES;
}

/* A value as the fields take it: a label, or $hex, 0xhex, hexh, #dec or bare
 * hex digits. On failure the reason is in *why. */
- (BOOL)evaluate:(NSString *)text into:(long *)out why:(NSString **)why
{
    NSString *t = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    long value;
    if (t.length == 0) {
        if (why) *why = @"nothing entered";
        return NO;
    }
    const int label = a7800debug_label_address(_dbg, t.UTF8String);
    if (label >= 0) {
        *out = label;
        return YES;
    }
    if (parseNum(t, &value)) {
        *out = value;
        return YES;
    }
    if (why) *why = @"not a number or label";
    return NO;
}

/* An address: the above, within $0000-$FFFF. -1 when it is not one. */
- (int)resolveAddr:(NSString *)text why:(NSString **)why
{
    long value;
    if (![self evaluate:text into:&value why:why]) return -1;
    if (value < 0 || value > 0xFFFF) {
        if (why) *why = @"out of range ($0000-$FFFF)";
        return -1;
    }
    return (int)value;
}

static NSFont *monoFont(void)
{
    return [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
}

static NSDictionary *monoAttrs(void)
{
    return @{ NSFontAttributeName: monoFont(), NSForegroundColorAttributeName: NSColor.textColor };
}

/* The accent fill with white on it: the PC's line, a marked row. */
static NSDictionary *accentAttrs(void)
{
    return @{ NSBackgroundColorAttributeName: A7800AccentColor(), NSForegroundColorAttributeName: NSColor.whiteColor };
}

static NSTextView *monoView(NSScrollView **scrollOut, BOOL wrap)
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = !wrap;
    NSTextView *view = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    view.editable = NO;
    view.richText = NO;
    view.font = monoFont();
    if (wrap) {
        view.autoresizingMask = NSViewWidthSizable;
    } else {
        view.horizontallyResizable = YES;
        view.textContainer.widthTracksTextView = NO;
        view.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
        view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    }
    scroll.documentView = view;
    *scrollOut = scroll;
    return view;
}

/* A monospace row view in a scroll view with no vertical bar: the wheel and
 * the clicks are the view's own (the disassembly, the memory dump). */
static DasmTextView *rowView(NSScrollView **scrollOut)
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = NO;
    scroll.hasHorizontalScroller = YES;
    DasmTextView *view = [[DasmTextView alloc] initWithFrame:NSMakeRect(0, 0, 600, 600)];
    view.editable = NO;
    view.richText = NO;
    view.selectable = NO;
    view.font = monoFont();
    view.horizontallyResizable = YES;
    view.textContainer.widthTracksTextView = NO;
    view.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
    view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    scroll.documentView = view;
    *scrollOut = scroll;
    return view;
}

/* Whether a view's reader is at its end (a log then follows new lines). */
static BOOL atEnd(NSTextView *view)
{
    NSScrollView *scroll = view.enclosingScrollView;
    if (!scroll) return YES;
    return NSMaxY(scroll.contentView.documentVisibleRect) >= NSHeight(view.frame) - 4;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void setText(NSTextView *view, NSString *text, BOOL follow)
{
    if (![view.string isEqualToString:text]) {
        NSScrollView *scroll = view.enclosingScrollView;
        const NSPoint at = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
        view.string = text;
        if (scroll && !follow) {
            [scroll.contentView scrollToPoint:at];
            [scroll reflectScrolledClipView:scroll.contentView];
        }
    }
    if (follow) [view scrollRangeToVisible:NSMakeRange(view.string.length, 0)];
}

/* The same for styled text, always where the reader left it. */
static void setStyledText(NSTextView *view, NSAttributedString *text)
{
    NSScrollView *scroll = view.enclosingScrollView;
    const NSPoint at = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
    [view.textStorage setAttributedString:text];
    if (scroll) {
        [scroll.contentView scrollToPoint:at];
        [scroll reflectScrolledClipView:scroll.contentView];
    }
}

- (NSButton *)button:(NSString *)title action:(SEL)sel
{
    NSButton *b = [NSButton buttonWithTitle:title target:self action:sel];
    [b setRefusesFirstResponder:YES];
    return b;
}

- (NSTextField *)label:(NSString *)text
{
    return [NSTextField labelWithString:text];
}

- (NSTextField *)hint:(NSString *)text
{
    NSTextField *l = [self label:text];
    l.textColor = NSColor.secondaryLabelColor;
    /* a long hint gives way to a narrow window instead of widening it */
    l.lineBreakMode = NSLineBreakByTruncatingTail;
    [l setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                forOrientation:NSLayoutConstraintOrientationHorizontal];
    return l;
}

- (NSTextField *)field:(NSString *)placeholder width:(CGFloat)width action:(SEL)sel
{
    NSTextField *f = [[NSTextField alloc] init];
    f.placeholderString = placeholder;
    f.font = monoFont();
    f.target = self;
    f.action = sel;
    if (width > 0) [f.widthAnchor constraintEqualToConstant:width].active = YES;
    return f;
}

/* A pull-down button: `title` on its face, `items` underneath (each one's
 * tag is its index). */
- (NSPopUpButton *)pullDown:(NSString *)title items:(NSArray<NSString *> *)items action:(SEL)sel
{
    NSPopUpButton *p = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:YES];
    [p addItemWithTitle:title];
    p.lastItem.tag = -1;
    for (NSUInteger i = 0; i < items.count; i++) {
        [p addItemWithTitle:items[i]];
        p.lastItem.tag = (NSInteger)i;
    }
    p.target = self;
    p.action = sel;
    [p setRefusesFirstResponder:YES];
    return p;
}

static NSStackView *hstack(NSArray<NSView *> *views)
{
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    s.spacing = 6;
    return s;
}

static NSStackView *vstack(NSArray<NSView *> *views)
{
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationVertical;
    s.alignment = NSLayoutAttributeLeading;
    s.spacing = 6;
    return s;
}

static void fill(NSStackView *stack, NSView *view)
{
    [view.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
}

/* ---- lifetime -------------------------------------------------------------- */

+ (void)showForSession:(a7800session *)session
{
    if (!g_debugger) g_debugger = [[A7800DebuggerWindow alloc] initWithSession:session];
    [g_debugger show];
}

+ (void)toggleForSession:(a7800session *)session
{
    if (g_debugger && [g_debugger->_window isVisible]) {
        /* windowWillClose: detaches */
        [g_debugger->_window close];
        return;
    }
    [self showForSession:session];
}

- (void)show
{
    if (_window.isMiniaturized) [_window deminiaturize:nil];
    [_window makeKeyAndOrderFront:nil];
    for (int t = 0; t < NUM_TABS; t++)
        if (_popped[t]) [_popped[t] orderFront:nil];
    const unsigned gen = a7800debug_generation(_dbg);
    a7800debug_attach(_dbg);
    [self settleFrom:gen];
    [self refreshNow];
    if ([self currentTab] == TAB_CONSOLE) [_window makeFirstResponder:_conIn];
}

- (instancetype)initWithSession:(a7800session *)session
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _dbg = a7800session_debugger(session);
    _fujiCount = -1;
    _memBase = 0x1800 - 2 * 16;     /* the console's RAM */
    _symAddrs = [NSMutableArray array];
    _history = [NSMutableArray array];
    [self buildWindow];

    /* For captures and tests, like A7800_DEBUGGER_TAB */
    {
        const char *watch = getenv("A7800_WATCH");
        const char *watchBreak = getenv("A7800_WATCH_BREAK");
        const char *gotoText = getenv("A7800_MEM_GOTO");
        const char *history = getenv("A7800_HISTORY");
        const char *popOut = getenv("A7800_POPOUT");
        uint16_t lo, hi;
        if (watch && *watch) {
            for (NSString *item in [cstr(watch) componentsSeparatedByString:@","]) {
                NSString *text = [item stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
                if (text.length) a7800watch_add(text.UTF8String);
            }
        }
        if (watchBreak && *watchBreak) {
            a7800watch_add(watchBreak);
            if (a7800sym_parse_range(_dbg, watchBreak, &lo, &hi)) [self breakOnChangeLo:lo hi:hi];
        }
        if (gotoText && *gotoText && a7800sym_parse_range(_dbg, gotoText, &lo, &hi)) {
            _holdMemOnce = YES;
            [self showMemLo:lo hi:hi];
        }
        if (history && *history) a7800debug_trace_enable(_dbg, 1);
        if (popOut && *popOut) {
            const int t = tabFromText(popOut);
            if (t >= 0 && t < NUM_TABS) [self popOutTab:t];
        }
    }

    __weak A7800DebuggerWindow *weakSelf = self;
    _tick = [NSTimer timerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *t) {
        (void)t;
        [weakSelf onTick];
    }];
    [[NSRunLoop mainRunLoop] addTimer:_tick forMode:NSRunLoopCommonModes];

    /* F5/F7/F8/F12 wherever the focus is in this window, the console's
     * field included */
    _keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                        handler:^NSEvent *(NSEvent *event) {
        A7800DebuggerWindow *strongSelf = weakSelf;
        if (!strongSelf || ![strongSelf ownsWindow:event.window] || !strongSelf->_window.isVisible) return event;
        return [strongSelf handleKey:event] ? nil : event;
    }];
    return self;
}

- (void)dealloc
{
    [_tick invalidate];
    if (_keyMonitor) [NSEvent removeMonitor:_keyMonitor];
}

- (void)windowWillClose:(NSNotification *)note
{
    if (note.object != _window) return;
    a7800debug_detach(_dbg);
    for (int t = 0; t < NUM_TABS; t++)
        [_popped[t] orderOut:nil];     /* they come back with the window */
}

/* Closing a popped-out tab's window docks the tab again. */
- (BOOL)windowShouldClose:(NSWindow *)sender
{
    for (int t = 0; t < NUM_TABS; t++)
        if (sender == _popped[t]) {
            [self dockTab:t];
            return NO;
        }
    return YES;
}

/* The debugger's main window or one of its popped-out tabs. */
- (BOOL)ownsWindow:(NSWindow *)window
{
    if (!window) return NO;
    if (window == _window) return YES;
    for (int t = 0; t < NUM_TABS; t++)
        if (_popped[t] == window) return YES;
    return NO;
}

/* The tab the main window's notebook shows, by its place in the full order
 * (-1 if none). */
- (NSInteger)currentTab
{
    NSTabViewItem *selected = _tabs.selectedTabViewItem;
    for (int t = 0; t < NUM_TABS; t++)
        if (_items[t] == selected) return t;
    return -1;
}

/* On screen: the notebook's page, or a window of its own. */
- (BOOL)tabVisible:(int)tab
{
    return _popped[tab] != nil || [self currentTab] == tab;
}

/* A new stop (a pause, a breakpoint, a step): every view goes to the current
 * spot, even if the reader scrolled away. The History and Trace logs always
 * follow their newest line. */
- (void)recentreViews
{
    _scrolledAway = NO;
    _followPc.state = NSControlStateValueOn;
    if (_holdMemOnce) {
        _holdMemOnce = NO;      /* A7800_MEM_GOTO's first look stays */
        return;
    }
    a7800debug_cpu cpu;
    a7800debug_cpu_get(_dbg, &cpu);
    _memBase = (cpu.pc & ~0xF) - 2 * 16;
    _memHl = NO;
}

/* The poll: refresh on a stop, a resume or a command (the generation), and
 * the visible tab's live values twice a second while the machine runs. The
 * FujiNet tab also follows the trace twice a second while stopped, as the
 * FujiNet's packets arrive on their own. */
- (void)onTick
{
    if (![_window isVisible]) return;
    const unsigned gen = a7800debug_generation(_dbg);
    const BOOL stopped = a7800debug_is_stopped(_dbg) != 0;
    const uint64_t stopSerial = a7800debug_stop_serial(_dbg);
    if (gen != _seenGen || stopped != _wasStopped || stopSerial != _seenStop) {
        if (stopSerial != _seenStop) {
            _seenStop = stopSerial;
            [self recentreViews];
        }
        [self refreshNow];
    } else if (++_liveTicks >= LIVE_TICKS) {
        _liveTicks = 0;
        if (!stopped) {
            [self refreshStatus];
            [self refreshTab];
        } else if ([self tabVisible:TAB_FUJINET]) {
            [self refreshFujinet:NO];
        }
    } else if (!stopped && [self tabVisible:TAB_TRACE] && ++_traceTicks >= TRACE_TICKS) {
        _traceTicks = 0;        /* the Trace tab follows the machine */
        [self refreshHist:0];
    }
}

/* Waits up to ~40 ms for a stop to land after the engine was asked to move:
 * the generation moves on and the machine reads as stopped. */
- (void)settleFrom:(unsigned)gen
{
    for (int i = 0; i < 20; i++) {
        if (a7800debug_generation(_dbg) != gen && a7800debug_is_stopped(_dbg)) break;
        usleep(2000);
    }
}

- (void)updateHelpContext
{
    const NSInteger index = [self currentTab];
    if (index >= 0 && index < NUM_TABS)
        A7800SetHelpContext(_window, [@"debugger/" stringByAppendingString:kHelpTabs[index]]);
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    (void)item;
    [self updateHelpContext];
    [self refreshTab];
    if ([self currentTab] == TAB_CONSOLE) [_window makeFirstResponder:_conIn];
}

/* ---- keys -------------------------------------------------------------------- */

- (BOOL)handleKey:(NSEvent *)event
{
    NSString *chars = event.charactersIgnoringModifiers;
    if (chars.length != 1) return NO;
    const NSEventModifierFlags mods = event.modifierFlags;
    if (mods & (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption)) return NO;
    const BOOL shift = (mods & NSEventModifierFlagShift) != 0;
    switch ([chars characterAtIndex:0]) {
    case NSF5FunctionKey: [self toggleRun:nil]; return YES;
    case NSF7FunctionKey: [self step:nil]; return YES;
    case NSF8FunctionKey: if (shift) [self stepOut:nil]; else [self stepOver:nil]; return YES;
    case NSF12FunctionKey: [_window performClose:nil]; return YES;
    default: return NO;
    }
}

/* ---- construction ------------------------------------------------------------ */

- (NSView *)buildConsole
{
    NSScrollView *outScroll;
    _conOut = monoView(&outScroll, YES);
    _conOut.string = @"Debugger console. Type 'help' for every command; Tab completes, Up/Down walk the history.\n";
    _conIn = [self field:@"command (help, step, bpset $C000, wpset $0080,1,w, print a, maria, video, ...) — Tab completes, ↑↓ history"
                   width:0 action:@selector(runCommand:)];
    _conIn.delegate = self;

    NSStackView *v = vstack(@[outScroll, _conIn]);
    fill(v, outScroll);
    fill(v, _conIn);
    return v;
}

/* The registers, flags and beam, under the tabs on every one of them. */
- (NSView *)buildRegStrip
{
    static NSString *const names[NUM_REGS] = { @"PC", @"SP", @"A", @"X", @"Y", @"PS" };
    NSMutableArray *row1 = [NSMutableArray array];
    for (int i = 0; i < NUM_REGS; i++) {
        const CGFloat width = i < 1 ? 52 : 36;
        [row1 addObject:[self label:names[i]]];
        _reg[i] = [self field:@"" width:width action:@selector(applyRegister:)];
        _reg[i].tag = i;
        _reg[i].toolTip = @"Edit while the machine is stopped (hex, $hex or a label), then Enter";
        [row1 addObject:_reg[i]];
    }
    static NSString *const flagNames[NUM_FLAGS] = { @"N", @"V", @"D", @"I", @"Z", @"C" };
    NSMutableArray *flags = [NSMutableArray arrayWithObject:[self label:@"Flags"]];
    for (int i = 0; i < NUM_FLAGS; i++) {
        _flag[i] = [NSButton checkboxWithTitle:flagNames[i] target:self action:@selector(flagClicked:)];
        _flag[i].tag = i;
        [flags addObject:_flag[i]];
    }
    _beam = [self hint:@""];
    [flags addObject:_beam];

    return vstack(@[hstack(row1), hstack(flags)]);
}

- (NSView *)buildMemory
{
    NSScrollView *memScroll;
    _mem = rowView(&memScroll);
    __weak A7800DebuggerWindow *weakSelf = self;
    _mem.onScroll = ^(int rows) {
        A7800DebuggerWindow *strongSelf = weakSelf;
        if (!strongSelf || rows == 0) return;
        strongSelf->_memBase += rows * 16;
        [strongSelf refreshMem];
    };
    _memGoto = [self field:@"address, label or lo-hi" width:180 action:@selector(gotoMem:)];
    NSButton *go = [self button:@"Go" action:@selector(gotoMem:)];
    NSButton *prev = [self button:@"◀" action:@selector(memPrev:)];
    NSButton *next = [self button:@"▶" action:@selector(memNext:)];
    NSStackView *memHead = hstack(@[[self label:@"Memory (64K, as the CPU sees it)"], [self label:@"Go to"], _memGoto,
                                    go, prev, next]);

    _srchKind = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_srchKind addItemsWithTitles:@[@"Hex", @"Text"]];
    _srchKind.toolTip = @"Hex bytes (A9 00) or text, one byte per character";
    [_srchKind setRefusesFirstResponder:YES];
    _srchEntry = [self field:@"hex bytes or text" width:220 action:@selector(searchNext:)];
    _srchNote = [self hint:@""];
    NSStackView *searchRow = hstack(@[[self label:@"Search"], _srchKind, _srchEntry,
                                      [self button:@"Find Next" action:@selector(searchNext:)],
                                      [self button:@"Find Previous" action:@selector(searchPrev:)], _srchNote]);

    _memWrite = [self field:@"address value" width:170 action:@selector(writeMem:)];
    NSStackView *edit = hstack(@[[self label:@"Poke"], _memWrite,
                                 [self hint:@"RAM takes it; a write to the cartridge's ROM is ignored. "
                                            @"Several values go to consecutive addresses"]]);

    NSStackView *v = vstack(@[memHead, searchRow, memScroll, edit]);
    fill(v, memScroll);
    return v;
}

- (NSView *)buildDisasm
{
    _followPc = [NSButton checkboxWithTitle:@"Follow PC" target:self action:@selector(followToggled:)];
    _followPc.state = NSControlStateValueOn;
    _jump = [self field:@"address or label" width:150 action:@selector(jumpTo:)];
    NSStackView *row = hstack(@[_followPc, [self label:@"Jump to"], _jump,
                                [self hint:@"Click a line to toggle its breakpoint (red dot on, grey ring off); "
                                           @"scroll to browse"]]);

    NSScrollView *scroll;
    _disasm = rowView(&scroll);
    __weak A7800DebuggerWindow *weakSelf = self;
    _disasm.onClick = ^(int line, int column, NSInteger clicks) {
        (void)column;
        (void)clicks;
        A7800DebuggerWindow *strongSelf = weakSelf;
        if (!strongSelf || line < 0 || line >= strongSelf->_lineCount) return;
        a7800debug_breakpoint_toggle(strongSelf->_dbg, strongSelf->_lineAddr[line]);
        [strongSelf refreshDisasm];
        if ([strongSelf tabVisible:TAB_BREAKS]) [strongSelf refreshBps];
    };
    _disasm.onScroll = ^(int rows) {
        A7800DebuggerWindow *strongSelf = weakSelf;
        if (!strongSelf || rows == 0) return;
        strongSelf->_followPc.state = NSControlStateValueOff;
        strongSelf->_scrolledAway = YES;
        strongSelf->_disasmAddr = (uint16_t)a7800debug_row_address(strongSelf->_dbg, strongSelf->_disasmAddr, rows);
        [strongSelf refreshDisasm];
    };

    NSStackView *v = vstack(@[row, scroll]);
    fill(v, row);
    fill(v, scroll);
    return v;
}

/* The Video tab: a controller (core a7800vidtab.h) behind its own views. */
- (NSView *)buildVideo
{
    _video = [[A7800VideoTab alloc] initWithSession:_session];
    return _video.view;
}

/* One table cell: a fixed-width monospace label, or (width 0) the flexible
 * one that takes what is left. */
static NSTextField *tableCell(NSString *text, CGFloat width, BOOL bold)
{
    NSTextField *l = [NSTextField labelWithString:text];
    l.font = bold ? [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightSemibold] : monoFont();
    l.lineBreakMode = NSLineBreakByTruncatingTail;
    if (width > 0) {
        [l.widthAnchor constraintEqualToConstant:width].active = YES;
    } else {
        [l setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
        [l setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                    forOrientation:NSLayoutConstraintOrientationHorizontal];
    }
    return l;
}

- (NSView *)buildBreaks
{
    /* the table's rows scroll: a session can collect more breakpoints than
     * the tab is tall. The stack is the scroll view's document, pinned to
     * the top and to the clip view's width so rows keep the tab's width. */
    _bpStop = [self label:@""];
    _bpStop.textColor = NSColor.secondaryLabelColor;
    NSStackView *header = hstack(@[tableCell(@"On  #", 70, YES), tableCell(@"Breakpoint", 0, YES),
                                   tableCell(@"Hits", 60, YES), tableCell(@"", 70, YES)]);
    header.distribution = NSStackViewDistributionFill;
    _bpList = vstack(@[]);
    _bpList.translatesAutoresizingMaskIntoConstraints = NO;
    NSScrollView *bpScroll = [[NSScrollView alloc] init];
    bpScroll.hasVerticalScroller = YES;
    bpScroll.drawsBackground = NO;
    bpScroll.documentView = _bpList;
    NSClipView *clip = bpScroll.contentView;
    [_bpList.topAnchor constraintEqualToAnchor:clip.topAnchor].active = YES;
    [_bpList.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor].active = YES;
    [_bpList.widthAnchor constraintEqualToAnchor:clip.widthAnchor].active = YES;

    _bpType = [[NSPopUpButton alloc] init];
    [_bpType addItemsWithTitles:@[@"Execute", @"Read", @"Write", @"Access"]];
    _bpStart = [self field:@"$C000, lo-hi or a label" width:250 action:@selector(addBreakpoint:)];
    _bpEnd = [self field:@"end (optional)" width:120 action:@selector(addBreakpoint:)];
    _bpCond = [self field:@"condition (optional): a == 5" width:0 action:@selector(addBreakpoint:)];
    NSButton *add = [self button:@"Add" action:@selector(addBreakpoint:)];
    NSButton *clearHits = [self button:@"Clear Hits" action:@selector(clearHits:)];
    NSButton *clear = [self button:@"Clear Breakpoints" action:@selector(clearBreaks:)];
    NSStackView *row = hstack(@[_bpType, [self label:@"Start"], _bpStart, [self label:@"End"], _bpEnd,
                                [self label:@"Condition"], _bpCond, add, clearHits, clear]);
    row.distribution = NSStackViewDistributionFill;
    _bpErr = [self label:@""];
    _bpErr.textColor = NSColor.systemRedColor;
    NSTextField *hint = [self hint:@"Untick to disable. A condition is an expression, e.g. a == 5 or, on a read or write "
                                   @"break, value > 3 (wpaddr is the address, old the byte before a write). "
                                   @"The end defaults to the start."];

    NSStackView *v = vstack(@[_bpStop, header, bpScroll, hint, row, _bpErr]);
    fill(v, header);
    fill(v, bpScroll);
    fill(v, row);
    [bpScroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    return v;
}

/* Watches: an entry, then a row for each with its value and three buttons. */
- (NSView *)buildWatches
{
    _wtEntry = [self field:@"address, label, expression or lo-hi" width:0 action:@selector(addWatch:)];
    NSStackView *row = hstack(@[[self label:@"Watch"], _wtEntry, [self button:@"Add" action:@selector(addWatch:)]]);
    row.distribution = NSStackViewDistributionFill;

    _wtList = vstack(@[]);
    _wtList.translatesAutoresizingMaskIntoConstraints = NO;
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.drawsBackground = NO;
    scroll.documentView = _wtList;
    NSClipView *clip = scroll.contentView;
    [_wtList.topAnchor constraintEqualToAnchor:clip.topAnchor].active = YES;
    [_wtList.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor].active = YES;
    [_wtList.widthAnchor constraintEqualToAnchor:clip.widthAnchor].active = YES;

    NSTextField *hint = [self hint:@"Values show while the machine is stopped. Show in Memory shows the watch in the Memory "
                                   @"tab; Break on Change stops when the machine writes a different value there."];
    NSStackView *v = vstack(@[row, scroll, hint]);
    fill(v, row);
    fill(v, scroll);
    [scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    return v;
}

/* The cartridge, the memory map and the console's ports, mapper and pads in one text. */
- (NSView *)buildCart
{
    NSScrollView *scroll;
    _cartText = monoView(&scroll, NO);
    NSStackView *v = vstack(@[scroll]);
    fill(v, scroll);
    return v;
}

- (NSView *)buildSound
{
    NSScrollView *scroll;
    _soundText = monoView(&scroll, YES);
    return scroll;
}

/* Trace (idx 0) and History (idx 1): two views of the engine's one ring. */
- (NSView *)buildHist:(int)idx
{
    _hRec[idx] = [self button:@"Record" action:@selector(histRec:)];
    [_hRec[idx] setButtonType:NSButtonTypePushOnPushOff];
    _hRec[idx].toolTip = @"Record every instruction the CPU runs (the machine runs a little slower)";
    _hFilter[idx] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_hFilter[idx] addItemsWithTitles:@[@"All instructions", @"Branches only", @"In range"]];
    _hFilter[idx].target = self;
    _hFilter[idx].action = @selector(histChanged:);
    [_hFilter[idx] setRefusesFirstResponder:YES];
    _hRange[idx] = [self field:@"range: lo-hi or a label" width:180 action:@selector(histChanged:)];
    _hNote[idx] = [self hint:@""];
    NSStackView *bar = hstack(@[_hRec[idx], _hFilter[idx], _hRange[idx],
                                [self button:@"Clear" action:@selector(histClear:)], _hNote[idx]]);
    NSScrollView *scroll;
    _hText[idx] = monoView(&scroll, NO);
    NSStackView *v = vstack(@[bar, scroll]);
    fill(v, bar);
    fill(v, scroll);
    return v;
}

/* Symbols: a filter over the table (built-ins and loaded), a click on a row
 * goes to its address in the Disassembly. */
- (NSView *)buildSymbols
{
    _symFilter = [self field:@"Filter by name (Enter to apply)" width:260 action:@selector(symFilterChanged:)];
    _symCount = [self hint:@""];
    NSStackView *bar = hstack(@[[self label:@"Filter"], _symFilter, _symCount]);

    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = YES;
    _symList = [[SymbolTextView alloc] initWithFrame:NSMakeRect(0, 0, 600, 400)];
    _symList.editable = NO;
    _symList.richText = NO;
    _symList.selectable = NO;
    _symList.font = monoFont();
    _symList.horizontallyResizable = YES;
    _symList.textContainer.widthTracksTextView = NO;
    _symList.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
    _symList.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    __weak A7800DebuggerWindow *weakSelf = self;
    _symList.onClickLine = ^(int line) {
        A7800DebuggerWindow *strongSelf = weakSelf;
        if (!strongSelf || line < 0 || line >= (int)strongSelf->_symAddrs.count) return;
        [strongSelf showDisassemblyAt:(uint16_t)strongSelf->_symAddrs[(NSUInteger)line].intValue];
    };
    scroll.documentView = _symList;

    NSTextField *hint = [self hint:@"Click a symbol to show its address in the Disassembly. Load Symbols adds a file's "
                                   @"labels; Clear Symbols drops the loaded ones (the built-ins stay)."];
    NSStackView *v = vstack(@[bar, scroll, hint]);
    fill(v, scroll);
    return v;
}

/* The packets left (a note and Clear over them), the cartridge's mailbox
 * right. */
- (NSView *)buildFujinet
{
    _fujiNote = [self label:@""];
    _fujiNote.textColor = NSColor.secondaryLabelColor;
    _fujiNote.lineBreakMode = NSLineBreakByTruncatingTail;
    [_fujiNote setContentHuggingPriority:NSLayoutPriorityDefaultLow
                          forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_fujiNote setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                        forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *row = hstack(@[_fujiNote, [self button:@"Clear" action:@selector(fujiClear:)]]);

    NSScrollView *fujiScroll, *stateScroll;
    _fujiText = monoView(&fujiScroll, NO);
    _fujiState = monoView(&stateScroll, NO);
    NSTextField *title = [self label:@"FujiBus packets (SLIP-decoded)"];
    title.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    NSStackView *left = vstack(@[title, row, fujiScroll]);
    left.distribution = NSStackViewDistributionFill;   /* the log takes the column's height */
    fill(left, row);
    fill(left, fujiScroll);
    NSTextField *stateTitle = [self label:@"The cartridge's mailbox"];
    stateTitle.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    NSStackView *right = vstack(@[stateTitle, stateScroll]);
    right.distribution = NSStackViewDistributionFill;
    fill(right, stateScroll);

    NSStackView *h = hstack(@[left, right]);
    h.alignment = NSLayoutAttributeTop;
    h.spacing = 12;
    [left.widthAnchor constraintGreaterThanOrEqualToConstant:520].active = YES;
    [right.widthAnchor constraintEqualToConstant:320].active = YES;
    [left.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    [right.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    return h;
}

- (void)buildWindow
{
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 1180, 840)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"Atari 7800 Debugger";
    _window.releasedWhenClosed = NO;
    _window.delegate = self;

    /* Toolbar rows. The keys are handled by the key monitor, not as the
     * buttons' key equivalents, so they work wherever the focus is. */
    _runBtn = [self button:@"Stop (F5)" action:@selector(toggleRun:)];
    NSButton *step = [self button:@"Step (F7)" action:@selector(step:)];
    NSButton *over = [self button:@"Step Over (F8)" action:@selector(stepOver:)];
    NSButton *outBtn = [self button:@"Step Out (⇧F8)" action:@selector(stepOut:)];
    NSButton *frame = [self button:@"Frame" action:@selector(frame:)];
    NSButton *clearBps = [self button:@"Clear Breakpoints" action:@selector(clearBreaks:)];
    NSButton *popBtn = [self button:@"Pop Out" action:@selector(popOutCurrent:)];
    popBtn.toolTip = @"Move the tab that shows into a window of its own; Dock puts it back";
    NSButton *helpBtn = A7800HelpButton(nil);
    [helpBtn setRefusesFirstResponder:YES];
    NSStackView *toolbar = hstack(@[_runBtn, step, over, outBtn, frame, clearBps, popBtn, helpBtn]);

    _breakAt = [self field:@"symbol or hex address" width:170 action:@selector(breakAt:)];
    NSButton *breakBtn = [self button:@"Break" action:@selector(breakAt:)];
    breakBtn.toolTip = @"Set an execute breakpoint at the symbol or address";
    NSPopUpButton *symbols = [self pullDown:@"Load Symbols…"
                                      items:@[@"From a File…", @"Beside the Cartridge"]
                                     action:@selector(loadSymbols:)];
    NSButton *clearSymbols = [self button:@"Clear Symbols" action:@selector(clearSymbols:)];
    NSMutableArray *saveTitles = [NSMutableArray array];
    for (int i = 0; i < NUM_FILE_KINDS; i++) [saveTitles addObject:kFileTitles[i]];
    NSPopUpButton *save = [self pullDown:@"Save…" items:saveTitles action:@selector(saveFile:)];
    NSStackView *toolbar2 = hstack(@[[self label:@"Break at:"], _breakAt, breakBtn, symbols, clearSymbols, save]);

    /* the stop reason gets a row of its own, so it is never cut short */
    _status = [self label:@"Running"];
    _status.textColor = NSColor.secondaryLabelColor;
    _status.lineBreakMode = NSLineBreakByTruncatingTail;
    [_status setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                      forOrientation:NSLayoutConstraintOrientationHorizontal];

    _tabs = [[NSTabView alloc] init];
    NSArray *pages = @[ [self buildConsole], [self buildDisasm], [self buildMemory], [self buildBreaks],
                        [self buildWatches], [self buildCart], [self buildHist:0], [self buildHist:1],
                        [self buildVideo], [self buildSound], [self buildSymbols], [self buildFujinet] ];
    for (int i = 0; i < NUM_TABS; i++) {
        NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:kHelpTabs[i]];
        item.label = kTabTitles[i];
        NSView *content = pages[(NSUInteger)i];
        NSView *holder = [[NSView alloc] init];
        [holder addSubview:content];
        content.translatesAutoresizingMaskIntoConstraints = NO;
        [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:6].active = YES;
        [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-6].active = YES;
        [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:6].active = YES;
        [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-6].active = YES;
        item.view = holder;
        _items[i] = item;
        [_tabs addTabViewItem:item];
    }
    const int first = tabFromText(getenv("A7800_DEBUGGER_TAB"));
    if (first >= 0 && first < (int)_tabs.numberOfTabViewItems) [_tabs selectTabViewItemAtIndex:first];
    _tabs.delegate = self;
    [self updateHelpContext];

    /* the registers sit under every tab */
    NSView *strip = [self buildRegStrip];
    NSStackView *root = vstack(@[toolbar, toolbar2, _status, _tabs, strip]);
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [toolbar.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [toolbar2.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [_status.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [_tabs.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [strip.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    _window.contentView = root;
    [_window center];
}

/* ---- actions ------------------------------------------------------------------ */

/* A line for the status label (and the console's log, when asked). */
- (void)message:(NSString *)text console:(BOOL)console
{
    _status.stringValue = text;
    if (console) [self appendConsole:[text stringByAppendingString:@"\n"]];
}

- (void)toggleRun:(id)sender
{
    (void)sender;
    const unsigned gen = a7800debug_generation(_dbg);
    if (a7800debug_is_stopped(_dbg)) a7800debug_resume(_dbg); else a7800debug_stop(_dbg);
    [self settleFrom:gen];
    [self refreshNow];
}

/* An action that moves the machine: the stop settles (or the poll catches
 * it, for a long run), then everything shown is read back. */
- (void)moved:(unsigned)gen
{
    [self settleFrom:gen];
    [self refreshNow];
}

- (void)step:(id)sender { (void)sender; const unsigned gen = a7800debug_generation(_dbg); a7800debug_step(_dbg); [self moved:gen]; }
- (void)stepOver:(id)sender { (void)sender; const unsigned gen = a7800debug_generation(_dbg); a7800debug_step_over(_dbg); [self moved:gen]; }
- (void)stepOut:(id)sender { (void)sender; const unsigned gen = a7800debug_generation(_dbg); a7800debug_step_out(_dbg); [self moved:gen]; }
- (void)frame:(id)sender { (void)sender; const unsigned gen = a7800debug_generation(_dbg); a7800debug_frame(_dbg); [self moved:gen]; }

- (void)fujiClear:(id)sender
{
    (void)sender;
    a7800debug_fujinet_trace_clear(_dbg);
    [self refreshFujinet:YES];
}

- (void)appendConsole:(NSString *)text
{
    const BOOL follow = atEnd(_conOut);     /* a reader scrolled back keeps their place */
    [_conOut.textStorage appendAttributedString:[[NSAttributedString alloc] initWithString:text attributes:monoAttrs()]];
    if (follow) [_conOut scrollRangeToVisible:NSMakeRange(_conOut.string.length, 0)];
}

- (void)runCommand:(id)sender
{
    (void)sender;
    static char out[65536];
    NSString *cmd = [_conIn.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (cmd.length == 0) return;
    if (![_history.lastObject isEqualToString:cmd]) [_history addObject:cmd];
    _historyPos = (NSInteger)_history.count;
    [self appendConsole:[NSString stringWithFormat:@"> %@\n", cmd]];
    out[0] = '\0';
    const unsigned gen = a7800debug_generation(_dbg);
    a7800debug_command(_dbg, cmd.UTF8String, out, (int)sizeof out);
    NSString *text = cstr(out);
    if (text.length) [self appendConsole:[text hasSuffix:@"\n"] ? text : [text stringByAppendingString:@"\n"]];
    _conIn.stringValue = @"";
    [self settleFrom:gen];
    [self refreshNow];
}

/* In the console, Tab completes instead of moving the focus, and Up/Down
 * walk the history. */
- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView doCommandBySelector:(SEL)sel
{
    (void)textView;
    if (control != _conIn) return NO;
    if (sel == @selector(moveUp:) || sel == @selector(moveDown:)) {
        const NSInteger count = (NSInteger)_history.count;
        if (count == 0) return YES;
        _historyPos += sel == @selector(moveUp:) ? -1 : 1;
        if (_historyPos < 0) _historyPos = 0;
        if (_historyPos > count) _historyPos = count;
        _conIn.stringValue = _historyPos < count ? _history[(NSUInteger)_historyPos] : @"";
        [_conIn.currentEditor setSelectedRange:NSMakeRange(_conIn.stringValue.length, 0)];
        return YES;
    }
    if (sel != @selector(insertTab:)) return NO;
    NSString *text = _conIn.stringValue;
    const NSRange space = [text rangeOfString:@" " options:NSBackwardsSearch];
    NSString *word = space.location == NSNotFound ? text : [text substringFromIndex:space.location + 1];
    char comps[4096];
    comps[0] = '\0';
    const int count = a7800debug_completions(_dbg, word.UTF8String, comps, (int)sizeof comps);
    NSString *list = cstr(comps);
    if (count == 1) {
        NSString *completion = [list componentsSeparatedByString:@"\n"][0];
        NSString *head = space.location == NSNotFound ? @"" : [text substringToIndex:space.location + 1];
        _conIn.stringValue = [NSString stringWithFormat:@"%@%@ ", head, completion];
        [_conIn.currentEditor setSelectedRange:NSMakeRange(_conIn.stringValue.length, 0)];
    } else if (count > 1) {
        if (![list hasSuffix:@"\n"]) list = [list stringByAppendingString:@"\n"];
        [self appendConsole:list];
    }
    return YES;
}

/* "From a File…" asks; "Beside the Cartridge" takes <cart>.lbl/.dbg/.sym
 * next to the opened cartridge. */
- (void)loadSymbols:(NSPopUpButton *)sender
{
    char msg[512];
    msg[0] = '\0';
    if (sender.selectedItem.tag == 1) {
        a7800debug_load_symbols(_dbg, NULL, msg, (int)sizeof msg);
    } else {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.title = @"Load Symbols (ld65 .lbl, ca65 .dbg, or name = $addr lines)";
        panel.allowedContentTypes = typesForExtensions(@[@"lbl", @"dbg", @"sym", @"txt"]);
        const char *cart = a7800session_cart_path(_session);
        if (cart && cart[0])
            [panel setDirectoryURL:[NSURL fileURLWithPath:[[NSString stringWithUTF8String:cart]
                                                              stringByDeletingLastPathComponent] isDirectory:YES]];
        if ([panel runModal] != NSModalResponseOK) return;
        a7800debug_load_symbols(_dbg, [[panel URL] fileSystemRepresentation], msg, (int)sizeof msg);
    }
    [self message:cstr(msg) console:YES];
    [self refreshNow];
}

- (void)clearSymbols:(id)sender
{
    (void)sender;
    a7800debug_clear_symbols(_dbg);
    [self message:@"Loaded symbols cleared" console:YES];
    [self refreshNow];
}

- (void)saveFile:(NSPopUpButton *)sender
{
    const NSInteger kind = sender.selectedItem.tag;
    if (kind < 0 || kind >= NUM_FILE_KINDS) return;
    NSSavePanel *panel = [NSSavePanel savePanel];
    panel.title = [NSString stringWithFormat:@"Save %@", kFileTitles[kind]];
    [panel setNameFieldStringValue:kFileNames[kind]];
    if ([panel runModal] != NSModalResponseOK) return;
    char msg[512];
    msg[0] = '\0';
    a7800debug_save(_dbg, kFileKinds[kind], [[panel URL] fileSystemRepresentation], msg, (int)sizeof msg);
    [self message:cstr(msg) console:YES];
}

- (void)applyRegister:(NSTextField *)sender
{
    static const int regIds[NUM_REGS] = {
        A7800_REG_PC, A7800_REG_SP, A7800_REG_A, A7800_REG_X, A7800_REG_Y, A7800_REG_PS
    };
    long value;
    NSString *why = nil;
    [_window makeFirstResponder:nil];       /* leave the field, so the refresh may rewrite it */
    if (!a7800debug_is_stopped(_dbg)) {
        [self message:@"Stop the machine (F5) to edit registers" console:NO];
    } else if (sender.tag < 0 || sender.tag >= NUM_REGS) {
        /* not one of ours */
    } else if ([self evaluate:sender.stringValue into:&value why:&why]) {
        a7800debug_cpu_set(_dbg, regIds[sender.tag], (int)value);
    } else {
        [self message:[NSString stringWithFormat:@"Register: %@", why] console:NO];
    }
    [self refreshCpu];
    if ([self tabVisible:TAB_DISASM]) [self refreshDisasm];
}

- (void)flagClicked:(NSButton *)sender
{
    static const int flagIds[NUM_FLAGS] = { A7800_FLAG_N, A7800_FLAG_V, A7800_FLAG_D, A7800_FLAG_I, A7800_FLAG_Z, A7800_FLAG_C };
    if (sender.tag >= 0 && sender.tag < NUM_FLAGS && a7800debug_is_stopped(_dbg))
        a7800debug_cpu_set(_dbg, flagIds[sender.tag], sender.state == NSControlStateValueOn);
    else
        [self message:@"Stop the machine (F5) to edit flags" console:NO];
    [self refreshCpu];
}

/* Brings a range into the dump (a couple of rows down from the top) and
 * highlights it. */
- (void)showMemLo:(int)lo hi:(int)hi
{
    _memBase = (lo & ~0xF) - 2 * 16;
    _memHl = YES;
    _memHlLo = lo;
    _memHlHi = hi;
    [self refreshMem];
}

- (void)gotoMem:(id)sender
{
    (void)sender;
    uint16_t lo, hi;
    if (!a7800sym_parse_range(_dbg, _memGoto.stringValue.UTF8String, &lo, &hi)) {
        [self message:[NSString stringWithFormat:@"Go to %@: not an address, label or range", _memGoto.stringValue]
              console:NO];
        return;
    }
    [self showMemLo:lo hi:hi];
}

- (void)memPrev:(id)sender { (void)sender; _memBase -= MEM_BYTES; [self refreshMem]; }
- (void)memNext:(id)sender { (void)sender; _memBase += MEM_BYTES; [self refreshMem]; }

/* "address value [value...]": the values land at consecutive addresses. */
- (void)writeMem:(id)sender
{
    (void)sender;
    NSArray<NSString *> *words = [_memWrite.stringValue componentsSeparatedByCharactersInSet:
        [NSCharacterSet whitespaceCharacterSet]];
    NSMutableArray<NSString *> *tokens = [NSMutableArray array];
    for (NSString *word in words)
        if (word.length) [tokens addObject:word];
    NSString *why = nil;
    const int address = tokens.count > 0 ? [self resolveAddr:tokens[0] why:&why] : -1;
    if (tokens.count < 2 || address < 0) {
        [self message:@"Poke: address value [value...]" console:NO];
        return;
    }
    for (NSUInteger i = 1; i < tokens.count; i++) {
        long value;
        if (![self evaluate:tokens[i] into:&value why:&why] || value < 0 || value > 0xFF ||
            address + (int)i - 1 > 0xFFFF) {
            [self message:[NSString stringWithFormat:@"Not a byte: %@", tokens[i]] console:NO];
            return;
        }
        a7800debug_write(_dbg, (uint16_t)(address + (int)i - 1), (uint8_t)value);
    }
    _memWrite.stringValue = @"";
    if (address < _memBase || address >= _memBase + MEM_BYTES) [self showMemLo:address hi:address];
    else [self refreshMem];
}

- (void)followToggled:(id)sender
{
    (void)sender;
    _scrolledAway = NO;
    [self refreshDisasm];
}

- (void)jumpTo:(id)sender
{
    (void)sender;
    NSString *why = nil;
    const int addr = [self resolveAddr:_jump.stringValue why:&why];
    if (addr < 0) {
        [self message:[NSString stringWithFormat:@"Jump to %@: %@", _jump.stringValue, why] console:NO];
        return;
    }
    [self showDisassemblyAt:(uint16_t)addr];
}

/* The Disassembly from an address on (off the PC, until the next stop). */
- (void)showDisassemblyAt:(uint16_t)addr
{
    _followPc.state = NSControlStateValueOff;
    _scrolledAway = YES;
    _disasmAddr = addr;
    [self showTab:TAB_DISASM];
    [self refreshDisasm];
}

/* The toolbar's "Break at:": a symbol or hex address gets an execute
 * breakpoint (one already there is left as it is). */
- (void)breakAt:(id)sender
{
    (void)sender;
    NSString *why = nil;
    const int addr = [self resolveAddr:_breakAt.stringValue why:&why];
    if (addr < 0) {
        [self message:[NSString stringWithFormat:@"Break at %@: %@", _breakAt.stringValue, why] console:NO];
        return;
    }
    if (a7800debug_breakpoint_check(_dbg, (uint16_t)addr)) {
        [self message:[NSString stringWithFormat:@"A breakpoint is already set at $%04X", addr] console:NO];
        return;
    }
    a7800debug_breakpoint_toggle(_dbg, (uint16_t)addr);
    _breakAt.stringValue = @"";
    [self message:[NSString stringWithFormat:@"Breakpoint at $%04X", addr] console:NO];
    [self refreshNow];
}

- (void)symFilterChanged:(id)sender
{
    (void)sender;
    [self refreshSymbols];
}

- (void)addBreakpoint:(id)sender
{
    (void)sender;
    static const int types[4] = { A7800DEBUG_BP_EXEC, A7800DEBUG_BP_READ, A7800DEBUG_BP_WRITE, A7800DEBUG_BP_ACCESS };
    const NSInteger sel = _bpType.indexOfSelectedItem;
    const int type = types[sel >= 0 && sel < 4 ? sel : 0];
    NSString *what = @"address";
    NSString *startText = [_bpStart.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    NSString *endText = [_bpEnd.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (startText.length == 0) {
        _bpErr.stringValue = [NSString stringWithFormat:@"Give a %@", what];
        return;
    }
    NSString *why = nil;
    int start, end;
    if (endText.length) {
        start = [self resolveAddr:startText why:&why];
        end = [self resolveAddr:endText why:&why];
    } else {
        /* one field: a number, a symbol or a range lo-hi */
        uint16_t lo, hi;
        if (a7800sym_parse_range(_dbg, startText.UTF8String, &lo, &hi)) {
            start = lo;
            end = hi;
        } else {
            start = end = -1;
        }
    }
    const int last = 0xFFFF;
    if (start < 0 || end < 0 || end < start || end > last) {
        _bpErr.stringValue = [NSString stringWithFormat:@"Bad %@ range: %@%@%@%@%@ (allowed up to $%X)", what, startText,
                              endText.length ? @" - " : @"", endText, why.length ? @" (" : @"",
                              why.length ? [why stringByAppendingString:@")"] : @"", last];
        return;
    }
    const int bpId = a7800debug_breakpoint_add(_dbg, type, (uint16_t)start, (uint16_t)end, _bpCond.stringValue.UTF8String);
    if (bpId < 0) {
        _bpErr.stringValue = @"The condition does not parse";
        return;
    }
    _bpErr.stringValue = @"";
    _bpStart.stringValue = @"";
    _bpEnd.stringValue = @"";
    _bpCond.stringValue = @"";
    [self refreshNow];
}

- (void)bpEnabled:(NSButton *)sender
{
    a7800debug_breakpoint_enable(_dbg, (int)sender.tag, sender.state == NSControlStateValueOn);
    [self refreshNow];
}

- (void)bpRemove:(NSButton *)sender
{
    a7800debug_breakpoint_remove(_dbg, (int)sender.tag);
    [self refreshNow];
}

- (void)clearBreaks:(id)sender
{
    (void)sender;
    a7800debug_breakpoint_clear(_dbg);
    [self refreshNow];
}

- (void)clearHits:(id)sender
{
    (void)sender;
    a7800debug_breakpoint_clear_hits(_dbg);
    _bpSig = nil;       /* the hit counts redraw */
    [self refreshNow];
}

/* ---- watches, search, trace, pop out ---------------------------------------------- */

- (void)addWatch:(id)sender
{
    (void)sender;
    NSString *text = [_wtEntry.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (text.length == 0 || a7800watch_add(text.UTF8String) < 0) {
        [self message:@"Type an address, label, expression or lo-hi range to watch" console:NO];
        return;
    }
    _wtEntry.stringValue = @"";
    [self refreshWatches];
    if ([self tabVisible:TAB_MEMORY]) [self refreshMem];
}

/* A Write breakpoint over lo..hi that fires when the value changes. */
- (void)breakOnChangeLo:(uint16_t)lo hi:(uint16_t)hi
{
    if (a7800debug_breakpoint_add(_dbg, A7800DEBUG_BP_WRITE, lo, hi, "value != old") < 0) {
        [self message:@"The breakpoint was refused" console:NO];
        return;
    }
    [self message:@"Breaking when it changes" console:NO];
    [self refreshNow];
}

/* Selects a tab, or raises the window it was popped out into. */
- (void)showTab:(int)tab
{
    if (_popped[tab]) {
        [_popped[tab] makeKeyAndOrderFront:nil];
        return;
    }
    [_tabs selectTabViewItem:_items[tab]];
}

- (void)wtMemory:(NSButton *)sender
{
    uint16_t lo, hi;
    if (!a7800watch_range(_dbg, (int)sender.tag, &lo, &hi)) {
        [self message:@"That watch does not resolve" console:NO];
        return;
    }
    [self showTab:TAB_MEMORY];
    [self showMemLo:lo hi:hi];
}

- (void)wtBreak:(NSButton *)sender
{
    uint16_t lo, hi;
    if (!a7800watch_range(_dbg, (int)sender.tag, &lo, &hi)) {
        [self message:@"That watch does not resolve" console:NO];
        return;
    }
    [self breakOnChangeLo:lo hi:hi];
}

- (void)wtRemove:(NSButton *)sender
{
    a7800watch_remove((int)sender.tag);
    [self refreshWatches];
}

- (void)searchBackward:(BOOL)backward
{
    uint8_t pattern[A7800SEARCH_MAX], mask = 0xFF;
    int length = 0, flags = 0;
    if (!a7800search_parse((int)_srchKind.indexOfSelectedItem, _srchEntry.stringValue.UTF8String, pattern, &length,
                         &mask, &flags) || length <= 0) {
        _srchNote.stringValue = @"Type hex bytes (A9 00) or some text.";
        return;
    }
    if (!a7800debug_is_stopped(_dbg)) {
        _srchNote.stringValue = @"Stop the machine first.";
        return;
    }
    const uint16_t from = _memHl ? (backward ? (uint16_t)_memHlLo : (uint16_t)(_memHlLo + 1)) : (uint16_t)_memBase;
    uint16_t found = 0;
    const int result = a7800debug_search_next(_dbg, pattern, length, mask, flags, from, backward ? 1 : 0, &found);
    if (result < 0) {
        _srchNote.stringValue = @"Stop the machine first.";
    } else if (result == 0) {
        _srchNote.stringValue = @"Not found.";
    } else {
        _srchNote.stringValue = [NSString stringWithFormat:@"Found at $%04X", found];
        [self showMemLo:found hi:found + length - 1];
    }
}

- (void)searchNext:(id)sender { (void)sender; [self searchBackward:NO]; }
- (void)searchPrev:(id)sender { (void)sender; [self searchBackward:YES]; }

/* Both Record buttons follow the engine's one recorder. */
- (void)syncRecord:(BOOL)on
{
    for (int k = 0; k < 2; k++) {
        const NSInteger want = on ? NSControlStateValueOn : NSControlStateValueOff;
        if (_hRec[k].state != want) {
            _hRec[k].state = want;
            setButtonAccent(_hRec[k], on);
        }
    }
}

- (void)histRec:(NSButton *)sender
{
    a7800debug_trace_enable(_dbg, sender.state == NSControlStateValueOn);
    [self syncRecord:a7800debug_trace_enabled(_dbg) != 0];
    [self refreshTab];
}

- (void)histChanged:(id)sender
{
    (void)sender;
    [self refreshTab];
}

- (void)histClear:(id)sender
{
    (void)sender;
    a7800debug_trace_clear(_dbg);
    [self refreshTab];
}

- (void)popOutCurrent:(id)sender
{
    (void)sender;
    const NSInteger tab = [self currentTab];
    if (tab >= 0) [self popOutTab:(int)tab];
}

/* Moves a tab into a window of its own. One tab always stays. */
- (void)popOutTab:(int)tab
{
    if (tab < 0 || tab >= NUM_TABS || _popped[tab] || _tabs.numberOfTabViewItems <= 1) return;
    NSTabViewItem *item = _items[tab];
    NSView *view = item.view;
    [_tabs removeTabViewItem:item];

    NSWindow *win = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 820, 580)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    win.title = [@"Atari 7800 Debugger - " stringByAppendingString:kTabTitles[tab]];
    win.releasedWhenClosed = NO;
    win.delegate = self;
    NSButton *dockBtn = [self button:@"Dock" action:@selector(dockButton:)];
    dockBtn.tag = tab;
    dockBtn.toolTip = @"Put this tab back in the debugger";
    NSView *root = [[NSView alloc] init];
    dockBtn.translatesAutoresizingMaskIntoConstraints = NO;
    view.translatesAutoresizingMaskIntoConstraints = NO;
    [root addSubview:dockBtn];
    [root addSubview:view];
    [dockBtn.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:8].active = YES;
    [dockBtn.topAnchor constraintEqualToAnchor:root.topAnchor constant:8].active = YES;
    [view.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:8].active = YES;
    [view.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-8].active = YES;
    [view.topAnchor constraintEqualToAnchor:dockBtn.bottomAnchor constant:6].active = YES;
    [view.bottomAnchor constraintEqualToAnchor:root.bottomAnchor constant:-8].active = YES;
    win.contentView = root;
    A7800SetHelpContext(win, [@"debugger/" stringByAppendingString:kHelpTabs[tab]]);
    _popped[tab] = win;
    [win center];
    [win makeKeyAndOrderFront:nil];
    [self updateHelpContext];
    [self refreshTab];
}

- (void)dockButton:(id)sender
{
    [self dockTab:(int)((NSButton *)sender).tag];
}

/* Puts a popped-out tab back where it was in the order. */
- (void)dockTab:(int)tab
{
    if (tab < 0 || tab >= NUM_TABS) return;
    NSWindow *win = _popped[tab];
    if (!win) return;
    NSTabViewItem *item = _items[tab];
    NSView *view = item.view;
    int position = 0;
    _popped[tab] = nil;
    win.delegate = nil;
    [view removeFromSuperview];
    view.translatesAutoresizingMaskIntoConstraints = YES;     /* the tab view sizes it again */
    item.view = view;
    for (int t = 0; t < tab; t++)
        if (!_popped[t]) position++;
    [_tabs insertTabViewItem:item atIndex:position];
    [_tabs selectTabViewItem:item];
    [win orderOut:nil];
    [self updateHelpContext];
    [self refreshTab];
}

/* ---- refreshers ------------------------------------------------------------------ */

/* Everything the window shows, read back now: the status and the visible
 * tab. The poll compares against what was seen here. */
- (void)refreshNow
{
    _seenGen = a7800debug_generation(_dbg);
    _wasStopped = a7800debug_is_stopped(_dbg) != 0;
    _liveTicks = 0;
    [self refreshStatus];
    [self refreshTab];
}

/* The registers, and each tab on show (the notebook's page and any popped-out
 * window): the others refresh when they are selected. */
- (void)refreshTab
{
    [self refreshCpu];
    if (![self tabVisible:TAB_VIDEO]) _videoShown = NO;
    for (int t = 0; t < NUM_TABS; t++)
        if ([self tabVisible:t]) [self refreshTabAt:t];
}

- (void)refreshTabAt:(int)tab
{
    switch (tab) {
    case TAB_DISASM: [self refreshDisasm]; break;
    case TAB_MEMORY: [self refreshMem]; break;
    case TAB_BREAKS: [self refreshBps]; break;
    case TAB_WATCHES: [self refreshWatches]; break;
    case TAB_CART: [self refreshCart]; break;
    case TAB_TRACE: [self refreshHist:0]; break;
    case TAB_HIST: [self refreshHist:1]; break;
    case TAB_VIDEO: [self refreshVideo]; break;
    case TAB_SOUND: [self refreshSound]; break;
    case TAB_SYMBOLS: [self refreshSymbols]; break;
    case TAB_FUJINET: [self refreshFujinet:NO]; break;
    default: break;
    }
}

- (void)refreshStatus
{
    char reason[160];
    int addr = -1;
    const BOOL stopped = a7800debug_is_stopped(_dbg) != 0;
    reason[0] = '\0';
    a7800debug_stop_reason(_dbg, reason, (int)sizeof reason, &addr);
    NSString *why = cstr(reason);
    _status.stringValue = stopped ? [NSString stringWithFormat:@"Stopped%@%@", why.length ? @": " : @"", why] : @"Running";
    _runBtn.title = stopped ? @"Run (F5)" : @"Stop (F5)";
    setButtonAccent(_runBtn, stopped);    /* the accent while stopped */
}

- (void)refreshCpu
{
    a7800debug_cpu c;
    a7800debug_cpu_get(_dbg, &c);
    const int vals[NUM_REGS] = { c.pc, c.sp, c.a, c.x, c.y, c.ps };
    for (int i = 0; i < NUM_REGS; i++)
        if (!_reg[i].currentEditor)         /* never over a field being edited */
            _reg[i].stringValue = [NSString stringWithFormat:(i < 1 ? @"%04X" : @"%02X"), vals[i]];
    const int flags[NUM_FLAGS] = { c.n, c.v, c.d, c.i, c.z, c.c };
    for (int i = 0; i < NUM_FLAGS; i++) _flag[i].state = flags[i] ? NSControlStateValueOn : NSControlStateValueOff;
    _beam.stringValue = [NSString stringWithFormat:@"scanline %d   dot %d   frame %u   cycles %llu", c.scanline, c.dot,
                                                   c.frame, (unsigned long long)c.total_cycles];
}

/* The full 64K a page at a time; each row ends with the name of the region it
 * lies in (one a7800debug_memory_map per refresh). The highlighted range is
 * the accent fill, the watched ranges the selection colour. */
- (void)refreshMem
{
    static a7800debug_region regions[MAX_REGIONS];
    uint8_t data[MEM_BYTES];
    if (_memBase > 0x10000 - MEM_BYTES) _memBase = 0x10000 - MEM_BYTES;
    if (_memBase < 0) _memBase = 0;
    _memBase &= ~0xF;
    memset(data, 0, sizeof data);
    a7800debug_read(_dbg, (uint16_t)_memBase, data, MEM_BYTES);
    int regionCount = a7800debug_memory_map(_dbg, regions, MAX_REGIONS);
    if (regionCount < 0) regionCount = 0;
    if (regionCount > MAX_REGIONS) regionCount = MAX_REGIONS;

    int watchLo[A7800WATCH_MAX], watchHi[A7800WATCH_MAX], watchCount = 0;
    for (int i = 0; i < a7800watch_count() && watchCount < A7800WATCH_MAX; i++) {
        uint16_t lo, hi;
        if (a7800watch_range(_dbg, i, &lo, &hi)) {
            watchLo[watchCount] = lo;
            watchHi[watchCount] = hi;
            watchCount++;
        }
    }

    NSMutableString *text = [NSMutableString stringWithString:@"      0  1  2  3  4  5  6  7   8  9  A  B  C  D  E  F\n"];
    NSMutableArray<NSValue *> *marks = [NSMutableArray array];          /* the highlighted bytes' cells */
    NSMutableArray<NSValue *> *watchMarks = [NSMutableArray array];     /* the watched ones */
    for (int row = 0; row < MEM_ROWS; row++) {
        const int base = _memBase + row * 16;
        [text appendFormat:@"%04X  ", base];
        for (int pass = 0; pass < 2; pass++) {
            for (int col = 0; col < 16; col++) {
                const int addr = base + col;
                if (pass == 0 && col == 8) [text appendString:@" "];
                NSMutableArray<NSValue *> *target = nil;
                if (_memHl && addr >= _memHlLo && addr <= _memHlHi) {
                    target = marks;
                } else {
                    for (int w = 0; w < watchCount; w++)
                        if (addr >= watchLo[w] && addr <= watchHi[w]) { target = watchMarks; break; }
                }
                const uint8_t byte = data[row * 16 + col];
                if (target) [target addObject:[NSValue valueWithRange:NSMakeRange(text.length, pass == 0 ? 2 : 1)]];
                if (pass == 0) [text appendFormat:@"%02X ", byte];
                else [text appendFormat:@"%c", (byte >= 0x20 && byte < 0x7F) ? (char)byte : '.'];
            }
            if (pass == 0) [text appendString:@" "];
        }
        char region[64];
        region[0] = '\0';
        a7800mem_region_name(regions, regionCount, (uint16_t)base, region, (int)sizeof region);
        [text appendFormat:@"  %s\n", region];
    }
    NSMutableAttributedString *styled = [[NSMutableAttributedString alloc] initWithString:text attributes:monoAttrs()];
    for (NSValue *value in watchMarks)
        [styled addAttribute:NSBackgroundColorAttributeName value:NSColor.selectedTextBackgroundColor
                       range:value.rangeValue];
    for (NSValue *value in marks) [styled addAttributes:accentAttrs() range:value.rangeValue];
    setStyledText(_mem, styled);
}

- (void)refreshDisasm
{
    static a7800debug_line lines[DISASM_WINDOW];
    int pcLine = -1;
    if (_followPc.state == NSControlStateValueOn) {
        a7800debug_cpu c;
        a7800debug_cpu_get(_dbg, &c);
        /* the PC a third of the way down */
        _disasmAddr = (uint16_t)a7800debug_row_address(_dbg, (uint16_t)c.pc, -(DISASM_WINDOW / 3));
    }
    int n = a7800debug_disassemble(_dbg, _disasmAddr, lines, DISASM_WINDOW, &pcLine);
    if (n < 0) n = 0;
    if (n > DISASM_WINDOW) n = DISASM_WINDOW;
    _lineCount = n;

    /* "●> C00B  20 20 C0     LABEL            JSR $C020    ; note": the
     * ranges are the line, its label and its comment, for the colours */
    NSMutableString *text = [NSMutableString string];
    NSRange ranges[DISASM_WINDOW], labels[DISASM_WINDOW], notes[DISASM_WINDOW];
    for (int i = 0; i < n; i++) {
        _lineAddr[i] = lines[i].address;
        /* a red dot for a breakpoint, a grey ring for a disabled one */
        NSString *gutter = lines[i].has_breakpoint ? @"●" : (lines[i].bp_disabled ? @"○" : @" ");
        NSString *label = cstr(lines[i].label), *comment = cstr(lines[i].comment);
        NSMutableString *line = [NSMutableString stringWithFormat:@"%@%c %04X  ", gutter,
                                 lines[i].is_pc ? '>' : ' ', lines[i].address];
        [line appendString:padTo(cstr(lines[i].bytes), 11)];
        [line appendString:@"  "];
        labels[i] = NSMakeRange(text.length + line.length, label.length);
        [line appendString:padTo(label, 16)];
        [line appendString:@" "];
        [line appendString:padTo(cstr(lines[i].disasm), 24)];
        notes[i] = NSMakeRange(NSNotFound, 0);
        if (comment.length) {
            notes[i] = NSMakeRange(text.length + line.length, comment.length + 3);
            [line appendString:@" ; "];
            [line appendString:comment];
        }
        ranges[i] = NSMakeRange(text.length, line.length);
        [text appendString:line];
        [text appendString:@"\n"];
    }
    if (n == 0) [text appendString:@"(no disassembly: the debugger is not attached)\n"];

    NSMutableAttributedString *styled = [[NSMutableAttributedString alloc] initWithString:text attributes:monoAttrs()];
    for (int i = 0; i < n; i++) {
        /* labels and comments grey */
        if (labels[i].length)
            [styled addAttribute:NSForegroundColorAttributeName value:NSColor.systemGrayColor range:labels[i]];
        if (notes[i].location != NSNotFound)
            [styled addAttribute:NSForegroundColorAttributeName value:NSColor.systemGrayColor range:notes[i]];
        /* the gutter: a breakpoint red, a disabled one grey */
        if (lines[i].has_breakpoint || lines[i].bp_disabled)
            [styled addAttribute:NSForegroundColorAttributeName
                           value:lines[i].has_breakpoint ? breakpointRed() : NSColor.systemGrayColor
                           range:NSMakeRange(ranges[i].location, 1)];
    }
    /* the PC's line in the accent colour with white text, last so its
     * colours win */
    for (int i = 0; i < n; i++)
        if (lines[i].is_pc) [styled addAttributes:accentAttrs() range:ranges[i]];
    [_disasm.textStorage setAttributedString:styled];
    NSScrollView *scroll = _disasm.enclosingScrollView;
    [scroll.contentView scrollToPoint:NSMakePoint(scroll.contentView.bounds.origin.x, 0)];
    [scroll reflectScrolledClipView:scroll.contentView];
}

/* The Video tab: the controller says what changed. Forced when the tab
 * has just come on show. */
- (void)refreshVideo
{
    [_video refreshForce:!_videoShown];
    _videoShown = YES;
}

- (void)refreshBps
{
    static a7800debug_breakpoint bps[MAX_BPS];
    int n = a7800debug_breakpoint_list(_dbg, bps, MAX_BPS);
    if (n < 0) n = 0;
    if (n > MAX_BPS) n = MAX_BPS;

    /* the first line: why it stopped, or that it runs */
    {
        char reason[160];
        int where = -1;
        reason[0] = '\0';
        a7800debug_stop_reason(_dbg, reason, (int)sizeof reason, &where);
        NSString *why = cstr(reason);
        _bpStop.stringValue = a7800debug_is_stopped(_dbg)
            ? [NSString stringWithFormat:@"Stopped: %@", why.length ? why : @"stopped"]
            : @"Running.";
    }

    /* the rows are rebuilt only when something in them changed (a refresh
     * must not eat a click) */
    NSMutableString *signature = [NSMutableString string];
    NSMutableArray<NSString *> *descriptions = [NSMutableArray array];
    for (int i = 0; i < n; i++) {
        char desc[256];
        desc[0] = '\0';
        a7800bp_describe(_dbg, &bps[i], desc, (int)sizeof desc);
        [descriptions addObject:cstr(desc)];
        [signature appendFormat:@"%d %d %u %@\n", bps[i].id, bps[i].enabled, bps[i].hits, descriptions[(NSUInteger)i]];
    }
    if ([signature isEqualToString:_bpSig]) return;
    _bpSig = signature;

    for (NSView *v in [_bpList.arrangedSubviews copy]) [_bpList removeView:v];
    if (n == 0) [_bpList addArrangedSubview:[self label:@"No breakpoints"]];
    for (int i = 0; i < n; i++) {
        NSButton *box = [NSButton checkboxWithTitle:[NSString stringWithFormat:@"#%d", bps[i].id]
                                             target:self action:@selector(bpEnabled:)];
        box.state = bps[i].enabled ? NSControlStateValueOn : NSControlStateValueOff;
        box.tag = bps[i].id;
        box.font = monoFont();
        [box.widthAnchor constraintEqualToConstant:70].active = YES;
        NSButton *remove = [self button:@"Remove" action:@selector(bpRemove:)];
        remove.tag = bps[i].id;
        [remove.widthAnchor constraintEqualToConstant:70].active = YES;
        NSStackView *row = hstack(@[box, tableCell(descriptions[(NSUInteger)i], 0, NO),
                                    tableCell([NSString stringWithFormat:@"%u", bps[i].hits], 60, NO), remove]);
        row.distribution = NSStackViewDistributionFill;
        [_bpList addArrangedSubview:row];
        [row.widthAnchor constraintEqualToAnchor:_bpList.widthAnchor].active = YES;
    }
}

- (void)refreshWatches
{
    const int n = a7800watch_count();
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    NSMutableString *signature = [NSMutableString string];
    for (int i = 0; i < n; i++) {
        char buf[256];
        buf[0] = '\0';
        a7800watch_format(_dbg, i, buf, (int)sizeof buf);
        [lines addObject:cstr(buf)];
        [signature appendFormat:@"%@\n", lines[(NSUInteger)i]];
    }
    [signature appendFormat:@"%u", a7800watch_generation()];
    if ([signature isEqualToString:_wtSig]) return;      /* a refresh must not eat a click */
    _wtSig = signature;

    for (NSView *v in [_wtList.arrangedSubviews copy]) [_wtList removeView:v];
    if (n == 0) [_wtList addArrangedSubview:[self label:@"No watches"]];
    for (int i = 0; i < n; i++) {
        NSButton *mem = [self button:@"Show in Memory" action:@selector(wtMemory:)];
        mem.tag = i;
        NSButton *brk = [self button:@"Break on Change" action:@selector(wtBreak:)];
        brk.tag = i;
        NSButton *remove = [self button:@"Remove" action:@selector(wtRemove:)];
        remove.tag = i;
        NSStackView *row = hstack(@[tableCell(lines[(NSUInteger)i], 0, NO), mem, brk, remove]);
        row.distribution = NSStackViewDistributionFill;
        [_wtList addArrangedSubview:row];
        [row.widthAnchor constraintEqualToAnchor:_wtList.widthAnchor].active = YES;
    }
}

/* The cartridge, the memory map (drawn once) and the console's I/O: INPTCTRL,
 * the RIOT, the INPT lines and the controllers. */
- (void)refreshCart
{
    static a7800debug_region regions[MAX_REGIONS];
    static const char *const handovers[3] = { "none", "the BIOS", "the loader" };
    a7800debug_cart c;
    a7800debug_io io;
    char info[256];
    memset(&c, 0, sizeof c);
    memset(&io, 0, sizeof io);
    info[0] = '\0';
    a7800debug_cart_get(_dbg, &c);
    a7800debug_cart_info(_dbg, info, (int)sizeof info);
    a7800debug_io_get(_dbg, &io);
    NSMutableString *s = [NSMutableString stringWithFormat:@"%@\n\n", cstr(info)];
    if (!c.present) {
        [s appendString:@"(no cartridge running)\n"];
    } else {
        [s appendFormat:@"Link        %s", c.link_up ? "up" : "down"];
        if (!c.link_up && c.link_error[0]) [s appendFormat:@": %@", cstr(c.link_error)];
        [s appendFormat:@"\nMailbox     %s   ACKSEQ $%02X   last error %u   queue %u\n",
            c.worker ? "served" : "not served", c.ackseq, c.last_error, c.queue_depth];
        [s appendFormat:@"Mode        %s   hand-over by %s\n", c.mode_name,
            (c.handover >= 0 && c.handover < 3) ? handovers[c.handover] : "?"];
        [s appendFormat:@"Boot        state $%02X   %u%%   error %u\n", c.boot_state, c.boot_pct, c.boot_err];
        if (c.mode == 1) [s appendFormat:@"Loading     %d%%\n", c.load_pct];
        [s appendFormat:@"Image       %s   %s   CRC %08X\n", c.booted_image ? "a game" : "CONFIG", c.kind, c.live_crc];
        if (c.staged) [s appendFormat:@"Staged      %s   CRC %08X\n", c.staged_kind, c.staged_crc];
        else [s appendString:@"Staged      nothing\n"];
        [s appendFormat:@"High Score  %s%s%s%s\n", (c.hsc & 1) ? "ROM installed" : "no ROM",
            (c.hsc & 2) ? ", on" : ", off", (c.hsc & 4) ? ", saved" : "", (c.hsc & 8) ? ", unsaved changes" : ""];
        [s appendFormat:@"TV          %s\n", c.tv ? "PAL" : "NTSC"];
    }

    /* the memory map, once */
    [s appendString:@"\nMemory map\n"];
    int regionCount = a7800debug_memory_map(_dbg, regions, MAX_REGIONS);
    if (regionCount < 0) regionCount = 0;
    if (regionCount > MAX_REGIONS) regionCount = MAX_REGIONS;
    if (regionCount == 0) [s appendString:@"  (not available)\n"];
    for (int i = 0; i < regionCount; i++)
        [s appendFormat:@"  %04X-%04X  %c%c  %@\n", regions[i].lo, regions[i].hi, regions[i].readable ? 'r' : '-',
                        regions[i].writable ? 'w' : '-', cstr(regions[i].name)];

    /* the console's I/O: INPTCTRL, the RIOT, the INPT lines and the controllers */
    [s appendString:@"\nI/O\n"];
    [s appendFormat:@"INPTCTRL  $%02X   %s\n", io.inptctrl,
        io.inpt_locked ? "locked (the BIOS is out; MARIA on)" : "unlocked"];
    [s appendFormat:@"SWCHA     $%02X   player 1:", io.swcha];
    appendStick(s, (uint8_t)(io.swcha >> 4));
    [s appendString:@"   player 2:"];
    appendStick(s, (uint8_t)(io.swcha & 0x0F));
    [s appendFormat:@"\nSWCHB     $%02X  %s%s%s  left difficulty %c   right difficulty %c\n", io.swchb,
        (io.swchb & 0x01) ? "" : " RESET", (io.swchb & 0x02) ? "" : " SELECT", (io.swchb & 0x08) ? "" : " PAUSE",
        (io.swchb & 0x40) ? 'A' : 'B', (io.swchb & 0x80) ? 'A' : 'B'];
    [s appendString:@"INPT0-5  "];
    for (int i = 0; i < 6; i++) [s appendFormat:@" $%02X", io.inpt[i]];
    [s appendString:@"\n"];
    for (int port = 0; port < 2; port++) {
        const char *type = a7800_ctrl_type_name(io.port_type[port]);
        [s appendFormat:@"Player %d  %s, held:", port + 1, type ? type : "?"];
        appendHeld(s, io.held[port]);
        [s appendString:@"\n"];
    }
    setText(_cartText, s, NO);
}

/* The sound chips: the TIA's two channels and, when the cartridge has one, the POKEY. */
- (void)refreshSound
{
    a7800debug_sound snd;
    memset(&snd, 0, sizeof snd);
    a7800debug_sound_get(_dbg, &snd);
    NSMutableString *s = [NSMutableString string];
    [s appendString:@"TIA (two channels; volume 0 is off, 15 loudest)\n"];
    for (int ch = 0; ch < 2; ch++)
        [s appendFormat:@"  Channel %d   AUDC $%02X   AUDF $%02X   AUDV $%02X\n", ch, snd.tia_audc[ch], snd.tia_audf[ch],
                        snd.tia_audv[ch]];
    [s appendString:@"\n"];
    if (snd.pokey_present) {
        [s appendString:@"POKEY (on the cartridge)\n"];
        for (int ch = 0; ch < 4; ch++)
            [s appendFormat:@"  Channel %d   AUDF $%02X   AUDC $%02X\n", ch, snd.pokey_audf[ch], snd.pokey_audc[ch]];
        [s appendFormat:@"  AUDCTL      $%02X\n", snd.pokey_audctl];
    } else {
        [s appendString:@"POKEY: none on this cartridge\n"];
    }
    setText(_soundText, s, NO);
}

/* The symbol table, filtered: "AAAA  name", ordered by address.
 * The table's own order is unstable, so it is asked again each time. */
- (void)refreshSymbols
{
    const int total = a7800debug_symbols_count(_dbg);
    NSString *filter = [_symFilter.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    int *indices = total > 0 ? malloc((size_t)total * sizeof *indices) : NULL;
    int shown = 0;
    if (indices) shown = a7800debug_symbols_filter(_dbg, filter.UTF8String, indices, total);
    if (shown < 0) shown = 0;
    NSMutableString *text = [NSMutableString string];
    [_symAddrs removeAllObjects];
    for (int i = 0; i < shown; i++) {
        uint16_t address = 0;
        char name[96];
        name[0] = '\0';
        if (!a7800debug_symbols_get(_dbg, indices[i], &address, name, (int)sizeof name)) continue;
        [text appendFormat:@"%04X  %@\n", address, cstr(name)];
        [_symAddrs addObject:@(address)];
    }
    free(indices);
    if (_symAddrs.count == 0) [text appendString:@"(no symbols)\n"];
    NSScrollView *scroll = _symList.enclosingScrollView;
    const NSPoint at = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
    if (![_symList.string isEqualToString:text]) {
        _symList.string = text;
        if (scroll) {
            [scroll.contentView scrollToPoint:at];
            [scroll reflectScrolledClipView:scroll.contentView];
        }
    }
    _symCount.stringValue = [NSString stringWithFormat:@"%lu of %d symbols%@", (unsigned long)_symAddrs.count, total,
                                                       filter.length ? @" (filtered)" : @""];
}

/* Trace (idx 0) and History (idx 1): the newest HIST_SHOWN lines of the
 * engine's ring that pass the filter, oldest first. */
- (void)refreshHist:(int)idx
{
    static a7800debug_trace ring[HIST_RING];
    static int hit[HIST_SHOWN];
    const BOOL on = a7800debug_trace_enabled(_dbg) != 0;
    [self syncRecord:on];

    uint16_t lo = 0, hi = 0xFFFF;
    int filter = A7800DEBUG_TRACE_ALL;
    BOOL badRange = NO;
    switch (_hFilter[idx].indexOfSelectedItem) {
    case 1: filter = A7800DEBUG_TRACE_BRANCHES; break;
    case 2:
        if (a7800sym_parse_range(_dbg, _hRange[idx].stringValue.UTF8String, &lo, &hi)) filter = A7800DEBUG_TRACE_RANGE;
        else badRange = YES;
        break;
    default: break;
    }
    const int total = a7800debug_trace_count(_dbg);
    /* newest first: unfiltered, only the lines shown are needed */
    int n = a7800debug_trace_read(_dbg, ring, filter == A7800DEBUG_TRACE_ALL ? HIST_SHOWN : HIST_RING);
    if (n < 0) n = 0;
    int shown = 0;
    for (int i = 0; i < n && shown < HIST_SHOWN; i++)
        if (a7800debug_trace_match(&ring[i], filter, lo, hi)) hit[shown++] = i;

    NSMutableString *t = [NSMutableString string];
    for (int k = shown - 1; k >= 0; k--) {
        char line[256];
        line[0] = '\0';
        a7800debug_trace_format(&ring[hit[k]], line, (int)sizeof line);
        [t appendString:cstr(line)];
        [t appendString:@"\n"];
    }
    NSString *note;
    if (!on && total == 0)
        note = @"recording off";
    else
        note = [NSString stringWithFormat:@"%@, %d instructions%@", on ? @"recording" : @"recording off", total,
                shown < total ? [NSString stringWithFormat:@" (showing %d)", shown] : @""];
    if (badRange) note = [note stringByAppendingString:@" (range not understood: showing all)"];
    _hNote[idx].stringValue = note;
    setText(_hText[idx], t, YES);
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
- (void)refreshFujinet:(BOOL)force
{
    static a7800debug_fuji_packet p[FUJI_PACKETS_MAX];
    static char state[2048];
    char status[128];
    int n = a7800debug_fujinet_trace(_dbg, p, FUJI_PACKETS_MAX);
    if (n < 0) n = 0;

    status[0] = '\0';
    if (!a7800session_is_running(_session)) {
        _fujiNote.stringValue = @"The console is off.";
    } else if (a7800session_fujinet_external(_session)) {
        a7800session_cart_status(_session, status, (int)sizeof status);
        _fujiNote.stringValue = [NSString stringWithFormat:@"External FujiNet at %s. Cartridge: %@.",
                                 a7800session_boip_endpoint(_session), cstr(status)];
    } else if (!a7800session_fujinet_running(_session)) {
        _fujiNote.stringValue = @"FujiNet is off (Settings ▸ Enable FujiNet).";
    } else {
        a7800session_cart_status(_session, status, (int)sizeof status);
        _fujiNote.stringValue = [NSString stringWithFormat:@"FujiNet is on. Cartridge: %@.", cstr(status)];
    }
    state[0] = '\0';
    a7800debug_fujinet_state(_dbg, state, (int)sizeof state);
    setText(_fujiState, cstr(state), NO);

    if (!force && n == _fujiCount && (n == 0 || p[n - 1].serial == _fujiLast)) return;
    const BOOL follow = force || _fujiCount < 0 || atEnd(_fujiText);
    _fujiCount = n;
    _fujiLast = n > 0 ? p[n - 1].serial : 0;
    NSMutableString *s = [NSMutableString string];
    for (int i = 0; i < n; i++) {
        if (i) [s appendString:@"\n"];
        [s appendString:cstr(p[i].text)];
        if (p[i].repeat > 1) [s appendFormat:@" ×%u", p[i].repeat];
        [s appendString:@"\n     "];
        for (int k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++)
            [s appendFormat:@"%02X ", p[i].data[k]];
        if (p[i].nbytes > (int)sizeof p[i].data) [s appendString:@"…"];
    }
    if (n == 0) [s appendString:@"(no FujiBus packets since the trace was cleared)"];
    setText(_fujiText, s, follow);
}

@end
