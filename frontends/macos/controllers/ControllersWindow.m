/*
 * A7800ControllersWindow -- the AppKit Controllers panel: the two ports'
 * controllers, each drawn by what the port has (the 7800's ProLine two-button
 * joystick, a 2600 CX40, the XG-1 light gun, or an empty socket), over the
 * console's strip (the silver band and rainbow, PAUSE / SELECT / RESET, POWER
 * dimmed, the two A / B difficulty slides), all painted from the session's
 * layout (a7800session_controls_layout) so every frontend shows the same
 * thing. Then the controller each port has, the gamepads with the player each
 * drives, and Map mode for rebinding. The art follows
 * ~/Workspace/fujinet-go-all/controllers-port/DESIGN-7800.md (colours are the
 * A7800SESSION_*_RGB defines, read from photos of the real hardware).
 *
 * Gestures: press a red button or a console key and it is held until it is
 * released or dragged off; press in a stick's dish and drag to steer it (the
 * eight 45-degree sectors, a7800session_pointer_set; it lets go on release);
 * click a difficulty slide to flip it (it latches). The pointer is its own
 * input source, so the panel lets go of everything the mouse holds when it is
 * hidden or loses key status (a7800session_release_pointer); what the console
 * sees held from any source (the host keyboard, a gamepad, the mouse) is
 * painted in the held colour, so the panel doubles as an input tester for a new
 * gamepad.
 *
 * Keys typed with the panel key go to the console as in the main window; no
 * view here takes the keyboard, so the panel itself is the first responder.
 *
 * FIXED SIZE: an NSPanel without a resize mask, sized from its contents. Only
 * more pads than the two rows kept for them change its height (the top edge
 * stays put). Hidden at start, ordered out rather than closed, so its position
 * survives. The 30 Hz poll runs only while the panel can be seen.
 *
 * MAP MODE: Map arms a two-step rebind -- click a control on the picture (a
 * stick direction or its label, a red button, a console key, a slide) or choose
 * one from the list (which also holds the session's own actions); every
 * control other than the pick dims to 40%. Then press a key or a gamepad
 * button; the next one is bound and whatever held it is told so in the hint
 * line. Clear unbinds the pick's key and pad button. Esc backs out of the
 * pick. F9, F11 and F12 are the app's and never bound.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "ControllersWindow.h"

#import "../KeyForward.h"
#import "../debugger/DebuggerWindow.h"
#import "../help/HelpWindow.h"

#include <math.h>
#include <string.h>

#define UNIT         22.0     /* points per layout unit */
#define MIN_WIN_W    640.0    /* the rows under the picture need this much */
#define MARGIN       16.0
#define STAGE_PAD    10.0
#define TITLE_H      18.0
#define ROW_H        26.0
#define EFF_W       130.0
#define POPUP_W     130.0
#define TYPE_W      230.0
#define BTN_H        28.0
#define MIN_PAD_ROWS  2       /* rows kept for pads, more when more are connected */
#define MAX_PAD_ROWS  4
#define SLOW_TICKS    8       /* the 30 Hz tick's 250 ms part */
#define PILL_H       16.0
#define MAX_PILLS   128
#define DIM_ALPHA    0.4      /* controls that are not the Map pick */

/* ---- colours: the shared A7800SESSION_*_RGB, and the stage behind them ---------- */

#define COL_STAGE_TOP   0x6B6C72
#define COL_STAGE_BOT   0x55565B
#define COL_STAGE_EDGE  0x3A3B3F
#define COL_CAPTION     0xEEEEF2
#define COL_PILL        0x1A1A1C
/* the XG-1's warm grey plastic and its dark trigger (Atari_XG-1_light_gun.jpg) */
#define COL_GUN         0xBDB9B0
#define COL_GUN_HI      0xD0CCC4
#define COL_GUN_LO      0xA29E96
#define COL_GUN_TRIGGER 0x6A6A70

/* ---- helpers -------------------------------------------------------------------- */

static NSString *nsstr(const char *text)
{
    NSString *res = text ? [NSString stringWithUTF8String:text] : nil;
    return res ? res : @"";
}

static NSString *keyName(uint32_t keysym)
{
    char name[48];
    name[0] = '\0';
    if (keysym) a7800session_keysym_name(keysym, name, (int)sizeof name);
    return nsstr(name);
}

static NSColor *rgb(uint32_t value)
{
    return [NSColor colorWithSRGBRed:((value >> 16) & 0xff) / 255.0
                               green:((value >> 8) & 0xff) / 255.0
                                blue:(value & 0xff) / 255.0
                               alpha:1.0];
}

static NSColor *rgba(uint32_t value, CGFloat alpha)
{
    return [NSColor colorWithSRGBRed:((value >> 16) & 0xff) / 255.0
                               green:((value >> 8) & 0xff) / 255.0
                                blue:(value & 0xff) / 255.0
                               alpha:alpha];
}

static uint32_t mixKey(uint32_t from, uint32_t to, double frac)
{
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        const double ca = (double)((from >> shift) & 0xff), cb = (double)((to >> shift) & 0xff);
        out |= (uint32_t)lround(ca + (cb - ca) * frac) << shift;
    }
    return out;
}

/* Toggled-on look for a push button: accent bezel, white title. */
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
                    NSFontAttributeName : button.font ?: [NSFont systemFontOfSize:NSFont.systemFontSize],
                    NSForegroundColorAttributeName : NSColor.whiteColor,
                    NSParagraphStyleAttributeName : para
                }];
    } else {
        button.bezelColor = nil;
        button.title = title;
    }
}

/* A linear gradient clipped to a path, from a to b. */
static void fillLinear(NSBezierPath *path, NSPoint from, NSPoint to, uint32_t rgbA, uint32_t rgbB)
{
    const CGFloat comps[8] = {
        ((rgbA >> 16) & 0xff) / 255.0, ((rgbA >> 8) & 0xff) / 255.0, (rgbA & 0xff) / 255.0, 1.0,
        ((rgbB >> 16) & 0xff) / 255.0, ((rgbB >> 8) & 0xff) / 255.0, (rgbB & 0xff) / 255.0, 1.0 };
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGGradientRef grad = CGGradientCreateWithColorComponents(space, comps, NULL, 2);
    [NSGraphicsContext saveGraphicsState];
    [path addClip];
    CGContextDrawLinearGradient(ctx, grad, CGPointMake(from.x, from.y), CGPointMake(to.x, to.y),
                                kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation);
    [NSGraphicsContext restoreGraphicsState];
    CGGradientRelease(grad);
    CGColorSpaceRelease(space);
}

/* A vertical gradient clipped to a path (the views here are flipped, so "top"
 * is the smaller y). */
static void fillGradient(NSBezierPath *path, uint32_t topRgb, uint32_t botRgb)
{
    const NSRect bnd = [path bounds];
    fillLinear(path, NSMakePoint(NSMinX(bnd), NSMinY(bnd)), NSMakePoint(NSMinX(bnd), NSMaxY(bnd)), topRgb, botRgb);
}

/* A horizontal gradient clipped to a path. */
static void fillGradientH(NSBezierPath *path, uint32_t leftRgb, uint32_t rightRgb)
{
    const NSRect bnd = [path bounds];
    fillLinear(path, NSMakePoint(NSMinX(bnd), NSMinY(bnd)), NSMakePoint(NSMaxX(bnd), NSMinY(bnd)), leftRgb, rightRgb);
}

/* Text centred in rect, the font shrunk (to 5 pt) until it fits. */
static void drawFittedStyled(NSString *text, NSRect rect, CGFloat size, BOOL bold, BOOL oblique, NSColor *ink)
{
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    para.lineBreakMode = NSLineBreakByClipping;
    NSDictionary *attrs = nil;
    CGFloat lineHeight = 0;
    for (;;) {
        NSFont *font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
        NSMutableDictionary *dict = [NSMutableDictionary dictionaryWithDictionary:@{
            NSFontAttributeName: font,
            NSParagraphStyleAttributeName: para,
            NSForegroundColorAttributeName: ink }];
        if (oblique) dict[NSObliquenessAttributeName] = @(0.2);
        attrs = dict;
        const NSSize extent = [text sizeWithAttributes:attrs];
        lineHeight = extent.height;
        if ((extent.width <= rect.size.width && lineHeight <= rect.size.height + 2) || size <= 5) break;
        size -= 0.5;
    }
    const CGFloat ypos = rect.origin.y + floor((rect.size.height - lineHeight) / 2);
    [text drawInRect:NSMakeRect(rect.origin.x, ypos, rect.size.width, lineHeight) withAttributes:attrs];
}

static void drawFitted(NSString *text, NSRect rect, CGFloat size, BOOL bold, NSColor *ink)
{
    drawFittedStyled(text, rect, size, bold, NO, ink);
}

static void fillOval(NSRect rect, NSColor *colour)
{
    [colour setFill];
    [[NSBezierPath bezierPathWithOvalInRect:rect] fill];
}

static void strokeLine(NSPoint from, NSPoint to, CGFloat width, NSColor *colour)
{
    NSBezierPath *line = [NSBezierPath bezierPath];
    [line moveToPoint:from];
    [line lineToPoint:to];
    line.lineWidth = width;
    line.lineCapStyle = NSLineCapStyleRound;
    [colour setStroke];
    [line stroke];
}

static NSRect discRect(NSPoint ctr, CGFloat rad)
{
    return NSMakeRect(ctr.x - rad, ctr.y - rad, 2 * rad, 2 * rad);
}

/* A point of a part at (ux, uy) layout units from the part's own top-left. */
static NSPoint unitPoint(NSRect rect, double ux, double uy)
{
    return NSMakePoint(NSMinX(rect) + ux * UNIT, NSMinY(rect) + uy * UNIT);
}

/* A rectangle (ux, uy, uw, uh) layout units inside a part. */
static NSRect unitRect(NSRect rect, double ux, double uy, double uw, double uh)
{
    return NSMakeRect(NSMinX(rect) + ux * UNIT, NSMinY(rect) + uy * UNIT, uw * UNIT, uh * UNIT);
}

/* A regular polygon (num sides, radius rad), the first vertex at phase degrees. */
static NSBezierPath *polygon(NSPoint ctr, CGFloat rad, int num, double phase)
{
    NSBezierPath *path = [NSBezierPath bezierPath];
    for (int idx = 0; idx < num; idx++) {
        const double ang = (phase + idx * 360.0 / num) * M_PI / 180.0;
        const NSPoint pnt = NSMakePoint(ctr.x + rad * cos(ang), ctr.y + rad * sin(ang));
        if (idx == 0) [path moveToPoint:pnt];
        else [path lineToPoint:pnt];
    }
    [path closePath];
    return path;
}

/* A closed path through normalised points (0..1 of a square), scaled into box. */
static NSBezierPath *scaledPolygon(NSRect box, const double (*pts)[2], int num)
{
    NSBezierPath *path = [NSBezierPath bezierPath];
    for (int idx = 0; idx < num; idx++) {
        const NSPoint pnt = NSMakePoint(NSMinX(box) + pts[idx][0] * NSWidth(box),
                                        NSMinY(box) + pts[idx][1] * NSHeight(box));
        if (idx == 0) [path moveToPoint:pnt];
        else [path lineToPoint:pnt];
    }
    [path closePath];
    path.lineJoinStyle = NSLineJoinStyleRound;
    return path;
}

