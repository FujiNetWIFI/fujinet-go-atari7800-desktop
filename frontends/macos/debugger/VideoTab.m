/*
 * The debugger's Video tab (AppKit).
 *
 * Everything that decides what to show lives in the core (a7800vidtab.h): this file is views.
 * The picture comes from the controller's cached 0xFFRRGGBB buffer and is drawn as a CGImage
 * (kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, no interpolation) over a copy of
 * that buffer; the image is rebuilt only when the controller re-rendered. A mouse move redraws
 * only the marks. Only the section showing is filled in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import "VideoTab.h"

#import "../KeyForward.h"

#include <stdio.h>
#include <string.h>

#include "a7800vidtab.h"

#define MAX_OVERLAYS 8
#define SWATCH_ROWS 32

/* A C string as an NSString, never nil (invalid UTF-8 reads as empty). */
static NSString *str(const char *text)
{
    NSString *s = text ? [NSString stringWithUTF8String:text] : nil;
    return s ? s : @"";
}

/* A 0xFFRRGGBB word as a colour. */
static NSColor *colorFromRGB(uint32_t rgb)
{
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 255) / 255.0
                               green:((rgb >> 8) & 255) / 255.0
                                blue:(rgb & 255) / 255.0
                               alpha:1.0];
}

static NSFont *monoFont11(void)
{
    return [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
}

/* A selectable monospace label that wraps at `width` (0: no limit). */
static NSTextField *monoLabel(CGFloat width)
{
    NSTextField *label = [NSTextField wrappingLabelWithString:@""];
    label.font = monoFont11();
    label.selectable = YES;
    if (width > 0) label.preferredMaxLayoutWidth = width;
    return label;
}

/* ---- the picture ------------------------------------------------------------------ */

@interface A7800VideoPicture : NSView
@property (nonatomic, assign) a7800vid_tab *tab;
@property (nonatomic, copy) void (^onChanged)(void);        /* the inspector changed */
@property (nonatomic, copy) void (^onBreak)(void);          /* a double-click: break on the pixel */
- (void)sync;                                               /* the controller re-rendered */
@end

@implementation A7800VideoPicture {
    CGImageRef _image;
    NSTrackingArea *_area;
    NSString *_message;
    NSSize _size;
}

- (BOOL)isFlipped { return YES; }

- (void)dealloc
{
    if (_image) CGImageRelease(_image);
}

- (NSSize)intrinsicContentSize { return _size.width > 0 ? _size : NSMakeSize(320, 240); }

- (void)sync
{
    int width = 0, height = 0;
    const uint32_t *pixels = a7800vid_tab_picture(_tab, &width, &height);
    if (_image) {
        CGImageRelease(_image);
        _image = NULL;
    }
    if (pixels && width > 0 && height > 0) {
        /* a copy: the controller's buffer lives only until its next refresh */
        CFDataRef data = CFDataCreate(NULL, (const UInt8 *)pixels, (CFIndex)width * height * 4);
        CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        _image = CGImageCreate((size_t)width, (size_t)height, 8, 32, (size_t)width * 4, space,
                               kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, provider, NULL, false,
                               kCGRenderingIntentDefault);
        CGColorSpaceRelease(space);
        CGDataProviderRelease(provider);
        CFRelease(data);
        _message = nil;
        _size = NSMakeSize(width, height);
    } else {
        const char *why = a7800vid_tab_why(_tab);
        _message = str(why ? why : "Nothing to show");
        _size = NSMakeSize(0, 0);
    }
    [self invalidateIntrinsicContentSize];
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    CGContextRef context = [[NSGraphicsContext currentContext] CGContext];
    [[NSColor colorWithCalibratedRed:0.07 green:0.07 blue:0.09 alpha:1] setFill];
    NSRectFill(self.bounds);
    if (!_image) {
        NSDictionary *attrs = @{ NSForegroundColorAttributeName: [NSColor colorWithCalibratedWhite:0.6 alpha:1],
                                 NSFontAttributeName: [NSFont systemFontOfSize:13] };
        NSString *message = _message ? _message : @"";
        const NSSize textSize = [message sizeWithAttributes:attrs];
        [message drawAtPoint:NSMakePoint((self.bounds.size.width - textSize.width) / 2,
                                         (self.bounds.size.height - textSize.height) / 2)
              withAttributes:attrs];
        return;
    }
    /* the view is flipped: draw the image upright */
    CGContextSaveGState(context);
    CGContextSetInterpolationQuality(context, kCGInterpolationNone);
    CGContextTranslateCTM(context, 0, _size.height);
    CGContextScaleCTM(context, 1, -1);
    CGContextDrawImage(context, CGRectMake(0, 0, _size.width, _size.height), _image);
    CGContextRestoreGState(context);
    int hover[4] = { 0 }, pin[4] = { 0 };
    const int marks = a7800vid_tab_marks(_tab, hover, pin);
    if (marks & 1) {
        [[NSColor colorWithCalibratedWhite:1 alpha:0.9] setStroke];
        NSBezierPath *path = [NSBezierPath bezierPathWithRect:NSMakeRect(hover[0] + 0.5, hover[1] + 0.5,
                                                                         hover[2] - 1, hover[3] - 1)];
        path.lineWidth = 1;
        [path stroke];
    }
    if (marks & 2) {
        [[NSColor colorWithCalibratedRed:1 green:0.85 blue:0.1 alpha:1] setStroke];
        NSBezierPath *path = [NSBezierPath bezierPathWithRect:NSMakeRect(pin[0] + 1, pin[1] + 1,
                                                                         pin[2] - 2, pin[3] - 2)];
        path.lineWidth = 2;
        [path stroke];
    }
}

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    if (_area) [self removeTrackingArea:_area];
    _area = [[NSTrackingArea alloc] initWithRect:self.bounds
                                         options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                                                 NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                           owner:self userInfo:nil];
    [self addTrackingArea:_area];
}

