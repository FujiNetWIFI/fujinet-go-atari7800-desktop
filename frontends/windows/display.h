/*
 * The emulator picture -- see display.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef A7800_WIN_DISPLAY_H
#define A7800_WIN_DISPLAY_H

#include <windows.h>

#include "a7800session.h"

/* Creates the picture as a child of parent (size it with MoveWindow) and
 * starts its render thread. One per process. */
HWND a7800_display_create(HWND parent, a7800session *session);
/* Stops the render thread; call before the parent is destroyed. */
void a7800_display_destroy(void);
/* aspect: 0 the television's 4:3, 1 square pixels; smooth:
 * bilinear (else sharp pixels). Safe from any thread. */
void a7800_display_set_options(int aspect, int smooth);
/* Where the picture was last drawn (client coordinates) and its line count,
 * for mapping the pointer back to frame pixels. 0 before the first frame. */
int a7800_display_picture(RECT *r, int *fbh);
/* "Direct3D 11", "Direct3D 11 (WARP)" or "GDI": what is drawing. */
const char *a7800_display_renderer(void);

#endif
