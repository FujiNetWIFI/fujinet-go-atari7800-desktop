/*
 * FujiNetWindows -- see FujiNetWindows.h.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "FujiNetWindows.h"

#include <QColor>
#include <QDesktopServices>
#include <QFont>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

QColor a7800AccentColor()
{
    return QColor((A7800SESSION_ACCENT_RGB >> 16) & 0xff,
                  (A7800SESSION_ACCENT_RGB >> 8) & 0xff,
                  A7800SESSION_ACCENT_RGB & 0xff);
}

void fujinet_log_show(QWidget *parent, a7800session *session)
{
    static QPointer<QWidget> win;
    if (win) {
        win->raise();
        win->activateWindow();
        return;
    }
    win = new QWidget(parent, Qt::Window);
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->setObjectName(QStringLiteral("fujilog"));
    win->setWindowTitle(QStringLiteral("FujiNet Console Log"));
    win->resize(820, 560);

    auto *view = new QPlainTextEdit(win);
    view->setReadOnly(true);
    QFont mono = view->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    view->setFont(mono);

    auto *layout = new QVBoxLayout(win);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(view);

    auto *timer = new QTimer(win);
    auto refresh = [view, session]() {
        static char buf[128 * 1024];
        const int n = a7800session_fujinet_copy_log(session, buf, sizeof(buf));
        const bool atEnd = view->verticalScrollBar()->value() ==
                           view->verticalScrollBar()->maximum();
        view->setPlainText(n > 0 ? QString::fromUtf8(buf)
                                 : QStringLiteral("(no FujiNet output yet)"));
        if (atEnd)
            view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
    };
    QObject::connect(timer, &QTimer::timeout, view, refresh);
    timer->start(1000);
    refresh();
    win->show();
}

void fujinet_config_show(QWidget *parent, a7800session *session)
{
    (void)parent;
    QDesktopServices::openUrl(QUrl(QString::fromUtf8(a7800session_fujinet_webui_url(session))));
}
