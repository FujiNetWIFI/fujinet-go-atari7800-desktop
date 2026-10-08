/*
 * DebuggerWindow -- see DebuggerWindow.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DebuggerWindow.h"

#include <cstring>

#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QScrollBar>
#include <QSplitter>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextCursor>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "../FujiNetWindows.h"

#define DISASM_WINDOW 48
#define MEM_ROWS 64

namespace {

QPlainTextEdit *monoView(bool editable)
{
    auto *v = new QPlainTextEdit;
    v->setReadOnly(!editable);
    QFont f = v->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    v->setFont(f);
    v->setLineWrapMode(QPlainTextEdit::NoWrap);
    return v;
}

QString stripControl(const char *s)
{
    QString out;
    for (; *s; ++s)
        if ((unsigned char)*s >= 0x20 || *s == '\n' || *s == '\t') out += QChar(*s);
    return out;
}

bool parseNum(const QString &t, long *out)
{
    QString s = t.trimmed();
    bool ok = false;
    long v = 0;
    if (s.startsWith('$')) v = s.mid(1).toLong(&ok, 16);
    else if (s.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) v = s.mid(2).toLong(&ok, 16);
    else if (s.startsWith('#')) v = s.mid(1).toLong(&ok, 10);
    else v = s.toLong(&ok, 16);
    if (ok) *out = v;
    return ok;
}

QString hx(unsigned v, int digits)
{
    return QStringLiteral("%1").arg(v, digits, 16, QLatin1Char('0')).toUpper();
}

const char *onOff(int v) { return v ? "on" : "off"; }

/* The names of a port's held a7800_action bits. */
QString heldNames(unsigned held)
{
    static const char *const names[A7800_ACT_PER_PORT] = { "Up", "Down", "Left", "Right", "Button 1", "Button 2" };
    QStringList out;
    for (int b = 0; b < A7800_ACT_PER_PORT; ++b)
        if (held & (1u << b)) out << QString::fromUtf8(names[b]);
    return out.isEmpty() ? QStringLiteral("nothing") : out.join(QStringLiteral(", "));
}

QPointer<DebuggerWindow> s_win;

} // namespace

void DebuggerWindow::showFor(QWidget *parent, a7800session *session)
{
    if (!s_win) s_win = new DebuggerWindow(session, parent);
    s_win->show();
    s_win->raise();
    s_win->activateWindow();
    /* Opening the debugger stops the machine, as on every sibling. */
    a7800debug_attach(s_win->m_dbg);
    s_win->refreshAll();
}

void DebuggerWindow::toggleFor(QWidget *parent, a7800session *session)
{
    if (s_win && s_win->isVisible()) s_win->hide();
    else showFor(parent, session);
}

DebuggerWindow::DebuggerWindow(a7800session *session, QWidget *parent)
    : QMainWindow(parent, Qt::Window), m_session(session), m_dbg(a7800session_debugger(session))
{
    setObjectName(QStringLiteral("debugger"));
    setWindowTitle(QStringLiteral("Debugger"));
    resize(1100, 780);
    m_palettePx.resize(A7800DEBUG_PALETTE_WIDTH * A7800DEBUG_PALETTE_HEIGHT);

    auto *central = new QWidget;
    auto *root = new QVBoxLayout(central);
    root->addWidget(buildToolbar());
    auto *tabs = new QTabWidget;
    tabs->addTab(buildPrompt(), QStringLiteral("Console"));
    tabs->addTab(buildCpu(), QStringLiteral("CPU && Memory"));
    tabs->addTab(buildDisasm(), QStringLiteral("Disassembly"));
    tabs->addTab(buildMaria(), QStringLiteral("MARIA"));
    tabs->addTab(buildIo(), QStringLiteral("I/O"));
    tabs->addTab(buildCart(), QStringLiteral("Cartridge"));
    tabs->addTab(buildBreaks(), QStringLiteral("Breakpoints && Watchpoints"));
    /* A7800_DEBUGGER_TAB: a tab's index, or the start of its name */
    const QString tab = qEnvironmentVariable("A7800_DEBUGGER_TAB");
    if (!tab.isEmpty()) {
        bool isIndex = false;
        const int idx = tab.toInt(&isIndex);
        for (int i = 0; i < tabs->count(); ++i)
            if (isIndex ? i == idx : tabs->tabText(i).remove(QLatin1Char('&')).startsWith(tab, Qt::CaseInsensitive)) {
                tabs->setCurrentIndex(i);
                break;
            }
    }
    root->addWidget(tabs, 1);
    setCentralWidget(central);

    connect(&m_timer, &QTimer::timeout, this, &DebuggerWindow::tick);
    m_timer.start(100);
}

/* ---- building ------------------------------------------------------------- */

void DebuggerWindow::stepAnd(void (*fn)(a7800debug *))
{
    fn(m_dbg);
    refreshAll();
}