/* The direction an action bit points in, as the screen angle (degrees, +y down). */
static double directionAngle(int act)
{
    switch (act) {
    case A7800_ACT_RIGHT: return 0.0;
    case A7800_ACT_DOWN:  return 90.0;
    case A7800_ACT_LEFT:  return 180.0;
    default:              return 270.0;   /* up */
    }
}

/* The deflection (-1..1 each way) a mask of held directions makes. */
static void deflection(unsigned mask, double *defx, double *defy)
{
    double dx = (int)((mask >> A7800_ACT_RIGHT) & 1u) - (int)((mask >> A7800_ACT_LEFT) & 1u);
    double dy = (int)((mask >> A7800_ACT_DOWN) & 1u) - (int)((mask >> A7800_ACT_UP) & 1u);
    if (dx != 0 && dy != 0) { dx *= M_SQRT1_2; dy *= M_SQRT1_2; }
    *defx = dx;
    *defy = dy;
}

/* The binding target a Map click on a stick at (dx, dy) from its centre
 * means: the direction's, a diagonal going to its stronger axis; -1 inside the
 * dead centre. */
static int directionTarget(int port, float dx, float dy, float radius)
{
    const unsigned mask = a7800session_stick_from_vector(dx, dy, radius);
    if (!mask) return -1;
    int act;
    if (mask & (mask - 1u)) act = fabsf(dx) >= fabsf(dy) ? (dx < 0 ? A7800_ACT_LEFT : A7800_ACT_RIGHT)
                                                         : (dy < 0 ? A7800_ACT_UP : A7800_ACT_DOWN);
    else if (mask & (1u << A7800_ACT_UP)) act = A7800_ACT_UP;
    else if (mask & (1u << A7800_ACT_DOWN)) act = A7800_ACT_DOWN;
    else if (mask & (1u << A7800_ACT_LEFT)) act = A7800_ACT_LEFT;
    else act = A7800_ACT_RIGHT;
    return A7800_TARGET_PORT(port, act);
}

/* A label on a dark pill, filled with the accent when picked. */
static void drawPill(NSRect rect, NSString *text, BOOL picked)
{
    NSBezierPath *pill = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:rect.size.height / 2
                                                         yRadius:rect.size.height / 2];
    [(picked ? A7800AccentColor() : rgba(COL_PILL, 0.88)) setFill];
    [pill fill];
    drawFitted(text, NSInsetRect(rect, 4, 1), 10, YES, NSColor.whiteColor);
}

/* ---- views ---------------------------------------------------------------------- */

/* Laid out from the top, like the other frontends' panels. */
@interface A7800CtlFlippedView : NSView
@end

@implementation A7800CtlFlippedView
- (BOOL)isFlipped { return YES; }
@end

/* Keys typed with the panel key go to its owner. */
@class A7800ControllersWindow;
@interface A7800ControllersPanel : NSPanel
@property (nonatomic, weak) A7800ControllersWindow *owner;
@end

/* Everything the views and the class methods call on the window. */
@interface A7800ControllersWindow () <NSWindowDelegate>
- (instancetype)initWithSession:(a7800session *)session;
- (void)show;
- (void)hide;
- (void)toggle;
- (BOOL)isVisible;
- (BOOL)isActive;
- (void)refresh;
- (void)mapPick:(int)target;
- (NSString *)legendForTarget:(int)target;
- (void)panelKeyDown:(NSEvent *)e;
- (void)panelKeyUp:(NSEvent *)e;
- (void)panelFlagsChanged:(NSEvent *)e;
@end

/* One label the stage shows over a control while Map is armed or Show bindings
 * is on. */
typedef struct {
    NSRect rect;
    int target;
    char text[64];
} Pill;

/* The stage: both ports and the console on a grey backdrop. Everything is drawn
 * here and hit-tested by the core (the stick's drag maths are the core's too). */
@interface A7800StageView : A7800CtlFlippedView
@property (nonatomic) a7800session *session;
@property (nonatomic, weak) A7800ControllersWindow *owner;
@property (nonatomic) BOOL mapping;          /* Map armed: bindings shown, nothing lit */
@property (nonatomic) int mapTarget;         /* the control waiting for a key, or -1 */
@property (nonatomic) BOOL showKeys;         /* the bound host key as a small legend */
@property (nonatomic) CGFloat winWidth;      /* the panel's content width */
@property (nonatomic, readonly) NSSize contentSize;
- (void)configure;
/* Whether the layout is out of date: a port has another controller to draw. */
- (BOOL)layoutStale;
/* Reads what the console sees held; redraws when anything changed. */
- (void)poll;
/* Lets go of everything the pointer holds. */
- (void)letGo;
@end

@implementation A7800StageView {
    const a7800_control *_ctls;   /* the core's static table */
    int _count;
    float _dimW, _dimH;
    int _types[A7800_PORTS];      /* the view types the layout was read for */
    NSSize _size;

    unsigned _held[A7800_PORTS];  /* what the console sees held, bit per a7800_action */
    int _sw[A7800_SW_COUNT];      /* Select / Reset / Pause: held; the slides: 1 = A */

    /* the mouse */
    int _downIdx;                 /* the control pressed, or -1 */
    int _downTarget;              /* the target the pointer holds with a press, or -1 */
    BOOL _stickDrag;
    int _hover;                   /* the pressable part under the pointer (Map), or -1 */
    NSTrackingArea *_tracking;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _downIdx = -1;
    _downTarget = -1;
    _mapTarget = -1;
    _hover = -1;
    _winWidth = MIN_WIN_W;
    return self;
}

- (BOOL)isOpaque { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent *)evt { (void)evt; return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (NSSize)contentSize { return _size; }

/* (Re)reads the layout for the controllers the ports show. */
- (void)configure
{
    for (int port = 0; port < A7800_PORTS; port++) _types[port] = a7800session_view_type(_session, port);
    _ctls = a7800session_controls_layout(_types[0], _types[1], &_count, &_dimW, &_dimH);
    if (!_ctls || _count < 0) _count = 0;
    _winWidth = fmax(MIN_WIN_W, ceil(_dimW * UNIT) + 2 * STAGE_PAD);
    _size = NSMakeSize(_winWidth, STAGE_PAD * 2 + ceil(_dimH * UNIT));
    _hover = -1;
    [self setNeedsDisplay:YES];
}

- (BOOL)layoutStale
{
    for (int port = 0; port < A7800_PORTS; port++)
        if (a7800session_view_type(_session, port) != _types[port]) return YES;
    return NO;
}

- (void)updateTrackingAreas
{
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:(NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                      NSTrackingActiveAlways | NSTrackingInVisibleRect)
               owner:self
            userInfo:nil];
    [self addTrackingArea:_tracking];
    [super updateTrackingAreas];
}

/* ---- geometry ------------------------------------------------------------------ */

/* Where the layout's origin is, in view points. */
- (NSPoint)origin
{
    return NSMakePoint(floor((_winWidth - _dimW * UNIT) / 2), STAGE_PAD);
}

- (NSRect)rectOf:(const a7800_control *)ctl
{
    const NSPoint org = [self origin];
    return NSMakeRect(org.x + ctl->x * UNIT, org.y + ctl->y * UNIT, ctl->w * UNIT, ctl->h * UNIT);
}

- (const a7800_control *)ctlAt:(int)idx
{
    return idx >= 0 && idx < _count ? &_ctls[idx] : NULL;
}

/* The pressable part under a view point, or -1. */
- (int)hit:(NSPoint)pnt
{
    const NSPoint org = [self origin];
    return a7800session_controls_hit(_types[0], _types[1], (float)((pnt.x - org.x) / UNIT),
                                     (float)((pnt.y - org.y) / UNIT));
}

/* Whether the console sees a target held (the window is a gamepad tester). */
- (BOOL)heldTarget:(int)target
{
    if (self.mapping || target < 0) return NO;
    if (target < A7800_PORTS * A7800_ACT_PER_PORT)
        return ((_held[target / A7800_ACT_PER_PORT] >> (target % A7800_ACT_PER_PORT)) & 1u) != 0;
    if (target >= A7800_TARGET_SWITCH(0) && target < A7800_TARGET_SWITCH(A7800_SW_LEFT_DIFF))
        return _sw[target - A7800_TARGET_SWITCH(0)] != 0;
    return NO;
}

/* A slide's position: 1 is A (pro, the slider at the right). */
- (int)slidePos:(int)target
{
    const int sw = target - A7800_TARGET_SWITCH(0);
    if (sw < A7800_SW_LEFT_DIFF || sw >= A7800_SW_COUNT) return 0;
    return _sw[sw];
}

/* Whether a stick is off centre. */
- (BOOL)stickHeld:(int)port
{
    if (self.mapping || port < 0 || port >= A7800_PORTS) return NO;
    const unsigned dirs = (1u << A7800_ACT_UP) | (1u << A7800_ACT_DOWN) | (1u << A7800_ACT_LEFT) | (1u << A7800_ACT_RIGHT);
    return (_held[port] & dirs) != 0;
}

/* Whether a Map pick is one of the port's four directions. */
static BOOL isDirectionOf(int target, int port)
{
    return target >= A7800_TARGET_PORT(port, A7800_ACT_UP) && target <= A7800_TARGET_PORT(port, A7800_ACT_RIGHT);
}

- (BOOL)picked:(int)target
{
    return self.mapping && target >= 0 && target == self.mapTarget;
}

- (BOOL)stickPicked:(int)port
{
    return self.mapping && self.mapTarget >= 0 && isDirectionOf(self.mapTarget, port);
}

/* Whether a part is drawn faint: in Map mode every pressable part but the pick
 * (or, while nothing is picked yet, the one under the pointer). */
- (BOOL)dimmedIdx:(int)idx
{
    if (!self.mapping) return NO;
    const a7800_control *ctl = &_ctls[idx];
    switch (ctl->kind) {
    case A7800_CTL_STICK:
        if ([self stickPicked:ctl->port]) return NO;
        break;
    case A7800_CTL_ROUND_BUTTON:
    case A7800_CTL_BUTTON:
    case A7800_CTL_SWITCH:
        if ([self picked:ctl->target]) return NO;
        break;
    default:
        return NO;
    }
    if (self.mapTarget >= 0) return YES;
    if (_hover < 0) return YES;
    const a7800_control *over = &_ctls[_hover];
    if (ctl->kind == A7800_CTL_STICK) return !(over->kind == A7800_CTL_STICK && over->port == ctl->port);
    return idx != _hover;
}

- (void)beginDim:(BOOL)dim alpha:(CGFloat)alpha
{
    if (!dim) return;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGContextSaveGState(ctx);
    CGContextSetAlpha(ctx, alpha);
    CGContextBeginTransparencyLayer(ctx, NULL);
}

- (void)endDim:(BOOL)dim
{
    if (!dim) return;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGContextEndTransparencyLayer(ctx);
    CGContextRestoreGState(ctx);
}

/* ---- the labels shown over the controls ---------------------------------------- */

