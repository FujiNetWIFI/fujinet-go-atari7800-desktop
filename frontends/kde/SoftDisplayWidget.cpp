/*
 * SoftDisplayWidget -- see SoftDisplayWidget.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SoftDisplayWidget.h"

#include "DisplayGeometry.h"

#include <QEnterEvent>
#include <QEvent>
#include <QMouseEvent>
#include <QPainter>

SoftDisplayWidget::SoftDisplayWidget(a7800session *session, QWidget *parent)
    : QWidget(parent), m_session(session), m_feed(new FrameFeed(session, this)),
      m_scratch(A7800SESSION_FB_WIDTH, A7800SESSION_FB_MAX_HEIGHT, QImage::Format_RGB32)
{
    m_scratch.fill(0);
    setMinimumSize(320, 224);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);
    setMouseTracking(true);
    connect(m_feed, &FrameFeed::frameReady, this, &SoftDisplayWidget::onFrame);
}

SoftDisplayWidget::~SoftDisplayWidget()
{
    m_feed->setActive(false);
}

void SoftDisplayWidget::setAspect(int aspect) { m_aspect = aspect; update(); }
void SoftDisplayWidget::setSmooth(bool smooth) { m_smooth = smooth; update(); }

void SoftDisplayWidget::updateActive()
{
    m_feed->setActive(isVisible() && window() && !window()->isMinimized());
}

void SoftDisplayWidget::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (window())
        window()->installEventFilter(this);
    updateActive();
}

void SoftDisplayWidget::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    if (window())
        window()->removeEventFilter(this);
    updateActive();
}

bool SoftDisplayWidget::eventFilter(QObject *watched, QEvent *e)
{
    if (e->type() == QEvent::WindowStateChange || e->type() == QEvent::Show ||
        e->type() == QEvent::Hide)
        updateActive();
    return QWidget::eventFilter(watched, e);
}

void SoftDisplayWidget::onFrame()
{
    int h = 0;
    syncGun();
    if (!a7800session_copy_frame(m_session, reinterpret_cast<uint32_t *>(m_scratch.bits()),
                               &h, &m_serial))
        return;
    if (h <= 0 || h > A7800SESSION_FB_MAX_HEIGHT)
        return;
    m_height = h;
    /* The pixels are 0x00RRGGBB; Format_RGB32 needs 0xFFRRGGBB, or KWin
     * composites the zero alpha as see-through, so force alpha on. */
    m_image = m_scratch.copy(0, 0, A7800SESSION_FB_WIDTH, h);
    quint32 *px = reinterpret_cast<quint32 *>(m_image.bits());
    const size_t n = (size_t)A7800SESSION_FB_WIDTH * h;
    for (size_t i = 0; i < n; i++)
        px[i] |= 0xFF000000u;
    update();
}

/* ---- the light gun ----------------------------------------------------------
 * A gun comes and goes with the controller type (and with AUTO, with the
 * game). There is no polling timer: the cursor follows on each new frame and
 * whenever the mouse is about, which is when it matters. */

void SoftDisplayWidget::syncGun()
{
    const bool gun = a7800session_lightgun_active(m_session) != 0;
    if (gun == m_gun)
        return;
    m_gun = gun;
    if (gun) setCursor(Qt::CrossCursor);
    else {
        unsetCursor();
        a7800session_pointer(m_session, 0, 0, 0, 0);
    }
}

/* Widget coordinates to frame pixels; bit 0 the primary button (fires every
 * light gun), bit 1 the secondary (fires off-screen, a reload in some
 * games). */
void SoftDisplayWidget::sendPointer(const QPointF &pos, Qt::MouseButtons buttons)
{
    syncGun();
    if (!m_gun || m_height <= 0) return;
    const QRectF r = displayRect(QSizeF(size()), m_height, m_aspect == 0);
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

void SoftDisplayWidget::mouseMoveEvent(QMouseEvent *e)
{
    sendPointer(e->position(), e->buttons());
    QWidget::mouseMoveEvent(e);
}

void SoftDisplayWidget::mousePressEvent(QMouseEvent *e)
{
    sendPointer(e->position(), e->buttons());
    QWidget::mousePressEvent(e);
}

void SoftDisplayWidget::mouseReleaseEvent(QMouseEvent *e)
{
    sendPointer(e->position(), e->buttons());
    QWidget::mouseReleaseEvent(e);
}

void SoftDisplayWidget::enterEvent(QEnterEvent *e)
{
    syncGun();
    QWidget::enterEvent(e);
}

void SoftDisplayWidget::leaveEvent(QEvent *e)
{
    if (m_gun) a7800session_pointer(m_session, 0, 0, 0, 0);
    QWidget::leaveEvent(e);
}

void SoftDisplayWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (m_height <= 0) return;

    p.setRenderHint(QPainter::SmoothPixmapTransform, m_smooth);
    p.drawImage(displayRect(QSizeF(size()), m_height, m_aspect == 0), m_image);
}