- (NSPoint)local:(NSEvent *)event { return [self convertPoint:event.locationInWindow fromView:nil]; }

- (void)mouseMoved:(NSEvent *)event
{
    const NSPoint point = [self local:event];
    if (a7800vid_tab_hover(_tab, (int)point.x, (int)point.y)) {
        [self setNeedsDisplay:YES];
        if (_onChanged) _onChanged();
    }
}

- (void)mouseExited:(NSEvent *)event
{
    (void)event;
    if (a7800vid_tab_hover(_tab, -1, -1)) {
        [self setNeedsDisplay:YES];
        if (_onChanged) _onChanged();
    }
}

- (void)mouseDown:(NSEvent *)event
{
    const NSPoint point = [self local:event];
    if (event.clickCount >= 2) {
        a7800vid_tab_hover(_tab, (int)point.x, (int)point.y);
        if (_onBreak) _onBreak();
    } else {
        a7800vid_tab_pin(_tab, (int)point.x, (int)point.y);
    }
    [self setNeedsDisplay:YES];
    if (_onChanged) _onChanged();
}
@end

/* ---- a colour swatch ---------------------------------------------------------------- */

@interface A7800SwatchView : NSView
@property (nonatomic, strong) NSColor *color;
@end

@implementation A7800SwatchView
- (NSSize)intrinsicContentSize { return NSMakeSize(22, 16); }

- (void)setColor:(NSColor *)color
{
    _color = color;
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    [(_color ? _color : NSColor.blackColor) setFill];
    NSRectFill(self.bounds);
    [[NSColor colorWithCalibratedWhite:0.5 alpha:1] setStroke];
    NSBezierPath *path = [NSBezierPath bezierPathWithRect:NSInsetRect(self.bounds, 0.5, 0.5)];
    path.lineWidth = 1;
    [path stroke];
}
@end

/* ---- the tab ------------------------------------------------------------------------------ */

@interface A7800VideoTab () <NSTableViewDataSource, NSTableViewDelegate, NSTabViewDelegate>
@end

