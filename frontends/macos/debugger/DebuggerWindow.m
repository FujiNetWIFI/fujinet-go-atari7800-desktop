/*
 * Debugger window (AppKit) over MAME's own debugger engine, via
 * core/include/a7800debug.h. Mirrors the GTK, Qt and Win32 debuggers tab
 * for tab: Console, CPU & Memory, Disassembly, MARIA, I/O, Cartridge,
 * Breakpoints & Watchpoints.
 *
 * Opening the window attaches the engine, which stops the machine; closing
 * it detaches, and the machine runs on.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"

#import "../KeyForward.h"

#include <stdlib.h>
#include <string.h>

#include "a7800debug.h"

#define DISASM_WINDOW 48
#define MAX_BPS 64
#define MEM_ROWS 32             /* the memory view: 32 rows of 16 bytes */

static A7800DebuggerWindow *g_debugger;

/* Disassembly text view: a click puts the cursor on the clicked line (for
 * Run to Cursor and F9), a double click toggles its breakpoint; the wheel
 * browses. */
@interface DasmTextView : NSTextView
@property (nonatomic, copy) void (^onSelectLine)(int line);
@property (nonatomic, copy) void (^onToggleLine)(int line);
@property (nonatomic, copy) void (^onScroll)(int lines);
@end

@implementation DasmTextView
- (void)mouseDown:(NSEvent *)event
{
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger idx = [self characterIndexForInsertionAtPoint:p];
    NSString *text = self.string;
    if (idx > text.length) idx = text.length;
    int line = 0;
    for (NSUInteger i = 0; i < idx && i < text.length; i++)
        if ([text characterAtIndex:i] == '\n') line++;
    if (event.clickCount >= 2) {
        if (self.onToggleLine) self.onToggleLine(line);
    } else if (self.onSelectLine) {
        self.onSelectLine(line);
    }
}

- (void)scrollWheel:(NSEvent *)event
{
    if (self.onScroll) self.onScroll((int)(-event.scrollingDeltaY / 6.0));
}
@end

/* A picture (the palettes): nearest-neighbour, at the image's own
 * proportions, letterboxed. */
@interface PictureView : NSView
@property (nonatomic) CGImageRef image;
@end

@implementation PictureView
- (void)setImage:(CGImageRef)image
{
    if (_image) CGImageRelease(_image);
    _image = image;
    [self setNeedsDisplay:YES];
}
- (void)dealloc { if (_image) CGImageRelease(_image); }
- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    CGContextRef dc = [[NSGraphicsContext currentContext] CGContext];
    const NSRect b = [self bounds];
    CGContextSetRGBFillColor(dc, 0.1, 0.1, 0.1, 1);
    CGContextFillRect(dc, b);
    if (!_image) return;
    const double want = (double)CGImageGetWidth(_image) / (double)CGImageGetHeight(_image);
    double w = b.size.width, h = b.size.height, sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; } else { sw = w; sh = sw / want; }
    CGContextSetInterpolationQuality(dc, kCGInterpolationNone);
    CGContextDrawImage(dc, CGRectMake((w - sw) / 2, (h - sh) / 2, sw, sh), _image);
}
@end

@interface A7800DebuggerWindow ()
- (instancetype)initWithSession:(a7800session *)session;
- (void)refreshAll;
@end

@implementation A7800DebuggerWindow {
    a7800session *_session;
    a7800debug *_dbg;
    NSWindow *_window;
    NSTimer *_tick;
    unsigned _seenGen;
    BOOL _wasStopped;
    int _runningTicks;
    NSTabView *_tabs;

    NSButton *_runBtn;
    NSTextField *_status;

    NSTextView *_promptOut;
    NSTextField *_promptIn;

    NSTextField *_reg[6];
    NSButton *_flag[6];
    NSTextField *_cycles;
    NSTextView *_mem;
    NSTextField *_memAt;
    uint16_t _memAddr;
    NSTextField *_memWriteAddr, *_memWriteVal;

    NSButton *_followPc;
    NSTextField *_jump;
    DasmTextView *_disasm;
    uint16_t _disasmAddr;
    uint16_t _lineAddr[DISASM_WINDOW];
    int _lineCount;
    int _cursorAddr;            /* the line the cursor is on, or -1 */

    NSTextView *_mariaText;
    PictureView *_palettePic;
    uint32_t *_palettePx;

    NSTextView *_io;

    NSPopUpButton *_bpType;
    NSTextField *_bpRange, *_bpCond;
    NSStackView *_bpList;

    NSTextView *_cart;
}

static const char *const kFileKinds[3] = { "dis", "ram", "mem" };
static NSString *const kFileTitles[3] = { @"Disassembly…", @"RAM…", @"Memory…" };

/* ---- helpers --------------------------------------------------------------- */

static NSString *stripControl(const char *s)
{
    NSMutableString *out = [NSMutableString string];
    char buf[2] = { 0, 0 };
    for (; *s; s++) {
        if ((unsigned char)*s >= 0x20 || *s == '\n' || *s == '\t') {
            buf[0] = *s;
            [out appendString:[NSString stringWithUTF8String:buf] ?: @""];
        }
    }
    return out;
}

