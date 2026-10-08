/*
 * The Controllers window: both ProLine joysticks on screen (they press the
 * machine's controls and light up with whatever the keyboard, a gamepad or
 * the mouse is holding), the console's switches -- Select, Reset, Pause and
 * the two difficulty switches -- and Reboot to CONFIG, each port's
 * controller type and gamepad assignment, and Map mode for rebinding any
 * control to a key or a gamepad button.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QWidget>
#include <vector>

#include "a7800session.h"

/* A button that reports press and release, not "clicked": a joystick
 * button is HELD, since the machine samples it once per frame. */
class PadButton : public QPushButton {
    Q_OBJECT
public:
    PadButton(const QString &face, int target, QWidget *parent = nullptr);
    int target() const { return m_target; }
    QString face() const { return m_face; }
    /* the label outside Map mode (a difficulty switch shows its position) */
    void setFace(const QString &face) { m_face = face; }
    void setHeld(bool held);
signals:
    void pressedTarget(int target);
    void releasedTarget(int target);
protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;
private:
    int m_target;
    QString m_face;
    bool m_down = false;
    bool m_held = false;
};

class ControllersWindow : public QWidget {
    Q_OBJECT
public:
    explicit ControllersWindow(a7800session *session, QWidget *parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;

private:
    QWidget *buildController(int port);
    QWidget *buildConsole();
    PadButton *control(const QString &face, int target);
    void onPressed(int target);
    void onReleased(int target);
    void setMapState(int state);
    void refreshLabels();
    void refreshFaces();
    void pollCapture();
    void poll();
    void rebuildPads();

    a7800session *m_session;
    std::vector<PadButton *> m_controls;
    PadButton *m_diff[2] = { nullptr, nullptr };
    QPushButton *m_mapButton = nullptr;
    QLabel *m_hint = nullptr;
    QComboBox *m_type[2] = { nullptr, nullptr };
    QLabel *m_portNote[2] = { nullptr, nullptr };
    int m_shownType[2] = { -1, -1 };
    QFormLayout *m_padForm = nullptr;
    std::vector<QWidget *> m_padRows;
    unsigned m_padGen = ~0u;
    int m_mouseHeld = -1;      /* the target the mouse is holding, if any */
    QTimer m_captureTimer;
    QTimer m_pollTimer;
    /* -2 idle, -1 armed and waiting for a target, >=0 waiting for a key or
     * gamepad button. */
    int m_mapState = -2;
};
