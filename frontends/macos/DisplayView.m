/*
 * A7800DisplayView -- the framebuffer, on a CVDisplayLink, and the pointer
 * over it for the light gun.
 *
 * CVDisplayLink is this platform's frame clock -- the equivalent of
 * GdkFrameClock and DwmFlush -- and feeding it to the session is what lets
 * the emulator phase-lock to the panel instead of beating against it.
 *
 * Its callback runs on its OWN high-priority thread, not the main one. So it
 * does the two cheap things (hand over the tick, pull the frame) and then
 * asks AppKit to redraw on the main thread. Drawing from the callback thread
 * would be a use of AppKit off the main thread, which is undefined.
 *
 * While a port has a light gun, the pointer over the picture aims it: a
 * crosshair cursor, and every move and click is handed to the session in
 * frame pixels (the left button pulls the trigger, the right one shoots
 * off-screen). Otherwise the mouse does nothing here.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DisplayView.h"

#import <CoreVideo/CoreVideo.h>

/* CoreVideo hands out host time in mach_absolute_time units, which are NOT
 * nanoseconds on every machine -- the timebase ratio converts them. */
#include <mach/mach_time.h>

#include <stdlib.h>
#include <string.h>

@implementation A7800DisplayView {
    a7800session *_session;
    CVDisplayLinkRef _link;
    uint32_t *_fb;
    int _height;
    uint64_t _serial;
    CGContextRef _ctx;
    CGColorSpaceRef _cs;
    BOOL _tv;
    BOOL _smooth;
    unsigned _buttons;          /* bit 0 left, bit 1 right, as the session wants them */
    BOOL _aiming;               /* the pointer was last sent over the picture */
}

static CVReturn displayCallback(CVDisplayLinkRef link, const CVTimeStamp *now,
                                const CVTimeStamp *out, CVOptionFlags flagsIn,
                                CVOptionFlags *flagsOut, void *ctx)
{
    (void)link; (void)out; (void)flagsIn; (void)flagsOut;
    A7800DisplayView *self = (__bridge A7800DisplayView *)ctx;
    [self tick:now->hostTime];
    return kCVReturnSuccess;
}

- (instancetype)initWithSession:(a7800session *)session
{
    self = [super initWithFrame:NSMakeRect(0, 0, 960, 720)];
    if (!self) return nil;
    _session = session;
    _tv = YES;
    _smooth = NO;

    const size_t n = (size_t)A7800SESSION_FB_WIDTH * A7800SESSION_FB_MAX_HEIGHT;
    _fb = calloc(n, sizeof *_fb);
    _cs = CGColorSpaceCreateDeviceRGB();
    /* The session's pixels are 0x00RRGGBB in host order: a little-endian
     * 32-bit context with the alpha byte skipped reads them as they are. */
    _ctx = CGBitmapContextCreate(_fb, A7800SESSION_FB_WIDTH, A7800SESSION_FB_MAX_HEIGHT, 8,
                                 A7800SESSION_FB_WIDTH * 4, _cs,
                                 kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);

    /* The light gun's pointer: moves, and the cursor over the picture. */
    [self addTrackingArea:[[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:(NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                      NSTrackingCursorUpdate | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect)
               owner:self
            userInfo:nil]];

    CVDisplayLinkCreateWithActiveCGDisplays(&_link);
    CVDisplayLinkSetOutputCallback(_link, displayCallback, (__bridge void *)self);
    CVDisplayLinkStart(_link);
    return self;
}

- (void)stop
{
    if (_link) {
        CVDisplayLinkStop(_link);
        CVDisplayLinkRelease(_link);
        _link = NULL;
    }
}

- (void)dealloc
{
    [self stop];
    if (_ctx) CGContextRelease(_ctx);
    if (_cs) CGColorSpaceRelease(_cs);
    free(_fb);
}

- (void)tick:(uint64_t)hostTime
{
    static double toNs = 0.0;
    if (toNs == 0.0) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        toNs = (double)tb.numer / (double)tb.denom;
    }
    a7800session_notify_vsync(_session, (int64_t)((double)hostTime * toNs));

    int h = 0;
    if (!a7800session_copy_frame(_session, _fb, &h, &_serial)) return;
    _height = h;
    /* AppKit is main-thread only; the display link's callback is not. */
    dispatch_async(dispatch_get_main_queue(), ^{ [self setNeedsDisplay:YES]; });
}

- (void)setTvAspect:(BOOL)tv { _tv = tv; [self setNeedsDisplay:YES]; }
- (void)setSmooth:(BOOL)smooth { _smooth = smooth; [self setNeedsDisplay:YES]; }