/* $hex, 0xhex, #dec, or bare hex. */
static BOOL parseNum(NSString *text, long *out)
{
    NSString *t = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    int base = 16;
    if ([t hasPrefix:@"$"]) t = [t substringFromIndex:1];
    else if ([t.lowercaseString hasPrefix:@"0x"]) t = [t substringFromIndex:2];
    else if ([t hasPrefix:@"#"]) { t = [t substringFromIndex:1]; base = 10; }
    if (t.length == 0) return NO;
    char *end;
    long v = strtol(t.UTF8String, &end, base);
    if (*end) return NO;
    *out = v;
    return YES;
}

/* A label, or a number. -1 when neither. */
- (int)resolveAddr:(NSString *)text
{
    long v;
    NSString *t = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (t.length == 0) return -1;
    int addr = a7800debug_label_address(_dbg, t.UTF8String);
    if (addr < 0 && parseNum(t, &v) && v >= 0 && v <= 0xffff) addr = (int)v;
    return addr;
}

static NSTextView *monoView(NSScrollView **scrollOut, BOOL wrap)
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = !wrap;
    NSTextView *view = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    view.editable = NO;
    view.richText = NO;
    view.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
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

/* Replace a text view's text, keeping the reader's place. */
static void setTextKeepingPlace(NSTextView *view, NSString *text)
{
    NSScrollView *scroll = view.enclosingScrollView;
    const NSPoint at = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
    view.string = text;
    if (scroll) [scroll.contentView scrollToPoint:at];
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

- (NSTextField *)field:(NSString *)placeholder width:(CGFloat)width action:(SEL)sel
{
    NSTextField *f = [[NSTextField alloc] init];
    f.placeholderString = placeholder;
    f.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    f.target = self;
    f.action = sel;
    if (width > 0) [f.widthAnchor constraintEqualToConstant:width].active = YES;
    return f;
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
    [g_debugger->_window makeKeyAndOrderFront:nil];
    a7800debug_attach(g_debugger->_dbg);
    [g_debugger refreshAll];
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

- (instancetype)initWithSession:(a7800session *)session
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _dbg = a7800session_debugger(session);
    _palettePx = calloc((size_t)A7800DEBUG_PALETTE_WIDTH * A7800DEBUG_PALETTE_HEIGHT, sizeof *_palettePx);
    _memAddr = 0x1800;          /* the console's RAM */
    _cursorAddr = -1;
    [self buildWindow];
    __weak A7800DebuggerWindow *weakSelf = self;
    _tick = [NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *t) {
        (void)t;
        [weakSelf onTick];
    }];
    return self;
}

- (void)dealloc
{
    [_tick invalidate];
    free(_palettePx);
}

- (void)windowWillClose:(NSNotification *)note
{
    if (note.object != _window) return;
    a7800debug_detach(_dbg);
}

- (void)onTick
{
    if (![_window isVisible]) return;
    const unsigned gen = a7800debug_generation(_dbg);
    const BOOL stopped = a7800debug_is_stopped(_dbg) != 0;
    if (gen != _seenGen || stopped != _wasStopped) {
        _seenGen = gen;
        _wasStopped = stopped;
        [self refreshAll];
    } else if (!stopped && ++_runningTicks >= 5) {
        /* running: the live values twice a second */
        _runningTicks = 0;
        [self refreshStatus];
        [self refreshCpu];
        [self refreshMaria];
        [self refreshIo];
        [self refreshCart];
    }
}

/* ---- construction ------------------------------------------------------------ */

- (NSView *)buildPrompt
{
    NSScrollView *outScroll;
    _promptOut = monoView(&outScroll, YES);
    _promptOut.string = @"MAME's debugger console. Type 'help' for every command; 'cart' and 'maria' are this app's.\n";
    _promptIn = [self field:@"command (help, step, bpset c000, wpset 80,1,w, print a, dump 1800,40, cart, maria) — Tab completes"
                      width:0 action:@selector(runPrompt:)];
    _promptIn.delegate = self;
    NSButton *sym = [self button:@"Load symbols…" action:@selector(loadSymbols:)];
    NSStackView *row = hstack(@[_promptIn, sym]);

    NSMutableArray *files = [NSMutableArray arrayWithObject:[self label:@"Save"]];
    for (int i = 0; i < 3; i++) {
        NSButton *b = [self button:kFileTitles[i] action:@selector(saveFile:)];
        b.tag = i;
        [files addObject:b];
    }
    NSStackView *saves = hstack(files);

    NSStackView *v = vstack(@[outScroll, row, saves]);
    fill(v, outScroll);
    fill(v, row);
    return v;
}