- (int)pills:(Pill *)out max:(int)max
{
    int num = 0;
    if (!(self.mapping || self.showKeys)) return 0;
#define ADD_PILL(cx, cy, wid, tgt, prefix) do { \
        if (num < max) { \
            Pill *pill = &out[num++]; \
            pill->rect = NSMakeRect((cx) - (wid) / 2, (cy) - PILL_H / 2, (wid), PILL_H); \
            pill->target = (tgt); \
            NSString *name = [self.owner legendForTarget:(tgt)]; \
            NSString *text = [NSString stringWithFormat:@"%@%@", prefix, name.length ? name : @"-"]; \
            strncpy(pill->text, text.UTF8String, sizeof pill->text - 1); \
            pill->text[sizeof pill->text - 1] = '\0'; \
        } } while (0)
    for (int idx = 0; idx < _count; idx++) {
        const a7800_control *ctl = &_ctls[idx];
        const NSRect rect = [self rectOf:ctl];
        const NSPoint ctr = NSMakePoint(NSMidX(rect), NSMidY(rect));
        switch (ctl->kind) {
        case A7800_CTL_STICK: {
            const int port = ctl->port;
            ADD_PILL(ctr.x - 1.55 * UNIT, ctr.y, 50.0, A7800_TARGET_PORT(port, A7800_ACT_LEFT), @"◀ ");
            ADD_PILL(ctr.x + 1.55 * UNIT, ctr.y, 50.0, A7800_TARGET_PORT(port, A7800_ACT_RIGHT), @"▶ ");
            ADD_PILL(ctr.x, ctr.y - 1.3 * UNIT, 50.0, A7800_TARGET_PORT(port, A7800_ACT_UP), @"▲ ");
            ADD_PILL(ctr.x, ctr.y + 1.3 * UNIT, 50.0, A7800_TARGET_PORT(port, A7800_ACT_DOWN), @"▼ ");
            break;
        }
        case A7800_CTL_ROUND_BUTTON:
            ADD_PILL(ctr.x, NSMaxY(rect) + 0.2 * UNIT, 44.0, ctl->target, @"");
            break;
        case A7800_CTL_BUTTON:
        case A7800_CTL_SWITCH:
            ADD_PILL(ctr.x, NSMaxY(rect), 44.0, ctl->target, @"");
            break;
        default:
            break;
        }
    }
#undef ADD_PILL
    return num;
}

/* The target Map would pick at a point (view points), or -1. */
- (int)mapTargetAt:(NSPoint)pnt
{
    Pill pills[MAX_PILLS];
    const int num = [self pills:pills max:MAX_PILLS];
    for (int idx = 0; idx < num; idx++)
        if (NSPointInRect(pnt, pills[idx].rect)) return pills[idx].target;
    const a7800_control *ctl = [self ctlAt:[self hit:pnt]];
    if (!ctl) return -1;
    switch (ctl->kind) {
    case A7800_CTL_ROUND_BUTTON:
    case A7800_CTL_BUTTON:
    case A7800_CTL_SWITCH:
        return ctl->target;
    case A7800_CTL_STICK: {
        const NSRect rect = [self rectOf:ctl];
        return directionTarget(ctl->port, (float)(pnt.x - NSMidX(rect)), (float)(pnt.y - NSMidY(rect)),
                               (float)(NSWidth(rect) / 2));
    }
    default:
        return -1;
    }
}

/* ---- polling ------------------------------------------------------------------- */

- (void)poll
{
    BOOL dirty = NO;
    for (int port = 0; port < A7800_PORTS; port++) {
        const unsigned held = self.mapping ? 0u : a7800session_buttons_held(_session, port);
        if (held != _held[port]) { _held[port] = held; dirty = YES; }
    }
    for (int sw = 0; sw < A7800_SW_COUNT; sw++) {
        int val;
        if (sw < A7800_SW_LEFT_DIFF) val = self.mapping ? 0 : (a7800session_switch_held(_session, sw) != 0);
        else val = a7800session_switch_get(_session, sw) != 0;
        if (val != _sw[sw]) { _sw[sw] = val; dirty = YES; }
    }
    if (dirty) [self setNeedsDisplay:YES];
}

/* ---- drawing -------------------------------------------------------------------- */

- (NSString *)legendOfTarget:(int)target
{
    if (!(self.mapping || self.showKeys) || target < 0) return nil;
    return [self.owner legendForTarget:target];
}

/* A held part: the ring and a 35% fill of the held colour. */
- (void)markHeld:(NSBezierPath *)path
{
    [rgba(A7800SESSION_HELD_RGB, 0.35) setFill];
    [path fill];
    [rgb(A7800SESSION_HELD_RGB) setStroke];
    path.lineWidth = 2.5;
    [path stroke];
}

/* The picked part in Map mode: the ring. */
- (void)ringPicked:(NSBezierPath *)path
{
    [rgb(A7800SESSION_HELD_RGB) setStroke];
    path.lineWidth = 2.5;
    [path stroke];
}

/* A soft shadow on the stage under a shape. */
- (void)shadowOf:(NSBezierPath *)path
{
    NSBezierPath *shadow = [path copy];
    NSAffineTransform *move = [NSAffineTransform transform];
    [move translateXBy:2 yBy:3];
    [shadow transformUsingAffineTransform:move];
    [rgba(0x000000, 0.35) setFill];
    [shadow fill];
}

/* -- the ProLine (DESIGN-7800.md section 1), seen from above in a 16 x 8 box -- */

/* The Atari fuji: a centre bar and two outer strokes flaring at the foot. */
static void drawFuji(NSPoint ctr, CGFloat high, NSColor *ink)
{
    strokeLine(NSMakePoint(ctr.x, ctr.y - high), NSMakePoint(ctr.x, ctr.y + high), 2, ink);
    for (int side = -1; side <= 1; side += 2) {
        NSBezierPath *arm = [NSBezierPath bezierPath];
        [arm moveToPoint:NSMakePoint(ctr.x + side * 3, ctr.y - high)];
        [arm curveToPoint:NSMakePoint(ctr.x + side * 0.65 * UNIT, ctr.y + high)
            controlPoint1:NSMakePoint(ctr.x + side * 3, ctr.y)
            controlPoint2:NSMakePoint(ctr.x + side * 0.4 * UNIT, ctr.y + high * 0.6)];
        arm.lineWidth = 2;
        arm.lineCapStyle = NSLineCapStyleRound;
        [ink setStroke];
        [arm stroke];
    }
}

- (void)drawProLine:(NSRect)rect
{
    NSBezierPath *body = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:0.5 * UNIT yRadius:0.5 * UNIT];
    [self shadowOf:body];
    fillGradient(body, A7800SESSION_CASE_BEVEL_RGB, A7800SESSION_CASE_RGB);
    [rgb(A7800SESSION_CASE_SHADOW_RGB) setStroke];
    body.lineWidth = 1.5;
    [body stroke];
    NSBezierPath *inner = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, 0.2 * UNIT, 0.2 * UNIT)
                                                          xRadius:0.35 * UNIT yRadius:0.35 * UNIT];
    inner.lineWidth = 1;
    [rgba(A7800SESSION_CASE_BEVEL_RGB, 0.9) setStroke];
    [inner stroke];

    /* the raised triangle panel over the left half, its hypotenuse running from
     * the top (7.8, 0.2) down to the lower left */
    NSBezierPath *panel = [NSBezierPath bezierPath];
    [panel moveToPoint:unitPoint(rect, 0.45, 0.4)];
    [panel lineToPoint:unitPoint(rect, 7.8, 0.4)];
    [panel lineToPoint:unitPoint(rect, 0.45, 7.55)];
    [panel closePath];
    panel.lineJoinStyle = NSLineJoinStyleRound;
    fillGradient(panel, mixKey(A7800SESSION_CASE_RGB, A7800SESSION_CASE_BEVEL_RGB, 0.45), A7800SESSION_CASE_RGB);
    [rgb(A7800SESSION_PANEL_EDGE_RGB) setStroke];
    panel.lineWidth = 1.5;
    [panel stroke];

    /* the ribbed swoosh running down and to the right from each button */
    const double buttonX[2] = { 8.4, 12.4 };
    for (int btn = 0; btn < 2; btn++) {
        for (int rib = -1; rib <= 1; rib++) {
            const NSPoint from = unitPoint(rect, buttonX[btn] + 0.8 + 0.28 * rib, 5.2 + 0.8 - 0.28 * rib);
            const NSPoint to = unitPoint(rect, buttonX[btn] + 2.4 + 0.28 * rib, 5.2 + 2.4 - 0.28 * rib);
            strokeLine(from, to, 2.2, rgb(A7800SESSION_SWOOSH_RGB));
            strokeLine(NSMakePoint(from.x + 1, from.y + 1.5), NSMakePoint(to.x + 1, to.y + 1.5), 0.8,
                       rgba(A7800SESSION_CASE_BEVEL_RGB, 0.6));
        }
    }

    /* the embossed fuji and "ATARI", tilted a little up to the right */
    [NSGraphicsContext saveGraphicsState];
    NSAffineTransform *tilt = [NSAffineTransform transform];
    const NSPoint logo = unitPoint(rect, 12.3, 1.9);
    [tilt translateXBy:logo.x yBy:logo.y];
    [tilt rotateByDegrees:-15];
    [tilt concat];
    drawFuji(NSMakePoint(-1.9 * UNIT, 0), 0.45 * UNIT, rgba(A7800SESSION_LOGO_RGB, 0.9));
    drawFittedStyled(@"ATARI", NSMakeRect(-1.2 * UNIT, -0.45 * UNIT, 2.6 * UNIT, 0.9 * UNIT), 11, YES, NO,
                     rgba(A7800SESSION_LOGO_RGB, 0.9));
    [NSGraphicsContext restoreGraphicsState];

    /* the cable leaving the top right edge */
    NSBezierPath *cable = [NSBezierPath bezierPathWithRoundedRect:unitRect(rect, 13.3, -0.5, 0.4, 0.7)
                                                          xRadius:2 yRadius:2];
    [rgb(A7800SESSION_CABLE_RGB) setFill];
    [cable fill];
}

/* The stick's dish with its cross-shaped recess and the held quadrants, then the
 * grey cap shifted toward the held direction. */
