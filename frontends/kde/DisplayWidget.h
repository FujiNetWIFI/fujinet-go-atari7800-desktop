/*
 * The emulator display: a QWidget that pulls frames from the session, and
 * aims the light guns with the mouse while a port has one.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QImage>
#include <QTimer>
#include <QWidget>
#include <cstdint>
#include <vector>

#include "a7800session.h"

class DisplayWidget : public QWidget {
    Q_OBJECT
public:
    explicit DisplayWidget(a7800session *session, QWidget *parent = nullptr);
    /* "aspect" setting: 0 = TV (a 4:3 picture), 1 = square pixels. */
    void setAspect(int aspect);
    void setSmooth(bool smooth);

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    void tick();
    QRectF pictureRect() const;
    void sendPointer(const QPointF &pos, Qt::MouseButtons buttons);

    a7800session *m_session;
    QImage m_image;
    std::vector<uint32_t> m_fb;
    int m_height = 0;
    uint64_t m_serial = 0;
    QTimer m_timer;
    int m_aspect = 0;
    bool m_smooth = false;
    bool m_gun = false;        /* a port has a light gun: crosshair, pointer forwarded */
};
