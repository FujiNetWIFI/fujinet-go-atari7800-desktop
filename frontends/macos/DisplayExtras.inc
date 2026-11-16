/*
 * DisplayExtras.inc -- the light gun, included into the display view's
 * @implementation (see DisplayGlue.h).
 *
 * While a port has a light gun, the pointer over the picture aims it: a
 * crosshair cursor, and every move and click is handed to the session in
 * frame pixels (the left button pulls the trigger, the right one shoots
 * off-screen). Otherwise the mouse does nothing here.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* The light gun's pointer: moves, and the cursor over the picture. */
- (void)extraInit
{
    [self addTrackingArea:[[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:(NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                      NSTrackingCursorUpdate | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect)
               owner:self
            userInfo:nil]];
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