- (void)drawProLineStick:(const a7800_control *)ctl
{
    const int port = ctl->port;
    const NSRect rect = [self rectOf:ctl];
    const NSPoint ctr = NSMakePoint(NSMidX(rect), NSMidY(rect));
    const CGFloat rad = NSWidth(rect) / 2;
    const unsigned held = (port >= 0 && port < A7800_PORTS && !self.mapping) ? _held[port] : 0u;

    NSBezierPath *dish = [NSBezierPath bezierPathWithOvalInRect:rect];
    fillGradient(dish, mixKey(A7800SESSION_DISH_RGB, 0xFFFFFF, 0.12), A7800SESSION_DISH_RGB);
    [rgb(A7800SESSION_CASE_SHADOW_RGB) setStroke];
    dish.lineWidth = 1.5;
    [dish stroke];
    /* the four notches at N, S, E and W */
    for (int act = 0; act < 4; act++) {
        const double ang = directionAngle(act) * M_PI / 180.0;
        const NSPoint mid = NSMakePoint(ctr.x + 0.86 * rad * cos(ang), ctr.y + 0.86 * rad * sin(ang));
        fillOval(discRect(mid, 0.1 * rad), rgb(A7800SESSION_DISH_RECESS_RGB));
        strokeLine(NSMakePoint(ctr.x + 0.74 * rad * cos(ang), ctr.y + 0.74 * rad * sin(ang)),
                   NSMakePoint(ctr.x + 0.95 * rad * cos(ang), ctr.y + 0.95 * rad * sin(ang)), 2.2,
                   rgb(A7800SESSION_DISH_RECESS_RGB));
    }
    /* a held direction tints its quadrant */
    for (int act = 0; act < 4; act++) {
        if (!((held >> act) & 1u)) continue;
        const double mid = directionAngle(act);
        NSBezierPath *wedge = [NSBezierPath bezierPath];
        [wedge moveToPoint:ctr];
        for (int step = 0; step <= 12; step++) {
            const double ang = (mid - 45.0 + step * 90.0 / 12.0) * M_PI / 180.0;
            [wedge lineToPoint:NSMakePoint(ctr.x + rad * cos(ang), ctr.y + rad * sin(ang))];
        }
        [wedge closePath];
        [rgba(A7800SESSION_HELD_RGB, 0.35) setFill];
        [wedge fill];
    }

    /* the cap: up to 0.9 u toward the held direction (it stays inside the dish) */
    double frx = 0, fry = 0;
    deflection(held, &frx, &fry);
    const NSPoint cap = NSMakePoint(ctr.x + frx * 0.9 * UNIT, ctr.y + fry * 0.9 * UNIT);
    const CGFloat capR = 1.0 * UNIT;
    const BOOL tilted = frx != 0 || fry != 0;
    fillOval(discRect(NSMakePoint(cap.x + 1.5, cap.y + 2.5), capR), rgba(0x000000, 0.35));
    NSBezierPath *capPath = [NSBezierPath bezierPathWithOvalInRect:discRect(cap, capR)];
    if (tilted) {
        [rgb(A7800SESSION_HELD_RGB) setFill];
        [capPath fill];
    } else {
        fillGradient(capPath, A7800SESSION_CAP_HI_RGB, A7800SESSION_CAP_RGB);
    }
    [rgb(A7800SESSION_CASE_SHADOW_RGB) setStroke];
    capPath.lineWidth = 1;
    [capPath stroke];
    fillOval(NSMakeRect(cap.x - 0.4 * capR, cap.y - 0.7 * capR, 0.8 * capR, 0.4 * capR), rgba(0xFFFFFF, 0.22));

    if ([self stickPicked:port]) {
        [self ringPicked:[NSBezierPath bezierPathWithOvalInRect:NSInsetRect(rect, -3, -3)]];
        /* a mark on the rim where the picked direction is */
        const double ang = directionAngle(self.mapTarget % A7800_ACT_PER_PORT) * M_PI / 180.0;
        fillOval(discRect(NSMakePoint(ctr.x + rad * cos(ang), ctr.y + rad * sin(ang)), 4), rgb(A7800SESSION_HELD_RGB));
    }
}

/* One of the ProLine's two red buttons, in its dark recess, with its numeral. */
- (void)drawProLineButton:(const a7800_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    const NSPoint ctr = NSMakePoint(NSMidX(rect), NSMidY(rect));
    const BOOL held = [self heldTarget:ctl->target];
    NSString *legend = [self legendOfTarget:ctl->target];

    drawFittedStyled(nsstr(ctl->label), NSMakeRect(ctr.x - 2.1 * UNIT, ctr.y - 2.0 * UNIT, 0.9 * UNIT, 0.8 * UNIT), 10, YES,
                     NO, rgb(A7800SESSION_NUMERAL_RGB));
    fillOval(discRect(ctr, 1.4 * UNIT), rgb(A7800SESSION_BUTTON_RECESS_RGB));
    const NSRect face = discRect(NSMakePoint(ctr.x, ctr.y + (held ? 1.0 : 0.0)), 1.25 * UNIT);
    NSBezierPath *disc = [NSBezierPath bezierPathWithOvalInRect:face];
    fillGradient(disc, held ? mixKey(A7800SESSION_HELD_BUTTON_FILL_RGB, 0xFFFFFF, 0.12) : A7800SESSION_BUTTON_HI_RGB,
                 held ? A7800SESSION_HELD_BUTTON_FILL_RGB : A7800SESSION_BUTTON_RGB);
    [rgba(0x000000, 0.4) setStroke];
    disc.lineWidth = 0.75;
    [disc stroke];
    if (!held) fillOval(NSMakeRect(ctr.x - 0.75 * UNIT, ctr.y - 0.85 * UNIT, 0.7 * UNIT, 0.4 * UNIT), rgba(0xFFFFFF, 0.3));
    if (held) [self markHeld:[NSBezierPath bezierPathWithOvalInRect:NSInsetRect(face, -3, -3)]];
    if (legend.length) drawFitted(legend, NSInsetRect(face, 3, 3), 10, YES, NSColor.whiteColor);
    if ([self picked:ctl->target])
        [self ringPicked:[NSBezierPath bezierPathWithOvalInRect:NSInsetRect(discRect(ctr, 1.4 * UNIT), -4, -4)]];
}

/* -- the 2600 CX40 (DESIGN-atari.md section 2), in its 8 x 8 case ---------------- */

- (void)drawCX40:(NSRect)rect port:(int)port
{
    const unsigned held = (port >= 0 && port < A7800_PORTS && !self.mapping) ? _held[port] : 0u;
    double defx = 0, defy = 0;
    deflection(held, &defx, &defy);

    NSBezierPath *casePath = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:0.9 * UNIT yRadius:0.9 * UNIT];
    [self shadowOf:casePath];
    fillGradient(casePath, A7800SESSION_CX40_BEVEL_RGB, A7800SESSION_CX40_CASE_RGB);
    NSBezierPath *bevel = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, 0.2 * UNIT, 0.2 * UNIT)
                                                          xRadius:0.75 * UNIT yRadius:0.75 * UNIT];
    bevel.lineWidth = 1.5;
    [rgb(A7800SESSION_CX40_BEVEL_RGB) setStroke];
    [bevel stroke];

    /* the fire button's recess: a teardrop, its neck leading to the ring */
    strokeLine(unitPoint(rect, 1.7, 1.7), unitPoint(rect, 3.1, 3.1), 0.9 * UNIT, rgb(A7800SESSION_CX40_RING_RGB));
    fillOval(discRect(unitPoint(rect, 1.7, 1.7), 1.1 * UNIT), rgb(A7800SESSION_CX40_RING_RGB));

    /* the ring: 36 tick dashes, the ones on the deflected side lit */
    fillOval(discRect(unitPoint(rect, 4.5, 4.5), 3.0 * UNIT), rgb(A7800SESSION_CX40_RING_RGB));
    const BOOL tilted = defx != 0 || defy != 0;
    const double dirAngle = tilted ? atan2(defx, -defy) * 180.0 / M_PI : 0;
    for (int tick = 0; tick < 36; tick++) {
        const double ang = tick * 10.0;
        const double diff = fabs(fmod(ang - dirAngle + 540.0, 360.0) - 180.0);
        const BOOL lit = tilted && diff <= 25.0;
        const double rad = ang * M_PI / 180.0;
        strokeLine(unitPoint(rect, 4.5 + 2.55 * sin(rad), 4.5 - 2.55 * cos(rad)),
                   unitPoint(rect, 4.5 + 2.85 * sin(rad), 4.5 - 2.85 * cos(rad)), lit ? 2.5 : 1.4,
                   lit ? rgb(A7800SESSION_HELD_RGB) : rgb(A7800SESSION_CX40_TICK_RGB));
    }
    drawFitted(@"TOP", unitRect(rect, 3.75, 1.6, 1.5, 0.6), 7, YES, rgb(A7800SESSION_CX40_BEVEL_RGB));

    /* the boot: ribbed rings, squashed a little toward the tilt for depth */
    const CGFloat shx = defx * 0.14 * UNIT, shy = defy * 0.14 * UNIT;
    const NSPoint mid = unitPoint(rect, 4.5, 4.5);
    fillOval(discRect(NSMakePoint(mid.x + shx, mid.y + shy), 2.1 * UNIT), rgb(0x151515));
    fillOval(discRect(NSMakePoint(mid.x + shx, mid.y + shy), 1.7 * UNIT), rgb(0x262626));
    fillOval(discRect(NSMakePoint(mid.x + shx * 1.3, mid.y + shy * 1.3), 1.3 * UNIT), rgb(0x151515));
}

/* The CX40's shaft cap: an octagon that slides toward the held direction. */
- (void)drawCX40Stick:(const a7800_control *)ctl
{
    const int port = ctl->port;
    const NSRect rect = [self rectOf:ctl];
    const unsigned held = (port >= 0 && port < A7800_PORTS && !self.mapping) ? _held[port] : 0u;
    double defx = 0, defy = 0;
    deflection(held, &defx, &defy);
    const NSPoint ctr = NSMakePoint(NSMidX(rect) + defx * 1.0 * UNIT, NSMidY(rect) + defy * 1.0 * UNIT);
    NSBezierPath *cap = polygon(ctr, 0.85 * UNIT, 8, 22.5);
    [rgb(0x0C0C0C) setFill];
    [cap fill];
    [rgb(A7800SESSION_CX40_BEVEL_RGB) setStroke];
    cap.lineWidth = 1.5;
    [cap stroke];
    fillOval(NSMakeRect(ctr.x - 0.2 * UNIT, ctr.y - 0.3 * UNIT, 0.4 * UNIT, 0.28 * UNIT), rgba(0xFFFFFF, 0.1));
    if (defx != 0 || defy != 0) [self markHeld:polygon(ctr, 0.98 * UNIT, 8, 22.5)];
    if ([self stickPicked:port]) {
        const NSPoint centre = NSMakePoint(NSMidX(rect), NSMidY(rect));
        [self ringPicked:[NSBezierPath bezierPathWithOvalInRect:NSInsetRect(rect, -3, -3)]];
        const double ang = directionAngle(self.mapTarget % A7800_ACT_PER_PORT) * M_PI / 180.0;
        const CGFloat rad = NSWidth(rect) / 2;
        fillOval(discRect(NSMakePoint(centre.x + rad * cos(ang), centre.y + rad * sin(ang)), 4), rgb(A7800SESSION_HELD_RGB));
    }
}

/* The CX40's one red fire button. */
- (void)drawCX40Fire:(const a7800_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    const NSPoint ctr = NSMakePoint(NSMidX(rect), NSMidY(rect));
    const BOOL held = [self heldTarget:ctl->target];
    NSString *legend = [self legendOfTarget:ctl->target];
    const CGFloat rad = 0.8 * UNIT;
    const NSRect face = discRect(NSMakePoint(ctr.x, ctr.y + (held ? 1.5 : 0)), rad);
    NSBezierPath *disc = [NSBezierPath bezierPathWithOvalInRect:face];
    fillGradient(disc, held ? mixKey(A7800SESSION_HELD_BUTTON_FILL_RGB, 0xFFFFFF, 0.12) : A7800SESSION_CX40_FIRE_HI_RGB,
                 held ? A7800SESSION_HELD_BUTTON_FILL_RGB : A7800SESSION_CX40_FIRE_RGB);
    [rgba(0x000000, 0.3) setStroke];
    disc.lineWidth = 0.75;
    [disc stroke];
    if (!held) fillOval(NSMakeRect(ctr.x - 0.55 * UNIT, ctr.y - 0.6 * UNIT, 0.5 * UNIT, 0.32 * UNIT), rgba(0xFFFFFF, 0.3));
    if (held) [self markHeld:[NSBezierPath bezierPathWithOvalInRect:NSInsetRect(face, -3, -3)]];
    if (legend.length) drawFitted(legend, NSInsetRect(face, 3, 3), 10, YES, NSColor.whiteColor);
    if ([self picked:ctl->target])
        [self ringPicked:[NSBezierPath bezierPathWithOvalInRect:NSInsetRect(discRect(ctr, rad), -5, -5)]];
}

