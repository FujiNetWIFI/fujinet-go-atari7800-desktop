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

#include <QMouseEvent>
#include <QPainter>
#include <chrono>
#include <cstring>

DisplayWidget::DisplayWidget(a7800session *session, QWidget *parent)
    : QWidget(parent), m_session(session)
{
    m_fb.resize(A7800SESSION_FB_WIDTH * A7800SESSION_FB_MAX_HEIGHT);
    setMinimumSize(320, 224);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);
    /* motion without a button held aims a light gun */
    setMouseTracking(true);

    connect(&m_timer, &QTimer::timeout, this, &DisplayWidget::tick);
    /* ~120 Hz: comfortably above the machine's 60 so a finished frame is
     * never held back by the poll interval. */
    m_timer.start(8);
}

void DisplayWidget::tick()
{
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    a7800session_notify_vsync(
        m_session,
        (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());

    /* a light gun comes and goes with the controller type (and with AUTO,
     * with the game): the cursor follows */
    const bool gun = a7800session_lightgun_active(m_session) != 0;
    if (gun != m_gun) {
        m_gun = gun;
        if (gun) setCursor(Qt::CrossCursor);
        else {
            unsetCursor();
            a7800session_pointer(m_session, 0, 0, 0, 0);
        }
    }

    int h = 0;
    if (!a7800session_copy_frame(m_session, m_fb.data(), &h, &m_serial))
        return;
    if (h <= 0 || h > A7800SESSION_FB_MAX_HEIGHT) return;

    /* The session's pixels are 0x00RRGGBB, which is exactly
     * QImage::Format_RGB32 (the alpha byte is ignored); a straight copy. */
    if (m_height != h) {
        m_height = h;
        m_image = QImage(A7800SESSION_FB_WIDTH, h, QImage::Format_RGB32);
    }
    std::memcpy(m_image.bits(), m_fb.data(), (size_t)A7800SESSION_FB_WIDTH * h * 4);
    update();
}

void DisplayWidget::setAspect(int aspect) { m_aspect = aspect; update(); }
void DisplayWidget::setSmooth(bool smooth) { m_smooth = smooth; update(); }

/* Where the picture is drawn: letterboxed, at 4:3 (a television's picture
 * -- MARIA's pixels are a little wider than tall) or with square pixels. */
QRectF DisplayWidget::pictureRect() const
{
    if (m_height <= 0) return QRectF();
    const double want = m_aspect == 0 ? 4.0 / 3.0
                                      : (double)A7800SESSION_FB_WIDTH / (double)m_height;
    const double w = width(), h = height();
    double sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }
    return QRectF((w - sw) / 2.0, (h - sh) / 2.0, sw, sh);
}

void DisplayWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (m_height <= 0) return;
    p.setRenderHint(QPainter::SmoothPixmapTransform, m_smooth);
    p.drawImage(pictureRect(), m_image);
}

/* Widget coordinates to frame pixels; bit 0 the primary button (fires every
 * light gun), bit 1 the secondary (fires off-screen, a reload in some
 * games). */
void DisplayWidget::sendPointer(const QPointF &pos, Qt::MouseButtons buttons)
{
    if (!m_gun) return;
    const QRectF r = pictureRect();
    if (r.isEmpty()) return;
    const int x = (int)((pos.x() - r.x()) * A7800SESSION_FB_WIDTH / r.width());
    const int y = (int)((pos.y() - r.y()) * m_height / r.height());
    const int inside = r.contains(pos) ? 1 : 0;
    unsigned b = 0;
    if (buttons & Qt::LeftButton) b |= 1u;
    if (buttons & Qt::RightButton) b |= 2u;
    a7800session_pointer(m_session, qBound(0, x, A7800SESSION_FB_WIDTH - 1),
                         qBound(0, y, m_height - 1), inside, b);
}

void DisplayWidget::mouseMoveEvent(QMouseEvent *e)
{
    sendPointer(e->position(), e->buttons());
    QWidget::mouseMoveEvent(e);
}

void DisplayWidget::mousePressEvent(QMouseEvent *e)
{
    sendPointer(e->position(), e->buttons());
    QWidget::mousePressEvent(e);
}

void DisplayWidget::mouseReleaseEvent(QMouseEvent *e)
{
    sendPointer(e->position(), e->buttons());
    QWidget::mouseReleaseEvent(e);
}

void DisplayWidget::leaveEvent(QEvent *e)
{
    if (m_gun) a7800session_pointer(m_session, 0, 0, 0, 0);
    QWidget::leaveEvent(e);
}
