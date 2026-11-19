/*
 * The Controllers window (F9): both controller ports drawn as what is plugged
 * in (a ProLine joystick, a 2600 CX40, the XG-1 light gun or an empty socket)
 * over the console's front panel (PAUSE, SELECT, RESET, the two difficulty
 * slides), from a7800session_controls_layout; each port's type, the gamepads
 * with their assignments, and Map mode for rebinding. Hidden at start; a
 * fixed-size floating window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <vector>

#include "a7800session.h"

class ControllersStage;
class ControllersHint;

class ControllersWindow : public QWidget {
public:
    explicit ControllersWindow(a7800session *session, QWidget *parent = nullptr);

    /* Shows the window and raises it. */
    void present();

protected:
    bool event(QEvent *evt) override;
    void keyPressEvent(QKeyEvent *evt) override;
    void keyReleaseEvent(QKeyEvent *evt) override;
    void showEvent(QShowEvent *evt) override;
    void hideEvent(QHideEvent *evt) override;
    void changeEvent(QEvent *evt) override;
    /* Tab is a key of the console's, not focus traversal. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    void setMapState(int state);
    void bound(const QString &what, const QString &stolen);
    void pollCapture();
    void pollSlow();
    void syncTypes();
    void rebuildPads();
    void refreshPadPlayers();

    a7800session *m_session;
    ControllersStage *m_stage = nullptr;
    QComboBox *m_type[A7800_PORTS] = {};
    QWidget *m_padArea = nullptr;
    QVBoxLayout *m_padBox = nullptr;
    std::vector<QWidget *> m_padRows;
    std::vector<QLabel *> m_padEff;       /* each pad's "plays Player N" */
    unsigned m_padGen = ~0u;
    QPushButton *m_mapButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    QPushButton *m_sysButton[A7800_SYSACT_COUNT] = {};
    ControllersHint *m_hint = nullptr;
    QTimer m_captureTimer;
    QTimer m_heldTimer;
    QTimer m_slowTimer;
    uint32_t m_swallow = 0;               /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a target, >= 0 waiting for a key or
     * gamepad button for that target. */
    int m_mapState = -2;
};
