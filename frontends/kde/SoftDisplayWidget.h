/*
 * The software display: the fallback for a machine with no usable GPU
 * device (or QT_QUICK_BACKEND=software style setups). A QWidget that scales
 * with QPainter, fed by FrameFeed like the GPU one.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QImage>
#include <QPointF>
#include <QWidget>
#include <cstdint>

#include "DisplayView.h"
#include "FrameFeed.h"
#include "a7800session.h"

class SoftDisplayWidget : public QWidget, public DisplayView {
    Q_OBJECT
public:
    explicit SoftDisplayWidget(a7800session *session, QWidget *parent = nullptr);
    ~SoftDisplayWidget() override;

    QWidget *widget() override { return this; }
    void setAspect(int aspect) override;
    void setSmooth(bool smooth) override;

protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    bool eventFilter(QObject *watched, QEvent *e) override;
    bool focusNextPrevChild(bool) override { return false; }
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    void onFrame();
    void updateActive();
    void syncGun();
    void sendPointer(const QPointF &pos, Qt::MouseButtons buttons);

    a7800session *m_session;
    FrameFeed *m_feed;
    QImage m_image;            /* Format_RGB32, sized to the newest frame */
    QImage m_scratch;          /* FB_WIDTH x FB_MAX_HEIGHT, filled by copy_frame */
    uint64_t m_serial = 0;
    int m_height = 0;
    int m_aspect = 0;
    bool m_gun = false;
    bool m_smooth = false;
};