/* -- the XG-1 light gun and the empty socket --------------------------------------- */

/* The XG-1 from the side, its barrel to the left (Atari_XG-1_light_gun.jpg): warm
 * grey plastic, a ribbed grip, a dark trigger. Nothing on it is pressable. */
- (void)drawGun:(NSRect)rect
{
    const CGFloat side = fmin(NSWidth(rect), NSHeight(rect));
    const NSRect box = NSMakeRect(NSMidX(rect) - side / 2, NSMinY(rect), side, side);
    static const double bodyPts[][2] = {
        { 0.28, 0.16 }, { 0.50, 0.15 }, { 0.93, 0.31 }, { 0.95, 0.47 }, { 0.88, 0.54 }, { 0.77, 0.52 },
        { 0.77, 0.88 }, { 0.66, 0.88 }, { 0.63, 0.60 }, { 0.55, 0.40 }, { 0.28, 0.30 } };
    static const double barrelPts[][2] = {
        { 0.03, 0.12 }, { 0.12, 0.10 }, { 0.30, 0.15 }, { 0.30, 0.29 }, { 0.12, 0.27 }, { 0.03, 0.24 } };
    static const double sightPts[][2] = { { 0.06, 0.04 }, { 0.10, 0.04 }, { 0.10, 0.11 }, { 0.06, 0.11 } };
    static const double triggerPts[][2] = { { 0.59, 0.47 }, { 0.65, 0.47 }, { 0.66, 0.60 }, { 0.62, 0.64 } };

    NSBezierPath *body = scaledPolygon(box, bodyPts, 11);
    [self shadowOf:body];
    NSBezierPath *barrel = scaledPolygon(box, barrelPts, 6);
    NSBezierPath *sight = scaledPolygon(box, sightPts, 4);
    [rgb(COL_GUN) setFill];
    [barrel fill];
    [sight fill];
    fillGradient(body, COL_GUN_HI, COL_GUN);
    [rgb(COL_GUN_LO) setStroke];
    body.lineWidth = 1.25;
    barrel.lineWidth = 1.25;
    sight.lineWidth = 1.0;
    [body stroke];
    [barrel stroke];
    [sight stroke];
    /* the ribbed grip and the body's moulded panel line */
    for (int rib = 0; rib < 9; rib++) {
        const double ypos = 0.55 + rib * 0.034;
        strokeLine(NSMakePoint(NSMinX(box) + 0.665 * side, NSMinY(box) + ypos * side),
                   NSMakePoint(NSMinX(box) + 0.765 * side, NSMinY(box) + ypos * side), 0.8, rgb(COL_GUN_LO));
    }
    strokeLine(NSMakePoint(NSMinX(box) + 0.40 * side, NSMinY(box) + 0.24 * side),
               NSMakePoint(NSMinX(box) + 0.88 * side, NSMinY(box) + 0.38 * side), 1, rgb(COL_GUN_LO));
    NSBezierPath *trigger = scaledPolygon(box, triggerPts, 4);
    [rgb(COL_GUN_TRIGGER) setFill];
    [trigger fill];
}

/* An empty port: the socket's outline with a row of pin dots. */
- (void)drawSocket:(NSRect)rect
{
    NSBezierPath *outer = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:0.8 * UNIT yRadius:0.8 * UNIT];
    [rgba(A7800SESSION_CASE_SHADOW_RGB, 0.5) setFill];
    [outer fill];
    [rgb(A7800SESSION_SOCKET_RGB) setStroke];
    outer.lineWidth = 2;
    const CGFloat dash[2] = { 6, 4 };
    [outer setLineDash:dash count:2 phase:0];
    [outer stroke];
    for (int pin = 0; pin < 8; pin++)
        fillOval(discRect(unitPoint(rect, 2.1 + pin * 1.1, 1.0), 0.22 * UNIT), rgb(A7800SESSION_SOCKET_RGB));
}

/* -- the console strip (DESIGN-7800.md section 5) --------------------------------- */

- (void)drawConsoleBar:(NSRect)rect
{
    NSBezierPath *bar = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:0.6 * UNIT yRadius:0.6 * UNIT];
    [self shadowOf:bar];
    fillGradient(bar, A7800SESSION_CASE_BEVEL_RGB, A7800SESSION_CASE_RGB);
    [rgb(A7800SESSION_CASE_SHADOW_RGB) setStroke];
    bar.lineWidth = 1.5;
    [bar stroke];
}

/* The brushed-silver band the label sits on. */
- (void)drawBand:(NSRect)rect
{
    NSBezierPath *strip = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:0.35 * UNIT yRadius:0.35 * UNIT];
    fillGradient(strip, A7800SESSION_BAND_HI_RGB, A7800SESSION_BAND_LO_RGB);
    [NSGraphicsContext saveGraphicsState];
    [strip addClip];
    for (CGFloat ypos = NSMinY(rect) + 2; ypos < NSMaxY(rect) - 1; ypos += 3)
        strokeLine(NSMakePoint(NSMinX(rect), ypos), NSMakePoint(NSMaxX(rect), ypos), 0.5, rgba(0xFFFFFF, 0.18));
    [NSGraphicsContext restoreGraphicsState];
    [rgba(A7800SESSION_CASE_SHADOW_RGB, 0.7) setStroke];
    strip.lineWidth = 1;
    [strip stroke];
}

/* The rainbow line under the band's text. */
- (void)drawRainbow:(NSRect)rect
{
    const uint32_t stripes[4] = { A7800SESSION_RAINBOW_ORANGE_RGB, A7800SESSION_RAINBOW_VIOLET_RGB,
                                  A7800SESSION_RAINBOW_BLUE_RGB, A7800SESSION_RAINBOW_GREEN_RGB };
    const CGFloat segW = NSWidth(rect) / 4;
    for (int idx = 0; idx < 4; idx++) {
        [rgb(stripes[idx]) setFill];
        NSRectFill(NSMakeRect(NSMinX(rect) + idx * segW, NSMinY(rect), ceil(segW), NSHeight(rect)));
    }
}

/* A flat black console key with its legend: SELECT, RESET, PAUSE, and the dimmed
 * POWER. */
- (void)drawKey:(NSRect)rect label:(NSString *)label held:(BOOL)held picked:(BOOL)picked
{
    if (held) rect.origin.y += 1;
    NSBezierPath *bezel = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -2, -2) xRadius:5 yRadius:5];
    [rgb(A7800SESSION_CASE_SHADOW_RGB) setFill];
    [bezel fill];
    NSBezierPath *key = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:4 yRadius:4];
    fillGradient(key, A7800SESSION_CASE_BEVEL_RGB, A7800SESSION_KEY_RGB);
    NSBezierPath *face = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, 1, 1) xRadius:3 yRadius:3];
    fillGradient(face, mixKey(A7800SESSION_KEY_RGB, 0xFFFFFF, 0.06), A7800SESSION_KEY_RGB);
    drawFitted(label, NSInsetRect(rect, 2, 1), 10, YES, rgb(A7800SESSION_KEY_LABEL_RGB));
    if (held) [self markHeld:[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -2, -2) xRadius:6 yRadius:6]];
    if (picked)
        [self ringPicked:[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -4, -4) xRadius:7 yRadius:7]];
}

- (void)drawConsoleKey:(const a7800_control *)ctl
{
    [self drawKey:[self rectOf:ctl] label:nsstr(ctl->label) held:[self heldTarget:ctl->target]
           picked:[self picked:ctl->target]];
}

/* A difficulty slide: a slot with B printed at its left and A at its right, a
 * silver slider at the end the switch is at (A is pro, the right). Latched, so
 * it is a position, not a held light. */
- (void)drawSlide:(const a7800_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    const BOOL proPos = [self slidePos:ctl->target] != 0;
    const CGFloat midY = NSMidY(rect);
    const NSRect slot = NSMakeRect(NSMinX(rect) + 0.8 * UNIT, midY - 0.6 * UNIT, 3.0 * UNIT, 1.2 * UNIT);
    NSBezierPath *slotPath = [NSBezierPath bezierPathWithRoundedRect:slot xRadius:0.4 * UNIT yRadius:0.4 * UNIT];
    [rgb(A7800SESSION_SLIDE_SLOT_RGB) setFill];
    [slotPath fill];
    [rgba(A7800SESSION_CASE_BEVEL_RGB, 0.8) setStroke];
    slotPath.lineWidth = 1;
    [slotPath stroke];
    const CGFloat knobW = 1.5 * UNIT;
    const CGFloat knobX = proPos ? NSMaxX(slot) - knobW - 1 : NSMinX(slot) + 1;
    NSBezierPath *knob = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(knobX, NSMinY(slot) + 1.5, knobW, NSHeight(slot) - 3)
                                                         xRadius:3 yRadius:3];
    fillGradientH(knob, A7800SESSION_SLIDE_KNOB_RGB, mixKey(A7800SESSION_SLIDE_KNOB_RGB, 0x000000, 0.3));
    [rgba(0x000000, 0.4) setStroke];
    knob.lineWidth = 0.75;
    [knob stroke];
    strokeLine(NSMakePoint(knobX + knobW / 2, NSMinY(slot) + 4), NSMakePoint(knobX + knobW / 2, NSMaxY(slot) - 4), 0.8,
               rgba(0x000000, 0.35));
    NSColor *letter = rgb(A7800SESSION_KEY_LABEL_RGB);
    drawFitted(@"B", NSMakeRect(NSMinX(rect), NSMinY(rect), 0.8 * UNIT, NSHeight(rect)), 10, YES, letter);
    drawFitted(@"A", NSMakeRect(NSMaxX(rect) - 0.8 * UNIT, NSMinY(rect), 0.8 * UNIT, NSHeight(rect)), 10, YES, letter);
    NSString *what = ctl->legend ? [nsstr(ctl->legend) uppercaseString] : @"";
    if (what.length)
        drawFitted(what, NSMakeRect(NSMinX(rect) - 0.5 * UNIT, NSMaxY(rect) + 0.05 * UNIT, NSWidth(rect) + UNIT, 0.7 * UNIT), 8,
                   NO, rgba(A7800SESSION_KEY_LABEL_RGB, 0.8));
    if ([self picked:ctl->target])
        [self ringPicked:[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -3, -3) xRadius:7 yRadius:7]];
}

- (void)drawBody:(const a7800_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    const char *name = ctl->label ? ctl->label : "";
    if (!strcmp(name, "ProLine")) [self drawProLine:rect];
    else if (!strcmp(name, "CX40")) [self drawCX40:rect port:ctl->port];
    else if (!strcmp(name, "XG-1")) [self drawGun:rect];
    else if (!strcmp(name, "Socket")) [self drawSocket:rect];
    else if (!strcmp(name, "Console")) [self drawConsoleBar:rect];
    else if (!strcmp(name, "Band")) [self drawBand:rect];
    else if (!strcmp(name, "Rainbow")) [self drawRainbow:rect];
    else if (!strcmp(name, "POWER")) {
        /* the emulator is on: drawn dimmed, never a target */
        [self beginDim:YES alpha:0.35];
        [self drawKey:rect label:@"POWER" held:NO picked:NO];
        [self endDim:YES];
    }
}

