/*
 * DisplayWidget -- see DisplayWidget.h.
 *
 * A QTimer rather than a frame-clock callback: Qt Widgets has no equivalent
 * of GdkFrameClock. The timer runs a little faster than the machine so no
 * frame waits a whole period, and the session's own wall-clock pacing does
 * the real work -- notify_vsync is still fed, so the phase lock engages
 * whenever the ticks happen to be steady.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DisplayWidget.h"

#include <QPainter>
#include <chrono>
#include <cstring>

DisplayWidget::DisplayWidget(nessession *session, QWidget *parent)
    : QWidget(parent), m_session(session)
{
    m_fb.resize(NESSESSION_FB_WIDTH * NESSESSION_FB_MAX_HEIGHT);
    setMinimumSize(292, 240);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);

    connect(&m_timer, &QTimer::timeout, this, &DisplayWidget::tick);
    /* ~120 Hz: comfortably above the machine's 60 so a finished frame is
     * never held back by the poll interval. */
    m_timer.start(8);
}

void DisplayWidget::tick()
{
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    nessession_notify_vsync(
        m_session,
        (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());

    int h = 0;
    if (!nessession_copy_frame(m_session, m_fb.data(), &h, &m_serial))
        return;
    if (h <= 0 || h > NESSESSION_FB_MAX_HEIGHT) return;

    /* The session's pixels are 0x00RRGGBB, which is exactly
     * QImage::Format_RGB32 (the alpha byte is ignored); a straight copy. */
    if (m_height != h) {
        m_height = h;
        m_image = QImage(NESSESSION_FB_WIDTH, h, QImage::Format_RGB32);
    }
    std::memcpy(m_image.bits(), m_fb.data(), (size_t)NESSESSION_FB_WIDTH * h * 4);
    update();
}

void DisplayWidget::setAspect(int aspect) { m_aspect = aspect; update(); }
void DisplayWidget::setSmooth(bool smooth) { m_smooth = smooth; update(); }

void DisplayWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (m_height <= 0) return;

    /* On a television each NES pixel is 8:7 (wider than tall), so the
     * 256x240 frame shows as about 292x240; square pixels show the raw
     * buffer. */
    const double par = m_aspect == 0 ? 8.0 / 7.0 : 1.0;
    const double want = (double)NESSESSION_FB_WIDTH * par / (double)m_height;
    double w = width(), h = height(), sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }

    p.setRenderHint(QPainter::SmoothPixmapTransform, m_smooth);
    p.drawImage(QRectF((w - sw) / 2.0, (h - sh) / 2.0, sw, sh), m_image);
}