- (BOOL)isOpaque { return YES; }

/* Where the picture is drawn, letterboxed: a TV showed MARIA's 320 pixels
 * by 224 (NTSC) or 260 (PAL) lines at 4:3 whatever the line count; square
 * pixels show them as they are. */
- (NSRect)pictureRect
{
    const NSRect b = [self bounds];
    if (_height <= 0) return NSZeroRect;
    const double want = _tv ? (4.0 / 3.0) : (double)A7800SESSION_FB_WIDTH / (double)_height;
    const double w = b.size.width, h = b.size.height;
    double sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }
    return NSMakeRect((w - sw) / 2, (h - sh) / 2, sw, sh);
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    CGContextRef dc = [[NSGraphicsContext currentContext] CGContext];
    const NSRect b = [self bounds];

    CGContextSetRGBFillColor(dc, 0, 0, 0, 1);
    CGContextFillRect(dc, b);
    if (_height <= 0) return;

    CGImageRef whole = CGBitmapContextCreateImage(_ctx);
    if (!whole) return;
    /* Only the lines MARIA produced (224 NTSC, 260 PAL). */
    CGImageRef img = CGImageCreateWithImageInRect(whole, CGRectMake(0, 0, A7800SESSION_FB_WIDTH, _height));
    CGImageRelease(whole);
    if (!img) return;

    CGContextSetInterpolationQuality(dc, _smooth ? kCGInterpolationHigh : kCGInterpolationNone);
    CGContextDrawImage(dc, NSRectToCGRect([self pictureRect]), img);
    CGImageRelease(img);
}

/* ---- the light gun ------------------------------------------------------------ */

/* A point in the window, in frame pixels; NO when it is off the picture. */
- (BOOL)framePoint:(NSPoint)inWindow x:(int *)x y:(int *)y
{
    const NSPoint p = [self convertPoint:inWindow fromView:nil];
    const NSRect r = [self pictureRect];
    *x = 0;
    *y = 0;
    if (r.size.width <= 0 || r.size.height <= 0 || !NSPointInRect(p, r)) return NO;
    /* the view's y grows upwards; the frame's first line is at the top */
    int fx = (int)((p.x - r.origin.x) / r.size.width * A7800SESSION_FB_WIDTH);
    int fy = (int)((NSMaxY(r) - p.y) / r.size.height * _height);
    if (fx < 0) fx = 0;
    if (fx > A7800SESSION_FB_WIDTH - 1) fx = A7800SESSION_FB_WIDTH - 1;
    if (fy < 0) fy = 0;
    if (fy > _height - 1) fy = _height - 1;
    *x = fx;
    *y = fy;
    return YES;
}

- (void)aimAt:(NSPoint)inWindow
{
    if (!a7800session_lightgun_active(_session)) {
        [[NSCursor arrowCursor] set];
        if (_aiming) {
            a7800session_pointer(_session, 0, 0, 0, 0);
            _aiming = NO;
        }
        return;
    }
    int x, y;
    const BOOL inside = [self framePoint:inWindow x:&x y:&y];
    [(inside ? [NSCursor crosshairCursor] : [NSCursor arrowCursor]) set];
    a7800session_pointer(_session, x, y, inside ? 1 : 0, _buttons);
    _aiming = inside;
}

- (void)aim:(NSEvent *)e { [self aimAt:[e locationInWindow]]; }

/* A cursor-update event need not carry a location: ask the window. */
- (void)cursorUpdate:(NSEvent *)e
{
    (void)e;
    [self aimAt:[[self window] mouseLocationOutsideOfEventStream]];
}
- (void)mouseMoved:(NSEvent *)e { [self aim:e]; }
- (void)mouseDragged:(NSEvent *)e { [self aim:e]; }
- (void)rightMouseDragged:(NSEvent *)e { [self aim:e]; }

- (void)mouseDown:(NSEvent *)e { _buttons |= 1u; [self aim:e]; }
- (void)mouseUp:(NSEvent *)e { _buttons &= ~1u; [self aim:e]; }
- (void)rightMouseDown:(NSEvent *)e { _buttons |= 2u; [self aim:e]; }
- (void)rightMouseUp:(NSEvent *)e { _buttons &= ~2u; [self aim:e]; }

- (void)mouseExited:(NSEvent *)e
{
    (void)e;
    [[NSCursor arrowCursor] set];
    /* A drag that leaves the view keeps its button until the mouseUp, which
     * AppKit still delivers here; only a plain exit lets go. */
    if (_buttons) return;
    a7800session_pointer(_session, 0, 0, 0, 0);
    _aiming = NO;
}
@end