- (void)drawCaption:(const a7800_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    if (ctl->port < 0) {
        /* "7800 PROSYSTEM", printed on the band; the fuji embossed at its left */
        drawFittedStyled(nsstr(ctl->label), rect, 12, YES, YES, rgb(A7800SESSION_BAND_TEXT_RGB));
        drawFuji(NSMakePoint(NSMinX(rect) + 0.8 * UNIT, NSMidY(rect)), 0.5 * UNIT, rgba(A7800SESSION_BAND_TEXT_RGB, 0.85));
        return;
    }
    drawFitted(nsstr(ctl->label), NSInsetRect(rect, 0, -3), 12, YES, rgba(COL_CAPTION, 0.9));
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSBezierPath *stage = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(0, 0, _size.width, _size.height)
                                                          xRadius:10 yRadius:10];
    fillGradient(stage, COL_STAGE_TOP, COL_STAGE_BOT);
    [rgb(COL_STAGE_EDGE) setStroke];
    stage.lineWidth = 1;
    [stage stroke];

    for (int idx = 0; idx < _count; idx++) {
        const a7800_control *ctl = &_ctls[idx];
        const BOOL dim = [self dimmedIdx:idx];
        [self beginDim:dim alpha:DIM_ALPHA];
        switch (ctl->kind) {
        case A7800_CTL_CAPTION:
            [self drawCaption:ctl];
            break;
        case A7800_CTL_BODY:
            [self drawBody:ctl];
            break;
        case A7800_CTL_STICK:
            if (ctl->device == A7800_CTRL_JOY2600) [self drawCX40Stick:ctl];
            else [self drawProLineStick:ctl];
            break;
        case A7800_CTL_ROUND_BUTTON:
            if (ctl->device == A7800_CTRL_JOY2600) [self drawCX40Fire:ctl];
            else [self drawProLineButton:ctl];
            break;
        case A7800_CTL_BUTTON:
            [self drawConsoleKey:ctl];
            break;
        case A7800_CTL_SWITCH:
            [self drawSlide:ctl];
            break;
        default:
            break;
        }
        [self endDim:dim];
    }

    /* the Map / Show bindings labels, over everything */
    Pill pills[MAX_PILLS];
    const int num = [self pills:pills max:MAX_PILLS];
    for (int idx = 0; idx < num; idx++)
        drawPill(pills[idx].rect, nsstr(pills[idx].text), [self picked:pills[idx].target]);
}

/* ---- the mouse ---------------------------------------------------------------------- */

- (void)letGo
{
    if (_downTarget >= 0) a7800session_press(_session, _downTarget, 0);
    if (_downIdx >= 0 && _downIdx < _count && _stickDrag)
        a7800session_pointer_set(_session, _ctls[_downIdx].port, 0);
    _downIdx = -1;
    _downTarget = -1;
    _stickDrag = NO;
    _hover = -1;
    [self setNeedsDisplay:YES];
}

/* The stick follows the pointer: the core turns the offset from the dish's centre
 * into one of eight 45-degree sectors (nothing in the dead centre). */
- (void)dragStickAt:(NSPoint)pnt
{
    const a7800_control *ctl = [self ctlAt:_downIdx];
    if (!ctl) return;
    const NSRect rect = [self rectOf:ctl];
    const unsigned mask = a7800session_stick_from_vector((float)(pnt.x - NSMidX(rect)), (float)(pnt.y - NSMidY(rect)),
                                                         (float)(NSWidth(rect) / 2));
    a7800session_pointer_set(_session, ctl->port, mask);
}

- (void)mouseDown:(NSEvent *)evt
{
    if (_downIdx >= 0) return;
    const NSPoint pnt = [self convertPoint:[evt locationInWindow] fromView:nil];

    if (self.mapping) {
        /* Map: a label, a stick direction, a button, a key or a slide is picked */
        const int target = [self mapTargetAt:pnt];
        if (target >= 0) [self.owner mapPick:target];
        return;
    }

    const int idx = [self hit:pnt];
    const a7800_control *ctl = [self ctlAt:idx];
    if (!ctl) return;
    switch (ctl->kind) {
    case A7800_CTL_STICK:
        _downIdx = idx;
        _stickDrag = YES;
        [self dragStickAt:pnt];
        break;
    case A7800_CTL_ROUND_BUTTON:
    case A7800_CTL_BUTTON:
        if (ctl->target < 0) break;
        _downIdx = idx;
        _downTarget = ctl->target;
        a7800session_press(_session, ctl->target, 1);
        break;
    case A7800_CTL_SWITCH:
        /* a difficulty slide flips on the press and latches: nothing to let go of */
        if (ctl->target < 0) break;
        _downIdx = idx;
        a7800session_press(_session, ctl->target, 1);
        break;
    default:
        break;
    }
    [self setNeedsDisplay:YES];
    [self poll];
}

- (void)mouseDragged:(NSEvent *)evt
{
    if (_downIdx < 0) return;
    const NSPoint pnt = [self convertPoint:[evt locationInWindow] fromView:nil];
    if (_stickDrag) {
        [self dragStickAt:pnt];
        [self poll];
        return;
    }
    /* a button or a key: dragging off lets go */
    const a7800_control *ctl = [self ctlAt:_downIdx];
    if (ctl && !NSPointInRect(pnt, [self rectOf:ctl])) {
        [self letGo];
        [self poll];
    }
}

- (void)mouseUp:(NSEvent *)evt
{
    (void)evt;
    if (_downIdx < 0) return;
    [self letGo];
    [self poll];
}

/* In Map mode the part under the pointer stays bright until one is picked. */
- (void)mouseMoved:(NSEvent *)evt
{
    if (!self.mapping || self.mapTarget >= 0) return;
    const int idx = [self hit:[self convertPoint:[evt locationInWindow] fromView:nil]];
    if (idx != _hover) {
        _hover = idx;
        [self setNeedsDisplay:YES];
    }
}

- (void)mouseExited:(NSEvent *)evt
{
    (void)evt;
    if (_hover >= 0) {
        _hover = -1;
        [self setNeedsDisplay:YES];
    }
}

/* The tooltip: the target's name and what is bound to it. */
- (NSString *)view:(NSView *)view stringForToolTip:(NSToolTipTag)tag point:(NSPoint)pnt userData:(void *)data
{
    (void)view;
    (void)tag;
    (void)data;
    const a7800_control *ctl = [self ctlAt:[self hit:pnt]];
    if (!ctl) return nil;
    int target = ctl->target;
    if (ctl->kind == A7800_CTL_STICK) {
        const NSRect rect = [self rectOf:ctl];
        target = directionTarget(ctl->port, (float)(pnt.x - NSMidX(rect)), (float)(pnt.y - NSMidY(rect)),
                                 (float)(NSWidth(rect) / 2));
        if (target < 0) return [NSString stringWithFormat:@"Player %d stick: drag to steer", ctl->port + 1];
    }
    if (target < 0) return nil;
    const a7800_binding bind = a7800session_binding_get(_session, target);
    NSString *key = bind.keysym ? keyName(bind.keysym) : @"none";
    NSString *padText = bind.button != A7800_PAD_BTN_NONE ? nsstr(a7800_pad_button_name(bind.button)) : @"none";
    return [NSString stringWithFormat:@"%@\nKey: %@    Gamepad: %@", nsstr(a7800_target_name(target)), key, padText];
}

/* One tooltip rect over the whole stage; the string is made per point. */
- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    [self removeAllToolTips];
    [self addToolTipRect:[self bounds] owner:self userData:NULL];
}
@end

@implementation A7800ControllersPanel
- (void)keyDown:(NSEvent *)e { [self.owner panelKeyDown:e]; }
- (void)keyUp:(NSEvent *)e { [self.owner panelKeyUp:e]; }
- (void)flagsChanged:(NSEvent *)e { [self.owner panelFlagsChanged:e]; }
/* Escape is a key of the console's (or cancels a Map pick), not "close the
 * panel". */
- (void)cancelOperation:(id)sender { (void)sender; }
@end

/* ---- the panel ------------------------------------------------------------------- */

static A7800ControllersWindow *g_singleton;

@implementation A7800ControllersWindow {
    a7800session *_session;
    A7800ControllersPanel *_panel;
    A7800CtlFlippedView *_content;
    A7800StageView *_stage;
    NSTextField *_typeLabel[A7800_PORTS];
    NSPopUpButton *_typePopup[A7800_PORTS];
    NSTextField *_padsTitle;
    NSTextField *_noPads;
    NSTextField *_padName[MAX_PAD_ROWS];
    NSTextField *_padEff[MAX_PAD_ROWS];     /* each pad's "drives player N" */
    NSPopUpButton *_padAssign[MAX_PAD_ROWS];
    int _padRows;                           /* the rows showing a pad */
    int _reservedRows;                      /* the rows the panel has room for */
    unsigned _padGeneration;
    NSButton *_mapButton;
    NSButton *_clearButton;
    NSButton *_defaultsButton;
    NSButton *_showKeysBox;
    NSPopUpButton *_mapList;                /* every target, the session's actions too */
    NSButton *_helpButton;
    NSTextField *_hint;
    NSTimer *_timer;
    int _slow;
    CGFloat _winW;
    uint32_t _swallowKey;                   /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a control, >= 0 waiting for a key or
     * gamepad button to bind to that target */
    int _mapState;
    BOOL _placed;
}

+ (A7800ControllersWindow *)instanceForSession:(a7800session *)session
{
    if (!g_singleton) g_singleton = [[A7800ControllersWindow alloc] initWithSession:session];
    return g_singleton;
}

+ (void)toggleWithSession:(a7800session *)session
{
    [[self instanceForSession:session] toggle];
}

+ (void)showWithSession:(a7800session *)session
{
    [[self instanceForSession:session] show];
}

+ (BOOL)isVisible
{
    return g_singleton && [g_singleton isVisible];
}

+ (BOOL)isActive
{
    return g_singleton && [g_singleton isActive];
}

+ (void)refresh
{
    if (g_singleton) [g_singleton refresh];
}