QWidget *DebuggerWindow::buildToolbar()
{
    auto *bar = new QWidget;
    auto *h = new QHBoxLayout(bar);
    h->setContentsMargins(0, 0, 0, 0);
    auto add = [&](const QString &label, auto fn) {
        auto *b = new QPushButton(label);
        b->setFocusPolicy(Qt::NoFocus);
        connect(b, &QPushButton::clicked, this, fn);
        h->addWidget(b);
        return b;
    };
    m_runBtn = add(QStringLiteral("Stop (F5)"), [this] {
        if (a7800debug_is_stopped(m_dbg)) a7800debug_resume(m_dbg); else a7800debug_stop(m_dbg);
        refreshAll();
    });
    add(QStringLiteral("Step (F7)"), [this] { stepAnd(a7800debug_step); });
    add(QStringLiteral("Step Over (F8)"), [this] { stepAnd(a7800debug_step_over); });
    add(QStringLiteral("Step Out (⇧F8)"), [this] { stepAnd(a7800debug_step_out); });
    add(QStringLiteral("Frame"), [this] { stepAnd(a7800debug_frame); });
    add(QStringLiteral("Run to Cursor"), [this] {
        if (m_selAddr < 0) { m_status->setText(QStringLiteral("Click a disassembly line first")); return; }
        a7800debug_run_to(m_dbg, (uint16_t)m_selAddr);
        refreshAll();
    });
    m_status = new QLabel;
    m_status->setStyleSheet(QStringLiteral("color: gray;"));
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    h->addWidget(m_status, 1);
    return bar;
}

QWidget *DebuggerWindow::buildPrompt()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    m_promptOut = monoView(false);
    m_promptOut->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_promptOut->setPlainText(QStringLiteral("MAME's debugger console. Type 'help' for every command; "
                                             "'cart' and 'maria' are this app's own.\n"));
    auto *row = new QHBoxLayout;
    m_promptIn = new QLineEdit;
    m_promptIn->setPlaceholderText(QStringLiteral("command (help, step, over, out, go, bpset, wpset, print, dump, cart, maria ...) — Tab completes"));
    m_promptIn->installEventFilter(this);
    connect(m_promptIn, &QLineEdit::returnPressed, this, &DebuggerWindow::runPrompt);
    auto *sym = new QPushButton(QStringLiteral("Load Symbols..."));
    connect(sym, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Load Symbols"), QString(),
            QStringLiteral("Symbol files (*.dbg *.lbl *.sym *.labels);;All files (*)"));
        if (path.isEmpty()) return;
        char msg[512];
        a7800debug_load_symbols(m_dbg, path.toLocal8Bit().constData(), msg, sizeof msg);
        appendPrompt(QString::fromUtf8(msg) + QLatin1Char('\n'));
        refreshAll();
    });
    auto *save = new QPushButton(QStringLiteral("Save..."));
    auto *menu = new QMenu(save);
    static const struct { const char *kind, *title; } saves[] = {
        { "dis", "Disassembly ($4000-$FFFF)" }, { "ram", "RAM ($1800-$27FF)" },
        { "mem", "Memory (the whole 64K bus)" } };
    for (const auto &s : saves) {
        const char *kind = s.kind;
        const char *title = s.title;
        menu->addAction(QString::fromUtf8(title), this, [this, kind, title] {
            const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save %1").arg(QString::fromUtf8(title)));
            if (path.isEmpty()) return;
            char msg[512];
            a7800debug_save(m_dbg, kind, path.toLocal8Bit().constData(), msg, sizeof msg);
            appendPrompt(stripControl(msg) + QLatin1Char('\n'));
        });
    }
    save->setMenu(menu);
    row->addWidget(m_promptIn, 1);
    row->addWidget(sym);
    row->addWidget(save);
    v->addWidget(m_promptOut, 1);
    v->addLayout(row);
    return w;
}