@implementation A7800VideoTab {
    a7800vid_tab *_tab;
    NSView *_root;
    NSPopUpButton *_viewPopup, *_sourcePopup, *_zoomPopup;
    NSButton *_overlayBox[MAX_OVERLAYS];
    NSTextField *_status, *_note, *_pixelText, *_timingText;
    NSTextField *_swatchLabel[SWATCH_ROWS];
    A7800SwatchView *_swatchView[SWATCH_ROWS];
    A7800VideoPicture *_picture;
    NSScrollView *_scroll;
    NSTabView *_sections;
    NSTableView *_regsTable, *_linesTable;
    NSProgressIndicator *_beam;
    NSString *_regsSignature, *_linesSignature;
    BOOL _updating;
    int _regCount, _lineCount;
    int _viewportWidth, _viewportHeight;
    id _resizeObserver;
}

- (NSView *)view { return _root; }

- (instancetype)initWithSession:(a7800session *)session
{
    self = [super init];
    if (!self) return nil;
    _tab = a7800vid_tab_new(session);
    [self build];
    return self;
}

- (void)dealloc
{
    if (_resizeObserver) [[NSNotificationCenter defaultCenter] removeObserver:_resizeObserver];
    if (_tab) a7800vid_tab_free(_tab);
    _tab = NULL;
}

