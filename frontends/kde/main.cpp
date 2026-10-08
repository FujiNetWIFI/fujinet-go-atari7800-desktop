/*
 * FujiNet Go Atari 7800 -- the KDE (Qt6 Widgets) frontend.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QIcon>

#include "MainWindow.h"
#include "a7800session.h"

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("fujinet-go-atari7800-kde"));
    app.setApplicationDisplayName(QStringLiteral("FujiNet Go Atari 7800"));
    app.setDesktopFileName(QStringLiteral("online.fujinet.go.atari7800.kde"));
    /* The installed icon is named after the desktop-entry id; running out
     * of the build tree, the in-tree artwork stands in. */
    {
        QIcon icon = QIcon::fromTheme(QStringLiteral("online.fujinet.go.atari7800.kde"));
        if (icon.isNull()) {
            QIcon::setThemeSearchPaths(QIcon::themeSearchPaths()
                                       << QStringLiteral(A7800_SOURCE_ICON_DIR));
            icon = QIcon::fromTheme(QStringLiteral("fujinet-go-atari7800"));
        }
        if (!icon.isNull()) app.setWindowIcon(icon);
    }

    a7800session *session = a7800session_new(nullptr);
    if (!session) {
        qCritical("Could not create the session (unusable config/data dirs?)");
        return 1;
    }

    MainWindow win(session);

    a7800session_start_opts opts;
    a7800session_default_opts(session, &opts);
    if (argc > 1) opts.cart_path = argv[1];

    if (a7800session_start(session, &opts) != 0)
        qWarning("%s", a7800session_last_error(session));
    win.show();

    const int rc = app.exec();
    a7800session_stop(session);
    a7800session_free(session);
    return rc;
}