QWidget *DebuggerWindow::buildCpu()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *regs = new QHBoxLayout;
    static const char *const names[6] = { "PC", "SP", "A", "X", "Y", "P" };
    static const int regIds[6] = { A7800_REG_PC, A7800_REG_SP, A7800_REG_A, A7800_REG_X, A7800_REG_Y, A7800_REG_PS };
    for (int i = 0; i < 6; ++i) {
        regs->addWidget(new QLabel(QString::fromUtf8(names[i])));
        m_reg[i] = new QLineEdit;
        m_reg[i]->setMaxLength(5);
        m_reg[i]->setMaximumWidth(64);
        connect(m_reg[i], &QLineEdit::returnPressed, this, [this, i] {
            long val;
            if (parseNum(m_reg[i]->text(), &val)) a7800debug_cpu_set(m_dbg, regIds[i], (int)val);
            m_reg[i]->clearFocus();
            refreshAll();
        });
        regs->addWidget(m_reg[i]);
    }
    regs->addStretch();
    v->addLayout(regs);
    auto *flags = new QHBoxLayout;
    static const char *const fnames[6] = { "N", "V", "D", "I", "Z", "C" };
    static const int flagIds[6] = { A7800_FLAG_N, A7800_FLAG_V, A7800_FLAG_D, A7800_FLAG_I, A7800_FLAG_Z, A7800_FLAG_C };
    for (int i = 0; i < 6; ++i) {
        m_flag[i] = new QCheckBox(QString::fromUtf8(fnames[i]));
        connect(m_flag[i], &QCheckBox::clicked, this, [this, i](bool on) {
            if (a7800debug_is_stopped(m_dbg)) a7800debug_cpu_set(m_dbg, flagIds[i], on);
            refreshAll();
        });
        flags->addWidget(m_flag[i]);
    }
    m_cycles = new QLabel;
    m_cycles->setStyleSheet(QStringLiteral("color: gray;"));
    flags->addWidget(m_cycles, 1);
    v->addLayout(flags);

    /* the 6502's view of memory, from any address */
    auto *from = new QHBoxLayout;
    from->addWidget(new QLabel(QStringLiteral("Memory from")));
    m_memFrom = new QLineEdit(QStringLiteral("$1800"));
    m_memFrom->setMaximumWidth(80);
    connect(m_memFrom, &QLineEdit::returnPressed, this, [this] {
        long a;
        int lbl = a7800debug_label_address(m_dbg, m_memFrom->text().trimmed().toUtf8().constData());
        if (lbl < 0 && parseNum(m_memFrom->text(), &a)) lbl = (int)(a & 0xFFFF);
        if (lbl < 0) { m_status->setText(QStringLiteral("No such address or label")); return; }
        m_memTop = (uint16_t)(lbl & 0xFFF0);
        refreshMemory();
    });
    from->addWidget(m_memFrom);
    from->addWidget(new QLabel(QStringLiteral("(RAM $1800-$27FF, zero page $40-$FF, the cartridge from $4000)")));
    from->addStretch();
    v->addLayout(from);
    m_mem = monoView(false);
    v->addWidget(m_mem, 1);
    auto *edit = new QHBoxLayout;
    m_memAddr = new QLineEdit; m_memAddr->setPlaceholderText(QStringLiteral("$1800")); m_memAddr->setMaximumWidth(80);
    m_memVal = new QLineEdit; m_memVal->setPlaceholderText(QStringLiteral("$00")); m_memVal->setMaximumWidth(60);
    auto write = [this] {
        long a, val;
        if (parseNum(m_memAddr->text(), &a) && parseNum(m_memVal->text(), &val))
            a7800debug_write(m_dbg, (uint16_t)a, (uint8_t)val);
        refreshAll();
    };
    connect(m_memVal, &QLineEdit::returnPressed, this, write);
    connect(m_memAddr, &QLineEdit::returnPressed, this, write);
    edit->addWidget(new QLabel(QStringLiteral("Write address")));
    edit->addWidget(m_memAddr);
    edit->addWidget(new QLabel(QStringLiteral("value")));
    edit->addWidget(m_memVal);
    edit->addStretch();
    v->addLayout(edit);
    return w;
}

QWidget *DebuggerWindow::buildDisasm()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    m_followPc = new QCheckBox(QStringLiteral("Follow PC"));
    m_followPc->setChecked(true);
    connect(m_followPc, &QCheckBox::toggled, this, [this](bool on) { if (on) refreshDisasm(); });
    m_jump = new QLineEdit;
    m_jump->setPlaceholderText(QStringLiteral("$address or label"));
    m_jump->setMaximumWidth(160);
    connect(m_jump, &QLineEdit::returnPressed, this, [this] { jumpTo(m_jump->text()); });
    row->addWidget(m_followPc);
    row->addWidget(new QLabel(QStringLiteral("Jump to")));
    row->addWidget(m_jump);
    row->addWidget(new QLabel(QStringLiteral("Click a line to select it; double-click or F9 toggles its breakpoint; scroll to browse")));
    row->addStretch();
    v->addLayout(row);
    m_disasm = monoView(false);
    m_disasm->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_disasm->viewport()->installEventFilter(this);
    v->addWidget(m_disasm, 1);
    return w;
}

QWidget *DebuggerWindow::buildMaria()
{
    auto *split = new QSplitter(Qt::Horizontal);
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    m_maria = monoView(false);
    lv->addWidget(m_maria, 1);
    lv->addWidget(new QLabel(QStringLiteral("Palettes as written (BACKGRND, then each palette's three colours)")));
    m_palette = new QLabel;
    m_palette->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_palette->setFixedSize(2 * A7800DEBUG_PALETTE_WIDTH, 2 * A7800DEBUG_PALETTE_HEIGHT + 16);
    lv->addWidget(m_palette);
    split->addWidget(left);
    auto *right = new QWidget;
    auto *rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->addWidget(new QLabel(QStringLiteral("Display list list")));
    m_dll = monoView(false);
    rv->addWidget(m_dll, 1);
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    return split;
}

QWidget *DebuggerWindow::buildIo()
{
    m_io = monoView(false);
    m_io->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    return m_io;
}

QWidget *DebuggerWindow::buildCart()
{
    m_cart = monoView(false);
    m_cart->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    return m_cart;
}