- (void)build
{
    int overlayCount = 0;
    const a7800vid_overlay_info *overlays = a7800vid_overlays(&overlayCount);
    if (overlayCount > MAX_OVERLAYS) overlayCount = MAX_OVERLAYS;
    int zoomCount = 0;
    const int *zooms = a7800vid_zooms(&zoomCount);

    _viewPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int i = 0; i < A7800VID_V_COUNT; i++) [_viewPopup addItemWithTitle:str(a7800vid_view_name(i))];
    _viewPopup.target = self;
    _viewPopup.action = @selector(onView:);
    _viewPopup.toolTip = @"Display: the last finished frame, as the TV showed it.\n"
                          "Decoded: the display list's objects drawn over the frame (see Source).\n"
                          "Beam: this frame above the beam, the last one dimmed below.\n"
                          "Values: the raw palette and colour numbers (cool = palettes 0-3, warm = 4-7).";
    _sourcePopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int i = 0; i < A7800VID_D_COUNT; i++) [_sourcePopup addItemWithTitle:str(a7800vid_source_name(i))];
    _sourcePopup.target = self;
    _sourcePopup.action = @selector(onSource:);
    _sourcePopup.toolTip = @"What the Decoded view shows";
    _zoomPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int i = 0; i < zoomCount; i++)
        [_zoomPopup addItemWithTitle:zooms[i] == 0 ? @"Fit" : [NSString stringWithFormat:@"%d×", zooms[i]]];
    _zoomPopup.target = self;
    _zoomPopup.action = @selector(onZoom:);
    NSButton *breakButton = [NSButton buttonWithTitle:@"Break on writes" target:self action:@selector(onBreak:)];
    breakButton.toolTip = @"Add write breakpoints on the inspected pixel's object headers, its zone's DLL entry and its palette register";
    NSStackView *bar = [NSStackView stackViewWithViews:@[[NSTextField labelWithString:@"View"], _viewPopup,
                                                         [NSTextField labelWithString:@"Source"], _sourcePopup,
                                                         [NSTextField labelWithString:@"Zoom"], _zoomPopup,
                                                         breakButton]];
    bar.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    bar.spacing = 8;

    NSMutableArray<NSView *> *row1 = [NSMutableArray array], *row2 = [NSMutableArray array];
    for (int i = 0; i < overlayCount; i++) {
        _overlayBox[i] = [NSButton checkboxWithTitle:str(overlays[i].label) target:self action:@selector(onOverlay:)];
        [(i < 4 ? row1 : row2) addObject:_overlayBox[i]];
    }
    NSStackView *overlayRow1 = [NSStackView stackViewWithViews:row1];
    NSStackView *overlayRow2 = [NSStackView stackViewWithViews:row2];
    overlayRow1.orientation = overlayRow2.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    overlayRow1.spacing = overlayRow2.spacing = 10;

    _status = monoLabel(0);
    _picture = [[A7800VideoPicture alloc] initWithFrame:NSMakeRect(0, 0, 320, 240)];
    _picture.tab = _tab;
    __weak A7800VideoTab *weak = self;
    _picture.onChanged = ^{ [weak updatePixel]; };
    _picture.onBreak = ^{ [weak breakOnPixel]; };
    _scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 640, 400)];
    _scroll.hasVerticalScroller = YES;
    _scroll.hasHorizontalScroller = YES;
    _scroll.documentView = _picture;
    _scroll.drawsBackground = NO;
    [_scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    [_scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    _note = [NSTextField labelWithString:@""];

    NSStackView *left = [NSStackView stackViewWithViews:@[bar, overlayRow1, overlayRow2, _status, _scroll, _note]];
    left.orientation = NSUserInterfaceLayoutOrientationVertical;
    left.alignment = NSLayoutAttributeLeading;
    left.spacing = 6;
    [_scroll.widthAnchor constraintEqualToAnchor:left.widthAnchor].active = YES;

    /* Fit follows the scroll view's size */
    _scroll.postsFrameChangedNotifications = YES;
    _resizeObserver = [[NSNotificationCenter defaultCenter]
        addObserverForName:NSViewFrameDidChangeNotification
                    object:_scroll
                     queue:nil
                usingBlock:^(NSNotification *note) {
        (void)note;
        [weak viewportChanged];
    }];

    /* ---- the sections, in a7800vid_section_name order */
    _sections = [[NSTabView alloc] init];
    _sections.delegate = self;
    {   /* Pixel */
        _pixelText = monoLabel(370);
        NSButton *pixelBreak = [NSButton buttonWithTitle:@"Break on writes" target:self action:@selector(onBreak:)];
        NSButton *unpin = [NSButton buttonWithTitle:@"Unpin" target:self action:@selector(onUnpin:)];
        NSStackView *buttons = [NSStackView stackViewWithViews:@[pixelBreak, unpin]];
        buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        buttons.spacing = 6;
        NSStackView *page = [NSStackView stackViewWithViews:@[_pixelText, buttons]];
        page.orientation = NSUserInterfaceLayoutOrientationVertical;
        page.alignment = NSLayoutAttributeLeading;
        page.spacing = 8;
        [self addSection:@"Pixel" content:page];
    }
    {   /* Registers */
        _regsTable = [[NSTableView alloc] init];
        [self addSection:@"Registers" content:[self scrollForTable:_regsTable
                                                           columns:@[@"name", @"val", @"meaning"]
                                                            titles:@[@"Register", @"Value", @"Meaning"]
                                                            widths:@[@70, @50, @400]]];
    }
    {   /* Colours: the 32 palette registers, each with a write breakpoint */
        NSMutableArray<NSView *> *rows = [NSMutableArray array];
        for (int i = 0; i < SWATCH_ROWS; i++) {
            _swatchView[i] = [[A7800SwatchView alloc] initWithFrame:NSMakeRect(0, 0, 22, 16)];
            _swatchLabel[i] = monoLabel(0);
            _swatchLabel[i].selectable = NO;
            NSButton *rowBreak = [NSButton buttonWithTitle:@"Break on write" target:self
                                                    action:@selector(onSwatchBreak:)];
            rowBreak.tag = i;
            rowBreak.controlSize = NSControlSizeSmall;
            rowBreak.font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
            NSStackView *row = [NSStackView stackViewWithViews:@[_swatchView[i], _swatchLabel[i], rowBreak]];
            row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
            row.spacing = 8;
            [rows addObject:row];
        }
        NSStackView *page = [NSStackView stackViewWithViews:rows];
        page.orientation = NSUserInterfaceLayoutOrientationVertical;
        page.alignment = NSLayoutAttributeLeading;
        page.spacing = 4;
        page.translatesAutoresizingMaskIntoConstraints = NO;
        NSScrollView *colours = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
        colours.hasVerticalScroller = YES;
        colours.drawsBackground = NO;
        colours.documentView = page;
        NSClipView *clip = colours.contentView;
        [page.topAnchor constraintEqualToAnchor:clip.topAnchor].active = YES;
        [page.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor].active = YES;
        [self addSection:@"Colours" content:colours];
    }
    {   /* Lines */
        _linesTable = [[NSTableView alloc] init];
        [self addSection:@"Lines" content:[self scrollForTable:_linesTable
                                                       columns:@[@"row", @"line", @"ctrl", @"dpp", @"dll", @"dl", @"objects", @"cycles", @"bg", @"notes"]
                                                        titles:@[@"Row", @"Line", @"CTRL", @"DPP", @"DLL", @"DL", @"Obj", @"Cyc", @"BG", @"Notes"]
                                                        widths:@[@36, @40, @40, @50, @50, @50, @34, @38, @34, @70]]];
    }
    {   /* Timing */
        _beam = [[NSProgressIndicator alloc] init];
        _beam.indeterminate = NO;
        _beam.minValue = 0;
        _beam.maxValue = 1000;
        _timingText = monoLabel(370);
        NSStackView *page = [NSStackView stackViewWithViews:@[_beam, _timingText]];
        page.orientation = NSUserInterfaceLayoutOrientationVertical;
        page.alignment = NSLayoutAttributeLeading;
        page.spacing = 8;
        [_beam.widthAnchor constraintEqualToAnchor:page.widthAnchor].active = YES;
        [self addSection:@"Timing" content:page];
    }
    [_sections.widthAnchor constraintEqualToConstant:520].active = YES;

    NSStackView *root = [NSStackView stackViewWithViews:@[left, _sections]];
    root.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    root.alignment = NSLayoutAttributeTop;
    root.spacing = 10;
    _root = root;
    [left setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];

    /* start from the controller's state (the A7800_DEBUGGER_VIDEO_* variables) */
    _updating = YES;
    [_viewPopup selectItemAtIndex:a7800vid_tab_view(_tab)];
    [_sourcePopup selectItemAtIndex:a7800vid_tab_source(_tab)];
    _sourcePopup.enabled = a7800vid_tab_view(_tab) == A7800VID_V_DECODED;
    for (int i = 0; i < zoomCount; i++)
        if (zooms[i] == a7800vid_tab_zoom(_tab)) [_zoomPopup selectItemAtIndex:i];
    for (int i = 0; i < overlayCount; i++)
        _overlayBox[i].state = (a7800vid_tab_overlays(_tab) & overlays[i].bit) ? NSControlStateValueOn
                                                                             : NSControlStateValueOff;
    const int section = a7800vid_tab_env_section(_tab);
    if (section > 0 && section < (int)_sections.numberOfTabViewItems) [_sections selectTabViewItemAtIndex:section];
    _updating = NO;
}

/* Sets a table up and returns the scroll view that holds it. */
- (NSScrollView *)scrollForTable:(NSTableView *)table
                         columns:(NSArray<NSString *> *)ids
                          titles:(NSArray<NSString *> *)titles
                          widths:(NSArray<NSNumber *> *)widths
{
    for (NSUInteger i = 0; i < ids.count; i++) {
        NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:ids[i]];
        column.title = titles[i];
        column.width = widths[i].doubleValue;
        [table addTableColumn:column];
    }
    table.dataSource = self;
    table.delegate = self;
    table.usesAlternatingRowBackgroundColors = YES;
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    scroll.hasVerticalScroller = YES;
    scroll.documentView = table;
    return scroll;
}

