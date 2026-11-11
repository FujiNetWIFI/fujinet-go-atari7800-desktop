/*
 * debugger_test -- the Debugger window (debugger/DebuggerWindow.cpp) driven
 * with synthetic events on the offscreen platform against a running 7800
 * session (the CONFIG client, FujiNet off) whose console RAM holds a
 * hand-assembled loop written through the debugger: the tabs,
 * attach/detach with show and hide, F12, the stepping keys and buttons, the
 * console (transcript, Tab completion, history), the disassembly (the PC's
 * accent bar, click / double-click / F9 for a breakpoint, selection and Run
 * to Cursor, the wheel, the jump box), the register and memory entries, the
 * Breakpoints tab (every type), Watches, search, Trace and History, Symbols,
 * Break at, pop-out and dock, the Video tab on the real CONFIG display, and
 * F12 and the View menu from the main window.
 *
 * DBG_TEST_SHOTS=<dir> also grabs a PNG of each tab (DBG_TEST_LIGHT=1: in the
 * light scheme).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QOpenGLContext>
#include <QTemporaryDir>
#include <QtTest>

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QMenu>
#include <QStatusBar>
#include <QStyleHints>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBlock>
#include <QThread>
#include <QToolButton>

#include "a7800debug.h"
#include "a7800memtools.h"
#include "a7800session.h"
#include "DisplayWidget.h"
#include "MainWindow.h"
#include "debugger/DebuggerWindow.h"

namespace {

/* The test subject, in console RAM at $2400:
 *   2400  LDX #$FF
 *   2402  TXS
 *   2403  LDA #$00
 *   2405  LDY #$03
 *   2407  JSR $2450      ; INX / RTS
 *   240A  DEY
 *   240B  BNE $2407
 *   240D  CLC
 *   240E  ADC #$01
 *   2410  STA $2500      ; write
 *   2413  LDA $2500      ; read
 *   2416  JMP $2403 */
const uint8_t kProg[] = {
    0xA2, 0xFF, 0x9A, 0xA9, 0x00, 0xA0, 0x03, 0x20, 0x50, 0x24, 0x88, 0xD0,
    0xFA, 0x18, 0x69, 0x01, 0x8D, 0x00, 0x25, 0xAD, 0x00, 0x25, 0x4C, 0x03, 0x24,
};
const uint8_t kSub[] = { 0xE8, 0x60 };

/* The accent: the 7800's orange. */
const QColor kAccent(0xF0, 0x93, 0x2F);

int pcOf(a7800debug *d)
{
    a7800debug_cpu c;
    a7800debug_cpu_get(d, &c);
    return c.pc;
}

/* a7800debug_cpu_set(A7800_REG_PC) is ignored by a stopped 6502 (the fork sets
 * STATE_GENPC, but MAME has fetched the opcode already): MAME's own `pc=`
 * assignment does move it. */
void setPc(a7800debug *d, unsigned pc)
{
    char out[64];
    const QByteArray cmd = QStringLiteral("pc=%1").arg(pc, 4, 16, QLatin1Char('0')).toUtf8();
    a7800debug_command(d, cmd.constData(), out, sizeof out);
}

/* With the machine stopped: the program in place, PC at its LDY #3. */
void loadProgram(a7800debug *d)
{
    for (size_t i = 0; i < sizeof kProg; i++) a7800debug_write(d, (uint16_t)(0x2400 + i), kProg[i]);
    for (size_t i = 0; i < sizeof kSub; i++) a7800debug_write(d, (uint16_t)(0x2450 + i), kSub[i]);
    a7800debug_cpu_set(d, A7800_FLAG_D, 0);
    a7800debug_cpu_set(d, A7800_REG_SP, 0xFF);
    a7800debug_cpu_set(d, A7800_REG_A, 0);
    /* PC at the LDA, then one step: the next stop is then a plain one, with its reason (the first stop after
     * a `pc=` carries none) */
    setPc(d, 0x2403);
    const int id = a7800debug_breakpoint_add(d, A7800DEBUG_BP_EXEC, 0x2405, 0x2405, "");
    a7800debug_resume(d);
    for (int i = 0; i < 600 && !(a7800debug_is_stopped(d) && pcOf(d) == 0x2405); i++) QThread::msleep(5);
    a7800debug_breakpoint_remove(d, id);
}


} // namespace

class DebuggerTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    a7800session *m_s = nullptr;
    a7800debug *m_d = nullptr;
    QMainWindow *m_win = nullptr;
    QTabWidget *m_tabs = nullptr;
    QString m_shots;
    a7800session_start_opts m_opts = {};

    QPushButton *button(const QString &textStart)
    {
        for (QPushButton *b : m_win->findChildren<QPushButton *>())
            if (b->text().startsWith(textStart)) return b;
        return nullptr;
    }
    QLineEdit *entry(const QString &placeholderStart)
    {
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->placeholderText().startsWith(placeholderStart)) return e;
        return nullptr;
    }
    QLineEdit *entryIs(const QString &placeholder)
    {
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->placeholderText() == placeholder) return e;
        return nullptr;
    }
    QPlainTextEdit *disasmView()
    {
        for (QPlainTextEdit *v : m_win->findChildren<QPlainTextEdit *>())
            if (v->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff) return v;
        return nullptr;
    }
    QPlainTextEdit *consoleView()
    {
        for (QPlainTextEdit *v : m_win->findChildren<QPlainTextEdit *>())
            if (v->lineWrapMode() == QPlainTextEdit::WidgetWidth) return v;
        return nullptr;
    }
    QLabel *statusLabel()
    {
        for (QLabel *l : m_win->findChildren<QLabel *>())
            if (l->text().startsWith(QLatin1String("Stopped")) ||
                l->text() == QLatin1String("Running"))
                return l;
        return nullptr;
    }
    QCheckBox *check(const QString &text)
    {
        for (QCheckBox *c : m_win->findChildren<QCheckBox *>())
            if (c->text() == text) return c;
        return nullptr;
    }
    QTableWidget *bpTable() { return m_tabs->widget(3)->findChild<QTableWidget *>(); }

    void waitStopped()
    {
        QTRY_VERIFY_WITH_TIMEOUT(a7800debug_is_stopped(m_d), 3000);
        /* the window follows within its poll */
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel() &&
                                     statusLabel()->text().startsWith(QLatin1String("Stopped")), 2000);
    }
    QByteArray why() { return (statusLabel() ? statusLabel()->text() : QString()).toUtf8(); }
    void shot(const QString &name)
    {
        if (!m_shots.isEmpty()) m_win->grab().save(m_shots + QLatin1Char('/') + name + QStringLiteral(".png"));
    }

    /* The block of the disassembly that shows an address, or -1. */
    int lineOf(QPlainTextEdit *v, int addr)
    {
        const QString want = QStringLiteral("%1").arg(addr, 4, 16, QLatin1Char('0')).toUpper();
        int n = 0;
        for (QTextBlock b = v->document()->begin(); b.isValid(); b = b.next(), ++n)
            if (b.text().mid(2, 4) == want) return n;
        return -1;
    }
    QPoint lineAt(QPlainTextEdit *v, int line, int column)
    {
        QTextCursor c(v->document()->findBlockByNumber(line));
        c.setPosition(c.block().position() + column);
        const QRect r = v->cursorRect(c);
        return QPoint(r.left() + 1, r.center().y());
    }
    void clickLine(int addr, int column, bool dbl = false)
    {
        QPlainTextEdit *v = disasmView();
        const int line = lineOf(v, addr);
        QVERIFY2(line >= 0, qPrintable(QStringLiteral("line for %1").arg(addr, 0, 16)));
        const QPoint p = lineAt(v, line, column);
        if (dbl) QTest::mouseDClick(v->viewport(), Qt::LeftButton, {}, p);
        else QTest::mouseClick(v->viewport(), Qt::LeftButton, {}, p);
    }
    void typeInto(QLineEdit *e, const QString &text)
    {
        e->clear();
        QTest::keyClicks(e, text);
        QTest::keyClick(e, Qt::Key_Return);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        qputenv("SDL_AUDIODRIVER", "dummy");
        if (qEnvironmentVariableIsSet("DBG_TEST_SHOTS")) m_shots = qEnvironmentVariable("DBG_TEST_SHOTS");
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        if (qEnvironmentVariableIsSet("DBG_TEST_LIGHT"))
            QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
#endif
        a7800session_paths paths = {};
        const QByteArray dir = m_dir.path().toUtf8();
        paths.config_dir = dir.constData();
        paths.data_dir = dir.constData();
        paths.fujinet_lib = "";
        m_s = a7800session_new(&paths);
        QVERIFY(m_s);
        m_d = a7800session_debugger(m_s);
        QVERIFY(m_d);

        a7800session_default_opts(m_s, &m_opts);
        m_opts.cart_path = nullptr;
                m_opts.enable_fujinet = 0;
        m_opts.enable_audio = 0;
        m_opts.enable_gamepad = 0;
        QCOMPARE(a7800session_start(m_s, &m_opts), 0);

        /* the window opens on the tab A7800_DEBUGGER_TAB names, and A7800_WATCH,
         * A7800_WATCH_BREAK and A7800_MEM_GOTO are read at creation */
        qputenv("A7800_DEBUGGER_TAB", "i/o");
        qputenv("A7800_WATCH", "$2510, $2520-$252F");
        qputenv("A7800_WATCH_BREAK", "$2530");
        qputenv("A7800_MEM_GOTO", "$2540-$254F");
        DebuggerWindow::showFor(nullptr, m_s);
        qunsetenv("A7800_DEBUGGER_TAB");
        qunsetenv("A7800_WATCH");
        qunsetenv("A7800_WATCH_BREAK");
        qunsetenv("A7800_MEM_GOTO");
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->objectName() == QLatin1String("debugger")) m_win = qobject_cast<QMainWindow *>(w);
        QVERIFY(m_win);
        QVERIFY(QTest::qWaitForWindowExposed(m_win));
        m_tabs = m_win->findChild<QTabWidget *>();
        QVERIFY(m_tabs);
        QCOMPARE(m_tabs->currentIndex(), 5);
        QCOMPARE(a7800watch_count(), 3);                      /* two watches and the break-on-change one */
        a7800debug_breakpoint list[4];
        QCOMPARE(a7800debug_breakpoint_list(m_d, list, 4), 1);
        QCOMPARE((int)list[0].start, 0x2530);
        QVERIFY(QString::fromUtf8(list[0].condition).contains(QStringLiteral("old")));
        a7800watch_clear();
        m_tabs->setCurrentIndex(0);
        waitStopped();
    }

    void cleanupTestCase()
    {
        delete m_win;       /* detaches */
        if (m_s) {
            a7800session_stop(m_s);
            a7800session_free(m_s);
        }
    }

    void init()
    {
        if (!m_win->isVisible()) DebuggerWindow::toggleFor(nullptr, m_s);
        QTRY_VERIFY_WITH_TIMEOUT(a7800debug_is_attached(m_d), 2000);
        if (!a7800debug_is_stopped(m_d)) a7800debug_stop(m_d);
        waitStopped();
        a7800debug_breakpoint_clear(m_d);
        loadProgram(m_d);
    }

    void tabsAndAttach()
    {
        QStringList names;
        for (int i = 0; i < m_tabs->count(); ++i) names << m_tabs->tabText(i);
        QCOMPARE(names, (QStringList{ QStringLiteral("Console"), QStringLiteral("Disassembly"),
                                      QStringLiteral("Memory"), QStringLiteral("Breakpoints"),
                                      QStringLiteral("Watches"), QStringLiteral("Cart && I/O"),
                                      QStringLiteral("Trace"), QStringLiteral("History"),
                                      QStringLiteral("Video"), QStringLiteral("Sound"),
                                      QStringLiteral("Symbols"), QStringLiteral("FujiNet") }));
        QVERIFY(a7800debug_is_attached(m_d));
        QVERIFY(a7800debug_is_stopped(m_d));
        /* Run is the accent while stopped, whichever text it wears, in one size */
        QPushButton *run = button(QStringLiteral("Run (F5)"));
        QVERIFY(run);
        QVERIFY(run->isChecked());
        QVERIFY(run->styleSheet().contains(kAccent.name()));
        const QSize size = run->size();
        QTest::mouseClick(run, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!a7800debug_is_stopped(m_d), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(run->text() == QLatin1String("Stop (F5)"), 2000);
        QVERIFY(!run->isChecked());
        QCOMPARE(run->size(), size);
        QCOMPARE(statusLabel()->text(), QStringLiteral("Running"));
        QTest::mouseClick(run, Qt::LeftButton);
        waitStopped();
        QVERIFY(run->isChecked());
        QCOMPARE(run->size(), size);
        /* the toolbar has the family's buttons */
        for (const char *label : { "Step (F7)", "Step Over", "Step Out", "Frame", "Clear Breakpoints",
                                   "Load Symbols", "Clear Symbols", "Save", "Pop Out" })
            QVERIFY2(button(QString::fromUtf8(label)), label);
    }

    void stepKeys()
    {
        QCOMPARE(pcOf(m_d), 0x2405);
        QTest::keyClick(m_win, Qt::Key_F7);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x2407, 2000);
        QCOMPARE(statusLabel()->text().left(13), QStringLiteral("Stopped: step"));
        /* F8 steps over the JSR */
        QTest::keyClick(m_win, Qt::Key_F8);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x240A, 2000);
        /* the buttons do the same */
        a7800debug_write(m_d, 0x01FE, 0x09);        /* a return address, as a JSR left it */
        a7800debug_write(m_d, 0x01FF, 0x24);
        a7800debug_cpu_set(m_d, A7800_REG_SP, 0xFD);
        setPc(m_d, 0x2450);
        QTest::mouseClick(button(QStringLiteral("Step Out")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x240A, 2000);
        /* Frame: Shift+F7 lands on a frame boundary */
        a7800debug_cpu c, c2;
        a7800debug_cpu_get(m_d, &c);
        QTest::keyClick(m_win, Qt::Key_F7, Qt::ShiftModifier);
        QTRY_VERIFY_WITH_TIMEOUT(a7800debug_is_stopped(m_d) &&
                                     (a7800debug_cpu_get(m_d, &c2), c2.frame > c.frame), 3000);
        /* stepping while running is refused with a word, not silently */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!a7800debug_is_stopped(m_d), 2000);
        QTest::keyClick(m_win, Qt::Key_F7);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Stop the machine")));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
    }

    /* The stop reason is the status line, and it is never cut: the label has
     * the width its whole text needs at the default window width. */
    void stopReasonNotElided()
    {
        a7800debug_breakpoint_add(m_d, A7800DEBUG_BP_WRITE, 0x2500, 0x2500, "value != old");
        a7800debug_write(m_d, 0x2500, 0x77);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(a7800debug_is_stopped(m_d), 4000);
        QTRY_VERIFY2_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("wrote $2500")), why().constData(), 2000);
        QLabel *status = statusLabel();
        QVERIFY(status->text().contains(QLatin1String("(was $77)")));
        QVERIFY2(status->width() >= status->fontMetrics().horizontalAdvance(status->text()),
                 qPrintable(QStringLiteral("status %1 wide for %2").arg(status->width())
                                .arg(status->fontMetrics().horizontalAdvance(status->text()))));
        QVERIFY(!status->text().contains(QChar(0x2026)));
        shot(QStringLiteral("stopreason"));
    }

    void consoleCommands()
    {
        m_tabs->setCurrentIndex(0);
        QLineEdit *in = entry(QStringLiteral("command"));
        QPlainTextEdit *out = consoleView();
        QVERIFY(in && out);
        QCOMPARE(m_win->focusWidget(), in);         /* typing goes straight in */
        typeInto(in, QStringLiteral("print pc"));
        QVERIFY(out->toPlainText().contains(QStringLiteral("> print pc")));
        QVERIFY2(out->toPlainText().contains(QStringLiteral("2405")), qPrintable(out->toPlainText()));
        QVERIFY(in->text().isEmpty());
        /* a command that moves the machine moves the window */
        typeInto(in, QStringLiteral("bpset 2407"));
        QVERIFY(a7800debug_breakpoint_check(m_d, 0x2407));
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY2_WITH_TIMEOUT(out->toPlainText().contains(QStringLiteral("$2407")) &&
                                      out->toPlainText().contains(QStringLiteral("— ")), qPrintable(out->toPlainText()), 3000);
        waitStopped();
        QCOMPARE(pcOf(m_d), 0x2407);
        QVERIFY(statusLabel()->text().contains(QStringLiteral("$2407")));
        /* Tab completes a command, lists several, and Up recalls */
        in->clear();
        QTest::keyClicks(in, QStringLiteral("wps"));
        QTest::keyClick(in, Qt::Key_Tab);
        QCOMPARE(in->text(), QStringLiteral("wpset "));
        in->clear();
        QTest::keyClicks(in, QStringLiteral("bp"));
        QTest::keyClick(in, Qt::Key_Tab);
        QVERIFY(out->toPlainText().contains(QStringLiteral("bpclear")));
        QVERIFY(out->toPlainText().contains(QStringLiteral("bpset")));
        QVERIFY(in->text().startsWith(QStringLiteral("bp")));
        in->clear();
        QTest::keyClick(in, Qt::Key_Up);
        QCOMPARE(in->text(), QStringLiteral("bpset 2407"));
        QTest::keyClick(in, Qt::Key_Up);
        QCOMPARE(in->text(), QStringLiteral("print pc"));
        QTest::keyClick(in, Qt::Key_Down);
        QCOMPARE(in->text(), QStringLiteral("bpset 2407"));
        in->clear();
        /* the family's own commands */
        typeInto(in, QStringLiteral("maria"));
        QVERIFY(out->toPlainText().contains(QStringLiteral("> maria")));
        typeInto(in, QStringLiteral("cart"));
        shot(QStringLiteral("console"));
    }

    /* Load Symbols > Next to the Cartridge (no cartridge file: said, not
     * silent), a .sym file loaded through the engine shows in the views, and
     * Break at takes a symbol or a hex address. */
    void symbolsAndBreakAt()
    {
        m_tabs->setCurrentIndex(0);
        QAction *next = nullptr;
        for (QAction *a : m_win->findChildren<QAction *>())
            if (a->text() == QLatin1String("Next to the Cartridge")) next = a;
        QVERIFY(next);
        const int before = consoleView()->document()->blockCount();
        next->trigger();
        QVERIFY(consoleView()->document()->blockCount() > before);
        QCOMPARE(a7800debug_label_address(m_d, "afterwards"), -1);
        QFile sym(m_dir.path() + QStringLiteral("/loop.sym"));
        QVERIFY(sym.open(QIODevice::WriteOnly));
        sym.write("afterwards = $240A\nhelper = $2450\n");
        sym.close();
        QLineEdit *in = entry(QStringLiteral("command"));
        typeInto(in, QStringLiteral("labels ") + sym.fileName());
        QCOMPARE(a7800debug_label_address(m_d, "afterwards"), 0x240A);

        /* Break at: a symbol, then a hex address, then a repeat */
        QLineEdit *breakAt = entry(QStringLiteral("symbol or"));
        QVERIFY(breakAt);
        typeInto(breakAt, QStringLiteral("afterwards"));
        QVERIFY(a7800debug_breakpoint_check(m_d, 0x240A));
        QVERIFY(breakAt->text().isEmpty());
        typeInto(breakAt, QStringLiteral("240D"));
        QVERIFY(a7800debug_breakpoint_check(m_d, 0x240D));
        typeInto(breakAt, QStringLiteral("240D"));
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("already")));
        a7800debug_breakpoint list[4];
        QCOMPARE(a7800debug_breakpoint_list(m_d, list, 4), 2);
        typeInto(breakAt, QStringLiteral("nosuchsymbol"));
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Not a number")));
        a7800debug_breakpoint_clear(m_d);

        /* the stop reason names where it is */
        typeInto(in, QStringLiteral("bpset afterwards"));
        QVERIFY(a7800debug_breakpoint_check(m_d, 0x240A));
        typeInto(in, QStringLiteral("go"));
        QTRY_VERIFY_WITH_TIMEOUT(a7800debug_is_stopped(m_d) && pcOf(m_d) == 0x240A, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("(afterwards)")), 2000);

        /* the Symbols tab lists them, filtered; activating a row opens the Disassembly there */
        m_tabs->setCurrentIndex(10);
        QTableWidget *table = m_tabs->currentWidget()->findChild<QTableWidget *>();
        QLineEdit *filter = m_tabs->currentWidget()->findChild<QLineEdit *>();
        QVERIFY(table && filter);
        QTRY_VERIFY_WITH_TIMEOUT(table->rowCount() >= 2, 2000);
        shot(QStringLiteral("symbols"));
        const int all = table->rowCount();
        filter->setText(QStringLiteral("AFTERW"));
        QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 1, 2000);
        QCOMPARE(table->item(0, 1)->text(), QStringLiteral("afterwards"));
        QCOMPARE(table->item(0, 0)->text(), QStringLiteral("$240A"));
        table->setCurrentCell(0, 0);
        QTest::keyClick(table, Qt::Key_Return);             /* activating a row */
        QCOMPARE(m_tabs->currentIndex(), 1);
        QVERIFY(!check(QStringLiteral("Follow PC"))->isChecked());
        QVERIFY(lineOf(disasmView(), 0x240A) >= 0);
        m_tabs->setCurrentIndex(10);
        filter->clear();
        QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), all, 2000);

        /* Clear Symbols drops the loaded ones */
        QTest::mouseClick(button(QStringLiteral("Clear Symbols")), Qt::LeftButton);
        QCOMPARE(a7800debug_label_address(m_d, "afterwards"), -1);
        QTRY_VERIFY_WITH_TIMEOUT(table->rowCount() < all, 2000);
        m_tabs->setCurrentIndex(0);
    }

    void disassemblyHighlights()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QVERIFY(v);
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x2405) >= 0, 2000);
        const int pcLine = lineOf(v, 0x2405);
        QCOMPARE(v->document()->findBlockByNumber(pcLine).text().left(2), QStringLiteral(" >"));
        /* follow-PC keeps it about a third of the way down */
        const int rows = v->document()->blockCount();
        QVERIFY(pcLine >= rows / 3 - 2 && pcLine <= rows / 3 + 2);
        /* the PC's line: the accent as the block's own background, so it lies under the gutter */
        const QTextBlock pcBlock = v->document()->findBlockByNumber(pcLine);
        QCOMPARE(pcBlock.blockFormat().background().color(), kAccent);
        for (const auto &s : v->extraSelections())
            QVERIFY(!s.format.boolProperty(QTextFormat::FullWidthSelection));
        QCOMPARE(v->document()->findBlockByNumber(pcLine + 1).blockFormat().background().style(), Qt::NoBrush);

        /* a breakpoint's dot: filled and red, hollow and grey when disabled */
        a7800debug_breakpoint_toggle(m_d, 0x2407);
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(lineOf(v, 0x2407)).text().startsWith(
                                     QStringLiteral("●")), 2000);
        auto dotColor = [&](int line) {
            QColor c;
            for (const auto &s : v->extraSelections())
                if (s.cursor.blockNumber() == line && s.cursor.hasSelection() &&
                    s.cursor.selectionStart() == s.cursor.block().position() &&
                    s.cursor.selectionEnd() == s.cursor.block().position() + 1)
                    c = s.format.foreground().color();
            return c;
        };
        QCOMPARE(dotColor(lineOf(v, 0x2407)), QColor(0xe0, 0x1b, 0x24));
        a7800debug_breakpoint b;
        QCOMPARE(a7800debug_breakpoint_list(m_d, &b, 1), 1);
        a7800debug_breakpoint_enable(m_d, b.id, 0);
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(lineOf(v, 0x2407)).text().startsWith(
                                     QStringLiteral("○")), 2000);
        QCOMPARE(dotColor(lineOf(v, 0x2407)), QColor(0x9a, 0x99, 0x96));
        /* a breakpoint on the PC's own line keeps its dot, in white on the bar */
        a7800debug_breakpoint_toggle(m_d, 0x2405);
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(lineOf(v, 0x2405)).text().startsWith(
                                     QStringLiteral("●")), 2000);
        QCOMPARE(dotColor(lineOf(v, 0x2405)), QColor(Qt::white));
        shot(QStringLiteral("disassembly"));
    }

    void clickSelectsAndGutterToggles()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x240A) >= 0, 2000);
        /* a click on the text selects (underlined, bold) and sets no breakpoint */
        clickLine(0x240A, 30);
        QVERIFY(!a7800debug_breakpoint_check(m_d, 0x240A));
        {
            bool underlined = false;
            for (const auto &s : v->extraSelections())
                if (s.cursor.blockNumber() == lineOf(v, 0x240A) && s.format.fontUnderline() &&
                    s.format.fontWeight() == QFont::Bold)
                    underlined = true;
            QVERIFY(underlined);
        }
        /* moving to another line moves the selection; no caret-toggled breakpoints */
        clickLine(0x240D, 30);
        QVERIFY(!a7800debug_breakpoint_check(m_d, 0x240A));
        QVERIFY(!a7800debug_breakpoint_check(m_d, 0x240D));
        /* the left edge toggles, a double-click toggles, F9 toggles the selection */
        clickLine(0x2410, 0);
        QVERIFY(a7800debug_breakpoint_check(m_d, 0x2410));
        clickLine(0x2410, 0);
        QVERIFY(!a7800debug_breakpoint_check(m_d, 0x2410));
        clickLine(0x2413, 30, true);
        QVERIFY(a7800debug_breakpoint_check(m_d, 0x2413));
        a7800debug_breakpoint_toggle(m_d, 0x2413);
        clickLine(0x2416, 30);
        QTest::keyClick(m_win, Qt::Key_F9);
        QVERIFY(a7800debug_breakpoint_check(m_d, 0x2416));
        QTest::keyClick(m_win, Qt::Key_F9);
        QVERIFY(!a7800debug_breakpoint_check(m_d, 0x2416));
        a7800debug_breakpoint list[4];
        QCOMPARE(a7800debug_breakpoint_list(m_d, list, 4), 0);
    }

    void runToCursor()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x2410) >= 0, 2000);
        clickLine(0x2410, 30);
        QTest::mouseClick(button(QStringLiteral("Run to Cursor")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x2410, 3000);
        QVERIFY2(statusLabel()->text().contains(QLatin1String("run to")), why().constData());
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x2410) >= 0, 2000);
    }

    void wheelJumpAndFollow()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QCheckBox *follow = check(QStringLiteral("Follow PC"));
        QVERIFY(follow && follow->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x2405) >= 0, 2000);
        const QPoint p = v->viewport()->rect().center();
        QWheelEvent wheel(p, v->viewport()->mapToGlobal(p), QPoint(), QPoint(0, -120), Qt::NoButton, {},
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(v->viewport(), &wheel);
        QVERIFY(!follow->isChecked());
        QVERIFY(lineOf(v, 0x2405) < 0 || lineOf(v, 0x2405) < v->document()->blockCount() / 3 - 1);
        /* the jump box: bare digits are hex here, a label wins first, then expressions */
        QLineEdit *jump = entry(QStringLiteral("$address"));
        QVERIFY(jump);
        typeInto(jump, QStringLiteral("2450"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x2450) >= 0, 2000);
        a7800debug_set_label(m_d, 0x240A, "back");
        typeInto(jump, QStringLiteral("back"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x240A) >= 0, 2000);
        QVERIFY(v->toPlainText().contains(QStringLiteral("back")));
        typeInto(jump, QStringLiteral("$2450+1"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x2451) >= 0, 2000);
        typeInto(jump, QStringLiteral("nosuchlabel"));
        QVERIFY(!m_win->statusBar()->currentMessage().isEmpty());
        /* Follow PC again returns to the PC */
        QTest::mouseClick(follow, Qt::LeftButton, {}, QPoint(4, follow->height() / 2));
        QVERIFY(follow->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x2405) >= 0, 2000);
        a7800debug_set_label(m_d, 0x240A, nullptr);
    }

    void registersFlagsMemory()
    {
        m_tabs->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QList<QLineEdit *> regs;
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->toolTip().startsWith(QLatin1String("Enter sets it"))) regs << e;
        QCOMPARE(regs.size(), 6);                   /* PC SP A X Y PS */
        QTRY_COMPARE_WITH_TIMEOUT(regs[0]->text(), QStringLiteral("2405"), 2000);   /* PC */
        typeInto(regs[0], QStringLiteral("240A"));
        QEXPECT_FAIL("", "core: a7800debug_cpu_set(PC) is ignored on a stopped 6502", Continue);
        QCOMPARE(pcOf(m_d), 0x240A);
        setPc(m_d, 0x240A);
        typeInto(regs[2], QStringLiteral("34"));    /* A */
        a7800debug_cpu c;
        a7800debug_cpu_get(m_d, &c);
        QCOMPARE(c.a, 0x34);
        typeInto(regs[4], QStringLiteral("$20+2"));     /* Y, an expression */
        a7800debug_cpu_get(m_d, &c);
        QCOMPARE(c.y, 0x22);
        /* the flags */
        QCheckBox *carry = check(QStringLiteral("C"));
        QVERIFY(carry);
        a7800debug_cpu_set(m_d, A7800_FLAG_C, 0);
        QTRY_VERIFY_WITH_TIMEOUT(!carry->isChecked(), 2000);
        QTest::mouseClick(carry, Qt::LeftButton, {}, QPoint(6, carry->height() / 2));
        a7800debug_cpu_get(m_d, &c);
        QCOMPARE(c.c, 1);
        QCheckBox *neg = check(QStringLiteral("N"));
        QVERIFY(neg);
        a7800debug_cpu_set(m_d, A7800_FLAG_N, 0);
        QTRY_VERIFY_WITH_TIMEOUT(!neg->isChecked(), 2000);
        QTest::mouseClick(neg, Qt::LeftButton, {}, QPoint(6, neg->height() / 2));
        a7800debug_cpu_get(m_d, &c);
        QCOMPARE(c.n, 1);
        a7800debug_cpu_set(m_d, A7800_FLAG_N, 0);
        a7800debug_cpu_set(m_d, A7800_FLAG_C, 0);
        /* the beam / cycles line */
        bool beam = false;
        for (QLabel *l : m_win->findChildren<QLabel *>())
            if (l->text().startsWith(QLatin1String("scanline"))) beam = l->text().contains(QLatin1String("cycles"));
        QVERIFY(beam);
        /* memory: poke through the map, go to a label or an address */
        QLineEdit *addr = entryIs(QStringLiteral("$4000"));
        QLineEdit *val = entryIs(QStringLiteral("$00"));
        QVERIFY(addr && val);
        addr->setText(QStringLiteral("2510"));       /* hex */
        typeInto(val, QStringLiteral("5a"));
        uint8_t b = 0;
        a7800debug_read(m_d, 0x2510, &b, 1);
        QCOMPARE((int)b, 0x5A);
        addr->setText(QStringLiteral("$2511"));
        typeInto(val, QStringLiteral("$A5"));
        a7800debug_read(m_d, 0x2511, &b, 1);
        QCOMPARE((int)b, 0xA5);
        a7800debug_write(m_d, 0x2512, 0x11);
        addr->setText(QStringLiteral("2512"));
        typeInto(val, QStringLiteral("1FF"));        /* not a byte */
        a7800debug_read(m_d, 0x2512, &b, 1);
        QCOMPARE((int)b, 0x11);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("byte")));
        QPlainTextEdit *mem = memView();
        QLineEdit *go = entryIs(QStringLiteral("$4000, label or lo-hi"));
        QVERIFY(go && mem);
        typeInto(go, QStringLiteral("2400"));
        /* all 64K are there: the first row and the last, each named by its region */
        QVERIFY(mem->toPlainText().contains(QStringLiteral("$0000: ")));
        QVERIFY(mem->toPlainText().contains(QStringLiteral("$FFF0: ")));
        QVERIFY(mem->toPlainText().contains(QStringLiteral("$2400: A2 FF 9A")));
        QCOMPARE(mem->document()->blockCount(), 4097);
        const QString first = mem->document()->findBlockByNumber(1).text();
        const QString ram = mem->document()->findBlockByNumber(1 + 0x2400 / 16).text();
        QVERIFY2(!first.mid(72).trimmed().isEmpty(), qPrintable(first));
        QVERIFY2(ram.contains(QLatin1String("RAM")), qPrintable(ram));
        /* Go to scrolled the row into view and marked it in the accent, white ink */
        QVERIFY(mem->verticalScrollBar()->value() >= 0x2400 / 16 - 40);
        bool marked = false;
        for (const auto &s : mem->extraSelections())
            marked |= s.format.background().color() == kAccent && s.format.foreground().color() == QColor(Qt::white);
        QVERIFY(marked);
        /* edits need a stopped machine */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!a7800debug_is_stopped(m_d), 2000);
        typeInto(regs[0], QStringLiteral("2400"));
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Stop the machine")));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        shot(QStringLiteral("memory"));
    }

    void breakpointsTab()
    {
        m_tabs->setCurrentIndex(3);
        QTableWidget *t = bpTable();
        QVERIFY(t);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QCOMPARE(t->rowCount(), 0);
        QLineEdit *start = entry(QStringLiteral("address, range"));
        QLineEdit *end = entry(QStringLiteral("end"));
        QLineEdit *cond = entry(QStringLiteral("condition"));
        QVERIFY(start && end && cond);
        QComboBox *type = m_tabs->currentWidget()->findChild<QComboBox *>();
        QVERIFY(type);
        QPushButton *add = nullptr;
        for (QPushButton *b : m_tabs->currentWidget()->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Add")) add = b;
        QPushButton *clear = nullptr;
        for (QPushButton *b : m_tabs->currentWidget()->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Clear Breakpoints")) clear = b;
        QLabel *msg = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->styleSheet().contains(QLatin1String("e01b24"))) msg = l;
        QVERIFY(add && clear && msg);

        /* exec at a hex address */
        typeInto(start, QStringLiteral("2407"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QCOMPARE(t->item(0, 1)->text(), QStringLiteral("Exec  $2407"));
        QCOMPARE(t->item(0, 0)->checkState(), Qt::Checked);
        QVERIFY(start->text().isEmpty());
        /* a write watchpoint over a range, with a condition */
        type->setCurrentText(QStringLiteral("Write"));
        start->setText(QStringLiteral("2500"));
        end->setText(QStringLiteral("$25FF"));
        cond->setText(QStringLiteral("value > 0"));
        QTest::mouseClick(add, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 2, 2000);
        QCOMPARE(t->item(1, 1)->text(), QStringLiteral("Write  $2500-$25FF  if value > 0"));
        /* the start may itself be a range */
        cond->clear();
        end->clear();
        type->setCurrentText(QStringLiteral("Access"));
        typeInto(start, QStringLiteral("2600-260F"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 3, 2000);
        QVERIFY2(t->item(2, 1)->text().startsWith(QLatin1String("Access  $2600-$260F")), qPrintable(t->item(2, 1)->text()));
        type->setCurrentText(QStringLiteral("Read"));
        typeInto(start, QStringLiteral("28"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 4, 2000);
        QCOMPARE(t->item(3, 1)->text(), QStringLiteral("Read  $0028"));
        /* errors are shown, not swallowed */
        const int rowsBefore = t->rowCount();
        type->setCurrentText(QStringLiteral("Execute"));
        cond->setText(QStringLiteral("a == (1 +"));
        start->setText(QStringLiteral("7000"));
        end->clear();
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(!msg->text().isEmpty());
        QCOMPARE(t->rowCount(), rowsBefore);
        cond->clear();
        start->setText(QStringLiteral("FF"));
        end->setText(QStringLiteral("FC"));
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(msg->text().contains(QLatin1String("before")));
        start->clear();
        end->clear();
        msg->clear();
        type->setCurrentText(QStringLiteral("Execute"));

        QStringList types;
        for (int i = 0; i < type->count(); ++i) types << type->itemText(i);
        QCOMPARE(types, (QStringList{ QStringLiteral("Execute"), QStringLiteral("Read"), QStringLiteral("Write"),
                                      QStringLiteral("Access") }));
        shot(QStringLiteral("breakpoints-types"));

        /* back to two: the exec one and the conditional write */
        QTest::mouseClick(clear, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 0, 2000);
        typeInto(start, QStringLiteral("2407"));
        type->setCurrentText(QStringLiteral("Write"));
        start->setText(QStringLiteral("2500"));
        end->setText(QStringLiteral("$25FF"));
        cond->setText(QStringLiteral("value > 0"));
        QTest::mouseClick(add, Qt::LeftButton);
        cond->clear();
        end->clear();
        type->setCurrentText(QStringLiteral("Execute"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 2, 2000);

        /* the exec breakpoint stops the run, with its hits counted */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(a7800debug_is_stopped(m_d) && pcOf(m_d) == 0x2407, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("$2407")), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(t->item(0, 2) && t->item(0, 2)->text().toInt() >= 1, 2000);
        /* unticking it lets the run pass; the write watch then stops it instead */
        t->item(0, 0)->setCheckState(Qt::Unchecked);
        a7800debug_breakpoint list[2];
        QCOMPARE(a7800debug_breakpoint_list(m_d, list, 2), 2);
        QCOMPARE(list[0].enabled, 0);
        QCOMPARE(list[1].enabled, 1);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(a7800debug_is_stopped(m_d) && statusLabel()->text().contains(QLatin1String("wrote $2500")),
                                 4000);
        QVERIFY(pcOf(m_d) != 0x2407);
        /* Clear Hits zeroes the counts */
        QPushButton *clearHits = nullptr;
        for (QPushButton *b : m_tabs->currentWidget()->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Clear Hits")) clearHits = b;
        QVERIFY(clearHits);
        QTRY_VERIFY_WITH_TIMEOUT(t->item(1, 2) && t->item(1, 2)->text().toInt() >= 1, 2000);
        QTest::mouseClick(clearHits, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->item(1, 2)->text(), QStringLiteral("0"), 2000);
        shot(QStringLiteral("breakpoints"));
        /* the row's delete button */
        auto *rm = qobject_cast<QToolButton *>(t->cellWidget(1, 3));
        QVERIFY(rm);
        QTest::mouseClick(rm, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QCOMPARE(a7800debug_breakpoint_list(m_d, list, 2), 1);
        QTest::mouseClick(clear, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 0, 2000);
        QCOMPARE(a7800debug_breakpoint_list(m_d, list, 2), 0);
    }

    /* A read watchpoint stops after the access. */
    void readWatchpoint()
    {
        QVERIFY(a7800debug_breakpoint_add(m_d, A7800DEBUG_BP_READ, 0x2500, 0x2500, "") >= 0);
        m_tabs->setCurrentIndex(3);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(a7800debug_is_stopped(m_d), 3000);
        QTRY_VERIFY2_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("read $2500")), why().constData(), 2000);
        QTRY_COMPARE_WITH_TIMEOUT(bpTable()->item(0, 1)->text(), QStringLiteral("Read  $2500"), 2000);
        a7800debug_breakpoint_clear(m_d);
        m_tabs->setCurrentIndex(0);
    }

    QPlainTextEdit *memView()
    {
        return m_tabs->widget(2)->findChild<QPlainTextEdit *>();
    }

    /* Watches: add, live value, Memory, Break on change, Remove. */
    void watchesTab()
    {
        a7800watch_clear();
        a7800debug_write(m_d, 0x2510, 0x42);
        m_tabs->setCurrentIndex(4);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QTableWidget *t = m_tabs->currentWidget()->findChild<QTableWidget *>();
        QLineEdit *e = m_tabs->currentWidget()->findChild<QLineEdit *>();
        QVERIFY(t && e);
        QCOMPARE(t->rowCount(), 0);
        typeInto(e, QStringLiteral("$2510"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QVERIFY(t->item(0, 0)->text().contains(QStringLiteral("$2510")));
        QVERIFY(t->item(0, 0)->text().contains(QStringLiteral("$42")));
        a7800debug_write(m_d, 0x2510, 0x43);
        QTest::keyClick(m_win, Qt::Key_F7);             /* a stop refreshes the values */
        QTRY_VERIFY_WITH_TIMEOUT(t->item(0, 0)->text().contains(QStringLiteral("$43")), 2000);
        typeInto(e, QStringLiteral("$2520-$252F"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 2, 2000);
        /* while the machine runs the value is a dash */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!a7800debug_is_stopped(m_d), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(t->item(0, 0)->text().contains(QLatin1String("-")) &&
                                     !t->item(0, 0)->text().contains(QStringLiteral("$43")), 2000);
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        shot(QStringLiteral("watches"));

        /* Break on change adds a write breakpoint with the condition */
        a7800debug_breakpoint_clear(m_d);
        auto buttonOf = [&](int row, const QString &text) -> QPushButton * {
            for (QPushButton *b : t->cellWidget(row, 1)->findChildren<QPushButton *>())
                if (b->text() == text) return b;
            return nullptr;
        };
        QPushButton *brk = buttonOf(0, QStringLiteral("Break on change"));
        QVERIFY(brk);
        QTest::mouseClick(brk, Qt::LeftButton);
        a7800debug_breakpoint bp[2];
        QCOMPARE(a7800debug_breakpoint_list(m_d, bp, 2), 1);
        QVERIFY(bp[0].type & A7800DEBUG_BP_WRITE);
        QCOMPARE((int)bp[0].start, 0x2510);
        QVERIFY(QString::fromUtf8(bp[0].condition).contains(QStringLiteral("old")));
        a7800debug_breakpoint_clear(m_d);

        /* Memory switches to the Memory tab with the range marked */
        QPushButton *mem = buttonOf(1, QStringLiteral("Memory"));
        QVERIFY(mem);
        QTest::mouseClick(mem, Qt::LeftButton);
        QCOMPARE(m_tabs->currentIndex(), 2);
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$2520: ")));
        QVERIFY(!memView()->extraSelections().isEmpty());
        /* and every watch is marked in the dump, in its own colour */
        bool watched = false;
        for (const auto &s : memView()->extraSelections())
            watched |= s.format.background().color() == QColor(0x1c, 0x71, 0xd8);
        QVERIFY(watched);

        m_tabs->setCurrentIndex(4);
        QTest::mouseClick(buttonOf(1, QStringLiteral("Remove")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QCOMPARE(a7800watch_count(), 1);
        a7800watch_clear();
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 0, 2000);
        m_tabs->setCurrentIndex(0);
    }

    /* Go to marks a range; Find Next / Previous step through hits. */
    void memorySearch()
    {
        const uint8_t pat[4] = { 0xDE, 0xEE, 0xDE, 0xEE };
        for (int i = 0; i < 4; ++i) a7800debug_write(m_d, (uint16_t)(0x2600 + i), pat[i]);
        for (int i = 0; i < 4; ++i) a7800debug_write(m_d, (uint16_t)(0x2700 + i), pat[i]);
        m_tabs->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QLineEdit *go = entryIs(QStringLiteral("$4000, label or lo-hi"));
        typeInto(go, QStringLiteral("2600-2603"));
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$2600: DE EE DE EE")));
        QVERIFY(!memView()->extraSelections().isEmpty());
        /* a label also works */
        a7800debug_set_label(m_d, 0x2700, "found");
        typeInto(go, QStringLiteral("found"));
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$2700: DE EE")));
        a7800debug_set_label(m_d, 0x2700, nullptr);

        QLineEdit *find = entry(QStringLiteral("bytes or text"));
        QVERIFY(find);
        QLabel *note = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->styleSheet().contains(QLatin1String("gray")) && l->text().isEmpty()) note = l;
        QVERIFY(note);
        typeInto(go, QStringLiteral("2600-2603"));
        find->setText(QStringLiteral("DE EE DE"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Found at $2700"));          /* after the marked one */
        QTest::mouseClick(button(QStringLiteral("Find Previous")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Found at $2600"));
        find->setText(QStringLiteral("DE EE DE EE FF 77 66"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Not found"));
        QCOMPARE(m_win->statusBar()->currentMessage(), QStringLiteral("Not found"));
        /* text, case-insensitive */
        for (int i = 0; i < 5; ++i) a7800debug_write(m_d, (uint16_t)(0x2780 + i), (uint8_t)"HeLLo"[i]);
        m_tabs->currentWidget()->findChild<QComboBox *>()->setCurrentText(QStringLiteral("Text"));
        find->setText(QStringLiteral("hello"));
        typeInto(go, QStringLiteral("2400"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Found at $2780"));
        shot(QStringLiteral("search"));
        m_tabs->currentWidget()->findChild<QComboBox *>()->setCurrentText(QStringLiteral("Hex"));
        /* it needs a stopped machine */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!a7800debug_is_stopped(m_d), 2000);
        find->setText(QStringLiteral("DE EE"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(m_win->statusBar()->currentMessage(), QStringLiteral("Stop the machine first"));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        m_tabs->setCurrentIndex(0);
    }

    /* Trace and History share the ring and the recorder. */
    void traceAndHistory()
    {
        a7800debug_trace_enable(m_d, 0);
        a7800debug_trace_clear(m_d);
        m_tabs->setCurrentIndex(6);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QPushButton *rec = m_tabs->currentWidget()->findChild<QPushButton *>();
        QVERIFY(rec && rec->text() == QLatin1String("Record"));
        QPushButton *rec2 = m_tabs->widget(7)->findChild<QPushButton *>();
        QVERIFY(rec2 && rec2 != rec);
        QVERIFY(!rec->isChecked());
        QTest::mouseClick(rec, Qt::LeftButton);
        QVERIFY(a7800debug_trace_enabled(m_d));
        QVERIFY(rec->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(rec2->isChecked(), 2000);                   /* in sync */
        QTest::keyClick(m_win, Qt::Key_F5);                                  /* runs */
        QTRY_VERIFY_WITH_TIMEOUT(!a7800debug_is_stopped(m_d), 2000);
        auto *tr = m_tabs->currentWidget()->findChild<QPlainTextEdit *>();
        QTRY_VERIFY_WITH_TIMEOUT(tr->toPlainText().contains(QStringLiteral("ADC #$01")), 3000);   /* live */
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        QTRY_VERIFY_WITH_TIMEOUT(a7800debug_trace_count(m_d) > 20, 2000);
        /* History shows the same ring, oldest first, at the stop */
        m_tabs->setCurrentIndex(7);
        auto *hist = m_tabs->currentWidget()->findChild<QPlainTextEdit *>();
        QTRY_VERIFY_WITH_TIMEOUT(hist->toPlainText().contains(QStringLiteral("ADC #$01")), 2000);
        QVERIFY(hist->toPlainText().count(QLatin1Char('\n')) < 2000);
        /* newest at the bottom: the view is scrolled to its end */
        QVERIFY(hist->verticalScrollBar()->value() >= hist->verticalScrollBar()->maximum() - 1);
        QLabel *note = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->text().contains(QLatin1String("instructions"))) note = l;
        QVERIFY(note && note->text().startsWith(QLatin1String("recording")));
        shot(QStringLiteral("history"));
        /* a branches-only filter leaves jumps, calls and returns */
        auto *filter = m_tabs->currentWidget()->findChild<QComboBox *>();
        filter->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(!hist->toPlainText().contains(QStringLiteral("ADC #$01")), 2000);
        QVERIFY(hist->toPlainText().contains(QStringLiteral("JMP ")));
        /* a range filter */
        filter->setCurrentIndex(2);
        auto *range = m_tabs->currentWidget()->findChild<QLineEdit *>();
        typeInto(range, QStringLiteral("2410-2410"));
        QTRY_VERIFY_WITH_TIMEOUT(hist->toPlainText().contains(QStringLiteral("STA $2500")) &&
                                     !hist->toPlainText().contains(QStringLiteral("ADC #$01")), 2000);
        filter->setCurrentIndex(0);
        /* Clear */
        for (QPushButton *b : m_tabs->currentWidget()->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Clear")) QTest::mouseClick(b, Qt::LeftButton);
        QCOMPARE(a7800debug_trace_count(m_d), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!hist->toPlainText().contains(QStringLiteral("ADC #$01")), 2000);
        /* and off, from either tab */
        QTest::mouseClick(m_tabs->widget(7)->findChild<QPushButton *>(), Qt::LeftButton);
        QVERIFY(!a7800debug_trace_enabled(m_d));
        QTRY_VERIFY_WITH_TIMEOUT(!m_tabs->widget(6)->findChild<QPushButton *>()->isChecked(), 2000);
        m_tabs->setCurrentIndex(0);
    }

    /* Any tab can leave for a window of its own and come back to its place. */
    void popOutAndDock()
    {
        m_tabs->setCurrentIndex(3);
        QPushButton *pop = button(QStringLiteral("Pop Out"));
        QVERIFY(pop);
        QTest::mouseClick(pop, Qt::LeftButton);
        QCOMPARE(m_tabs->count(), 11);
        QWidget *win = nullptr;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->windowTitle() == QLatin1String("Atari 7800 Debugger - Breakpoints")) win = w;
        QVERIFY(win);
        QTRY_VERIFY_WITH_TIMEOUT(win->isVisible(), 1000);
        QVERIFY(win->findChild<QTableWidget *>());
        QCOMPARE(win->property("a7800HelpContext").toString(), QStringLiteral("debugger/breakpoints"));
        /* the popped tab keeps refreshing: a breakpoint added shows in its window */
        a7800debug_breakpoint_toggle(m_d, 0x2407);
        QTRY_COMPARE_WITH_TIMEOUT(win->findChild<QTableWidget *>()->rowCount(), 1, 2000);
        a7800debug_breakpoint_clear(m_d);
        shot(QStringLiteral("popout"));
        QPushButton *dockBtn = nullptr;
        for (QPushButton *b : win->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Dock")) dockBtn = b;
        QVERIFY(dockBtn);
        QTest::mouseClick(dockBtn, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(m_tabs->count(), 12, 1000);
        QCOMPARE(m_tabs->tabText(3), QStringLiteral("Breakpoints"));       /* back in its place */
        /* closing the window docks it too; several can be out, but one stays */
        m_tabs->setCurrentIndex(5);
        QTest::mouseClick(pop, Qt::LeftButton);
        QCOMPARE(m_tabs->count(), 11);
        QCOMPARE(m_tabs->tabText(5), QStringLiteral("Trace"));
        QWidget *cart = nullptr;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->windowTitle() == QLatin1String("Atari 7800 Debugger - Cart & I/O")) cart = w;
        QVERIFY(cart);
        cart->close();
        QTRY_COMPARE_WITH_TIMEOUT(m_tabs->count(), 12, 1000);
        QCOMPARE(m_tabs->tabText(5), QStringLiteral("Cart && I/O"));
        for (int i = 0; i < 11; ++i) {
            m_tabs->setCurrentIndex(0);
            QTest::mouseClick(pop, Qt::LeftButton);
        }
        QCOMPARE(m_tabs->count(), 1);
        QTest::mouseClick(pop, Qt::LeftButton);                          /* the last stays */
        QCOMPARE(m_tabs->count(), 1);
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->windowTitle().startsWith(QLatin1String("Atari 7800 Debugger - "))) w->close();
        QTRY_COMPARE_WITH_TIMEOUT(m_tabs->count(), 12, 1000);
        QStringList names;
        for (int i = 0; i < m_tabs->count(); ++i) names << m_tabs->tabText(i);
        QCOMPARE(names.first(), QStringLiteral("Console"));
        QCOMPARE(names.last(), QStringLiteral("FujiNet"));
        QCOMPARE(names.at(5), QStringLiteral("Cart && I/O"));
        QCOMPARE(names.at(10), QStringLiteral("Symbols"));
    }

    /* A new stop pulls every view back to the spot from wherever it was
     * scrolled: the disassembly to the PC, Memory to the PC, the Trace and
     * History to their newest line; and the Breakpoints tab says why. */
    void stopRecentresAndNames()
    {
        m_tabs->setCurrentIndex(1);
        QCheckBox *follow = check(QStringLiteral("Follow PC"));
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x2405) >= 0, 2000);
        const QPoint p = v->viewport()->rect().center();
        QWheelEvent wheel(p, v->viewport()->mapToGlobal(p), QPoint(), QPoint(0, -600), Qt::NoButton, {},
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(v->viewport(), &wheel);
        QVERIFY(!follow->isChecked());
        /* Memory scrolled to the top, History away from its end */
        m_tabs->setCurrentIndex(2);
        memView()->verticalScrollBar()->setValue(0);
        m_tabs->setCurrentIndex(1);
        QTest::keyClick(m_win, Qt::Key_F7);
        QTRY_VERIFY_WITH_TIMEOUT(follow->isChecked(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, pcOf(m_d)) >= 0, 2000);
        m_tabs->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(memView()->verticalScrollBar()->value() >= pcOf(m_d) / 16 - 80, 2000);
        m_tabs->setCurrentIndex(3);
        QLabel *stop = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->text().startsWith(QLatin1String("Stopped"))) stop = l;
        QVERIFY(stop);
        QVERIFY(stop->text().startsWith(QLatin1String("Stopped")));
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(stop->text() == QLatin1String("Running."), 2000);
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        QVERIFY(button(QStringLiteral("Clear Hits")));
        m_tabs->setCurrentIndex(0);
    }

    void otherTabs()
    {
        /* Cart & I/O: the cartridge, the memory map once, INPTCTRL and the controls */
        m_tabs->setCurrentIndex(5);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        auto *io = qobject_cast<QPlainTextEdit *>(m_tabs->currentWidget());
        QVERIFY(io);
        QTRY_VERIFY_WITH_TIMEOUT(!io->toPlainText().isEmpty(), 2000);
        const QString text = io->toPlainText();
        QCOMPARE(text.count(QStringLiteral("Memory map")), 1);
        QVERIFY(text.contains(QStringLiteral("INPTCTRL")));
        QVERIFY(text.contains(QStringLiteral("SWCHA")));
        QVERIFY(text.contains(QStringLiteral("INPT0-5")));
        QVERIFY(text.contains(QStringLiteral("$1800-")));
        shot(QStringLiteral("cart"));
        /* Sound: the TIA, the POKEY when there is one */
        m_tabs->setCurrentIndex(9);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        auto *sound = qobject_cast<QPlainTextEdit *>(m_tabs->currentWidget());
        QVERIFY(sound);
        QTRY_VERIFY_WITH_TIMEOUT(sound->toPlainText().contains(QStringLiteral("TIA")), 2000);
        QVERIFY(sound->toPlainText().contains(QStringLiteral("POKEY")));
        QVERIFY(!sound->toPlainText().contains(QStringLiteral("INPTCTRL")));      /* in Cart & I/O */
        shot(QStringLiteral("sound"));
        /* FujiNet: empty without the runtime, and says so */
        m_tabs->setCurrentIndex(11);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        bool note = false;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            note |= l->text().contains(QLatin1String("FujiNet"));
        QVERIFY(note);
        QVERIFY(m_tabs->currentWidget()->findChild<QPushButton *>()->text() == QLatin1String("Clear"));
        shot(QStringLiteral("fujinet"));
        m_tabs->setCurrentIndex(0);
    }

    /* The Video tab on the real CONFIG display (its display list, its
     * objects): views, the Decoded source, zoom, overlays, hover and pin, the
     * tables, Break on writes and Break on a palette register's write. A fresh
     * machine is run for a few frames and stopped at a vertical blank. */
    void videoTab()
    {
        a7800debug_breakpoint_clear(m_d);
        m_tabs->setCurrentIndex(0);
        a7800session_stop(m_s);
        QCOMPARE(a7800session_start(m_s, &m_opts), 0);
        /* the engine stays attached; the new machine runs */
        QTest::qWait(200);
        a7800debug_cpu c, c2;
        a7800debug_cpu_get(m_d, &c);
        QTRY_VERIFY_WITH_TIMEOUT((a7800debug_cpu_get(m_d, &c2), c2.frame >= c.frame + 120), 8000);
        QTest::keyClick(m_win, Qt::Key_F5);                             /* stop */
        waitStopped();
        QTest::keyClick(m_win, Qt::Key_F7, Qt::ShiftModifier);          /* a frame: stops at the vblank */
        waitStopped();
        QTest::qWait(100);

        m_tabs->setCurrentIndex(8);
        QWidget *page = m_tabs->currentWidget();
        QTRY_VERIFY_WITH_TIMEOUT(page->isVisible(), 1000);
        auto combos = page->findChildren<QComboBox *>();
        QVERIFY(combos.size() >= 3);
        QComboBox *view = combos[0], *source = combos[1], *zoom = combos[2];
        QStringList views;
        for (int i = 0; i < view->count(); ++i) views << view->itemText(i);
        QCOMPARE(views, (QStringList{ QStringLiteral("Display"), QStringLiteral("Decoded"), QStringLiteral("Beam"),
                                      QStringLiteral("Values") }));
        QCOMPARE(source->itemText(0), QStringLiteral("Objects"));
        QCOMPARE(source->count(), 3);
        QVERIFY(!source->isEnabled());                      /* only Decoded has a source */
        QCOMPARE(zoom->itemText(0), QStringLiteral("Fit"));
        QWidget *pic = page->findChild<QWidget *>(QStringLiteral("videoPicture"));
        QVERIFY(pic);
        QTRY_VERIFY_WITH_TIMEOUT(pic->width() >= 100 && pic->height() >= 100, 2000);
        shot(QStringLiteral("video-display"));

        /* a change of view redraws */
        view->setCurrentIndex(1);
        QVERIFY(source->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(pic->width() >= 100, 1000);
        shot(QStringLiteral("video-decoded"));
        for (int i = 1; i < source->count(); ++i) {
            source->setCurrentIndex(i);
            QTest::qWait(30);
            QTRY_VERIFY_WITH_TIMEOUT(pic->width() >= 100, 1000);
            shot(QStringLiteral("video-decoded-%1").arg(i));
        }
        source->setCurrentIndex(0);
        view->setCurrentIndex(2);
        QTest::qWait(30);
        QTRY_VERIFY_WITH_TIMEOUT(pic->width() >= 100, 1000);     /* the machine is stopped: Beam works */
        shot(QStringLiteral("video-beam"));
        view->setCurrentIndex(3);
        QTest::qWait(30);
        shot(QStringLiteral("video-values"));
        view->setCurrentIndex(0);

        /* zoom resizes the picture */
        zoom->setCurrentIndex(zoom->findText(QStringLiteral("1×")));
        QTest::qWait(50);
        const int w1 = pic->width();
        zoom->setCurrentIndex(zoom->findText(QStringLiteral("2×")));
        QTRY_VERIFY2_WITH_TIMEOUT(pic->width() == w1 * 2, qPrintable(QStringLiteral("1x %1 2x %2").arg(w1).arg(pic->width())), 1000);
        zoom->setCurrentIndex(zoom->findText(QStringLiteral("1×")));

        /* an overlay checkbox */
        QCheckBox *zones = nullptr;
        for (QCheckBox *box : page->findChildren<QCheckBox *>())
            if (box->text().startsWith(QLatin1String("Zones"))) zones = box;
        QVERIFY(zones);
        zones->setChecked(true);
        QTest::qWait(50);
        shot(QStringLiteral("video-zones"));
        zones->setChecked(false);

        /* hover shows, a click pins, the same spot unpins */
        auto *tabs = page->findChild<QTabWidget *>();
        QVERIFY(tabs);
        QCOMPARE(tabs->tabText(0), QStringLiteral("Pixel"));
        tabs->setCurrentIndex(0);
        QLabel *pixel = nullptr;
        for (QLabel *l : tabs->widget(0)->findChildren<QLabel *>()) pixel = l;
        QVERIFY(pixel);
        QVERIFY(pixel->text().contains(QLatin1String("Move over")));
        const QPoint at(pic->width() / 2, pic->height() / 2);
        QTest::mouseMove(pic, at);
        QTRY_VERIFY_WITH_TIMEOUT(!pixel->text().contains(QLatin1String("Move over")), 1000);
        QTest::mouseClick(pic, Qt::LeftButton, {}, at);
        QTest::mouseMove(pic, at + QPoint(30, 30));
        QVERIFY(!pixel->text().contains(QLatin1String("Move over")));   /* the pin stays */
        shot(QStringLiteral("video-pixel"));
        QTest::mouseClick(pic, Qt::LeftButton, {}, at);
        QTest::mouseMove(pic, QPoint(-5, -5));

        /* the tables */
        QTableWidget *regs = nullptr, *lines = nullptr, *colours = nullptr;
        for (QTableWidget *t : page->findChildren<QTableWidget *>()) {
            if (t->horizontalHeaderItem(0) && t->horizontalHeaderItem(0)->text() == QLatin1String("Name")) regs = t;
            if (t->horizontalHeaderItem(0) && t->horizontalHeaderItem(0)->text() == QLatin1String("Row")) lines = t;
            if (t->horizontalHeaderItem(1) && t->horizontalHeaderItem(1)->text() == QLatin1String("Register")) colours = t;
        }
        QVERIFY(regs && lines && colours);
        tabs->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(regs->rowCount() > 0, 1000);
        shot(QStringLiteral("video-registers"));
        tabs->setCurrentIndex(2);
        QTRY_COMPARE_WITH_TIMEOUT(colours->rowCount(), 32, 1000);
        shot(QStringLiteral("video-colours"));
        tabs->setCurrentIndex(3);
        QTRY_VERIFY_WITH_TIMEOUT(lines->rowCount() > 0, 1000);
        shot(QStringLiteral("video-lines"));
        tabs->setCurrentIndex(4);
        QTest::qWait(50);
        shot(QStringLiteral("video-timing"));

        /* Break on write, per palette register */
        tabs->setCurrentIndex(2);
        auto *swatchButton = qobject_cast<QPushButton *>(colours->cellWidget(3, 5));
        QVERIFY(swatchButton);
        a7800debug_breakpoint bps[16];
        QCOMPARE(a7800debug_breakpoint_list(m_d, bps, 16), 0);
        QTest::mouseClick(swatchButton, Qt::LeftButton);
        QCOMPARE(a7800debug_breakpoint_list(m_d, bps, 16), 1);
        QVERIFY(bps[0].type & A7800DEBUG_BP_WRITE);
        QCOMPARE((int)bps[0].start, 0x23);
        a7800debug_breakpoint_clear(m_d);
        tabs->setCurrentIndex(0);

        /* Break on writes: write breakpoints on the inspected pixel's objects, zone and palette register */
        QPushButton *brk = nullptr;
        for (QPushButton *b : page->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Break on writes")) brk = b;
        QVERIFY(brk);
        int made = 0;
        for (int row = 1; row < 9 && !made; ++row)              /* a pixel the display uses */
            for (int col = 1; col < 5 && !made; ++col) {
                const QPoint spot(pic->width() * col / 5, pic->height() * row / 9);
                QTest::mouseMove(pic, spot);
                QTest::mouseClick(pic, Qt::LeftButton, {}, spot);
                QTest::mouseClick(brk, Qt::LeftButton);
                made = a7800debug_breakpoint_list(m_d, bps, 16);
            }
        QVERIFY2(made >= 1, "Break on writes made no breakpoint on any pixel");
        for (int i = 0; i < made; ++i) QVERIFY(bps[i].type & A7800DEBUG_BP_WRITE);
        shot(QStringLiteral("video-pixel-break"));
        a7800debug_breakpoint_clear(m_d);
        QTest::mouseClick(page->findChildren<QPushButton *>().first(), Qt::LeftButton);    /* a harmless click */
        zoom->setCurrentIndex(0);
        view->setCurrentIndex(0);
        m_tabs->setCurrentIndex(0);
    }

    /* The main window's F12 (display focused: its ShortcutOverride lets the
     * chord through to the View menu's action) and the View menu entry. */
    void f12FromTheMainWindow()
    {
        /* the display is a QRhiWidget; with no GPU device (CI's offscreen Qt)
         * the window swaps in the software display, so ask for whichever
         * is there */
        QVERIFY(QFile::exists(QStringLiteral(":/shaders/display.vert.qsb")));
        QVERIFY(QFile::exists(QStringLiteral(":/shaders/display.frag.qsb")));
        MainWindow main(m_s);
        main.show();
        QVERIFY(QTest::qWaitForWindowExposed(&main));
        main.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&main));
        QTest::qWait(300);   /* let a queued renderFailed swap the display */
        QWidget *display = main.centralWidget();
        QVERIFY(display);
        display->setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(QApplication::focusWidget() == display, 2000);
        QCOMPARE(QApplication::focusWidget(), display);
        QVERIFY(m_win->isVisible());
        QTest::keyClick(display, Qt::Key_F12);
        QVERIFY(!m_win->isVisible());
        QVERIFY(!a7800debug_is_attached(m_d));
        main.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&main));
        QTest::keyClick(display, Qt::Key_F12);
        QVERIFY(m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(a7800debug_is_attached(m_d), 2000);
        waitStopped();
        /* the menu entry does the same */
        QAction *entry = nullptr;
        for (QAction *a : main.findChildren<QAction *>())
            if (a->text() == QLatin1String("&Debugger")) entry = a;
        QVERIFY(entry);
        QCOMPARE(entry->shortcut(), QKeySequence(Qt::Key_F12));
        entry->trigger();
        QVERIFY(!m_win->isVisible());
        entry->trigger();
        QVERIFY(m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(a7800debug_is_attached(m_d), 2000);
        waitStopped();
    }

    /* A power cycle or media change keeps the engine attached (the new
     * machine runs on); the window follows a stop of it. */
    void sessionRestart()
    {
        m_tabs->setCurrentIndex(1);
        a7800debug_breakpoint_toggle(m_d, 0x2407);
        QCOMPARE(a7800session_power_cycle(m_s), 0);
        QVERIFY(a7800debug_is_attached(m_d));
        a7800debug_stop(m_d);                                    /* the new machine runs on: stop it */
        QTRY_VERIFY_WITH_TIMEOUT(a7800debug_is_stopped(m_d), 3000);
        const int resetPc = pcOf(m_d);
        QVERIFY(a7800debug_breakpoint_check(m_d, 0x2407));       /* kept */
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().startsWith(QLatin1String("Stopped")), 2000);
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(v->toPlainText().contains(QStringLiteral(">%1").arg(resetPc, 4, 16, QLatin1Char('0')).toUpper()),
                                 2000);
        /* and it runs on from there: frames pass, and F5 stops it again */
        a7800debug_cpu c, c2;
        a7800debug_cpu_get(m_d, &c);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT((a7800debug_cpu_get(m_d, &c2), c2.frame > c.frame + 2), 5000);
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        a7800debug_breakpoint_clear(m_d);
    }

    void f12HidesAndDetaches()
    {
        QVERIFY(m_win->isVisible());
        QTest::keyClick(m_win, Qt::Key_F12);
        QVERIFY(!m_win->isVisible());
        QVERIFY(!a7800debug_is_attached(m_d));
        a7800debug_cpu c, c2;
        a7800debug_cpu_get(m_d, &c);
        QTRY_VERIFY_WITH_TIMEOUT((a7800debug_cpu_get(m_d, &c2), c2.frame > c.frame + 3), 3000);
        /* the main window's F12 is toggleFor: it shows the window and attaches (stopping) */
        DebuggerWindow::toggleFor(nullptr, m_s);
        QVERIFY(m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(a7800debug_is_attached(m_d), 2000);
        waitStopped();
        DebuggerWindow::toggleFor(nullptr, m_s);
        QVERIFY(!m_win->isVisible());
        QVERIFY(!a7800debug_is_attached(m_d));
        DebuggerWindow::toggleFor(nullptr, m_s);
        QTRY_VERIFY_WITH_TIMEOUT(a7800debug_is_attached(m_d), 2000);
        waitStopped();
    }
};

QTEST_MAIN(DebuggerTest)
#include "debugger_test.moc"