QWidget *DebuggerWindow::buildBreaks()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    m_bpType = new QComboBox;
    m_bpType->addItems({ QStringLiteral("Execute"), QStringLiteral("Read"), QStringLiteral("Write"),
                         QStringLiteral("Read/Write") });
    m_bpRange = new QLineEdit;
    m_bpRange->setPlaceholderText(QStringLiteral("$C000 or $1800-$18FF or label"));
    m_bpRange->setMinimumWidth(m_bpRange->fontMetrics().horizontalAdvance(m_bpRange->placeholderText()) + 24);
    m_bpCond = new QLineEdit;
    m_bpCond->setPlaceholderText(QStringLiteral("condition (optional MAME expression, e.g. a == $FF)"));
    auto *add = new QPushButton(QStringLiteral("Add"));
    auto doAdd = [this] {
        static const int types[4] = { A7800DEBUG_BP_EXEC, A7800DEBUG_BP_READ, A7800DEBUG_BP_WRITE,
                                      A7800DEBUG_BP_READ | A7800DEBUG_BP_WRITE };
        const QString text = m_bpRange->text().trimmed();
        if (text.isEmpty()) return;
        const int dash = text.indexOf(QLatin1Char('-'));
        auto addrOf = [this](const QString &s, long *out) {
            const int lbl = a7800debug_label_address(m_dbg, s.trimmed().toUtf8().constData());
            if (lbl >= 0) { *out = lbl; return true; }
            return parseNum(s, out);
        };
        long a, b;
        if (!addrOf(dash > 0 ? text.left(dash) : text, &a)) { m_status->setText(QStringLiteral("Bad address")); return; }
        b = a;
        if (dash > 0 && !addrOf(text.mid(dash + 1), &b)) { m_status->setText(QStringLiteral("Bad address")); return; }
        const QByteArray cond = m_bpCond->text().trimmed().toUtf8();
        if (a7800debug_breakpoint_add(m_dbg, types[m_bpType->currentIndex()], (uint16_t)a, (uint16_t)b,
                                      cond.constData()) < 0) {
            m_status->setText(QStringLiteral("The condition does not parse"));
            return;
        }
        m_bpRange->clear();
        m_bpCond->clear();
        refreshAll();
    };
    connect(add, &QPushButton::clicked, this, doAdd);
    connect(m_bpRange, &QLineEdit::returnPressed, this, doAdd);
    connect(m_bpCond, &QLineEdit::returnPressed, this, doAdd);
    row->addWidget(m_bpType);
    row->addWidget(m_bpRange);
    row->addWidget(m_bpCond, 1);
    row->addWidget(add);
    v->addLayout(row);

    m_bpTable = new QTableWidget(0, 4);
    m_bpTable->setHorizontalHeaderLabels({ QStringLiteral("On"), QStringLiteral("Type"),
                                           QStringLiteral("Address"), QStringLiteral("Condition") });
    m_bpTable->horizontalHeader()->setStretchLastSection(true);
    m_bpTable->verticalHeader()->setVisible(false);
    m_bpTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_bpTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(m_bpTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_bpFilling || item->column() != 0) return;
        a7800debug_breakpoint_enable(m_dbg, item->data(Qt::UserRole).toInt(), item->checkState() == Qt::Checked);
    });
    v->addWidget(m_bpTable, 1);

    auto *btns = new QHBoxLayout;
    auto *remove = new QPushButton(QStringLiteral("Remove"));
    connect(remove, &QPushButton::clicked, this, [this] {
        const int r = m_bpTable->currentRow();
        if (r < 0) return;
        a7800debug_breakpoint_remove(m_dbg, m_bpTable->item(r, 0)->data(Qt::UserRole).toInt());
        refreshAll();
    });
    auto *clear = new QPushButton(QStringLiteral("Clear all"));
    connect(clear, &QPushButton::clicked, this, [this] {
        a7800debug_breakpoint_clear(m_dbg);
        refreshAll();
    });
    btns->addWidget(remove);
    btns->addWidget(clear);
    btns->addStretch();
    v->addLayout(btns);
    return w;
}

/* ---- refresh -------------------------------------------------------------- */

void DebuggerWindow::refreshStatus()
{
    char reason[160];
    int addr;
    const bool stopped = a7800debug_is_stopped(m_dbg) != 0;
    a7800debug_stop_reason(m_dbg, reason, sizeof reason, &addr);
    /* "stopped" says nothing "Stopped" does not */
    const bool why = reason[0] && strcmp(reason, "stopped") != 0;
    m_status->setText(stopped ? QStringLiteral("Stopped%1%2").arg(why ? ": " : "", why ? QString::fromUtf8(reason) : QString())
                              : QStringLiteral("Running"));
    m_runBtn->setText(stopped ? QStringLiteral("Run (F5)") : QStringLiteral("Stop (F5)"));
    m_runBtn->setStyleSheet(stopped ? QStringLiteral("background: %1; color: white;").arg(a7800AccentColor().name()) : QString());
}