- (instancetype)initWithSession:(a7800session *)session
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _mapState = -2;
    _padGeneration = (unsigned)-1;
    _reservedRows = MIN_PAD_ROWS;
    _winW = MIN_WIN_W;

    _panel = [[A7800ControllersPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, MIN_WIN_W, 640)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_panel setTitle:@"Controllers (F9)"];
    A7800SetHelpContext(_panel, @"controllers");
    /* Floats above the machine's window, a full-screen one too. */
    [_panel setLevel:NSFloatingWindowLevel];
    [_panel setCollectionBehavior:NSWindowCollectionBehaviorFullScreenAuxiliary];
    [_panel setReleasedWhenClosed:NO];
    [_panel setBecomesKeyOnlyIfNeeded:NO];
    _panel.owner = self;
    [_panel setDelegate:self];

    _content = [[A7800CtlFlippedView alloc] initWithFrame:NSMakeRect(0, 0, MIN_WIN_W, 640)];
    [_content setAutoresizesSubviews:NO];
    [_panel setContentView:_content];

    _stage = [[A7800StageView alloc] initWithFrame:NSZeroRect];
    _stage.owner = self;
    _stage.session = _session;
    _stage.mapTarget = -1;
    _stage.showKeys = a7800session_get_int(_session, "controllers_show_bindings", 0) > 0;
    [_content addSubview:_stage];

    /* What is plugged into each port; Auto says what the game asked for. */
    for (int port = 0; port < A7800_PORTS; port++) {
        _typeLabel[port] = [NSTextField labelWithString:(port ? @"Player 2" : @"Player 1")];
        [_typeLabel[port] setFont:[NSFont boldSystemFontOfSize:12]];
        [_content addSubview:_typeLabel[port]];
        _typePopup[port] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        for (int type = 0; a7800_ctrl_type_name(type); type++)
            [_typePopup[port] addItemWithTitle:nsstr(a7800_ctrl_type_name(type))];
        _typePopup[port].tag = port;
        _typePopup[port].target = self;
        _typePopup[port].action = @selector(typeChanged:);
        [_typePopup[port] setRefusesFirstResponder:YES];
        [_content addSubview:_typePopup[port]];
    }

    /* Gamepads: one row per pad, its name, the player it drives and the one it
     * is set to; rows reserved, so pads coming and going keep the size. */
    _padsTitle = [NSTextField labelWithString:@"Gamepads"];
    [_padsTitle setFont:[NSFont boldSystemFontOfSize:12]];
    [_content addSubview:_padsTitle];
    _noPads = [NSTextField labelWithString:@"No gamepads connected - plug one in"];
    [_noPads setTextColor:[NSColor secondaryLabelColor]];
    [_content addSubview:_noPads];
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        _padName[row] = [NSTextField labelWithString:@""];
        [_padName[row] setFont:[NSFont systemFontOfSize:11]];
        [_padName[row] setLineBreakMode:NSLineBreakByTruncatingTail];
        [_content addSubview:_padName[row]];
        _padEff[row] = [NSTextField labelWithString:@""];
        [_padEff[row] setFont:[NSFont systemFontOfSize:11]];
        [_padEff[row] setTextColor:[NSColor secondaryLabelColor]];
        [_padEff[row] setAlignment:NSTextAlignmentRight];
        [_content addSubview:_padEff[row]];
        /* Automatic (connection order), then one item per player */
        _padAssign[row] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        [_padAssign[row] addItemsWithTitles:@[@"Automatic", @"Player 1", @"Player 2"]];
        _padAssign[row].tag = row;
        _padAssign[row].target = self;
        _padAssign[row].action = @selector(padAssignChanged:);
        [_padAssign[row] setRefusesFirstResponder:YES];
        [_content addSubview:_padAssign[row]];
    }

    _mapButton = [NSButton buttonWithTitle:@"Map" target:self action:@selector(toggleMap:)];
    [_mapButton setRefusesFirstResponder:YES];
    [_content addSubview:_mapButton];
    _clearButton = [NSButton buttonWithTitle:@"Clear" target:self action:@selector(clearBinding:)];
    [_clearButton setRefusesFirstResponder:YES];
    [_content addSubview:_clearButton];
    _defaultsButton = [NSButton buttonWithTitle:@"Defaults" target:self action:@selector(restoreDefaults:)];
    [_defaultsButton setRefusesFirstResponder:YES];
    [_content addSubview:_defaultsButton];
    _showKeysBox = [NSButton checkboxWithTitle:@"Show bindings" target:self action:@selector(toggleShowKeys:)];
    [_showKeysBox setState:_stage.showKeys ? NSControlStateValueOn : NSControlStateValueOff];
    [_showKeysBox setRefusesFirstResponder:YES];
    [_content addSubview:_showKeysBox];
    /* The Map list: every target by name, the session's own actions (Reboot to
     * CONFIG, Break) which have no part on the picture too. Items go in by the
     * menu, as equal titles would collapse. */
    _mapList = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    {
        NSMenuItem *none = [[_mapList menu] addItemWithTitle:@"Choose a control..." action:nil keyEquivalent:@""];
        none.tag = -1;
        for (int target = 0; target < A7800_TARGET_COUNT; target++) {
            NSMenuItem *item = [[_mapList menu] addItemWithTitle:nsstr(a7800_target_name(target)) action:nil
                                                   keyEquivalent:@""];
            item.tag = target;
        }
    }
    _mapList.target = self;
    _mapList.action = @selector(mapListChanged:);
    [_mapList setRefusesFirstResponder:YES];
    [_content addSubview:_mapList];
    _helpButton = A7800HelpButton(@"controllers");
    [_helpButton setRefusesFirstResponder:YES];
    [_content addSubview:_helpButton];

    _hint = [NSTextField wrappingLabelWithString:@""];
    [_hint setFont:[NSFont systemFontOfSize:[NSFont smallSystemFontSize]]];
    [_hint setTextColor:[NSColor secondaryLabelColor]];
    [_hint setMaximumNumberOfLines:2];
    [[_hint cell] setTruncatesLastVisibleLine:YES];
    [_content addSubview:_hint];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(occlusionChanged:)
                                                 name:NSWindowDidChangeOcclusionStateNotification
                                               object:_panel];

    [_stage configure];
    [self refreshTypes];
    [self setMapState:-2];
    [self relayout];
    return self;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_timer invalidate];
    [_panel setDelegate:nil];
}

/* ---- layout ------------------------------------------------------------------------- */

/* A new content size with the top-left corner where it was. */
- (void)fitPanel:(NSSize)size
{
    const NSRect frame = [_panel frame];
    NSRect content = [_panel contentRectForFrameRect:frame];
    if (NSEqualSizes(content.size, size)) return;
    content.size = size;
    NSRect next = [_panel frameRectForContentRect:content];
    next.origin.x = frame.origin.x;
    next.origin.y = NSMaxY(frame) - next.size.height;
    [_panel setFrame:next display:YES];
}

- (void)relayout
{
    const NSSize ssize = _stage.contentSize;
    _winW = ssize.width;
    const CGFloat inner = _winW - 2 * MARGIN;
    const CGFloat typeY = ssize.height + 10;
    const CGFloat padsY = typeY + ROW_H + 8;
    const CGFloat rowsY = padsY + TITLE_H;
    const CGFloat mapY = rowsY + _reservedRows * ROW_H + 6;
    const CGFloat hintY = mapY + BTN_H + 8;
    const CGFloat total = hintY + 34 + MARGIN;

    [self fitPanel:NSMakeSize(_winW, total)];
    [_content setFrameSize:NSMakeSize(_winW, total)];
    [_stage setFrame:NSMakeRect(0, 0, ssize.width, ssize.height)];
    /* a type popup under each port's half of the picture */
    for (int port = 0; port < A7800_PORTS; port++) {
        const CGFloat colX = MARGIN + port * (inner / 2);
        [_typeLabel[port] setFrame:NSMakeRect(colX, typeY + 4, 62, 18)];
        [_typePopup[port] setFrame:NSMakeRect(colX + 64, typeY - 1, TYPE_W, 25)];
    }
    [_padsTitle setFrame:NSMakeRect(MARGIN, padsY, inner, 16)];
    [_noPads setFrame:NSMakeRect(MARGIN, rowsY + 3, inner, 18)];
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        const CGFloat rowY = rowsY + row * ROW_H;
        [_padName[row] setFrame:NSMakeRect(MARGIN, rowY + 4, inner - POPUP_W - EFF_W - 12, 16)];
        [_padEff[row] setFrame:NSMakeRect(MARGIN + inner - POPUP_W - EFF_W - 6, rowY + 4, EFF_W, 16)];
        [_padAssign[row] setFrame:NSMakeRect(MARGIN + inner - POPUP_W, rowY - 1, POPUP_W, 25)];
    }
    [_mapButton setFrame:NSMakeRect(MARGIN, mapY, 56, BTN_H)];
    [_clearButton setFrame:NSMakeRect(MARGIN + 62, mapY, 56, BTN_H)];
    [_defaultsButton setFrame:NSMakeRect(MARGIN + 124, mapY, 72, BTN_H)];
    [_showKeysBox setFrame:NSMakeRect(MARGIN + 206, mapY + 3, 122, 22)];
    [_mapList setFrame:NSMakeRect(MARGIN + 336, mapY + 1, 200, 25)];
    [_helpButton setFrameOrigin:NSMakePoint(_winW - MARGIN - _helpButton.frame.size.width, mapY + 2)];
    [_hint setFrame:NSMakeRect(MARGIN, hintY, inner, 34)];
    [_stage setNeedsDisplay:YES];
}

/* Lets go of what the mouse holds in this panel (the pointer source only). */
- (void)letGo
{
    [_stage letGo];
    a7800session_release_pointer(_session);
}

/* The ports' controllers changed (a game asked for another, or Settings did):
 * the picture is read again; the panel does not change size. */
- (void)syncLayout
{
    if (![_stage layoutStale]) return;
    [self letGo];
    [_stage configure];
    [self refreshTypes];
    [_stage poll];
}

/* ---- the controller each port has --------------------------------------------------- */

/* The popups show what is set, and Auto what it resolved to for the running
 * game. */
- (void)refreshTypes
{
    for (int port = 0; port < A7800_PORTS; port++) {
        const int detected = a7800session_port_detected(_session, port);
        const char *now = a7800_ctrl_type_name(detected);
        NSString *title = [NSString stringWithFormat:@"Auto (now: %@)", now ? nsstr(now) : @"?"];
        NSMenuItem *autoItem = [_typePopup[port] itemAtIndex:A7800_CTRL_AUTO];
        if (![autoItem.title isEqualToString:title]) autoItem.title = title;
        const int type = a7800session_port_type(_session, port);
        if (type >= 0 && type < (int)_typePopup[port].numberOfItems && _typePopup[port].indexOfSelectedItem != type)
            [_typePopup[port] selectItemAtIndex:type];
    }
}

- (void)typeChanged:(NSPopUpButton *)sender
{
    a7800session_set_port_type(_session, (int)sender.tag, (int)sender.indexOfSelectedItem);
    [self refreshTypes];
    [self syncLayout];
}

/* ---- Map mode --------------------------------------------------------------------- */

- (NSString *)legendForTarget:(int)target
{
    return keyName(a7800session_binding_get(_session, target).keysym);
}

/* What is bound to a target, for the hint line. */
- (NSString *)bindingText:(int)target
{
    const a7800_binding bind = a7800session_binding_get(_session, target);
    NSString *key = bind.keysym ? keyName(bind.keysym) : @"no key";
    if (bind.button != A7800_PAD_BTN_NONE)
        return [NSString stringWithFormat:@"%@, pad %@", key, nsstr(a7800_pad_button_name(bind.button))];
    return key;
}

/* In Map mode nothing is lit and every control shows what is bound to it. */
- (void)refreshLabels
{
    _stage.mapping = _mapState != -2;
    _stage.mapTarget = _mapState >= 0 ? _mapState : -1;
    [_stage setNeedsDisplay:YES];
    [_stage poll];
}