- (void)addSection:(NSString *)title content:(NSView *)content
{
    NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:title];
    item.label = title;
    NSView *holder = [[NSView alloc] init];
    [holder addSubview:content];
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:6].active = YES;
    [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-6].active = YES;
    [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:6].active = YES;
    if ([content isKindOfClass:[NSScrollView class]])
        [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-6].active = YES;
    item.view = holder;
    [_sections addTabViewItem:item];
}

/* ---- actions -------------------------------------------------------------------------- */

- (void)onView:(id)sender
{
    (void)sender;
    if (_updating) return;
    a7800vid_tab_set_view(_tab, (int)_viewPopup.indexOfSelectedItem);
    _sourcePopup.enabled = a7800vid_tab_view(_tab) == A7800VID_V_DECODED;
    [self refreshView:A7800VID_TAB_PICTURE | A7800VID_TAB_TABLES];
}

- (void)onSource:(id)sender
{
    (void)sender;
    if (_updating) return;
    a7800vid_tab_set_source(_tab, (int)_sourcePopup.indexOfSelectedItem);
    [self refreshView:A7800VID_TAB_PICTURE | A7800VID_TAB_TABLES];
}

- (void)onZoom:(id)sender
{
    (void)sender;
    int zoomCount = 0;
    const int *zooms = a7800vid_zooms(&zoomCount);
    const NSInteger index = _zoomPopup.indexOfSelectedItem;
    if (_updating || index < 0 || index >= zoomCount) return;
    a7800vid_tab_set_zoom(_tab, zooms[index]);
    [self refreshView:A7800VID_TAB_PICTURE];
}