- (NSView *)buildCpu
{
    static NSString *const names[6] = { @"PC", @"SP", @"A", @"X", @"Y", @"P" };
    NSMutableArray *regs = [NSMutableArray array];
    for (int i = 0; i < 6; i++) {
        [regs addObject:[self label:names[i]]];
        _reg[i] = [self field:@"" width:56 action:@selector(applyRegister:)];
        _reg[i].tag = i;
        [regs addObject:_reg[i]];
    }
    static NSString *const fnames[6] = { @"N", @"V", @"D", @"I", @"Z", @"C" };
    NSMutableArray *flags = [NSMutableArray array];
    for (int i = 0; i < 6; i++) {
        _flag[i] = [NSButton checkboxWithTitle:fnames[i] target:self action:@selector(flagClicked:)];
        _flag[i].tag = i;
        [flags addObject:_flag[i]];
    }
    _cycles = [self label:@""];
    _cycles.textColor = NSColor.secondaryLabelColor;
    [flags addObject:_cycles];

    NSScrollView *memScroll;
    _mem = monoView(&memScroll, NO);
    _memAt = [self field:@"$1800 or a label" width:140 action:@selector(memGoTo:)];
    NSTextField *where = [self label:@"$1800-$27FF is the console's RAM; $0040-$00FF and $0140-$01FF mirror it"];
    where.textColor = NSColor.secondaryLabelColor;
    NSStackView *view = hstack(@[[self label:@"Memory at"], _memAt, where]);

    _memWriteAddr = [self field:@"$1800" width:80 action:@selector(writeMem:)];
    _memWriteVal = [self field:@"$00" width:60 action:@selector(writeMem:)];
    NSTextField *hint = [self label:@"RAM and the cartridge's RAM take a write"];
    hint.textColor = NSColor.secondaryLabelColor;
    NSStackView *edit = hstack(@[[self label:@"Write address"], _memWriteAddr, [self label:@"value"], _memWriteVal, hint]);

    NSStackView *v = vstack(@[hstack(regs), hstack(flags), view, memScroll, edit]);
    fill(v, memScroll);
    return v;
}

- (NSView *)buildDisasm
{
    _followPc = [NSButton checkboxWithTitle:@"Follow PC" target:self action:@selector(refreshDisasm)];
    _followPc.state = NSControlStateValueOn;
    _jump = [self field:@"address or label" width:150 action:@selector(jumpTo:)];
    NSTextField *hint = [self label:@"Click a line for the cursor (F9, Run to Cursor); double-click toggles its breakpoint"];
    hint.textColor = NSColor.secondaryLabelColor;
    NSStackView *row = hstack(@[_followPc, [self label:@"Jump to"], _jump, hint]);

    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = NO;
    scroll.hasHorizontalScroller = YES;
    _disasm = [[DasmTextView alloc] initWithFrame:NSMakeRect(0, 0, 600, 600)];
    _disasm.editable = NO;
    _disasm.richText = NO;
    _disasm.selectable = NO;
    _disasm.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    _disasm.horizontallyResizable = YES;
    _disasm.textContainer.widthTracksTextView = NO;
    _disasm.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
    _disasm.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    __weak A7800DebuggerWindow *weakSelf = self;
    _disasm.onSelectLine = ^(int line) {
        A7800DebuggerWindow *s = weakSelf;
        if (!s || line < 0 || line >= s->_lineCount) return;
        s->_cursorAddr = s->_lineAddr[line];
        [s refreshDisasm];
    };
    _disasm.onToggleLine = ^(int line) {
        A7800DebuggerWindow *s = weakSelf;
        if (!s || line < 0 || line >= s->_lineCount) return;
        a7800debug_breakpoint_toggle(s->_dbg, s->_lineAddr[line]);
        [s refreshDisasm];
        [s refreshBps];
    };
    _disasm.onScroll = ^(int lines) {
        A7800DebuggerWindow *s = weakSelf;
        if (!s || lines == 0) return;
        s->_followPc.state = NSControlStateValueOff;
        s->_disasmAddr = (uint16_t)a7800debug_row_address(s->_dbg, s->_disasmAddr, lines);
        [s refreshDisasm];
    };
    scroll.documentView = _disasm;

    NSStackView *v = vstack(@[row, scroll]);
    fill(v, scroll);
    return v;
}

- (NSView *)buildMaria
{
    NSScrollView *textScroll;
    _mariaText = monoView(&textScroll, NO);
    NSStackView *left = vstack(@[textScroll]);
    fill(left, textScroll);

    NSTextField *title = [self label:@"Palettes: BACKGRND, then P0C1-P7C3"];
    title.textColor = NSColor.secondaryLabelColor;
    _palettePic = [[PictureView alloc] initWithFrame:NSMakeRect(0, 0, 512, 128)];
    [_palettePic.widthAnchor constraintGreaterThanOrEqualToConstant:512].active = YES;
    [_palettePic.heightAnchor constraintEqualToConstant:128].active = YES;
    NSStackView *right = vstack(@[title, _palettePic]);

    NSStackView *h = hstack(@[left, right]);
    h.alignment = NSLayoutAttributeTop;
    [left.widthAnchor constraintGreaterThanOrEqualToConstant:420].active = YES;
    [textScroll.heightAnchor constraintGreaterThanOrEqualToConstant:500].active = YES;
    return h;
}

- (NSView *)buildIo
{
    NSScrollView *scroll;
    _io = monoView(&scroll, YES);
    return scroll;
}

