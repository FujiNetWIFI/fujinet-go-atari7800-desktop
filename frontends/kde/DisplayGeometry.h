/*
 * Where the picture goes inside the display, shared by the GPU and the
 * software display.
 *
 * A television showed MARIA's frame at 4:3, NTSC or PAL alike, as MAME does.
 * Square pixels show the raw 320-pixel lines.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QRectF>
#include <QSizeF>

#include "a7800session.h"

inline QRectF displayRect(QSizeF avail, int lines, bool tvAspect)
{
    const double want = tvAspect ? 4.0 / 3.0
                                 : (double)A7800SESSION_FB_WIDTH / (double)lines;
    const double w = avail.width(), h = avail.height();
    double sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }
    return QRectF((w - sw) / 2.0, (h - sh) / 2.0, sw, sh);
}