void DebuggerWindow::refreshCpu()
{
    a7800debug_cpu c;
    a7800debug_cpu_get(m_dbg, &c);
    /* the stack pointer without its page ($01) */
    const int vals[6] = { c.pc, c.sp & 0xFF, c.a, c.x, c.y, c.ps };
    for (int i = 0; i < 6; ++i)
        if (!m_reg[i]->hasFocus())
            m_reg[i]->setText(hx((unsigned)vals[i], i == 0 ? 4 : 2));
    const int flags[6] = { c.n, c.v, c.d, c.i, c.z, c.c };
    for (int i = 0; i < 6; ++i) m_flag[i]->setChecked(flags[i] != 0);
    m_cycles->setText(QStringLiteral("cycles %1   scanline %2   dot %3   frame %4")
                          .arg(c.total_cycles).arg(c.scanline).arg(c.dot).arg(c.frame));
}

void DebuggerWindow::refreshMemory()
{
    uint8_t mem[MEM_ROWS * 16];
    a7800debug_read(m_dbg, m_memTop, mem, (int)sizeof mem);
    QString text = QStringLiteral("        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");
    for (int row = 0; row < MEM_ROWS; ++row) {
        text += QStringLiteral("$%1: ").arg(hx((m_memTop + row * 16) & 0xFFFF, 4));
        QString ascii;
        for (int col = 0; col < 16; ++col) {
            const uint8_t b = mem[row * 16 + col];
            text += hx(b, 2) + QLatin1Char(' ');
            ascii += (b >= 0x20 && b < 0x7f) ? QChar(b) : QChar('.');
        }
        text += QLatin1Char(' ') + ascii + QLatin1Char('\n');
    }
    const int scroll = m_mem->verticalScrollBar()->value();
    m_mem->setPlainText(text);
    m_mem->verticalScrollBar()->setValue(scroll);
}

void DebuggerWindow::refreshDisasm()
{
    static a7800debug_line lines[DISASM_WINDOW];
    if (m_followPc->isChecked()) {
        a7800debug_cpu c;
        a7800debug_cpu_get(m_dbg, &c);
        /* the PC a third of the way down */
        m_disasmTop = (uint16_t)a7800debug_row_address(m_dbg, (uint16_t)c.pc, -(DISASM_WINDOW / 3));
    }
    int pcLine = -1;
    const int n = a7800debug_disassemble(m_dbg, m_disasmTop, lines, DISASM_WINDOW, &pcLine);
    m_lineAddr.clear();
    QString text;
    char buf[200];
    int selLine = -1;
    for (int i = 0; i < n; ++i) {
        m_lineAddr.push_back(lines[i].address);
        if (lines[i].address == m_selAddr) selLine = i;
        snprintf(buf, sizeof buf, "%c%c %04X  %-9s %-12s %s", lines[i].has_breakpoint ? '*' : ' ',
                 lines[i].is_pc ? '>' : ' ', lines[i].address, lines[i].bytes, lines[i].label, lines[i].disasm);
        text += QString::fromUtf8(buf);
        if (lines[i].comment[0]) text += QStringLiteral("  ") + QString::fromUtf8(lines[i].comment);
        text += QLatin1Char('\n');
    }
    if (n == 0) text = QStringLiteral("(no disassembly: the debugger is not attached)\n");
    m_disasm->setPlainText(text);
    if (selLine >= 0 && selLine != pcLine) {
        QTextCursor cur(m_disasm->document()->findBlockByNumber(selLine));
        cur.select(QTextCursor::LineUnderCursor);
        QTextCharFormat fmt;
        QColor sel = a7800AccentColor();
        sel.setAlpha(70);
        fmt.setBackground(sel);
        cur.setCharFormat(fmt);
    }
    if (pcLine >= 0) {
        QTextCursor cur(m_disasm->document()->findBlockByNumber(pcLine));
        cur.select(QTextCursor::LineUnderCursor);
        QTextCharFormat fmt;
        fmt.setBackground(a7800AccentColor());
        fmt.setForeground(Qt::white);
        cur.setCharFormat(fmt);
    }
}

