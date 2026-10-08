/*
 * The main window.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QLabel>
#include <QMainWindow>
#include <QTimer>

#include "a7800session.h"

class DisplayWidget;
class ControllersWindow;
class QAction;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(a7800session *session, QWidget *parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    bool event(QEvent *e) override;

private:
    void buildMenus();
    void updateStatus();
    void syncSwitches();
    void drainSysactions();
    void runSysaction(int sa);
    void openCart(const QString &path);
    void loadMedia(const QString &path);
    void importBios();
    void toggleControllers();
    void pollGamepads();
    void applyPicture();
    void showSettings();
    void restartSession();
    void showAbout();
    void screenshotAndQuit(const QString &path);

    a7800session *m_session;
    DisplayWidget *m_display = nullptr;
    ControllersWindow *m_controllers = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_dot = nullptr;
    QLabel *m_console = nullptr;
    QTimer m_statusTimer;
    QTimer m_sysactTimer;
    QTimer m_padTimer;
    QAction *m_tvAction = nullptr;
    QAction *m_smoothAction = nullptr;
    QAction *m_diffAction[2] = { nullptr, nullptr };
    unsigned m_padGen = 0;
    bool m_sysactDown[A7800_SYSACT_COUNT] = {};
};