- (NSView *)buildBreaks
{
    _bpType = [[NSPopUpButton alloc] init];
    [_bpType addItemsWithTitles:@[@"Execute", @"Read", @"Write", @"Read/Write"]];
    _bpRange = [self field:@"$C000 or $1800-$18FF or a label" width:220 action:@selector(addBreakpoint:)];
    _bpCond = [self field:@"condition (optional): a == $10 && x > 2" width:0 action:@selector(addBreakpoint:)];
    NSButton *add = [self button:@"Add" action:@selector(addBreakpoint:)];
    NSButton *clear = [self button:@"Clear all" action:@selector(clearBreaks:)];
    NSStackView *row = hstack(@[_bpType, _bpRange, _bpCond, add, clear]);

    /* The rows scroll: a session can collect more breakpoints than the tab
     * is tall. The stack is the scroll view's document, pinned to the top
     * and to the clip view's width so rows keep the tab's width. */
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
    NSTextField *hint = [self label:@"Execute breakpoints stop at an address; Read/Write watchpoints on a range. "
                                    "Untick to disable; conditions are MAME debugger expressions"];
    hint.textColor = NSColor.secondaryLabelColor;
    NSStackView *v = vstack(@[row, hint, bpScroll]);
    fill(v, row);
    fill(v, bpScroll);
    [bpScroll setContentHuggingPriority:NSLayoutPriorityDefaultLow
                         forOrientation:NSLayoutConstraintOrientationVertical];
    return v;
}

- (NSView *)buildCart
{
    NSScrollView *scroll;
    _cart = monoView(&scroll, YES);
    return scroll;
}

- (NSButton *)toolButton:(NSString *)title action:(SEL)sel key:(unichar)fkey shift:(BOOL)shift
{
    NSButton *b = [self button:title action:sel];
    if (fkey) {
        b.keyEquivalent = [NSString stringWithFormat:@"%C", fkey];
        b.keyEquivalentModifierMask = shift ? NSEventModifierFlagShift : 0;
    }
    return b;
}