void DebuggerWindow::refreshMaria()
{
    a7800debug_maria m;
    a7800debug_maria_get(m_dbg, &m);
    static const char *const modes[4] = { "160A / 160B", "(reserved)", "320B / 320D", "320A / 320C" };
    QString s;
    s += QStringLiteral("CTRL      $%1\n").arg(hx(m.ctrl, 2));
    s += QStringLiteral("  DMA %1   colour kill %2   characters %3 byte%4   border %5   kangaroo %6\n")
        .arg(onOff(m.dma_on), onOff(m.color_kill)).arg(m.char_width).arg(m.char_width == 1 ? "" : "s")
        .arg(m.border_control ? "black" : "background", onOff(m.kangaroo));
    s += QStringLiteral("  read mode %1\n").arg(QString::fromUtf8(modes[m.read_mode & 3]));
    s += QStringLiteral("CHARBASE  $%1 ($%2)\n").arg(hx(m.charbase, 2), hx(m.charbase << 8, 4));
    s += QStringLiteral("DPP       $%1\n").arg(hx(m.dpp, 4));
    s += QStringLiteral("DMA now   DLL $%1   DL $%2   offset %3   holey %4   DLI %5\n")
        .arg(hx(m.dll, 4), hx(m.dl, 4)).arg(m.offset).arg(m.holey).arg(m.dli);
    s += QStringLiteral("MSTAT     VBLANK %1   scanline %2\n\n").arg(m.vblank ? "yes" : "no").arg(m.scanline);
    s += QStringLiteral("BACKGRND  $%1\n").arg(hx(m.palette[0], 2));
    for (int p = 0; p < 8; ++p)
        s += QStringLiteral("P%1        $%2 $%3 $%4\n").arg(p)
            .arg(hx(m.palette[p * 4 + 1], 2), hx(m.palette[p * 4 + 2], 2), hx(m.palette[p * 4 + 3], 2));
    m_maria->setPlainText(s);

    char dll[8192];
    a7800debug_dll_text(m_dbg, dll, sizeof dll);
    const int scroll = m_dll->verticalScrollBar()->value();
    m_dll->setPlainText(dll[0] ? QString::fromUtf8(dll) : QStringLiteral("(no display list)"));
    m_dll->verticalScrollBar()->setValue(scroll);

    /* the palettes at 2x, each swatch labelled with the value written */
    if (a7800debug_palette_image(m_dbg, m_palettePx.data())) {
        const QImage img(reinterpret_cast<const uchar *>(m_palettePx.data()), A7800DEBUG_PALETTE_WIDTH,
                         A7800DEBUG_PALETTE_HEIGHT, A7800DEBUG_PALETTE_WIDTH * 4, QImage::Format_RGB32);
        QPixmap pix(2 * A7800DEBUG_PALETTE_WIDTH, 2 * A7800DEBUG_PALETTE_HEIGHT + 16);
        pix.fill(palette().color(QPalette::Window));
        QPainter p(&pix);
        p.drawImage(QRect(0, 16, 2 * A7800DEBUG_PALETTE_WIDTH, 2 * A7800DEBUG_PALETTE_HEIGHT), img);
        QFont f = font();
        f.setFamily(QStringLiteral("monospace"));
        f.setPointSizeF(f.pointSizeF() * 0.8);
        p.setFont(f);
        for (int pal = 0; pal < 8; ++pal) {
            p.setPen(palette().color(QPalette::WindowText));
            p.drawText(QRect(pal * 64, 0, 64, 16), Qt::AlignCenter, QStringLiteral("P%1").arg(pal));
            for (int entry = 0; entry < 4; ++entry) {
                const uint8_t reg = entry == 0 ? m.palette[0] : m.palette[pal * 4 + entry];
                const QColor c = QColor::fromRgb(img.pixel(pal * 32 + 16, entry * 16 + 8));
                p.setPen(c.lightness() > 128 ? Qt::black : Qt::white);
                p.drawText(QRect(pal * 64, 16 + entry * 32, 64, 32), Qt::AlignCenter,
                           QStringLiteral("$%1").arg(hx(reg, 2)));
            }
        }
        p.end();
        m_palette->setPixmap(pix);
    }
}

void DebuggerWindow::refreshIo()
{
    a7800debug_io io;
    a7800debug_io_get(m_dbg, &io);
    QString s;
    s += QStringLiteral("INPTCTRL  $%1   %2\n\n").arg(hx(io.inptctrl, 2), io.inpt_locked ? "locked" : "unlocked");
    for (int ch = 0; ch < 2; ++ch)
        s += QStringLiteral("TIA %1     AUDC $%2   AUDF $%3   AUDV $%4\n").arg(ch)
            .arg(hx(io.audc[ch], 2), hx(io.audf[ch], 2), hx(io.audv[ch], 2));
    /* SWCHA: player 1 in the high nibble, player 2 in the low; 0 is pushed */
    static const char *const dirs[4] = { "up", "down", "left", "right" };
    s += QStringLiteral("\nSWCHA     $%1").arg(hx(io.swcha, 2));
    for (int player = 0; player < 2; ++player) {
        QStringList pushed;
        const int nibble = player ? (io.swcha & 0x0f) : (io.swcha >> 4);
        for (int b = 0; b < 4; ++b)
            if (!(nibble & (1 << b))) pushed << QString::fromUtf8(dirs[b]);
        s += QStringLiteral("   player %1: %2").arg(player + 1)
            .arg(pushed.isEmpty() ? QStringLiteral("centred") : pushed.join(QLatin1Char(' ')));
    }
    s += QStringLiteral("\nSWCHB     $%1   Reset %2   Select %3   Pause %4   difficulty left %5, right %6\n")
        .arg(hx(io.swchb, 2), (io.swchb & 0x01) ? "up" : "DOWN", (io.swchb & 0x02) ? "up" : "DOWN",
             (io.swchb & 0x08) ? "up" : "DOWN", (io.swchb & 0x40) ? "A" : "B", (io.swchb & 0x80) ? "A" : "B");
    s += QStringLiteral("INPT0-5  ");
    for (int i = 0; i < 6; ++i) s += QStringLiteral(" $%1").arg(hx(io.inpt[i], 2));
    s += QStringLiteral("\n\nPOKEY     %1\n").arg(io.pokey_present ? "on this cartridge" : "none on this cartridge");
    if (io.pokey_present) {
        for (int ch = 0; ch < 4; ++ch)
            s += QStringLiteral("  %1       AUDF $%2   AUDC $%3\n").arg(ch + 1)
                .arg(hx(io.pokey_audf[ch], 2), hx(io.pokey_audc[ch], 2));
        s += QStringLiteral("  AUDCTL  $%1\n").arg(hx(io.pokey_audctl, 2));
    }
    s += QLatin1Char('\n');
    for (int port = 0; port < 2; ++port) {
        const char *type = a7800_ctrl_type_name(io.port_type[port]);
        s += QStringLiteral("Port %1    %2   held: %3\n").arg(port + 1)
            .arg(QString::fromUtf8(type ? type : "?"), heldNames(io.held[port]));
    }
    m_io->setPlainText(s);
}