- (void)setMapState:(int)state
{
    _mapState = state;
    if (state >= 0) {
        a7800session_gamepad_capture_begin(_session);
        _hint.stringValue = [NSString stringWithFormat:@"Press a key or a gamepad button for %@ (now: %@). Esc backs out.",
                             nsstr(a7800_target_name(state)), [self bindingText:state]];
    } else {
        a7800session_gamepad_capture_cancel(_session);
        _hint.stringValue = state == -1
            ? @"Click a stick direction, a button, a console key or a slide to remap it, or choose one from the list."
            : @"Hold a button or a key, drag a stick, or click a slide. Keys and gamepads work here too.";
    }
    if (state != -2) {
        [self letGo];
        a7800session_release_all(_session);   /* Map's key-ups must not strand a held input */
    }
    [_mapButton setTitle:(state == -2 ? @"Map" : @"Done")];
    setButtonAccent(_mapButton, state != -2);
    _clearButton.enabled = state >= 0;
    _mapList.enabled = state != -2;
    if (![_mapList selectItemWithTag:(state >= 0 ? state : -1)]) [_mapList selectItemAtIndex:0];
    [self refreshLabels];
}

/* A control clicked on the picture in Map mode. */
- (void)mapPick:(int)target
{
    if (_mapState != -2 && target >= 0 && target < A7800_TARGET_COUNT) [self setMapState:target];
}

- (void)mapListChanged:(NSPopUpButton *)sender
{
    const NSInteger tag = sender.selectedItem.tag;
    if (tag >= 0) [self mapPick:(int)tag];
    else if (_mapState >= 0) [self setMapState:-1];
}

- (void)showBound:(NSString *)name target:(int)target stolen:(const char *)stolen
{
    NSString *what = nsstr(a7800_target_name(target));
    _hint.stringValue = stolen && stolen[0]
        ? [NSString stringWithFormat:@"Bound %@ to %@ (was %@)", name, what, nsstr(stolen)]
        : [NSString stringWithFormat:@"Bound %@ to %@", name, what];
}

- (void)bindKey:(uint32_t)keysym swallow:(BOOL)swallow
{
    char stolen[128];
    stolen[0] = '\0';
    const int target = _mapState;
    a7800session_binding_set_key(_session, target, keysym, stolen, (int)sizeof stolen);
    if (swallow) _swallowKey = keysym;
    [self setMapState:-1];   /* stay armed: remapping several in a row is normal */
    [self showBound:keyName(keysym) target:target stolen:stolen];
}

- (void)pollCapture
{
    int button = A7800_PAD_BTN_NONE;
    char stolen[128];
    if (_mapState < 0 || !a7800session_gamepad_capture_poll(_session, &button)) return;
    stolen[0] = '\0';
    const int target = _mapState;
    a7800session_binding_set_button(_session, target, button, stolen, (int)sizeof stolen);
    [self setMapState:-1];
    [self showBound:nsstr(a7800_pad_button_name(button)) target:target stolen:stolen];
}

- (void)toggleMap:(id)sender
{
    (void)sender;
    [self setMapState:(_mapState == -2 ? -1 : -2)];
}

/* Clear: the picked control loses its key and its gamepad button. */
- (void)clearBinding:(id)sender
{
    (void)sender;
    if (_mapState < 0) return;
    const int target = _mapState;
    a7800session_binding_clear(_session, target);
    [self setMapState:-1];
    _hint.stringValue = [NSString stringWithFormat:@"Cleared %@", nsstr(a7800_target_name(target))];
}

- (void)restoreDefaults:(id)sender
{
    (void)sender;
    a7800session_bindings_reset(_session);
    for (int pad = 0, count = a7800session_gamepad_count(_session); pad < count; pad++)
        a7800session_gamepad_assign(_session, pad, -1);
    [self refreshPads];
    [self refreshLabels];
    _hint.stringValue = @"Default bindings restored";
}

- (void)toggleShowKeys:(id)sender
{
    (void)sender;
    const BOOL show = [_showKeysBox state] == NSControlStateValueOn;
    a7800session_set_int(_session, "controllers_show_bindings", show ? 1 : 0);
    _stage.showKeys = show;
    [_stage setNeedsDisplay:YES];
}

/* ---- gamepads ----------------------------------------------------------------------- */

- (void)refreshPadPorts
{
    for (int row = 0; row < _padRows; row++) {
        const int port = a7800session_gamepad_effective_port(_session, row);
        NSString *text = port >= 0 ? [NSString stringWithFormat:@"drives player %d", port + 1] : @"drives nothing";
        if (![_padEff[row].stringValue isEqualToString:text]) _padEff[row].stringValue = text;
    }
}

/* Rebuilt whenever the pads' generation changes (one added or removed). */
- (void)refreshPads
{
    const int count = a7800session_gamepad_count(_session);
    _padGeneration = a7800session_gamepad_generation(_session);
    _padRows = count < MAX_PAD_ROWS ? (count > 0 ? count : 0) : MAX_PAD_ROWS;
    /* room for the pads there are (two rows to start with, more when more pads
     * are connected); the panel changes size only then */
    const int rooms = _padRows > MIN_PAD_ROWS ? _padRows : MIN_PAD_ROWS;
    if (rooms != _reservedRows) {
        _reservedRows = rooms;
        [self relayout];
    }
    _noPads.hidden = count > 0;
    _padsTitle.stringValue = count > MAX_PAD_ROWS
        ? [NSString stringWithFormat:@"Gamepads (%d of %d shown)", MAX_PAD_ROWS, count]
        : @"Gamepads";
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        const BOOL shown = row < _padRows;
        _padName[row].hidden = !shown;
        _padEff[row].hidden = !shown;
        _padAssign[row].hidden = !shown;
        if (!shown) continue;
        char name[128];
        name[0] = '\0';
        a7800session_gamepad_name(_session, row, name, (int)sizeof name);
        _padName[row].stringValue = nsstr(name);
        _padName[row].toolTip = nsstr(name);
        /* the pad's own player, or Automatic when it has none */
        const int assign = a7800session_gamepad_assignment(_session, row);
        [_padAssign[row] selectItemAtIndex:(assign < 0 || assign >= A7800_PORTS ? 0 : assign + 1)];
    }
    [self refreshPadPorts];
}

- (void)padAssignChanged:(NSPopUpButton *)sender
{
    const int sel = (int)sender.indexOfSelectedItem;
    a7800session_gamepad_assign(_session, (int)sender.tag, sel <= 0 ? -1 : sel - 1);
    [self refreshPadPorts];
}

/* ---- the poll ----------------------------------------------------------------------- */

/* 30 Hz while the panel can be seen: what is held and the gamepad capture;
 * every 250 ms the hot-plugged pads, auto-assignment and what Auto resolved to. */
- (void)tick
{
    if (_mapState >= 0) [self pollCapture];
    if (++_slow >= SLOW_TICKS) {
        _slow = 0;
        [self syncLayout];
        [self refreshTypes];
        if (a7800session_gamepad_generation(_session) != _padGeneration) [self refreshPads];
        else [self refreshPadPorts];
    }
    [_stage poll];
}

/* The timer runs only while the panel is on screen and not occluded or
 * hidden with its app. */
- (void)updateTimer
{
    const BOOL want = [_panel isVisible] && ([_panel occlusionState] & NSWindowOcclusionStateVisible);
    if (want && !_timer) {
        __weak A7800ControllersWindow *weakSelf = self;
        _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 repeats:YES block:^(NSTimer *tmr) {
            (void)tmr;
            [weakSelf tick];
        }];
        _timer.tolerance = 0.005;
    } else if (!want && _timer) {
        [_timer invalidate];
        _timer = nil;
    }
}

- (void)occlusionChanged:(NSNotification *)note
{
    (void)note;
    [self updateTimer];
}

/* ---- the keyboard ------------------------------------------------------------------- */

- (void)panelKeyDown:(NSEvent *)e
{
    if ([e modifierFlags] & NSEventModifierFlagCommand) return;
    const uint32_t ks = A7800KeysymFromEvent(e);
    if (!ks) return;
    /* the keys the app owns: F9 toggles this panel, F11 is full screen, F12 the debugger */
    if (ks == A7800_KEYSYM_F9 || ks == A7800_KEYSYM_F11 || ks == A7800_KEYSYM_F12) {
        if ([e isARepeat]) return;
        if (ks == A7800_KEYSYM_F9) [self toggle];
        else if (ks == A7800_KEYSYM_F11) [[NSApp mainWindow] toggleFullScreen:nil];
        else [A7800DebuggerWindow toggleForSession:_session];
        return;
    }
    if (_swallowKey && ks == _swallowKey) return;
    if ([e isARepeat]) return;
    if (_mapState >= 0) {
        /* the next key is the binding; Esc backs out of the pick; a Ctrl chord
         * and the app's keys are not bindings */
        if ([e modifierFlags] & NSEventModifierFlagControl) return;
        if (a7800session_key_reserved(ks)) return;
        if (ks == A7800_KEYSYM_ESCAPE) {
            [self setMapState:-1];
            _swallowKey = ks;
        } else {
            [self bindKey:ks swallow:YES];
        }
        return;
    }
    if (_mapState == -1) return;    /* armed: typing does nothing */
    /* a key bound to a system action goes to the app's queue, as a gamepad's does */
    const int sysact = a7800session_key_sysaction(_session, ks);
    if (sysact >= 0) {
        a7800session_sysaction_post(_session, sysact);
        return;
    }
    a7800session_key(_session, ks, 1);
}

- (void)panelKeyUp:(NSEvent *)e
{
    const uint32_t ks = A7800KeysymFromEvent(e);
    if (_swallowKey && ks == _swallowKey) {
        _swallowKey = 0;
        return;
    }
    if (_mapState != -2 || !ks) return;
    a7800session_key(_session, ks, 0);
}

/* Shift, Control and Option bind too (Command is the menus'); otherwise they are
 * the console's. A release always goes through. */
- (void)panelFlagsChanged:(NSEvent *)e
{
    int down = 0;
    const uint32_t ks = A7800KeysymFromFlagsChange(e, &down);
    if (!ks) return;
    if (_mapState >= 0) {
        /* a modifier pressed while a pick waits is the binding */
        if (down) [self bindKey:ks swallow:NO];
        return;
    }
    if (_mapState == -1) return;
    a7800session_key(_session, ks, down);
}

/* ---- the window --------------------------------------------------------------------- */

/* Hidden: Map disarmed, nothing held, no polling. */
- (void)quiesce
{
    [self setMapState:-2];
    [self letGo];
    a7800session_release_all(_session);
    _swallowKey = 0;
    [_timer invalidate];
    _timer = nil;
}

- (void)show
{
    [_stage configure];
    [self refreshTypes];
    [self refreshPads];
    [self setMapState:-2];
    [self relayout];
    if (!_placed) {
        [_panel center];
        _placed = YES;
    }
    [_panel makeFirstResponder:nil];    /* the panel itself takes the keys */
    [_panel makeKeyAndOrderFront:nil];
    [self updateTimer];
}

- (void)hide
{
    [_panel orderOut:nil];
    [self quiesce];
}

- (void)toggle
{
    if ([self isVisible]) [self hide];
    else [self show];
}

- (BOOL)isVisible
{
    return [_panel isVisible];
}

- (BOOL)isActive
{
    return [_panel isKeyWindow];
}

/* The bindings or the gamepads changed elsewhere (Settings, a hot-plug): read
 * again. */
- (void)refresh
{
    [self syncLayout];
    [self refreshTypes];
    [self refreshPads];
    [self refreshLabels];
}

/* The panel's mouse and keyboard holds: the main window releases its own. */
- (void)windowDidResignKey:(NSNotification *)note
{
    (void)note;
    [self letGo];
    a7800session_release_all(_session);
    _swallowKey = 0;    /* its release goes elsewhere */
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [self quiesce];
}
@end