- (void)onOverlay:(id)sender
{
    (void)sender;
    if (_updating) return;
    int overlayCount = 0;
    const a7800vid_overlay_info *overlays = a7800vid_overlays(&overlayCount);
    if (overlayCount > MAX_OVERLAYS) overlayCount = MAX_OVERLAYS;
    unsigned mask = 0;
    for (int i = 0; i < overlayCount; i++)
        if (_overlayBox[i].state == NSControlStateValueOn) mask |= overlays[i].bit;
    a7800vid_tab_set_overlays(_tab, mask);
    [self refreshView:A7800VID_TAB_PICTURE];
}

/* The pixel's write breakpoints (its objects' headers, its zone's DLL entry and its palette register), with a note of what happened. */
- (void)breakOnPixel
{
    const int ident = a7800vid_tab_break_on_pixel(_tab);
    _note.stringValue = ident >= 0 ? @"Break on writes to the pixel's display list objects, zone and palette register (see Breakpoints)"
                                   : @"Nothing to break on";
}

- (void)onBreak:(id)sender
{
    (void)sender;
    [self breakOnPixel];
}

- (void)onSwatchBreak:(id)sender
{
    const int index = (int)((NSButton *)sender).tag;
    const a7800vid_swatch *swatches = NULL;
    const int count = a7800vid_tab_swatches(_tab, &swatches);
    const int ident = a7800vid_tab_break_on_swatch(_tab, index);
    if (ident >= 0 && swatches && index >= 0 && index < count)
        _note.stringValue = [NSString stringWithFormat:@"Break on writes to %@ (see Breakpoints)",
                                                       str(swatches[index].name)];
    else
        _note.stringValue = @"Nothing to break on";
}

- (void)onUnpin:(id)sender
{
    (void)sender;
    a7800vid_tab_unpin(_tab);
    [_picture setNeedsDisplay:YES];
    [self updatePixel];
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    (void)item;
    if (!_updating) [self refreshView:A7800VID_TAB_TABLES];
}

/* The picture's room changed: Fit needs a new picture. */
- (void)viewportChanged
{
    const NSSize viewport = _scroll.contentSize;
    if ((int)viewport.width == _viewportWidth && (int)viewport.height == _viewportHeight) return;
    if (a7800vid_tab_zoom(_tab) == 0) {
        [self refreshView:A7800VID_TAB_PICTURE];
    } else {
        _viewportWidth = (int)viewport.width;
        _viewportHeight = (int)viewport.height;
        a7800vid_tab_set_viewport(_tab, _viewportWidth > 2 ? _viewportWidth - 2 : 1,
                                _viewportHeight > 2 ? _viewportHeight - 2 : 1);
    }
}

/* ---- the sections ----------------------------------------------------------------------- */

- (void)updatePixel
{
    const a7800vid_pixel *pixel = a7800vid_tab_pixel(_tab);
    _pixelText.stringValue = pixel ? str(pixel->text)
                                   : @"Move over the picture to inspect a pixel.\nClick to pin it; click again to unpin.\n"
                                      "Double-click to break on writes to it.";
    /* the swatch rows mark the inspected colour */
    if ([self sectionIndex] == A7800VID_S_COLOURS) [self fillColours];
}

- (NSInteger)sectionIndex
{
    return (NSInteger)[_sections.tabViewItems indexOfObject:_sections.selectedTabViewItem];
}