void DebuggerWindow::refreshBps()
{
    a7800debug_breakpoint bps[256];
    const int n = a7800debug_breakpoint_list(m_dbg, bps, 256);
    m_bpFilling = true;
    m_bpTable->setRowCount(n);
    for (int i = 0; i < n; ++i) {
        auto *on = new QTableWidgetItem;
        on->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        on->setCheckState(bps[i].enabled ? Qt::Checked : Qt::Unchecked);
        on->setData(Qt::UserRole, bps[i].id);
        m_bpTable->setItem(i, 0, on);
        QString type;
        if (bps[i].type & A7800DEBUG_BP_EXEC) type += QStringLiteral("exec ");
        if (bps[i].type & A7800DEBUG_BP_READ) type += QStringLiteral("read ");
        if (bps[i].type & A7800DEBUG_BP_WRITE) type += QStringLiteral("write");
        m_bpTable->setItem(i, 1, new QTableWidgetItem(type.trimmed()));
        QString range = QStringLiteral("$") + hx(bps[i].start, 4);
        if (bps[i].end != bps[i].start) range += QStringLiteral("-$") + hx(bps[i].end, 4);
        m_bpTable->setItem(i, 2, new QTableWidgetItem(range));
        m_bpTable->setItem(i, 3, new QTableWidgetItem(QString::fromUtf8(bps[i].condition)));
    }
    m_bpFilling = false;
}

void DebuggerWindow::refreshCart()
{
    a7800debug_cart c;
    char info[256];
    a7800debug_cart_get(m_dbg, &c);
    a7800debug_cart_info(m_dbg, info, sizeof info);
    QString s = QString::fromUtf8(info) + QStringLiteral("\n\n");
    if (!c.present) { m_cart->setPlainText(s); return; }
    static const char *const handovers[3] = { "none yet", "the BIOS", "the loader" };
    s += QStringLiteral("Link          %1%2\n").arg(c.link_up ? "up" : "down",
                                                     c.worker ? QString() : QStringLiteral(" (mailbox service stopped)"));
    if (c.link_error[0]) s += QStringLiteral("Last error    %1\n").arg(QString::fromUtf8(c.link_error));
    s += QStringLiteral("Mode          %1%2\n").arg(QString::fromUtf8(c.mode_name),
        c.mode == 1 ? QStringLiteral(", loading %1%").arg(c.load_pct) : QString());
    s += QStringLiteral("Image         %1\n").arg(c.booted_image ? "a booted game" : "CONFIG or a FujiNet app");
    s += QStringLiteral("Started by    %1\n").arg(QString::fromUtf8(c.handover >= 0 && c.handover < 3 ? handovers[c.handover] : "?"));
    s += QStringLiteral("Live image    %1   CRC %2\n").arg(QString::fromUtf8(c.kind[0] ? c.kind : "-"), hx(c.live_crc, 8));
    if (c.staged)
        s += QStringLiteral("Staged        %1   CRC %2\n").arg(QString::fromUtf8(c.staged_kind), hx(c.staged_crc, 8));
    else
        s += QStringLiteral("Staged        nothing\n");
    QStringList hsc;
    if (c.hsc & 1) hsc << QStringLiteral("ROM installed");
    if (c.hsc & 2) hsc << QStringLiteral("on");
    if (c.hsc & 4) hsc << QStringLiteral("saved");
    if (c.hsc & 8) hsc << QStringLiteral("unsaved changes");
    s += QStringLiteral("High Score    %1\n").arg(hsc.isEmpty() ? QStringLiteral("off") : hsc.join(QStringLiteral(", ")));
    s += QStringLiteral("TV            %1\n\n").arg(c.tv ? "PAL" : "NTSC");
    s += QStringLiteral("ACKSEQ $%1   ERR %2\n").arg(hx(c.ackseq, 2)).arg(c.last_error);
    s += QStringLiteral("BOOT state $%1   %2%   error %3\n").arg(hx(c.boot_state, 2)).arg(c.boot_pct).arg(c.boot_err);
    s += QStringLiteral("Mailbox queue %1\n").arg(c.queue_depth);
    m_cart->setPlainText(s);
}

void DebuggerWindow::refreshAll()
{
    refreshStatus(); refreshCpu(); refreshMemory(); refreshDisasm(); refreshMaria(); refreshIo();
    refreshBps(); refreshCart();
}

