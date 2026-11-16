/*
 * DisplayGlue.h -- what DisplayView.m needs to know about THIS machine: the
 * session's functions, the frame size and the shape of the picture.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "a7800session.h"

#define FN_VIEW      A7800DisplayView
#define FN_SESSION   a7800session
#define FN_FB_W      A7800SESSION_FB_WIDTH
#define FN_FB_H      A7800SESSION_FB_MAX_HEIGHT
#define FN_INITIAL_W 960
#define FN_INITIAL_H 720

/* The light gun: the pointer over the picture (DisplayExtras.inc). */
#define FN_EXTRA_IVARS \
    unsigned _buttons;      /* bit 0 left, bit 1 right, as the session wants them */ \
    BOOL _aiming;           /* the pointer was last sent over the picture */
#define FN_EXTRA_METHODS "DisplayExtras.inc"

static inline int fn_machine_hz(FN_SESSION *s) { return a7800session_refresh_rate(s); }

static inline void fn_notify_vsync(FN_SESSION *s, int64_t ns)
{
    a7800session_notify_vsync(s, ns);
}

static inline int fn_pull_frame(FN_SESSION *s, uint32_t *dst, int *lines, uint64_t *serial)
{
    return a7800session_copy_frame(s, dst, lines, serial);
}

/* A TV showed MARIA's 320 pixels by 224 (NTSC) or 260 (PAL) lines at 4:3
 * whatever the line count; square pixels show them as they are. */
static inline double fn_picture_aspect(FN_SESSION *s, int lines, BOOL tv)
{
    (void)s;
    return tv ? (4.0 / 3.0) : (double)FN_FB_W / (double)lines;
}
