/*
 * Where the picture goes inside the display, shared by the GPU and the
 * software display.
 *
 * On a television each NES pixel is 8:7 (wider than tall), so the 256x240
 * frame shows as about 292x240; square pixels show the raw buffer.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QRectF>
#include <QSizeF>

#include "nessession.h"

inline QRectF displayRect(QSizeF avail, int lines, bool tvAspect)
{
    const double par = tvAspect ? 8.0 / 7.0 : 1.0;
    const double want = (double)NESSESSION_FB_WIDTH * par / (double)lines;
    const double w = avail.width(), h = avail.height();
    double sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }
    return QRectF((w - sw) / 2.0, (h - sh) / 2.0, sw, sh);
}