void DebuggerWindow::tick()
{
    if (!isVisible()) return;
    const unsigned gen = a7800debug_generation(m_dbg);
    const bool stopped = a7800debug_is_stopped(m_dbg) != 0;
    if (gen != m_seenGen || stopped != m_wasStopped) {
        m_seenGen = gen; m_wasStopped = stopped;
        refreshAll();
    } else if (!stopped && ++m_runningTicks >= 5) {
        m_runningTicks = 0;
        refreshStatus(); refreshCpu(); refreshMaria(); refreshIo(); refreshCart();
    }
}

/* ---- the prompt and the rest ----------------------------------------------- */

void DebuggerWindow::appendPrompt(const QString &text)
{
    m_promptOut->moveCursor(QTextCursor::End);
    m_promptOut->insertPlainText(text);
    m_promptOut->verticalScrollBar()->setValue(m_promptOut->verticalScrollBar()->maximum());
}

void DebuggerWindow::runPrompt()
{
    static char out[65536];
    const QString cmd = m_promptIn->text().trimmed();
    if (cmd.isEmpty()) return;
    appendPrompt(QStringLiteral("> %1\n").arg(cmd));
    a7800debug_command(m_dbg, cmd.toUtf8().constData(), out, sizeof out);
    appendPrompt(stripControl(out) + QLatin1Char('\n'));
    m_promptIn->clear();
    refreshAll();
}

void DebuggerWindow::jumpTo(const QString &text)
{
    long a;
    int addr = a7800debug_label_address(m_dbg, text.trimmed().toUtf8().constData());
    if (addr < 0 && parseNum(text, &a)) addr = (int)(a & 0xFFFF);
    if (addr < 0) { m_status->setText(QStringLiteral("No such address or label")); return; }
    m_followPc->setChecked(false);
    m_disasmTop = (uint16_t)a7800debug_row_address(m_dbg, (uint16_t)addr, -(DISASM_WINDOW / 3));
    refreshDisasm();
}

int DebuggerWindow::selectedOrPc()
{
    if (m_selAddr >= 0) return m_selAddr;
    a7800debug_cpu c;
    a7800debug_cpu_get(m_dbg, &c);
    return c.pc;
}

void DebuggerWindow::toggleBreakpointAtSelection()
{
    a7800debug_breakpoint_toggle(m_dbg, (uint16_t)selectedOrPc());
    refreshDisasm();
    refreshBps();
}

bool DebuggerWindow::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_promptIn && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Tab) {
            const QString text = m_promptIn->text();
            const int sp = text.lastIndexOf(QLatin1Char(' '));
            const QString word = sp >= 0 ? text.mid(sp + 1) : text;
            char comps[4096];
            const int n = a7800debug_completions(m_dbg, word.toUtf8().constData(), comps, sizeof comps);
            if (n == 1) {
                QString c = QString::fromUtf8(comps).section(QLatin1Char('\n'), 0, 0);
                m_promptIn->setText(text.left(sp + 1) + c + QLatin1Char(' '));
            } else if (n > 1) {
                appendPrompt(QString::fromUtf8(comps));
            }
            return true;
        }
    }
    if (m_disasm && obj == m_disasm->viewport()) {
        if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonDblClick) {
            auto *me = static_cast<QMouseEvent *>(e);
            const int line = m_disasm->cursorForPosition(me->pos()).blockNumber();
            if (line >= 0 && line < (int)m_lineAddr.size()) {
                m_selAddr = m_lineAddr[line];
                if (e->type() == QEvent::MouseButtonDblClick) toggleBreakpointAtSelection();
                else refreshDisasm();
            }
            return true;
        }
        if (e->type() == QEvent::Wheel) {
            auto *we = static_cast<QWheelEvent *>(e);
            const int rows = -we->angleDelta().y() / 40;
            if (rows != 0) {
                m_followPc->setChecked(false);
                m_disasmTop = (uint16_t)a7800debug_row_address(m_dbg, m_disasmTop, rows);
                refreshDisasm();
            }
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, e);
}

void DebuggerWindow::keyPressEvent(QKeyEvent *e)
{
    switch (e->key()) {
    case Qt::Key_F5:
        if (a7800debug_is_stopped(m_dbg)) a7800debug_resume(m_dbg); else a7800debug_stop(m_dbg);
        refreshAll(); return;
    case Qt::Key_F7: stepAnd(a7800debug_step); return;
    case Qt::Key_F8:
        stepAnd((e->modifiers() & Qt::ShiftModifier) ? a7800debug_step_out : a7800debug_step_over);
        return;
    case Qt::Key_F9: toggleBreakpointAtSelection(); return;
    case Qt::Key_F12: hide(); return;
    default: QMainWindow::keyPressEvent(e);
    }
}

/* Closing only hides it (it is kept for next time); accepting, rather than
 * ignoring, matters: Qt 6 abandons an application quit that a window
 * refuses. */
void DebuggerWindow::closeEvent(QCloseEvent *e)
{
    hide();
    e->accept();
}

/* Hidden by F12, the close button or the main window: the engine goes
 * away and the machine runs on. */
void DebuggerWindow::hideEvent(QHideEvent *e)
{
    a7800debug_detach(m_dbg);
    QMainWindow::hideEvent(e);
}