- (void)buildWindow
{
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 1100, 760)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"Debugger";
    _window.releasedWhenClosed = NO;
    _window.delegate = self;

    /* Toolbar row. F5/F7/F8/Shift+F8/F9 work as the buttons' key
     * equivalents wherever the focus is inside the window. */
    _runBtn = [self toolButton:@"Stop (F5)" action:@selector(toggleRun:) key:NSF5FunctionKey shift:NO];
    NSButton *step = [self toolButton:@"Step (F7)" action:@selector(step:) key:NSF7FunctionKey shift:NO];
    NSButton *over = [self toolButton:@"Step Over (F8)" action:@selector(stepOver:) key:NSF8FunctionKey shift:NO];
    NSButton *outBtn = [self toolButton:@"Step Out (⇧F8)" action:@selector(stepOut:) key:NSF8FunctionKey shift:YES];
    NSButton *frame = [self toolButton:@"Frame" action:@selector(frame:) key:0 shift:NO];
    NSButton *runTo = [self toolButton:@"Run to Cursor" action:@selector(runToCursor:) key:0 shift:NO];
    NSButton *bp = [self toolButton:@"Breakpoint (F9)" action:@selector(toggleCursorBreakpoint:) key:NSF9FunctionKey shift:NO];
    _status = [self label:@"Running"];
    _status.alignment = NSTextAlignmentRight;
    _status.textColor = NSColor.secondaryLabelColor;
    _status.lineBreakMode = NSLineBreakByTruncatingTail;
    NSStackView *toolbar = hstack(@[_runBtn, step, over, outBtn, frame, runTo, bp, _status]);
    [_status setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_status setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                      forOrientation:NSLayoutConstraintOrientationHorizontal];

    _tabs = [[NSTabView alloc] init];
    NSArray *pages = @[ @[@"Console", [self buildPrompt]], @[@"CPU & Memory", [self buildCpu]],
                        @[@"Disassembly", [self buildDisasm]], @[@"MARIA", [self buildMaria]],
                        @[@"I/O", [self buildIo]], @[@"Cartridge", [self buildCart]],
                        @[@"Breakpoints & Watchpoints", [self buildBreaks]] ];
    for (NSArray *page in pages) {
        NSTabViewItem *item = [NSTabViewItem tabViewItemWithViewController:nil];
        item.label = page[0];
        NSView *content = page[1];
        NSView *holder = [[NSView alloc] init];
        [holder addSubview:content];
        content.translatesAutoresizingMaskIntoConstraints = NO;
        [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:6].active = YES;
        [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-6].active = YES;
        [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:6].active = YES;
        [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-6].active = YES;
        item.view = holder;
        [_tabs addTabViewItem:item];
    }
    const char *tab = getenv("A7800_DEBUGGER_TAB");
    if (tab && *tab) {
        const int t = atoi(tab);
        if (t >= 0 && t < (int)_tabs.numberOfTabViewItems) [_tabs selectTabViewItemAtIndex:t];
    }

    NSStackView *root = vstack(@[toolbar, _tabs]);
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [toolbar.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [_tabs.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    _window.contentView = root;
    [_window center];
}

/* ---- actions ------------------------------------------------------------------ */

- (void)toggleRun:(id)sender
{
    (void)sender;
    if (a7800debug_is_stopped(_dbg)) a7800debug_resume(_dbg); else a7800debug_stop(_dbg);
    [self refreshAll];
}
- (void)step:(id)sender { (void)sender; a7800debug_step(_dbg); [self refreshAll]; }
- (void)stepOver:(id)sender { (void)sender; a7800debug_step_over(_dbg); [self refreshAll]; }
- (void)stepOut:(id)sender { (void)sender; a7800debug_step_out(_dbg); [self refreshAll]; }
- (void)frame:(id)sender { (void)sender; a7800debug_frame(_dbg); [self refreshAll]; }

- (void)runToCursor:(id)sender
{
    (void)sender;
    if (_cursorAddr < 0) {
        _status.stringValue = @"Click a disassembly line first";
        return;
    }
    a7800debug_run_to(_dbg, (uint16_t)_cursorAddr);
    [self refreshAll];
}

- (void)toggleCursorBreakpoint:(id)sender
{
    (void)sender;
    if (_cursorAddr < 0) {
        _status.stringValue = @"Click a disassembly line first";
        return;
    }
    a7800debug_breakpoint_toggle(_dbg, (uint16_t)_cursorAddr);
    [self refreshDisasm];
    [self refreshBps];
}

- (void)appendPrompt:(NSString *)text
{
    [_promptOut.textStorage appendAttributedString:
        [[NSAttributedString alloc] initWithString:text attributes:@{ NSFontAttributeName: _promptOut.font ?: [NSFont userFixedPitchFontOfSize:11] }]];
    [_promptOut scrollRangeToVisible:NSMakeRange(_promptOut.string.length, 0)];
}

- (void)runPrompt:(id)sender
{
    (void)sender;
    static char out[65536];
    NSString *cmd = [_promptIn.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (cmd.length == 0) return;
    [self appendPrompt:[NSString stringWithFormat:@"> %@\n", cmd]];
    a7800debug_command(_dbg, cmd.UTF8String, out, sizeof out);
    [self appendPrompt:[stripControl(out) stringByAppendingString:@"\n"]];
    _promptIn.stringValue = @"";
    [self refreshAll];
}

/* Tab in the prompt completes instead of moving the focus. */
- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView doCommandBySelector:(SEL)sel
{
    (void)textView;
    if (control != _promptIn || sel != @selector(insertTab:)) return NO;
    NSString *text = _promptIn.stringValue;
    const NSRange sp = [text rangeOfString:@" " options:NSBackwardsSearch];
    NSString *word = sp.location == NSNotFound ? text : [text substringFromIndex:sp.location + 1];
    char comps[4096];
    const int n = a7800debug_completions(_dbg, word.UTF8String, comps, sizeof comps);
    if (n == 1) {
        NSString *c = [[NSString stringWithUTF8String:comps] componentsSeparatedByString:@"\n"][0];
        NSString *head = sp.location == NSNotFound ? @"" : [text substringToIndex:sp.location + 1];
        _promptIn.stringValue = [NSString stringWithFormat:@"%@%@ ", head, c];
        [_promptIn.currentEditor setSelectedRange:NSMakeRange(_promptIn.stringValue.length, 0)];
    } else if (n > 1) {
        [self appendPrompt:[NSString stringWithUTF8String:comps] ?: @""];
    }
    return YES;
}

- (void)loadSymbols:(id)sender
{
    (void)sender;
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.title = @"Load Symbols (ld65 -Ln / VICE, ca65 .dbg, name = $1234)";
    if ([p runModal] != NSModalResponseOK) return;
    char msg[512];
    a7800debug_load_symbols(_dbg, [[p URL] fileSystemRepresentation], msg, sizeof msg);
    [self appendPrompt:[stripControl(msg) stringByAppendingString:@"\n"]];
    [self refreshAll];
}

- (void)saveFile:(NSButton *)sender
{
    NSSavePanel *p = [NSSavePanel savePanel];
    p.title = [NSString stringWithFormat:@"Save %@", kFileTitles[sender.tag]];
    if ([p runModal] != NSModalResponseOK) return;
    char msg[512];
    a7800debug_save(_dbg, kFileKinds[sender.tag], [[p URL] fileSystemRepresentation], msg, sizeof msg);
    [self appendPrompt:[stripControl(msg) stringByAppendingString:@"\n"]];
}

- (void)applyRegister:(NSTextField *)sender
{
    static const int regIds[6] = { A7800_REG_PC, A7800_REG_SP, A7800_REG_A, A7800_REG_X, A7800_REG_Y, A7800_REG_PS };
    long v;
    if (parseNum(sender.stringValue, &v)) a7800debug_cpu_set(_dbg, regIds[sender.tag], (int)v);
    [self refreshAll];
}

- (void)flagClicked:(NSButton *)sender
{
    static const int flagIds[6] = { A7800_FLAG_N, A7800_FLAG_V, A7800_FLAG_D, A7800_FLAG_I, A7800_FLAG_Z, A7800_FLAG_C };
    if (a7800debug_is_stopped(_dbg))
        a7800debug_cpu_set(_dbg, flagIds[sender.tag], sender.state == NSControlStateValueOn);
    [self refreshCpu];
}

- (void)memGoTo:(id)sender
{
    (void)sender;
    const int a = [self resolveAddr:_memAt.stringValue];
    if (a < 0) return;
    _memAddr = (uint16_t)(a & 0xFFF0);
    [self refreshMem];
}

- (void)writeMem:(id)sender
{
    (void)sender;
    long v;
    const int a = [self resolveAddr:_memWriteAddr.stringValue];
    if (a >= 0 && parseNum(_memWriteVal.stringValue, &v))
        a7800debug_write(_dbg, (uint16_t)a, (uint8_t)v);
    [self refreshAll];
}

- (void)jumpTo:(id)sender
{
    (void)sender;
    const int addr = [self resolveAddr:_jump.stringValue];
    if (addr < 0) return;
    _followPc.state = NSControlStateValueOff;
    _disasmAddr = (uint16_t)addr;
    [self refreshDisasm];
}

- (void)addBreakpoint:(id)sender
{
    (void)sender;
    static const int types[4] = { A7800DEBUG_BP_EXEC, A7800DEBUG_BP_READ, A7800DEBUG_BP_WRITE,
                                  A7800DEBUG_BP_READ | A7800DEBUG_BP_WRITE };
    NSString *range = [_bpRange.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (range.length == 0) return;
    NSArray<NSString *> *ends = [range componentsSeparatedByString:@"-"];
    const int start = [self resolveAddr:ends[0]];
    const int end = ends.count > 1 ? [self resolveAddr:ends[1]] : start;
    if (start < 0 || end < 0) {
        [self appendPrompt:[NSString stringWithFormat:@"breakpoint: bad address %@\n", range]];
        _status.stringValue = [NSString stringWithFormat:@"Bad address: %@", range];
        return;
    }
    const NSInteger sel = _bpType.indexOfSelectedItem;
    const int bpId = a7800debug_breakpoint_add(_dbg, types[sel >= 0 && sel < 4 ? sel : 0],
                                               (uint16_t)start, (uint16_t)end, _bpCond.stringValue.UTF8String);
    if (bpId < 0) {
        _status.stringValue = @"The condition does not parse";
        return;
    }
    _bpRange.stringValue = @"";
    _bpCond.stringValue = @"";
    [self refreshAll];
}

- (void)bpEnabled:(NSButton *)sender
{
    a7800debug_breakpoint_enable(_dbg, (int)sender.tag, sender.state == NSControlStateValueOn);
    [self refreshAll];
}

- (void)bpRemove:(NSButton *)sender
{
    a7800debug_breakpoint_remove(_dbg, (int)sender.tag);
    [self refreshAll];
}

- (void)clearBreaks:(id)sender
{
    (void)sender;
    a7800debug_breakpoint_clear(_dbg);
    [self refreshAll];
}

/* ---- refreshers ------------------------------------------------------------------ */

- (void)refreshStatus
{
    char reason[160];
    int addr;
    const BOOL stopped = a7800debug_is_stopped(_dbg) != 0;
    a7800debug_stop_reason(_dbg, reason, sizeof reason, &addr);
    _status.stringValue = stopped ? [NSString stringWithFormat:@"Stopped%s%s", reason[0] ? ": " : "", reason] : @"Running";
    _runBtn.title = stopped ? @"Run (F5)" : @"Stop (F5)";
    _runBtn.bezelColor = stopped ? A7800AccentColor() : nil;
}

- (void)refreshCpu
{
    a7800debug_cpu c;
    a7800debug_cpu_get(_dbg, &c);
    const int vals[6] = { c.pc, c.sp, c.a, c.x, c.y, c.ps };
    for (int i = 0; i < 6; i++)
        if (!_reg[i].currentEditor)
            _reg[i].stringValue = [NSString stringWithFormat:(i == 0 ? @"%04X" : @"%02X"), vals[i]];
    const int flags[6] = { c.n, c.v, c.d, c.i, c.z, c.c };
    for (int i = 0; i < 6; i++) _flag[i].state = flags[i] ? NSControlStateValueOn : NSControlStateValueOff;
    _cycles.stringValue = [NSString stringWithFormat:@"cycle %llu   scanline %d   dot %d   frame %u",
                           (unsigned long long)c.total_cycles, c.scanline, c.dot, c.frame];
}

- (void)refreshMem
{
    static uint8_t bytes[MEM_ROWS * 16];
    const int n = a7800debug_read(_dbg, _memAddr, bytes, (int)sizeof bytes);
    NSMutableString *text = [NSMutableString stringWithString:@"        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n"];
    for (int row = 0; row < MEM_ROWS; row++) {
        const unsigned at = (_memAddr + row * 16) & 0xFFFF;
        [text appendFormat:@"$%04X: ", at];
        char ascii[17];
        for (int col = 0; col < 16; col++) {
            const int i = row * 16 + col;
            if (i < n) {
                [text appendFormat:@"%02X ", bytes[i]];
                ascii[col] = (bytes[i] >= 0x20 && bytes[i] < 0x7f) ? (char)bytes[i] : '.';
            } else {
                [text appendString:@"-- "];
                ascii[col] = ' ';
            }
        }
        ascii[16] = '\0';
        [text appendFormat:@" %s\n", ascii];
    }
    setTextKeepingPlace(_mem, text);
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
    const int n = a7800debug_disassemble(_dbg, _disasmAddr, lines, DISASM_WINDOW, &pcLine);
    _lineCount = n;
    NSMutableString *text = [NSMutableString string];
    NSUInteger pcStart = 0, pcLen = 0, curStart = 0, curLen = 0;
    BOOL havePc = NO, haveCursor = NO;
    for (int i = 0; i < n; i++) {
        _lineAddr[i] = lines[i].address;
        NSMutableString *line = [NSMutableString stringWithFormat:@"%c%c %04X  %-9s %-12s %s",
                          lines[i].has_breakpoint ? '*' : ' ', lines[i].is_pc ? '>' : ' ',
                          lines[i].address, lines[i].bytes, lines[i].label, lines[i].disasm];
        if (lines[i].comment[0]) [line appendFormat:@"  ; %s", lines[i].comment];
        [line appendString:@"\n"];
        if (lines[i].is_pc) { havePc = YES; pcStart = text.length; pcLen = line.length - 1; }
        else if ((int)lines[i].address == _cursorAddr) { haveCursor = YES; curStart = text.length; curLen = line.length - 1; }
        [text appendString:line];
    }
    if (n == 0) [text appendString:@"(no disassembly: the machine is not running)\n"];
    _disasm.string = text;
    if (haveCursor) {
        [_disasm.textStorage addAttributes:@{ NSBackgroundColorAttributeName: NSColor.selectedTextBackgroundColor }
                                     range:NSMakeRange(curStart, curLen)];
    }
    if (havePc) {
        [_disasm.textStorage addAttributes:@{ NSBackgroundColorAttributeName: A7800AccentColor(),
                                              NSForegroundColorAttributeName: NSColor.whiteColor }
                                     range:NSMakeRange(pcStart, pcLen)];
    }
}

- (void)refreshMaria
{
    static const char *const readModes[4] = { "160A / 160B", "?", "320B / 320D", "320A / 320C" };
    a7800debug_maria m;
    a7800debug_maria_get(_dbg, &m);
    NSMutableString *s = [NSMutableString string];
    [s appendFormat:@"Scanline  %d%@\n\n", m.scanline, m.vblank ? @"   (VBLANK)" : @""];
    [s appendFormat:@"CTRL      $%02X   DMA %s   colour kill %s   kangaroo %s   border %s\n",
        m.ctrl, m.dma_on ? "on" : "off", m.color_kill ? "on" : "off",
        m.kangaroo ? "on" : "off", m.border_control ? "black" : "background"];
    [s appendFormat:@"          character width %d byte%s   read mode %s\n",
        m.char_width, m.char_width == 1 ? "" : "s", readModes[m.read_mode & 3]];
    [s appendFormat:@"CHARBASE  $%02X (characters at $%02X00)\n", m.charbase, m.charbase];
    [s appendFormat:@"DPP       $%04X\n", m.dpp];
    [s appendFormat:@"DMA now   DLL $%04X   DL $%04X   offset %d%s%s\n\n",
        m.dll, m.dl, m.offset, m.holey ? "   holey" : "", m.dli ? "   DLI" : ""];
    [s appendFormat:@"BACKGRND  $%02X\n", m.palette[0]];
    for (int p = 0; p < 8; p++)
        [s appendFormat:@"P%d        $%02X $%02X $%02X\n", p,
            m.palette[1 + p * 3], m.palette[2 + p * 3], m.palette[3 + p * 3]];

    static char dll[16384];
    [s appendString:@"\nDisplay list list\n"];
    if (a7800debug_dll_text(_dbg, dll, sizeof dll) > 0) [s appendString:stripControl(dll)];
    else [s appendString:@"(none: DMA has not run)\n"];
    setTextKeepingPlace(_mariaText, s);

    if (a7800debug_palette_image(_dbg, _palettePx)) {
        const size_t w = A7800DEBUG_PALETTE_WIDTH, h = A7800DEBUG_PALETTE_HEIGHT;
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CFDataRef data = CFDataCreate(NULL, (const UInt8 *)_palettePx, (CFIndex)(w * h * 4));
        CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
        CGImageRef img = CGImageCreate(w, h, 8, 32, w * 4, space,
                                       kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, provider,
                                       NULL, false, kCGRenderingIntentDefault);
        CGDataProviderRelease(provider);
        CFRelease(data);
        CGColorSpaceRelease(space);
        _palettePic.image = img;   /* the view owns it now */
    }
}

/* The a7800_action bits a port holds, by name. */
static void appendHeld(NSMutableString *s, unsigned held)
{
    static const char *const names[A7800_ACT_PER_PORT] = { "Up", "Down", "Left", "Right", "Button 1", "Button 2" };
    BOOL any = NO;
    for (int a = 0; a < A7800_ACT_PER_PORT; a++)
        if (held & (1u << a)) { [s appendFormat:@" %s", names[a]]; any = YES; }
    if (!any) [s appendString:@" (nothing held)"];
}

/* A joystick's four lines in SWCHA (active low): player 1 in the high
 * nibble, player 2 in the low one. */
static void appendStick(NSMutableString *s, uint8_t nibble)
{
    static const char *const dirs[4] = { "up", "down", "left", "right" };
    BOOL any = NO;
    for (int b = 0; b < 4; b++)
        if (!(nibble & (1u << b))) { [s appendFormat:@" %s", dirs[b]]; any = YES; }
    if (!any) [s appendString:@" centred"];
}

- (void)refreshIo
{
    a7800debug_io io;
    a7800debug_io_get(_dbg, &io);
    NSMutableString *s = [NSMutableString string];
    [s appendFormat:@"INPTCTRL  $%02X   %s\n\n", io.inptctrl,
        io.inpt_locked ? "locked (the BIOS is out; MARIA on)" : "unlocked"];
    for (int ch = 0; ch < 2; ch++)
        [s appendFormat:@"TIA %d     AUDC $%02X   AUDF $%02X   AUDV $%02X\n", ch, io.audc[ch], io.audf[ch], io.audv[ch]];
    [s appendFormat:@"\nSWCHA     $%02X   player 1:", io.swcha];
    appendStick(s, (uint8_t)(io.swcha >> 4));
    [s appendString:@"   player 2:"];
    appendStick(s, (uint8_t)(io.swcha & 0x0F));
    [s appendFormat:@"\nSWCHB     $%02X  %s%s%s  left difficulty %c   right difficulty %c\n", io.swchb,
        (io.swchb & 0x01) ? "" : " RESET", (io.swchb & 0x02) ? "" : " SELECT", (io.swchb & 0x08) ? "" : " PAUSE",
        (io.swchb & 0x40) ? 'A' : 'B', (io.swchb & 0x80) ? 'A' : 'B'];
    [s appendString:@"INPT0-5  "];
    for (int i = 0; i < 6; i++) [s appendFormat:@" $%02X", io.inpt[i]];
    [s appendString:@"\n\n"];
    if (io.pokey_present) {
        for (int ch = 0; ch < 4; ch++)
            [s appendFormat:@"POKEY %d   AUDF $%02X   AUDC $%02X\n", ch, io.pokey_audf[ch], io.pokey_audc[ch]];
        [s appendFormat:@"AUDCTL    $%02X\n\n", io.pokey_audctl];
    } else {
        [s appendString:@"POKEY     none on this cartridge\n\n"];
    }
    for (int port = 0; port < 2; port++) {
        const char *type = a7800_ctrl_type_name(io.port_type[port]);
        [s appendFormat:@"Player %d  %s, held:", port + 1, type ? type : "?"];
        appendHeld(s, io.held[port]);
        [s appendString:@"\n"];
    }
    setTextKeepingPlace(_io, s);
}

- (void)refreshBps
{
    static a7800debug_breakpoint bps[MAX_BPS];
    const int n = a7800debug_breakpoint_list(_dbg, bps, MAX_BPS);
    for (NSView *v in [_bpList.arrangedSubviews copy]) [_bpList removeView:v];
    if (n == 0) [_bpList addArrangedSubview:[self label:@"No breakpoints or watchpoints"]];
    for (int i = 0; i < n; i++) {
        NSMutableString *what = [NSMutableString stringWithFormat:@"#%d  %s%s%s  $%04X", bps[i].id,
            (bps[i].type & A7800DEBUG_BP_EXEC) ? "x" : "", (bps[i].type & A7800DEBUG_BP_READ) ? "r" : "",
            (bps[i].type & A7800DEBUG_BP_WRITE) ? "w" : "", bps[i].start];
        if (bps[i].end != bps[i].start) [what appendFormat:@"-$%04X", bps[i].end];
        char label[64];
        if (a7800debug_address_label(_dbg, bps[i].start, label, sizeof label) > 0) [what appendFormat:@"  %s", label];
        if (bps[i].condition[0]) [what appendFormat:@"  if %s", bps[i].condition];
        NSButton *box = [NSButton checkboxWithTitle:what target:self action:@selector(bpEnabled:)];
        box.state = bps[i].enabled ? NSControlStateValueOn : NSControlStateValueOff;
        box.tag = bps[i].id;
        box.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
        NSButton *remove = [self button:@"Remove" action:@selector(bpRemove:)];
        remove.tag = bps[i].id;
        [_bpList addArrangedSubview:hstack(@[box, remove])];
    }
}

- (void)refreshCart
{
    static const char *const handovers[3] = { "none", "the BIOS", "the loader" };
    a7800debug_cart c;
    char info[256];
    a7800debug_cart_get(_dbg, &c);
    a7800debug_cart_info(_dbg, info, sizeof info);
    NSMutableString *s = [NSMutableString stringWithFormat:@"%@\n\n", stripControl(info)];
    if (!c.present) {
        [s appendString:@"(no cartridge running)\n"];
        setTextKeepingPlace(_cart, s);
        return;
    }
    [s appendFormat:@"Link        %s", c.link_up ? "up" : "down"];
    if (!c.link_up && c.link_error[0]) [s appendFormat:@": %s", c.link_error];
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
    setTextKeepingPlace(_cart, s);
}

- (void)refreshAll
{
    [self refreshStatus];
    [self refreshCpu];
    [self refreshMem];
    [self refreshDisasm];
    [self refreshMaria];
    [self refreshIo];
    [self refreshBps];
    [self refreshCart];
}
@end