static NSString *changeNotes(int mask)
{
    NSMutableString *text = [NSMutableString string];
    if (mask & A7800VID_LC_PALETTE) [text appendString:@"P "];
    if (mask & A7800VID_LC_CTRL) [text appendString:@"C "];
    if (mask & A7800VID_LC_DPP) [text appendString:@"D "];
    if (mask & A7800VID_LC_ZONE) [text appendString:@"Z "];
    return text;
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)table
{
    return table == _linesTable ? _lineCount : _regCount;
}

- (NSView *)tableView:(NSTableView *)table viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row
{
    NSString *text = @"";
    BOOL hot = NO;
    if (table == _regsTable) {
        const a7800vid_reg *regs = NULL;
        const int count = a7800vid_tab_regs(_tab, &regs);
        if (!regs || row < 0 || row >= count) return nil;
        if ([column.identifier isEqualToString:@"name"]) text = str(regs[row].name);
        else if ([column.identifier isEqualToString:@"val"]) text = str(regs[row].value);
        else text = str(regs[row].meaning);
        hot = regs[row].age < A7800VID_REG_FADE;
    } else {
        const a7800vid_line_row *lines = NULL;
        const int count = a7800vid_tab_lines(_tab, &lines);
        if (!lines || row < 0 || row >= count) return nil;
        NSString *identifier = column.identifier;
        if ([identifier isEqualToString:@"row"]) text = [NSString stringWithFormat:@"%d", lines[row].row];
        else if ([identifier isEqualToString:@"line"]) text = [NSString stringWithFormat:@"%d", lines[row].scanline];
        else if ([identifier isEqualToString:@"ctrl"]) text = [NSString stringWithFormat:@"%02X", lines[row].ctrl];
        else if ([identifier isEqualToString:@"dpp"]) text = [NSString stringWithFormat:@"%04X", lines[row].dpp];
        else if ([identifier isEqualToString:@"dll"]) text = [NSString stringWithFormat:@"%04X", lines[row].dll];
        else if ([identifier isEqualToString:@"dl"]) text = [NSString stringWithFormat:@"%04X", lines[row].dl];
        else if ([identifier isEqualToString:@"objects"]) text = [NSString stringWithFormat:@"%d", lines[row].objects];
        else if ([identifier isEqualToString:@"cycles"]) text = [NSString stringWithFormat:@"%d", lines[row].cycles];
        else if ([identifier isEqualToString:@"bg"]) text = [NSString stringWithFormat:@"%02X", lines[row].background];
        else text = [NSString stringWithFormat:@"%@%@%@", lines[row].holey ? @"holey " : @"", lines[row].dli ? @"dli " : @"",
                                               changeNotes(lines[row].changed_mask)];
        hot = lines[row].changed_mask != 0;
    }
    NSTextField *field = [table makeViewWithIdentifier:column.identifier owner:self];
    if (![field isKindOfClass:[NSTextField class]]) {
        field = [NSTextField labelWithString:@""];
        field.identifier = column.identifier;
        field.font = monoFont11();
    }
    field.stringValue = text;
    field.drawsBackground = hot;
    field.backgroundColor = hot ? A7800AccentColor() : NSColor.clearColor;
    field.textColor = hot ? NSColor.whiteColor : NSColor.labelColor;
    return field;
}

- (void)fillRegs
{
    const a7800vid_reg *regs = NULL;
    const int count = a7800vid_tab_regs(_tab, &regs);
    NSMutableString *signature = [NSMutableString string];
    for (int i = 0; regs && i < count; i++)
        [signature appendFormat:@"%s%c", regs[i].value, regs[i].age < A7800VID_REG_FADE ? '*' : '.'];
    [signature appendFormat:@"%d", count];
    if ([signature isEqualToString:_regsSignature]) return;
    _regsSignature = signature;
    _regCount = regs ? count : 0;
    [_regsTable reloadData];
}

