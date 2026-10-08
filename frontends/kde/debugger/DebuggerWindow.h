/*
 * Debugger window (Qt6 Widgets) over MAME's own debugger engine, via
 * core/include/a7800debug.h. Mirrors the GNOME one tab for tab.
 *
 * The engine is attached while the window is shown (which stops the
 * machine, as on every sibling) and detached when it is hidden, which lets
 * the machine run on.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <cstdint>
#include <vector>

extern "C" {
#include "a7800debug.h"
#include "a7800session.h"
}

class DebuggerWindow : public QMainWindow {
    Q_OBJECT
public:
    /* Shows the window (attaching, so the machine stops). */
    static void showFor(QWidget *parent, a7800session *session);
    /* F12: shows it, or hides it (detaching) when it is already up. */
    static void toggleFor(QWidget *parent, a7800session *session);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    explicit DebuggerWindow(a7800session *session, QWidget *parent);
    QWidget *buildToolbar();
    QWidget *buildPrompt();
    QWidget *buildCpu();
    QWidget *buildDisasm();
    QWidget *buildMaria();
    QWidget *buildIo();
    QWidget *buildCart();
    QWidget *buildBreaks();

    void refreshAll();
    void refreshStatus();
    void refreshCpu();
    void refreshMemory();
    void refreshDisasm();
    void refreshMaria();
    void refreshIo();
    void refreshBps();
    void refreshCart();
    void tick();
    void runPrompt();
    void appendPrompt(const QString &text);
    void jumpTo(const QString &text);
    void stepAnd(void (*fn)(a7800debug *));
    void toggleBreakpointAtSelection();
    int selectedOrPc();

    a7800session *m_session;
    a7800debug *m_dbg;
    unsigned m_seenGen = 0;
    bool m_wasStopped = false;
    int m_runningTicks = 0;
    QTimer m_timer;

    QLabel *m_status = nullptr;
    QPushButton *m_runBtn = nullptr;

    QPlainTextEdit *m_promptOut = nullptr;
    QLineEdit *m_promptIn = nullptr;

    QLineEdit *m_reg[6] = {};
    QCheckBox *m_flag[6] = {};
    QLabel *m_cycles = nullptr;
    QPlainTextEdit *m_mem = nullptr;
    QLineEdit *m_memFrom = nullptr;
    uint16_t m_memTop = 0x1800;
    QLineEdit *m_memAddr = nullptr, *m_memVal = nullptr;

    QPlainTextEdit *m_disasm = nullptr;
    QCheckBox *m_followPc = nullptr;
    QLineEdit *m_jump = nullptr;
    uint16_t m_disasmTop = 0;
    int m_selAddr = -1;            /* the line clicked last, or -1 */
    std::vector<uint16_t> m_lineAddr;

    QPlainTextEdit *m_maria = nullptr;
    QPlainTextEdit *m_dll = nullptr;
    QLabel *m_palette = nullptr;
    std::vector<uint32_t> m_palettePx;

    QPlainTextEdit *m_io = nullptr;

    QTableWidget *m_bpTable = nullptr;
    QComboBox *m_bpType = nullptr;
    QLineEdit *m_bpRange = nullptr, *m_bpCond = nullptr;
    bool m_bpFilling = false;

    QPlainTextEdit *m_cart = nullptr;
};