- (void)fillLines
{
    const a7800vid_line_row *lines = NULL;
    const int count = a7800vid_tab_lines(_tab, &lines);
    NSMutableString *signature = [NSMutableString string];
    for (int i = 0; lines && i < count; i++) {
        [signature appendFormat:@"%d:%d:%02X%04X%04X%04X%d%d%d%d%02X%d;", lines[i].row, lines[i].scanline, lines[i].ctrl,
                                lines[i].dpp, lines[i].dll, lines[i].dl, lines[i].objects, lines[i].cycles,
                                lines[i].holey, lines[i].dli, lines[i].background, lines[i].changed_mask];
    }
    [signature appendFormat:@"%d", count];
    if ([signature isEqualToString:_linesSignature]) return;
    _linesSignature = signature;
    _lineCount = lines ? count : 0;
    [_linesTable reloadData];
}

- (void)fillColours
{
    const a7800vid_swatch *swatches = NULL;
    const int count = a7800vid_tab_swatches(_tab, &swatches);
    for (int i = 0; i < SWATCH_ROWS; i++) {
        if (!swatches || i >= count) {
            _swatchLabel[i].stringValue = @"";
            _swatchView[i].color = nil;
            continue;
        }
        _swatchView[i].color = colorFromRGB(swatches[i].rgb);
        _swatchLabel[i].stringValue = [NSString stringWithFormat:@"%@%-8s $%02X = $%02X  #%06X  %ld pixels",
                                                                 swatches[i].inspected ? @"> " : @"  ",
                                                                 swatches[i].name, swatches[i].address,
                                                                 swatches[i].value, swatches[i].rgb & 0xFFFFFF,
                                                                 swatches[i].uses];
    }
}

- (void)fillTiming
{
    const char *const *lines = NULL;
    const int count = a7800vid_tab_timing(_tab, &lines);
    NSMutableString *text = [NSMutableString string];
    for (int i = 0; lines && i < count; i++) {
        NSString *line = str(lines[i]);
        const NSRange tab = [line rangeOfString:@"\t"];
        if (tab.location != NSNotFound)
            [text appendFormat:@"%-13s %@\n", [[line substringToIndex:tab.location] UTF8String],
                               [line substringFromIndex:tab.location + 1]];
        else
            [text appendFormat:@"%@\n", line];
    }
    _timingText.stringValue = count ? text : @"The machine is off";
    _beam.doubleValue = a7800vid_tab_beam_permille(_tab);
}

/* ---- refresh ------------------------------------------------------------------------------ */

- (void)refreshView:(unsigned)what
{
    if (what & A7800VID_TAB_PICTURE) {
        const NSSize viewport = _scroll.contentSize;
        _viewportWidth = (int)viewport.width;
        _viewportHeight = (int)viewport.height;
        if (viewport.width > 4 && viewport.height > 4)
            a7800vid_tab_set_viewport(_tab, (int)viewport.width - 2, (int)viewport.height - 2);
        [_picture sync];
        [_picture setFrameSize:_picture.intrinsicContentSize];
    }
    if (what & A7800VID_TAB_TABLES) {
        _status.stringValue = str(a7800vid_tab_status(_tab));
        switch ([self sectionIndex]) {
        case A7800VID_S_REGISTERS: [self fillRegs]; break;
        case A7800VID_S_COLOURS: [self fillColours]; break;
        case A7800VID_S_LINES: [self fillLines]; break;
        case A7800VID_S_TIMING: [self fillTiming]; break;
        default: break;
        }
        const a7800vid_pixel *pixel = a7800vid_tab_pixel(_tab);
        _pixelText.stringValue = pixel ? str(pixel->text)
                                       : @"Move over the picture to inspect a pixel.\nClick to pin it; click again to unpin.\n"
                                          "Double-click to break on writes to it.";
    }
}

- (void)refreshForce:(BOOL)force
{
    unsigned what = a7800vid_tab_refresh(_tab, force ? 1 : 0);
    if (force) what |= A7800VID_TAB_PICTURE | A7800VID_TAB_TABLES;
    if (what & A7800VID_TAB_OFF) what |= A7800VID_TAB_PICTURE | A7800VID_TAB_TABLES;     /* the message */
    [self refreshView:what];
}

@end
