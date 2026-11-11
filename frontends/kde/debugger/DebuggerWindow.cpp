/*
 * Debugger window (Qt6 Widgets) over MAME's own debugger engine, via
 * core/include/a7800debug.h. Mirrors the GNOME one tab for tab, and follows
 * the family's KDE debuggers.
 *
 * Tabs, in this order (A7800_DEBUGGER_TAB picks the one a window opens on, by
 * index or by name -- 0 console, 1 disassembly, 2 memory, 3 breakpoints,
 * 4 watches, 5 cart, 6 trace, 7 history, 8 video, 9 sound, 10 symbols,
 * 11 fujinet -- or by the start of a label): the Console (the engine's
 * command line, with a history and Tab completion), the Disassembly (click a
 * line to select it for Run to Cursor; click its left edge, double-click it
 * or press F9 for its breakpoint), Memory (the whole 64K the CPU sees, Go to
 * a label / address / range, a hex or text search, Poke), Breakpoints
 * (execute, read, write and access, with a condition), Watches (a value
 * list, Break on change), Cart & I/O (the FujiNet cartridge, the memory map,
 * INPTCTRL, the RIOT, the pads), Trace and History (the engine's instruction
 * ring: live, and read at a stop), Video (MARIA's pictures, registers,
 * colours and lines), Sound (the TIA and, when the cartridge has one, the
 * POKEY), Symbols (every label, filtered) and FujiNet (the FujiBus packets
 * on the cartridge's link). The registers sit in a strip under the tabs, on
 * every one of them. Any tab can be popped out into a window of its own
 * (A7800_POPOUT names one at start). The toolbar carries the stepping
 * controls, Break at, symbol loading and Save; the family's keys apply (F5
 * run/stop, F7 step, F8 step over, Shift+F8 step out, Shift+F7 frame, F9
 * breakpoint, F12 close).
 *
 * Showing the window attaches (which stops the machine, as on every
 * sibling); hiding it detaches, which lets the machine run on.
 *
 * A short timer polls the engine's generation and stop serial and refreshes
 * the tab showing when they moved; while the machine runs, the live tabs
 * refresh every 400 ms. Every new stop re-centres every view on the spot.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DebuggerWindow.h"
#include "VideoTab.h"
#include "../FujiNetWindows.h"
#include "../help/HelpWindow.h"

extern "C" {
#include "a7800memtools.h"
}

#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollBar>
#include <QShortcut>
#include <QShowEvent>
#include <QSplitter>
#include <QStatusBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QThread>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cstdio>
#include <cstring>
#include <utility>


#define DISASM_WINDOW 64        /* lines fetched; as many show as fit */
#define MEM_ROWS 4096           /* the whole 64K, 16 bytes a row */
#define MEM_ROW_TEXT 140        /* bytes of text a Memory row can take */
#define BP_MAX 256
#define TRACE_MAX 512
#define HIST_SHOWN 2000         /* trace lines shown */
#define RING_MAX 65537
#define CONSOLE_LINES 5000
#define REGION_MAX 64

namespace {

enum {
    TAB_CONSOLE, TAB_DISASM, TAB_MEMORY, TAB_BREAKS, TAB_WATCHES, TAB_CART, TAB_TRACE, TAB_HISTORY, TAB_VIDEO,
    TAB_SOUND, TAB_SYMBOLS, TAB_FUJINET, TAB_COUNT
};

/* The tabs, in order: the names A7800_DEBUGGER_TAB takes, their labels (which
 * it takes too), and their Help contexts (debugger/<help>). */
const struct { const char *name, *label, *help; } kTabs[TAB_COUNT] = {
    { "console", "Console", "console" },           { "disassembly", "Disassembly", "disassembly" },
    { "memory", "Memory", "memory" },              { "breakpoints", "Breakpoints", "breakpoints" },
    { "watches", "Watches", "watches" },           { "cart", "Cart & I/O", "cart" },
    { "trace", "Trace", "trace" },                 { "history", "History", "history" },
    { "video", "Video", "video" },                 { "sound", "Sound", "sound" },
    { "symbols", "Symbols", "symbols" },           { "fujinet", "FujiNet", "fujinet" },
};

/* The register entries, in the order shown, and the a7800debug_reg each sets;
 * every one shows two or four hex digits. */
const struct { const char *name; int id, digits; } kRegs[6] = {
    { "PC", A7800_REG_PC, 4 }, { "SP", A7800_REG_SP, 2 }, { "A", A7800_REG_A, 2 },
    { "X", A7800_REG_X, 2 },   { "Y", A7800_REG_Y, 2 },   { "PS", A7800_REG_PS, 2 },
};

/* The flags of PS the engine can set. */
const struct { const char *name; int id; } kFlags[6] = {
    { "N", A7800_FLAG_N }, { "V", A7800_FLAG_V }, { "D", A7800_FLAG_D },
    { "I", A7800_FLAG_I }, { "Z", A7800_FLAG_Z }, { "C", A7800_FLAG_C },
};

/* The breakpoint types the Breakpoints tab offers, in the order of its chooser. */
const struct { const char *label; int type; } kBpTypes[4] = {
    { "Execute", A7800DEBUG_BP_EXEC }, { "Read", A7800DEBUG_BP_READ },
    { "Write", A7800DEBUG_BP_WRITE },  { "Access", A7800DEBUG_BP_ACCESS },
};

const QString kGray = QStringLiteral("color: gray;");

/* The PC's line and a marked memory row: the 7800 orange, white ink. */
QColor accentColor()
{
    return a7800AccentColor();
}

QTextCharFormat accentFormat()
{
    QTextCharFormat f;
    f.setBackground(accentColor());
    f.setForeground(Qt::white);
    return f;
}

/* A watched range in the dump: a blue the accent does not hide. */
QTextCharFormat watchFormat()
{
    QTextCharFormat f;
    f.setBackground(QColor(0x1c, 0x71, 0xd8));
    f.setForeground(Qt::white);
    return f;
}

void setMono(QWidget *w)
{
    QFont f = w->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    w->setFont(f);
}

QPlainTextEdit *monoView()
{
    auto *v = new QPlainTextEdit;
    v->setReadOnly(true);
    setMono(v);
    v->setLineWrapMode(QPlainTextEdit::NoWrap);
    return v;
}

/* Engine text for a view: control bytes dropped. */
QString stripControl(const char *s)
{
    const QString in = QString::fromUtf8(s);
    QString out;
    out.reserve(in.size());
    for (const QChar c : in)
        if (c.unicode() >= 0x20 || c == QLatin1Char('\n') || c == QLatin1Char('\t')) out += c;
    return out;
}

QString hx(unsigned v, int digits)
{
    return QStringLiteral("%1").arg(v, digits, 16, QLatin1Char('0')).toUpper();
}

/* The names of a port's held a7800_action bits. */
QString heldNames(unsigned held)
{
    static const char *const names[A7800_ACT_PER_PORT] = { "Up", "Down", "Left", "Right", "Button 1", "Button 2" };
    QStringList out;
    for (int b = 0; b < A7800_ACT_PER_PORT; ++b)
        if (held & (1u << b)) out << QString::fromUtf8(names[b]);
    return out.isEmpty() ? QStringLiteral("nothing") : out.join(QStringLiteral(", "));
}

/* "Stopped" and the engine's reason: "stopped at $0038" reads "Stopped at
 * $0038", anything else "Stopped: <reason>". */
QString stoppedText(const QString &reason)
{
    if (reason.isEmpty()) return QStringLiteral("Stopped");
    if (reason.startsWith(QLatin1String("stopped"))) return QStringLiteral("S") + reason.mid(1);
    return QStringLiteral("Stopped: ") + reason;
}

QLabel *titleLabel(const QString &text)
{
    auto *l = new QLabel(text);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
}

QLabel *hintLabel(const QString &text)
{
    auto *l = new QLabel(text);
    l->setStyleSheet(kGray);
    l->setWordWrap(true);
    return l;
}

QLineEdit *smallEntry(const QString &placeholder, int chars)
{
    auto *e = new QLineEdit;
    e->setPlaceholderText(placeholder);
    e->setFixedWidth(e->fontMetrics().averageCharWidth() * chars + 16);
    return e;
}

/* A register entry exactly wide enough for its hex digits. */
void setDigits(QLineEdit *e, int digits)
{
    e->setFixedWidth(e->fontMetrics().horizontalAdvance(QString(digits, QLatin1Char('0'))) + 16);
}

/* A toggle takes the checked style (the accent, a box of its own), which is
 * not the native button's: one size for both states and every text,
 * so it never jumps. */
void fixToggleSize(QPushButton *b, const QStringList &texts, int minHeight)
{
    b->ensurePolished();
    QSize s(0, minHeight);
    for (const bool on : { false, true })
        for (const QString &t : texts) {
            b->setChecked(on);
            b->setText(t);
            s = s.expandedTo(b->sizeHint());
        }
    b->setChecked(false);
    b->setText(texts.first());
    b->setFixedSize(s);
}

/* Whether a view's reader is at its end (a log then follows new lines). */
bool atEnd(QPlainTextEdit *v)
{
    return v->verticalScrollBar()->value() >= v->verticalScrollBar()->maximum() - 1;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
void setText(QPlainTextEdit *v, const QString &text, bool follow = false)
{
    if (v->toPlainText() == text) {
        if (follow) v->verticalScrollBar()->setValue(v->verticalScrollBar()->maximum());
        return;
    }
    const int top = v->verticalScrollBar()->value(), left = v->horizontalScrollBar()->value();
    v->setPlainText(text);
    v->verticalScrollBar()->setValue(follow ? v->verticalScrollBar()->maximum() : top);
    v->horizontalScrollBar()->setValue(left);
}

/* A stretch of one line of a view in a format. */
QTextEdit::ExtraSelection spanSelection(QPlainTextEdit *v, int line, int from, int length, const QTextCharFormat &fmt)
{
    QTextEdit::ExtraSelection s;
    const QTextBlock block = v->document()->findBlockByNumber(line);
    s.cursor = QTextCursor(block);
    s.cursor.setPosition(block.position() + from);
    s.cursor.setPosition(block.position() + from + length, QTextCursor::KeepAnchor);
    s.format = fmt;
    return s;
}

/* A7800_DEBUGGER_TAB: a page number, or a tab's name or label (any case), or
 * the start of one, or of a word of a label ("i/o" ...). The names of the
 * tabs the older windows had still reach their successors. */
int tabFromEnv(const char *var = "A7800_DEBUGGER_TAB")
{
    const QString tab = qEnvironmentVariable(var).trimmed();
    if (tab.isEmpty()) return -1;
    static const struct { const char *name; int tab; } aliases[] = {
        { "io", TAB_CART }, { "cartridge", TAB_CART }, { "prompt", TAB_CONSOLE }, { "cpu", TAB_MEMORY },
        { "maria", TAB_VIDEO } };
    for (const auto &alias : aliases)
        if (tab.compare(QLatin1String(alias.name), Qt::CaseInsensitive) == 0) return alias.tab;
    bool isNum = false;
    const int n = tab.toInt(&isNum);
    if (isNum) return n >= 0 && n < TAB_COUNT ? n : -1;
    for (int pass = 0; pass < 3; ++pass)
        for (int i = 0; i < TAB_COUNT; ++i) {
            const QString name = QString::fromUtf8(kTabs[i].name), label = QString::fromUtf8(kTabs[i].label);
            if (pass == 0 && name.startsWith(tab, Qt::CaseInsensitive)) return i;
            if (pass == 1 && label.startsWith(tab, Qt::CaseInsensitive)) return i;
            if (pass == 2) {
                const QStringList words = label.split(QLatin1Char(' '));
                for (int w = 1; w < words.size(); ++w)
                    if (words[w].startsWith(tab, Qt::CaseInsensitive)) return i;
            }
        }
    return -1;
}

/* The family's keys: F5 run/stop, F7 step, Shift+F7 frame, F8 step over,
 * Shift+F8 step out, F9 breakpoint, F12 close. */
bool isDebuggerKey(const QKeyEvent *e)
{
    const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
    switch (e->key()) {
    case Qt::Key_F5: case Qt::Key_F9: case Qt::Key_F12: return mods == Qt::NoModifier;
    case Qt::Key_F7: case Qt::Key_F8: return mods == Qt::NoModifier || mods == Qt::ShiftModifier;
    default: return false;
    }
}

QPointer<DebuggerWindow> s_win;

/* The breakpoint type of a chooser row. */
int bpTypeAt(int row)
{
    return kBpTypes[qBound(0, row, 3)].type;
}

} // namespace

void DebuggerWindow::showFor(QWidget *parent, a7800session *session)
{
    if (!s_win) s_win = new DebuggerWindow(session, parent);
    /* showEvent attaches, which stops the machine */
    s_win->show();
    s_win->raise();
    s_win->activateWindow();
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
    setWindowTitle(QStringLiteral("Atari 7800 Debugger"));
    resize(1400, 820);

    auto *central = new QWidget;
    auto *root = new QVBoxLayout(central);
    root->addWidget(buildToolbar());
    m_tabs = new QTabWidget;
    m_tabs->setUsesScrollButtons(true);
    m_pages[TAB_CONSOLE] = buildConsole();
    m_pages[TAB_DISASM] = buildDisasm();
    m_pages[TAB_MEMORY] = buildMemory();
    m_pages[TAB_BREAKS] = buildBreaks();
    m_pages[TAB_WATCHES] = buildWatches();
    m_pages[TAB_CART] = buildCart();
    m_pages[TAB_TRACE] = buildHist(0);
    m_pages[TAB_HISTORY] = buildHist(1);
    m_pages[TAB_VIDEO] = buildVideo();
    m_pages[TAB_SOUND] = buildSound();
    m_pages[TAB_SYMBOLS] = buildSymbols();
    m_pages[TAB_FUJINET] = buildFujinet();
    for (int i = 0; i < TAB_COUNT; ++i) {
        /* a page's place in the tab widget is not its identity once tabs move
         * into windows of their own: each carries its own number */
        m_pages[i]->setProperty("tab", i);
        m_tabs->addTab(m_pages[i], QString::fromUtf8(kTabs[i].label).replace(QLatin1Char('&'), QLatin1String("&&")));
    }
    const int tab = tabFromEnv();
    if (tab >= 0) m_tabs->setCurrentIndex(tab);
    /* Help's contexts, one per tab */
    auto setHelpContext = [this](int page) {
        if (page >= 0 && page < TAB_COUNT)
            setProperty("a7800HelpContext", QStringLiteral("debugger/%1").arg(QLatin1String(kTabs[page].help)));
    };
    setHelpContext(currentTab());
    new QShortcut(QKeySequence(Qt::Key_F1), this, [this] { a7800HelpFor(this); });
    connect(m_tabs, &QTabWidget::currentChanged, this, [this, setHelpContext](int) {
        const int page = currentTab();
        setHelpContext(page);
        if (page == TAB_VIDEO) m_videoForce = true;
        if (page == TAB_HISTORY) m_hSeen[1] = -1;
        refreshPage(page, false);
        if (page == TAB_CONSOLE) m_conIn->setFocus();
    });
    root->addWidget(m_tabs, 1);
    root->addWidget(buildRegs());
    setCentralWidget(central);
    statusBar();

    /* for captures and tests: A7800_WATCH adds watches (a comma list),
     * A7800_WATCH_BREAK a watch with a break-on-change over it, A7800_MEM_GOTO
     * shows a range in Memory, A7800_HISTORY records the trace and A7800_POPOUT
     * peels a tab off */
    {
        const QByteArray mg = qgetenv("A7800_MEM_GOTO");
        const QString wb = qEnvironmentVariable("A7800_WATCH_BREAK").trimmed();
        for (const QString &text : qEnvironmentVariable("A7800_WATCH").split(QLatin1Char(','), Qt::SkipEmptyParts))
            if (!text.trimmed().isEmpty()) a7800watch_add(text.trimmed().toUtf8().constData());
        if (!wb.isEmpty()) {
            const int index = a7800watch_add(wb.toUtf8().constData());
            if (index >= 0) watchBreak(index);
        }
        uint16_t lo, hi;
        if (!mg.isEmpty() && a7800sym_parse_range(m_dbg, mg.constData(), &lo, &hi)) showInMemory(lo, hi);
        if (tabFromEnv("A7800_POPOUT") >= 0) popOut(tabFromEnv("A7800_POPOUT"));
    }

    /* the timer runs only while the window is shown (showEvent/hideEvent) */
    connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
}

/* Only at exit, with the main window: the session outlives its windows. */
DebuggerWindow::~DebuggerWindow()
{
    m_closing = true;       /* a peeled-off tab's window goes with the debugger */
    for (QWidget *&win : m_popped)
        if (win) {
            QWidget *w = win;
            win = nullptr;
            w->close();
        }
    if (a7800debug_is_attached(m_dbg)) a7800debug_detach(m_dbg);
}

/* ---- building ------------------------------------------------------------- */

/* Two rows: the stepping controls, Pop Out and Help; then Break at, the
 * symbols, Save and the status line, which has the room that is left and
 * wraps rather than being cut. */
QWidget *DebuggerWindow::buildToolbar()
{
    auto *bar = new QWidget;
    auto *rows = new QVBoxLayout(bar);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(2);
    auto *h = new QHBoxLayout;
    h->setContentsMargins(0, 0, 0, 0);
    rows->addLayout(h);
    auto add = [&](const QString &label, const QString &tip, std::function<void()> fn) {
        auto *b = new QPushButton(label);
        b->setFocusPolicy(Qt::NoFocus);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, fn);
        h->addWidget(b);
        return b;
    };
    /* Run is a checkable button, checked while the machine is stopped: its
     * stylesheet gives that the accent. */
    m_runBtn = add(QStringLiteral("Stop (F5)"), QStringLiteral("Run, or stop before the next instruction"),
                   [this] { toggleRun(); });
    m_runBtn->setCheckable(true);
    m_runBtn->setStyleSheet(
        QStringLiteral("QPushButton:checked { background:%1; color:white; }").arg(accentColor().name()));
    auto *step = add(QStringLiteral("Step (F7)"), QStringLiteral("One instruction"),
                     [this] { stepAnd([this] { a7800debug_step(m_dbg); }); });
    add(QStringLiteral("Step Over (F8)"), QStringLiteral("Over a JSR"),
        [this] { stepAnd([this] { a7800debug_step_over(m_dbg); }); });
    add(QStringLiteral("Step Out (Shift+F8)"), QStringLiteral("On to the RTS or RTI"),
        [this] { stepAnd([this] { a7800debug_step_out(m_dbg); }); });
    add(QStringLiteral("Run to Cursor"), QStringLiteral("On to the line selected in the Disassembly"),
        [this] { runToCursor(); });
    add(QStringLiteral("Frame (Shift+F7)"), QStringLiteral("On to the next vertical blank"),
        [this] { stepAnd([this] { a7800debug_frame(m_dbg); }); });
    add(QStringLiteral("Clear Breakpoints"), QStringLiteral("Remove every breakpoint"), [this] {
        a7800debug_breakpoint_clear(m_dbg);
        resync();
    });
    fixToggleSize(m_runBtn, { QStringLiteral("Stop (F5)"), QStringLiteral("Run (F5)") }, step->sizeHint().height());
    h->addStretch(1);
    auto *pop = new QPushButton(QStringLiteral("Pop Out"));
    pop->setFocusPolicy(Qt::NoFocus);
    pop->setToolTip(QStringLiteral("Move the tab that shows into a window of its own; Dock puts it back"));
    connect(pop, &QPushButton::clicked, this, [this] { popOut(currentTab()); });
    h->addWidget(pop);
    auto *help = qobject_cast<QToolButton *>(a7800HelpButton(QString()));
    if (help) {
        help->setAutoRaise(false);
        help->setText(QStringLiteral("Help"));
        help->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    }
    h->addWidget(help);

    auto *second = new QHBoxLayout;
    second->setContentsMargins(0, 0, 0, 0);
    rows->addLayout(second);
    second->addWidget(new QLabel(QStringLiteral("Break at:")));
    m_breakAt = smallEntry(QStringLiteral("symbol or $addr"), 18);
    m_breakAt->setToolTip(QStringLiteral("A symbol or a hex address, then Enter: an execute breakpoint there"));
    connect(m_breakAt, &QLineEdit::returnPressed, this, [this] { breakAt(); });
    second->addWidget(m_breakAt);

    auto *sym = new QPushButton(QStringLiteral("Load Symbols…"));
    sym->setFocusPolicy(Qt::NoFocus);
    sym->setToolTip(QStringLiteral("Labels from an ld65 or VICE label file, a ca65 .dbg or a .sym file"));
    auto *symMenu = new QMenu(sym);
    symMenu->addAction(QStringLiteral("Symbol File…"), this, [this] { loadSymbols(true); });
    symMenu->addAction(QStringLiteral("Next to the Cartridge"), this, [this] { loadSymbols(false); });
    sym->setMenu(symMenu);
    second->addWidget(sym);
    auto *clearSym = new QPushButton(QStringLiteral("Clear Symbols"));
    clearSym->setFocusPolicy(Qt::NoFocus);
    clearSym->setToolTip(QStringLiteral("Drop every loaded symbol and label; the built-in ones stay"));
    connect(clearSym, &QPushButton::clicked, this, [this] { clearSymbols(); });
    second->addWidget(clearSym);

    auto *saveBtn = new QPushButton(QStringLiteral("Save…"));
    saveBtn->setFocusPolicy(Qt::NoFocus);
    auto *menu = new QMenu(saveBtn);
    static const struct { const char *kind, *title, *file; } saves[] = {
        { "dis", "Disassembly ($4000-$FFFF)", "disassembly.asm" },
        { "ram", "Console RAM ($1800-$27FF)", "ram.bin" },
        { "mem", "Memory (the whole 64K bus)", "memory.bin" } };
    for (const auto &entry : saves) {
        const char *kind = entry.kind;
        const QString title = QString::fromUtf8(entry.title), file = QString::fromUtf8(entry.file);
        menu->addAction(title, this, [this, kind, title, file] { save(kind, title, file); });
    }
    saveBtn->setMenu(menu);
    second->addWidget(saveBtn);

    m_status = new QLabel;
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_status->setStyleSheet(kGray);
    m_status->setContentsMargins(8, 0, 8, 0);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    second->addWidget(m_status, 1);
    return bar;
}

QWidget *DebuggerWindow::buildConsole()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    m_conOut = monoView();
    m_conOut->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_conOut->setMaximumBlockCount(CONSOLE_LINES);
    m_conOut->setPlainText(QStringLiteral("MAME's debugger console. Type 'help' for every command; 'cart', 'maria', "
                                          "'video', 'watch' and 'find' are this app's own. Numbers are hex: 4000, "
                                          "$4000 or 0x4000.\n"));
    m_conIn = new QLineEdit;
    setMono(m_conIn);
    m_conIn->setPlaceholderText(QStringLiteral("command (help, step, over, out, go, bpset, wpset, print, dump, cart, maria, "
                                               "video ...) — Tab completes, Up/Down recall"));
    m_conIn->installEventFilter(this);
    connect(m_conIn, &QLineEdit::returnPressed, this, [this] { runConsole(); });
    v->addWidget(m_conOut, 1);
    v->addWidget(m_conIn);
    return w;
}

/* The registers, under the tabs and so on every one of them (Enter sets
 * one), then the flags of PS and the beam. */
QWidget *DebuggerWindow::buildRegs()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);

    auto *regs = new QHBoxLayout;
    regs->setSpacing(8);
    for (int i = 0; i < 6; ++i) {
        auto *l = new QLabel(QString::fromUtf8(kRegs[i].name));
        l->setStyleSheet(kGray);
        regs->addWidget(l);
        m_reg[i] = new QLineEdit;
        setMono(m_reg[i]);
        m_reg[i]->setMaxLength(12);
        setDigits(m_reg[i], kRegs[i].digits);
        m_reg[i]->setToolTip(QStringLiteral("Enter sets it (hex, or an expression; only while stopped)"));
        connect(m_reg[i], &QLineEdit::returnPressed, this, [this, i] { setReg(i); });
        regs->addWidget(m_reg[i]);
    }
    regs->addSpacing(16);
    /* N V - B D I Z C of PS; the break and unused bits have no setter */
    for (int i = 0; i < 6; ++i) {
        m_flag[i] = new QCheckBox(QString::fromUtf8(kFlags[i].name));
        connect(m_flag[i], &QCheckBox::clicked, this, [this, i](bool on) { setFlag(kFlags[i].id, on); });
        regs->addWidget(m_flag[i]);
    }
    regs->addStretch(1);
    v->addLayout(regs);

    m_beam = new QLabel;
    setMono(m_beam);
    m_beam->setStyleSheet(kGray);
    v->addWidget(m_beam);
    return w;
}

/* The dump, Go to (a label, address or range, which it marks), the write
 * control, and the search bar. */
QWidget *DebuggerWindow::buildMemory()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);

    v->addWidget(titleLabel(QStringLiteral("Memory (the 64K as the CPU sees it)")));
    m_mem = monoView();
    v->addWidget(m_mem, 1);

    auto *nav = new QHBoxLayout;
    nav->addWidget(new QLabel(QStringLiteral("Go to")));
    m_memGoto = smallEntry(QStringLiteral("$4000, label or lo-hi"), 20);
    connect(m_memGoto, &QLineEdit::returnPressed, this, [this] { memGoto(); });
    nav->addWidget(m_memGoto);
    nav->addSpacing(12);
    nav->addWidget(new QLabel(QStringLiteral("Poke")));
    m_memAddr = smallEntry(QStringLiteral("$4000"), 10);
    m_memVal = smallEntry(QStringLiteral("$00"), 5);
    connect(m_memAddr, &QLineEdit::returnPressed, this, [this] { memWrite(); });
    connect(m_memVal, &QLineEdit::returnPressed, this, [this] { memWrite(); });
    nav->addWidget(m_memAddr);
    nav->addWidget(new QLabel(QStringLiteral("value")));
    nav->addWidget(m_memVal);
    nav->addWidget(hintLabel(QStringLiteral("RAM takes it as a bus write; ROM and the chips' "
                                            "registers do not")), 1);
    v->addLayout(nav);
    v->addWidget(buildSearchBar());
    return w;
}

QWidget *DebuggerWindow::buildSearchBar()
{
    auto *bar = new QWidget;
    auto *row = new QHBoxLayout(bar);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(new QLabel(QStringLiteral("Search")));
    m_srchKind = new QComboBox;
    m_srchKind->addItems({ QStringLiteral("Hex"), QStringLiteral("Text") });
    m_srchKind->setToolTip(QStringLiteral("Hex bytes (3E 5A) or text, one byte per character"));
    row->addWidget(m_srchKind);
    m_srchEntry = new QLineEdit;
    setMono(m_srchEntry);
    m_srchEntry->setPlaceholderText(QStringLiteral("bytes or text to find"));
    connect(m_srchEntry, &QLineEdit::returnPressed, this, [this] { memSearch(false); });
    row->addWidget(m_srchEntry, 1);
    auto *next = new QPushButton(QStringLiteral("Find Next"));
    auto *prev = new QPushButton(QStringLiteral("Find Previous"));
    connect(next, &QPushButton::clicked, this, [this] { memSearch(false); });
    connect(prev, &QPushButton::clicked, this, [this] { memSearch(true); });
    row->addWidget(next);
    row->addWidget(prev);
    m_srchNote = new QLabel;
    m_srchNote->setStyleSheet(kGray);
    row->addWidget(m_srchNote);
    return bar;
}

QWidget *DebuggerWindow::buildDisasm()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    m_followPc = new QCheckBox(QStringLiteral("Follow PC"));
    m_followPc->setChecked(true);
    connect(m_followPc, &QCheckBox::clicked, this, [this] { refreshDisasm(); });
    m_jump = smallEntry(QStringLiteral("$address or label"), 20);
    connect(m_jump, &QLineEdit::returnPressed, this, [this] { jumpTo(); });
    row->addWidget(m_followPc);
    row->addSpacing(8);
    row->addWidget(new QLabel(QStringLiteral("Jump to")));
    row->addWidget(m_jump);
    row->addSpacing(8);
    row->addWidget(hintLabel(QStringLiteral("Click a line to select it (Run to Cursor); click its left edge, "
                                            "double-click it or press F9 for a breakpoint; scroll to browse")), 1);
    v->addLayout(row);
    m_disasm = monoView();
    /* no scrollbar: the wheel scrolls by instructions, as many lines as fit */
    m_disasm->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_disasm->setFocusPolicy(Qt::NoFocus);
    m_disasm->viewport()->setCursor(Qt::ArrowCursor);
    m_disasm->viewport()->installEventFilter(this);
    v->addWidget(m_disasm, 1);
    return w;
}

QWidget *DebuggerWindow::buildVideo()
{
    m_video = new VideoTab(m_session);
    return m_video;
}

QWidget *DebuggerWindow::buildBreaks()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);

    m_bpStop = new QLabel;
    m_bpStop->setStyleSheet(QStringLiteral("font-weight: bold;"));
    m_bpStop->setWordWrap(true);
    v->addWidget(m_bpStop);

    v->addWidget(hintLabel(QStringLiteral("Execute stops before the instruction runs; the others after the "
                                          "access. Breakpoints are kept while the debugger is closed, but only "
                                          "act while it is open.")));

    m_bpTable = new QTableWidget(0, 4);
    m_bpTable->setHorizontalHeaderLabels({ QStringLiteral("On"), QStringLiteral("Breakpoint"), QStringLiteral("Hits"),
                                           QString() });
    m_bpTable->verticalHeader()->setVisible(false);
    m_bpTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_bpTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_bpTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_bpTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_bpTable->setFocusPolicy(Qt::NoFocus);
    connect(m_bpTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_bpFilling || item->column() != 0) return;
        a7800debug_breakpoint_enable(m_dbg, item->data(Qt::UserRole).toInt(), item->checkState() == Qt::Checked);
        syncLater();
    });
    v->addWidget(m_bpTable, 1);
    m_bpEmpty = hintLabel(QStringLiteral("No breakpoints. Click the left edge of a disassembly line, or add one below."));
    v->addWidget(m_bpEmpty);

    auto *row = new QHBoxLayout;
    m_bpType = new QComboBox;
    for (const auto &entry : kBpTypes) m_bpType->addItem(QString::fromUtf8(entry.label));
    m_bpStart = smallEntry(QStringLiteral("address, range or symbol"), 24);
    m_bpEnd = smallEntry(QStringLiteral("end (optional)"), 16);
    m_bpCond = new QLineEdit;
    m_bpCond->setPlaceholderText(QStringLiteral("condition (optional, e.g. a == 5 && wpdata == $FF)"));
    auto *add = new QPushButton(QStringLiteral("Add"));
    auto *clear = new QPushButton(QStringLiteral("Clear Breakpoints"));
    auto *clearHits = new QPushButton(QStringLiteral("Clear Hits"));
    connect(clearHits, &QPushButton::clicked, this, [this] {
        a7800debug_breakpoint_clear_hits(m_dbg);
        resync();
    });
    connect(add, &QPushButton::clicked, this, [this] { addBreakpoint(); });
    connect(clear, &QPushButton::clicked, this, [this] {
        a7800debug_breakpoint_clear(m_dbg);
        resync();
    });
    for (QLineEdit *e : { m_bpStart, m_bpEnd, m_bpCond })
        connect(e, &QLineEdit::returnPressed, this, [this] { addBreakpoint(); });
    row->addWidget(m_bpType);
    row->addWidget(m_bpStart);
    row->addWidget(m_bpEnd);
    row->addWidget(m_bpCond, 1);
    row->addWidget(add);
    row->addWidget(clearHits);
    row->addWidget(clear);
    v->addLayout(row);

    m_bpMsg = new QLabel;
    m_bpMsg->setWordWrap(true);
    m_bpMsg->setStyleSheet(QStringLiteral("color: #e01b24;"));
    v->addWidget(m_bpMsg);
    return w;
}

QWidget *DebuggerWindow::buildWatches()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("Watch")));
    m_wtEntry = new QLineEdit;
    setMono(m_wtEntry);
    m_wtEntry->setPlaceholderText(QStringLiteral("address, label, expression or lo-hi"));
    connect(m_wtEntry, &QLineEdit::returnPressed, this, [this] { addWatch(m_wtEntry->text()); });
    auto *add = new QPushButton(QStringLiteral("Add"));
    connect(add, &QPushButton::clicked, this, [this] { addWatch(m_wtEntry->text()); });
    row->addWidget(m_wtEntry, 1);
    row->addWidget(add);
    v->addLayout(row);

    m_wtTable = new QTableWidget(0, 2);
    m_wtTable->setHorizontalHeaderLabels({ QStringLiteral("Watch"), QString() });
    m_wtTable->verticalHeader()->setVisible(false);
    m_wtTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_wtTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_wtTable->setColumnWidth(1, 350);
    m_wtTable->verticalHeader()->setDefaultSectionSize(34);
    m_wtTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_wtTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_wtTable->setFocusPolicy(Qt::NoFocus);
    v->addWidget(m_wtTable, 1);
    v->addWidget(hintLabel(QStringLiteral("The values are read at a stop and show - while the machine runs. A watch "
                                          "is kept as text, so a label that loads later starts resolving. "
                                          "Break on change stops after a write that alters the value.")));
    return w;
}

QWidget *DebuggerWindow::buildCart()
{
    m_io = monoView();
    return m_io;
}

/* Trace (idx 0) reads the instruction ring live; History (idx 1) reads it at
 * a stop. They share the one ring and recorder, so the two Record buttons
 * follow each other. */
QWidget *DebuggerWindow::buildHist(int idx)
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *bar = new QHBoxLayout;

    m_hRec[idx] = new QPushButton(QStringLiteral("Record"));
    m_hRec[idx]->setCheckable(true);
    m_hRec[idx]->setFocusPolicy(Qt::NoFocus);
    m_hRec[idx]->setToolTip(QStringLiteral("Record every instruction the CPU runs (the machine runs a little slower)"));
    m_hRec[idx]->setStyleSheet(
        QStringLiteral("QPushButton:checked { background:%1; color:white; }").arg(accentColor().name()));
    fixToggleSize(m_hRec[idx], { QStringLiteral("Record") }, m_hRec[idx]->sizeHint().height());
    connect(m_hRec[idx], &QPushButton::toggled, this, [this](bool on) {
        if (m_hSync) return;
        a7800debug_trace_enable(m_dbg, on);
        for (int i = 0; i < 2; ++i) refreshHist(i, true);
    });
    m_hFilter[idx] = new QComboBox;
    m_hFilter[idx]->addItems({ QStringLiteral("All instructions"), QStringLiteral("Branches only"),
                               QStringLiteral("In range") });
    m_hRange[idx] = new QLineEdit;
    setMono(m_hRange[idx]);
    m_hRange[idx]->setPlaceholderText(QStringLiteral("range: lo-hi or a label"));
    connect(m_hFilter[idx], QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, idx](int) { refreshHist(idx, true); });
    connect(m_hRange[idx], &QLineEdit::returnPressed, this, [this, idx] { refreshHist(idx, true); });
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] {
        a7800debug_trace_clear(m_dbg);
        for (int i = 0; i < 2; ++i) refreshHist(i, true);
    });
    m_hNote[idx] = new QLabel;
    m_hNote[idx]->setStyleSheet(kGray);
    m_hNote[idx]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    bar->addWidget(m_hRec[idx]);
    bar->addWidget(m_hFilter[idx]);
    bar->addWidget(m_hRange[idx], 1);
    bar->addWidget(clear);
    bar->addWidget(m_hNote[idx], 1);
    v->addLayout(bar);
    m_hText[idx] = monoView();
    v->addWidget(m_hText[idx], 1);
    return w;
}

QWidget *DebuggerWindow::buildSound()
{
    m_sound = monoView();
    return m_sound;
}

/* A filter over a table of address and name; activating a row opens
 * that address in the Disassembly. */
QWidget *DebuggerWindow::buildSymbols()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("Filter")));
    m_symFilter = new QLineEdit;
    m_symFilter->setPlaceholderText(QStringLiteral("part of a name"));
    m_symFilter->setClearButtonEnabled(true);
    connect(m_symFilter, &QLineEdit::textChanged, this, [this] { refreshSymbols(true); });
    row->addWidget(m_symFilter, 1);
    m_symCount = new QLabel;
    m_symCount->setStyleSheet(kGray);
    row->addWidget(m_symCount);
    v->addLayout(row);

    m_symTable = new QTableWidget(0, 2);
    m_symTable->setHorizontalHeaderLabels({ QStringLiteral("Address"), QStringLiteral("Name") });
    m_symTable->verticalHeader()->setVisible(false);
    m_symTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_symTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_symTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_symTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_symTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    setMono(m_symTable);
    auto activate = [this](int row) {
        QTableWidgetItem *item = row >= 0 ? m_symTable->item(row, 0) : nullptr;
        if (item) showInDisassembly((uint16_t)item->data(Qt::UserRole).toUInt());
    };
    connect(m_symTable, &QTableWidget::cellDoubleClicked, this, [activate](int row, int) { activate(row); });
    connect(m_symTable, &QTableWidget::itemActivated, this, [activate](QTableWidgetItem *item) { activate(item->row()); });
    v->addWidget(m_symTable, 1);
    v->addWidget(hintLabel(QStringLiteral("Double-click a row (or press Enter on it) to open its address in the "
                                          "Disassembly.")));
    return w;
}

/* The packets left (a note and Clear over them), the link and counts
 * right. */
QWidget *DebuggerWindow::buildFujinet()
{
    auto *split = new QSplitter(Qt::Horizontal);
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    lv->addWidget(titleLabel(QStringLiteral("FujiBus packets (SLIP-decoded)")));
    auto *row = new QHBoxLayout;
    m_fujiNote = new QLabel;
    m_fujiNote->setStyleSheet(kGray);
    m_fujiNote->setWordWrap(true);
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] {
        a7800debug_fujinet_trace_clear(m_dbg);
        refreshFujinet(true);
    });
    row->addWidget(m_fujiNote, 1);
    row->addWidget(clear);
    lv->addLayout(row);
    m_fujiText = monoView();
    lv->addWidget(m_fujiText, 1);
    left->setMinimumWidth(560);

    auto *right = new QWidget;
    auto *rv = new QVBoxLayout(right);
    rv->addWidget(titleLabel(QStringLiteral("The cartridge's mailbox")));
    m_fujiState = monoView();
    rv->addWidget(m_fujiState, 1);
    right->setMinimumWidth(320);
    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setChildrenCollapsible(false);
    return split;
}

/* ---- refresh -------------------------------------------------------------- */

void DebuggerWindow::refreshStatus()
{
    char reason[160];
    int addr = -1;
    const bool stopped = a7800debug_is_stopped(m_dbg) != 0;
    reason[0] = '\0';
    a7800debug_stop_reason(m_dbg, reason, sizeof reason, &addr);
    QString text = QStringLiteral("Running");
    if (stopped) {
        text = stoppedText(stripControl(reason));
        /* where, by name when it has one */
        char name[64];
        if (addr >= 0 && a7800debug_address_label(m_dbg, (uint16_t)addr, name, sizeof name) > 0 && name[0])
            text += QStringLiteral("  (%1)").arg(QString::fromUtf8(name));
    }
    if (m_status->text() != text) {
        m_status->setText(text);
        m_status->setToolTip(text);
    }
    const QString run = stopped ? QStringLiteral("Run (F5)") : QStringLiteral("Stop (F5)");
    if (m_runBtn->text() != run) m_runBtn->setText(run);
    if (m_runBtn->isChecked() != stopped) m_runBtn->setChecked(stopped);
}

/* The refresh never overwrites a value being typed. */
void DebuggerWindow::setEntry(QLineEdit *e, const QString &text)
{
    if (e->hasFocus()) return;
    if (e->text() != text) e->setText(text);
}

void DebuggerWindow::refreshCpu()
{
    a7800debug_cpu c;
    a7800debug_cpu_get(m_dbg, &c);
    const int values[6] = { c.pc, c.sp, c.a, c.x, c.y, c.ps };
    for (int i = 0; i < 6; ++i)
        setEntry(m_reg[i], hx((unsigned)values[i] & (kRegs[i].digits == 4 ? 0xFFFF : 0xFF), kRegs[i].digits));
    const int flags[6] = { c.n, c.v, c.d, c.i, c.z, c.c };
    for (int i = 0; i < 6; ++i) m_flag[i]->setChecked(flags[i] != 0);
    m_beam->setText(QStringLiteral("scanline %1  dot %2  frame %3  %4 cycles")
                        .arg(c.scanline, 3).arg(c.dot, 3).arg(c.frame).arg((qulonglong)c.total_cycles));
}

/* The whole 64K as the CPU sees it, a row of 16 bytes at a time, each row
 * named by the memory-map region it starts in (one fetch of the map for the
 * refresh). The Go to / search range stays marked, and so does every watch. */
void DebuggerWindow::refreshMem()
{
    static uint8_t mem[MEM_ROWS * 16];
    static char text[MEM_ROWS * MEM_ROW_TEXT + 256];
    static a7800debug_region map[REGION_MAX];
    memset(mem, 0, sizeof mem);
    a7800debug_read(m_dbg, 0, mem, (int)sizeof mem);
    const int regions = a7800debug_memory_map(m_dbg, map, REGION_MAX);

    int len = snprintf(text, sizeof text, "       ");
    for (int col = 0; col < 16; ++col) len += snprintf(text + len, sizeof text - (size_t)len, "%X  ", col);
    for (int row = 0; row < MEM_ROWS; ++row) {
        const uint8_t *bytes = mem + row * 16;
        len += snprintf(text + len, sizeof text - (size_t)len, "\n$%04X: ", row * 16);
        for (int col = 0; col < 16; ++col) len += snprintf(text + len, sizeof text - (size_t)len, "%02X ", bytes[col]);
        text[len++] = ' ';
        for (int col = 0; col < 16; ++col) text[len++] = bytes[col] >= 0x20 && bytes[col] < 0x7F ? (char)bytes[col] : '.';
        char name[64];
        a7800mem_region_name(map, regions, (uint16_t)(row * 16), name, sizeof name);
        len += snprintf(text + len, sizeof text - (size_t)len, "  %s", name);
    }
    setText(m_mem, QString::fromLatin1(text, len));

    /* the ranges marked: each row's bytes in the hex and in the text column */
    QList<QTextEdit::ExtraSelection> sel;
    auto mark = [&](uint16_t from, uint16_t to, const QTextCharFormat &fmt) {
        for (int row = from / 16; row <= to / 16; ++row) {
            const int first = qMax((int)from - row * 16, 0), last = qMin((int)to - row * 16, 15);
            sel << spanSelection(m_mem, 1 + row, 7 + first * 3, (last - first) * 3 + 2, fmt);
            sel << spanSelection(m_mem, 1 + row, 7 + 48 + 1 + first, last - first + 1, fmt);
        }
    };
    for (int i = 0; i < a7800watch_count(); ++i) {
        uint16_t lo, hi;
        if (a7800watch_range(m_dbg, i, &lo, &hi)) mark(lo, hi, watchFormat());
    }
    if (m_hl) mark(m_hlLo, m_hlHi, accentFormat());
    m_mem->setExtraSelections(sel);
}

/* Memory scrolled so the row holding addr sits a third of the way down. */
void DebuggerWindow::scrollMemory(uint16_t addr)
{
    const int line = 1 + addr / 16;
    const int shown = m_mem->viewport()->height() / qMax(1, m_mem->fontMetrics().lineSpacing());
    m_mem->verticalScrollBar()->setValue(qMax(0, line - shown * 3 / 10));
}

/* The disassembly lines that fit the view. */
int DebuggerWindow::visibleRows() const
{
    const int line = m_disasm->fontMetrics().lineSpacing();
    const int height = m_disasm->viewport()->height() - 2 * (int)m_disasm->document()->documentMargin();
    if (line <= 0 || height <= 0) return 32;
    return qBound(4, height / line, DISASM_WINDOW);
}

void DebuggerWindow::refreshDisasm()
{
    static a7800debug_line lines[DISASM_WINDOW];
    const int rows = visibleRows();
    if (m_followPc->isChecked()) {
        a7800debug_cpu c;
        a7800debug_cpu_get(m_dbg, &c);
        /* the PC a third of the way down */
        m_disasmTop = (uint16_t)a7800debug_row_address(m_dbg, (uint16_t)c.pc, -(rows / 3));
    }
    int pcLine = -1;
    const int n = a7800debug_disassemble(m_dbg, m_disasmTop, lines, rows, &pcLine);
    /* the label column is as wide as the longest label shown, within reason */
    int labelWidth = 0;
    for (int i = 0; i < n; ++i) labelWidth = qMax(labelWidth, (int)strlen(lines[i].label));
    labelWidth = qMin(labelWidth, 24);

    m_lineAddr.clear();
    QString text;
    char buf[128];
    struct Span { int line, from, length; };
    std::vector<Span> grays;
    for (int i = 0; i < n; ++i) {
        m_lineAddr.push_back(lines[i].address);
        /* the gutter: a breakpoint dot (hollow when disabled), then the
         * PC's arrow */
        QString ln = lines[i].has_breakpoint ? QStringLiteral("●") : lines[i].bp_disabled ? QStringLiteral("○")
                                                                                          : QStringLiteral(" ");
        snprintf(buf, sizeof buf, "%c%04X  %-11.11s ", lines[i].is_pc ? '>' : ' ', lines[i].address, lines[i].bytes);
        ln += QString::fromUtf8(buf);
        const int labelFrom = (int)ln.size();
        char label[48];
        snprintf(label, sizeof label, "%-*.*s ", labelWidth, labelWidth, lines[i].label);
        ln += QString::fromUtf8(label);
        if (lines[i].label[0] && i != pcLine)
            grays.push_back({ i, labelFrom, (int)strlen(lines[i].label) < labelWidth ? (int)strlen(lines[i].label)
                                                                                      : labelWidth });
        snprintf(buf, sizeof buf, "%-22s", lines[i].disasm);
        ln += QString::fromUtf8(buf);
        if (lines[i].comment[0]) {
            const QString comment = QStringLiteral("; ") + QString::fromUtf8(lines[i].comment);
            if (i != pcLine) grays.push_back({ i, (int)ln.size(), (int)comment.size() });
            ln += comment;
        }
        if (i) text += QLatin1Char('\n');
        text += ln;
    }
    if (n == 0) text = QStringLiteral("(no disassembly: the debugger is not attached)");
    m_disasm->setPlainText(stripControl(text.toUtf8().constData()));
    m_disasm->verticalScrollBar()->setValue(0);

    /* The PC's line is the accent bar: the block's own background, not an
     * extra selection -- a full-width selection paints over the gutter's
     * red dot, a block background lies under it. */
    if (pcLine >= 0 && pcLine < n) {
        QTextCursor cur(m_disasm->document()->findBlockByNumber(pcLine));
        QTextBlockFormat bf;
        bf.setBackground(accentColor());
        cur.setBlockFormat(bf);
        cur.select(QTextCursor::LineUnderCursor);
        QTextCharFormat cf;
        cf.setForeground(Qt::white);
        if (lines[pcLine].address == m_selected) {
            cf.setFontUnderline(true);
            cf.setFontWeight(QFont::Bold);
        }
        cur.mergeCharFormat(cf);
    }

    QList<QTextEdit::ExtraSelection> sel;
    /* labels and comments in the system's grey */
    QTextCharFormat grey;
    grey.setForeground(m_disasm->palette().color(QPalette::Disabled, QPalette::Text));
    for (const Span &g : grays) sel << spanSelection(m_disasm, g.line, g.from, g.length, grey);
    for (int i = 0; i < n; ++i) {
        /* the gutter: a breakpoint red (white on the PC's bar), a disabled one grey */
        if (lines[i].has_breakpoint || lines[i].bp_disabled) {
            QTextCharFormat f;
            if (lines[i].has_breakpoint) {
                f.setForeground(i == pcLine ? QColor(Qt::white) : QColor(0xe0, 0x1b, 0x24));
                f.setFontWeight(QFont::Bold);
            } else {
                f.setForeground(i == pcLine ? QColor(Qt::white) : QColor(0x9a, 0x99, 0x96));
            }
            sel << spanSelection(m_disasm, i, 0, 1, f);
        }
        /* the selected line underlined (the PC's own is done above) */
        if (lines[i].address == m_selected && i != pcLine) {
            QTextCharFormat f;
            f.setFontUnderline(true);
            f.setFontWeight(QFont::Bold);
            QTextEdit::ExtraSelection s;
            s.cursor = QTextCursor(m_disasm->document()->findBlockByNumber(i));
            s.cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            s.format = f;
            sel << s;
        }
    }
    m_disasm->setExtraSelections(sel);
}

/* The Video tab takes its own snapshot; a stopped machine redraws only when
 * the core says something changed. */
void DebuggerWindow::refreshVideo(bool force)
{
    m_video->refresh(force);
}

void DebuggerWindow::refreshBps()
{
    static a7800debug_breakpoint bps[BP_MAX];
    const int n = a7800debug_breakpoint_list(m_dbg, bps, BP_MAX);

    if (a7800debug_is_stopped(m_dbg)) {
        char reason[160];
        int addr = -1;
        reason[0] = '\0';
        a7800debug_stop_reason(m_dbg, reason, sizeof reason, &addr);
        m_bpStop->setText(stoppedText(stripControl(reason)) + QLatin1Char('.'));
    } else {
        m_bpStop->setText(QStringLiteral("Running."));
    }

    /* rebuilt only when it changed (each step refreshes), and never while a
     * row's own button is being handled */
    QString sig;
    for (int i = 0; i < n; ++i)
        sig += QStringLiteral("%1 %2 %3 %4 %5 %6 %7\n").arg(bps[i].id).arg(bps[i].enabled).arg(bps[i].hits)
                   .arg(bps[i].type).arg(bps[i].start).arg(bps[i].end).arg(QString::fromUtf8(bps[i].condition));
    if (sig == m_bpSig && m_bpTable->rowCount() == n) return;
    m_bpSig = sig;

    QFont mono = m_bpTable->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    m_bpFilling = true;
    m_bpTable->setRowCount(n);
    for (int i = 0; i < n; ++i) {
        auto *on = new QTableWidgetItem;
        on->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        on->setCheckState(bps[i].enabled ? Qt::Checked : Qt::Unchecked);
        on->setData(Qt::UserRole, bps[i].id);
        on->setToolTip(QStringLiteral("Enabled"));
        m_bpTable->setItem(i, 0, on);
        char desc[256];
        a7800bp_describe(m_dbg, &bps[i], desc, sizeof desc);
        m_bpTable->setItem(i, 1, new QTableWidgetItem(stripControl(desc)));
        m_bpTable->setItem(i, 2, new QTableWidgetItem(QString::number(bps[i].hits)));
        for (int col = 1; col < 3; ++col) m_bpTable->item(i, col)->setFont(mono);
        auto *rm = new QToolButton;
        rm->setIcon(QIcon::fromTheme(QStringLiteral("edit-delete"), QIcon::fromTheme(QStringLiteral("user-trash"))));
        if (rm->icon().isNull()) rm->setText(QStringLiteral("Remove"));
        rm->setAutoRaise(true);
        rm->setToolTip(QStringLiteral("Remove"));
        const int id = bps[i].id;
        connect(rm, &QToolButton::clicked, this, [this, id] {
            a7800debug_breakpoint_remove(m_dbg, id);
            syncLater();
        });
        m_bpTable->setCellWidget(i, 3, rm);
    }
    m_bpFilling = false;
    m_bpEmpty->setVisible(n == 0);
}

/* The FujiNet cartridge, the memory map as regions (drawn here once), then
 * INPTCTRL, the RIOT, the TIA's inputs and the controllers. */
void DebuggerWindow::refreshCart()
{
    static a7800debug_region reg[REGION_MAX];
    a7800debug_cart c;
    a7800debug_io io;
    char info[256];
    a7800debug_cart_get(m_dbg, &c);
    a7800debug_cart_info(m_dbg, info, sizeof info);
    a7800debug_io_get(m_dbg, &io);

    QString s = stripControl(info) + QStringLiteral("\n\n");
    if (c.present) {
        static const char *const handovers[3] = { "none yet", "the BIOS", "the loader" };
        s += QStringLiteral("Link          %1%2\n").arg(c.link_up ? "up" : "down",
                                                         c.worker ? QString() : QStringLiteral(" (mailbox service stopped)"));
        if (c.link_error[0]) s += QStringLiteral("Last error    %1\n").arg(QString::fromUtf8(c.link_error));
        s += QStringLiteral("Mode          %1%2\n").arg(QString::fromUtf8(c.mode_name),
                                                         c.mode == 1 ? QStringLiteral(", loading %1%").arg(c.load_pct) : QString());
        s += QStringLiteral("Image         %1\n").arg(c.booted_image ? "a booted game" : "CONFIG or a FujiNet app");
        s += QStringLiteral("Started by    %1\n")
                 .arg(QString::fromUtf8(c.handover >= 0 && c.handover < 3 ? handovers[c.handover] : "?"));
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
        s += QStringLiteral("Mailbox       ACKSEQ $%1   last error %2   queue %3\n")
                 .arg(hx(c.ackseq, 2)).arg(c.last_error).arg(c.queue_depth);
        s += QStringLiteral("Boot          state $%1   %2%   error %3\n").arg(hx(c.boot_state, 2)).arg(c.boot_pct).arg(c.boot_err);
    }

    s += QStringLiteral("\nMemory map\n");
    const int n = a7800debug_memory_map(m_dbg, reg, REGION_MAX);
    for (int i = 0; i < n; ++i)
        s += QStringLiteral("  $%1-$%2  %3%4  %5\n").arg(hx(reg[i].lo, 4), hx(reg[i].hi, 4))
                 .arg(reg[i].readable ? QLatin1Char('r') : QLatin1Char('-'))
                 .arg(reg[i].writable ? QLatin1Char('w') : QLatin1Char('-'))
                 .arg(QString::fromUtf8(reg[i].name));

    s += QStringLiteral("\nI/O\n");
    s += QStringLiteral("  INPTCTRL $01           $%1   %2\n").arg(hx(io.inptctrl, 2), io.inpt_locked ? "locked" : "unlocked");
    /* SWCHA: player 1 in the high nibble, player 2 in the low; 0 is pushed */
    static const char *const dirs[4] = { "up", "down", "left", "right" };
    s += QStringLiteral("  SWCHA                  $%1").arg(hx(io.swcha, 2));
    for (int player = 0; player < 2; ++player) {
        QStringList pushed;
        const int nibble = player ? (io.swcha & 0x0f) : (io.swcha >> 4);
        for (int b = 0; b < 4; ++b)
            if (!(nibble & (1 << b))) pushed << QString::fromUtf8(dirs[b]);
        s += QStringLiteral("   player %1: %2").arg(player + 1)
                 .arg(pushed.isEmpty() ? QStringLiteral("centred") : pushed.join(QLatin1Char(' ')));
    }
    s += QStringLiteral("\n  SWCHB                  $%1   Reset %2   Select %3   Pause %4   difficulty left %5, right %6\n")
             .arg(hx(io.swchb, 2), (io.swchb & 0x01) ? "up" : "DOWN", (io.swchb & 0x02) ? "up" : "DOWN",
                  (io.swchb & 0x08) ? "up" : "DOWN", (io.swchb & 0x40) ? "A" : "B", (io.swchb & 0x80) ? "A" : "B");
    s += QStringLiteral("  INPT0-5               ");
    for (int i = 0; i < 6; ++i) s += QStringLiteral(" $%1").arg(hx(io.inpt[i], 2));
    s += QLatin1Char('\n');
    for (int port = 0; port < 2; ++port) {
        const char *type = a7800_ctrl_type_name(io.port_type[port]);
        s += QStringLiteral("  Port %1                 %2   held: %3\n").arg(port + 1)
                 .arg(QString::fromUtf8(type ? type : "?"), heldNames(io.held[port]));
    }
    setText(m_io, s);
}

/* The TIA's two channels and, when the cartridge has one, the POKEY's four
 * and AUDCTL. */
void DebuggerWindow::refreshSound()
{
    a7800debug_sound a;
    a7800debug_sound_get(m_dbg, &a);
    QString s = QStringLiteral("TIA\n");
    for (int ch = 0; ch < 2; ++ch)
        s += QStringLiteral("  Channel %1   AUDC $%2   AUDF $%3   AUDV $%4%5\n").arg(ch)
                 .arg(hx(a.tia_audc[ch], 2), hx(a.tia_audf[ch], 2), hx(a.tia_audv[ch], 2),
                      (a.tia_audv[ch] & 0x0F) == 0 ? QStringLiteral("  (silent)") : QString());
    s += QLatin1Char('\n');
    if (a.pokey_present) {
        s += QStringLiteral("POKEY (on this cartridge)\n");
        for (int ch = 0; ch < 4; ++ch)
            s += QStringLiteral("  Channel %1   AUDF $%2   AUDC $%3   volume %4%5\n").arg(ch + 1)
                     .arg(hx(a.pokey_audf[ch], 2), hx(a.pokey_audc[ch], 2)).arg(a.pokey_audc[ch] & 0x0F)
                     .arg((a.pokey_audc[ch] & 0x0F) == 0 ? QStringLiteral("  (silent)") : QString());
        s += QStringLiteral("  AUDCTL      $%1\n").arg(hx(a.pokey_audctl, 2));
    } else {
        s += QStringLiteral("POKEY\n  none on this cartridge\n");
    }
    setText(m_sound, s);
}

/* The labels the engine holds, filtered, in the order the filter gives
 * (address, then name). The table is rebuilt only when what it would show
 * changed: the engine's own index order is not stable, so the entries are
 * fetched anew each time and compared by content. */
void DebuggerWindow::refreshSymbols(bool force)
{
    const int total = a7800debug_symbols_count(m_dbg);
    const QByteArray filter = m_symFilter->text().trimmed().toUtf8();
    std::vector<int> indices((size_t)qMax(total, 0));
    const int shown = total > 0 ? a7800debug_symbols_filter(m_dbg, filter.constData(), indices.data(), total) : 0;

    struct Row { uint16_t addr; char name[96]; };
    std::vector<Row> rows;
    rows.reserve((size_t)shown);
    QString sig = QStringLiteral("%1/%2/%3:").arg(total).arg(shown).arg(QString::fromUtf8(filter));
    for (int i = 0; i < shown; ++i) {
        Row row = {};
        if (!a7800debug_symbols_get(m_dbg, indices[(size_t)i], &row.addr, row.name, (int)sizeof row.name)) continue;
        rows.push_back(row);
        sig += QStringLiteral("%1 %2;").arg(row.addr).arg(QString::fromUtf8(row.name));
    }
    if (!force && sig == m_symSig) return;
    m_symSig = sig;

    m_symCount->setText(QStringLiteral("%1 of %2 symbols").arg(rows.size()).arg(total));
    const int top = m_symTable->verticalScrollBar()->value();
    m_symTable->setUpdatesEnabled(false);
    m_symTable->setRowCount((int)rows.size());
    for (int i = 0; i < (int)rows.size(); ++i) {
        auto *addr = new QTableWidgetItem(QStringLiteral("$") + hx(rows[(size_t)i].addr, 4));
        addr->setData(Qt::UserRole, rows[(size_t)i].addr);
        m_symTable->setItem(i, 0, addr);
        m_symTable->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(rows[(size_t)i].name)));
    }
    m_symTable->setUpdatesEnabled(true);
    m_symTable->verticalScrollBar()->setValue(top);
}

void DebuggerWindow::addWatch(const QString &entry)
{
    const QByteArray text = entry.trimmed().toUtf8();
    if (text.isEmpty()) return;
    if (a7800watch_add(text.constData()) < 0) {
        message(QStringLiteral("The watch list is full"));
        return;
    }
    m_wtEntry->clear();
    refreshWatches(true);
}

/* Break on change: stop after a write over the watch that alters a byte. */
void DebuggerWindow::watchBreak(int index)
{
    uint16_t lo, hi;
    if (!a7800watch_range(m_dbg, index, &lo, &hi)) {
        message(QStringLiteral("That watch does not resolve yet"));
        return;
    }
    if (a7800debug_breakpoint_add(m_dbg, A7800DEBUG_BP_WRITE, lo, hi, "value != old") < 0)
        message(QStringLiteral("The breakpoint could not be added"));
    else
        message(QStringLiteral("Breaking when it changes"));
    resync();
}

/* One row a watch, its value live (a dash while the machine runs). The rows
 * are rebuilt when the list changed; otherwise only their text moves. */
void DebuggerWindow::refreshWatches(bool force)
{
    const int n = a7800watch_count();
    const unsigned gen = a7800watch_generation();
    QFont mono = m_wtTable->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    if (force || gen != m_wtGen || m_wtTable->rowCount() != n) {
        m_wtGen = gen;
        m_wtTable->setRowCount(n);
        for (int i = 0; i < n; ++i) {
            auto *item = new QTableWidgetItem;
            item->setFont(mono);
            m_wtTable->setItem(i, 0, item);
            auto *box = new QWidget;
            auto *h = new QHBoxLayout(box);
            h->setContentsMargins(2, 0, 2, 0);
            auto *mem = new QPushButton(QStringLiteral("Memory"));
            mem->setToolTip(QStringLiteral("Show it in the Memory tab"));
            connect(mem, &QPushButton::clicked, this, [this, i] {
                uint16_t lo, hi;
                if (!a7800watch_range(m_dbg, i, &lo, &hi)) {
                    message(QStringLiteral("That watch does not resolve yet"));
                    return;
                }
                if (m_popped[TAB_MEMORY]) m_popped[TAB_MEMORY]->activateWindow();
                else m_tabs->setCurrentWidget(m_pages[TAB_MEMORY]);
                showInMemory(lo, hi);
            });
            auto *brk = new QPushButton(QStringLiteral("Break on change"));
            brk->setToolTip(QStringLiteral("Stop after a write that alters it"));
            connect(brk, &QPushButton::clicked, this, [this, i] { watchBreak(i); });
            auto *rm = new QPushButton(QStringLiteral("Remove"));
            connect(rm, &QPushButton::clicked, this, [this, i] {
                a7800watch_remove(i);
                QTimer::singleShot(0, this, [this] { refreshWatches(true); });
            });
            for (QPushButton *b : { mem, brk, rm }) {
                b->setFocusPolicy(Qt::NoFocus);
                h->addWidget(b);
            }
            m_wtTable->setCellWidget(i, 1, box);
        }
    }
    for (int i = 0; i < n; ++i) {
        char line[256];
        a7800watch_format(m_dbg, i, line, sizeof line);
        const QString text = stripControl(line);
        if (m_wtTable->item(i, 0)->text() != text) m_wtTable->item(i, 0)->setText(text);
    }
}

/* Trace (live) and History (at a stop): the ring newest first, shown
 * oldest first, the newest HIST_SHOWN lines that pass the filter. */
void DebuggerWindow::refreshHist(int idx, bool force)
{
    static a7800debug_trace buf[RING_MAX];
    const bool stopped = a7800debug_is_stopped(m_dbg) != 0;
    const bool on = a7800debug_trace_enabled(m_dbg) != 0;
    const int count = a7800debug_trace_count(m_dbg);

    m_hSync = true;                     /* both Record buttons follow the engine */
    for (int i = 0; i < 2; ++i)
        if (m_hRec[i]->isChecked() != on) m_hRec[i]->setChecked(on);
    m_hSync = false;

    if (force) m_hSeen[idx] = -1;
    /* History reads at a stop (it keeps what it showed while the machine
     * runs); at a stop neither view rebuilds for the same count */
    if (idx == 1 && !stopped && m_hSeen[1] >= 0) return;
    if (stopped && count == m_hSeen[idx]) return;
    m_hSeen[idx] = count;

    uint16_t lo = 0, hi = 0xFFFF;
    int filter = A7800DEBUG_TRACE_ALL;
    switch (m_hFilter[idx]->currentIndex()) {
    case 1: filter = A7800DEBUG_TRACE_BRANCHES; break;
    case 2:
        if (a7800sym_parse_range(m_dbg, m_hRange[idx]->text().toUtf8().constData(), &lo, &hi))
            filter = A7800DEBUG_TRACE_RANGE;
        break;
    default: break;
    }
    const int n = a7800debug_trace_read(m_dbg, buf, RING_MAX);
    QList<int> hit;
    for (int i = 0; i < n && hit.size() < HIST_SHOWN; ++i)
        if (a7800debug_trace_match(&buf[i], filter, lo, hi)) hit.append(i);
    QString t;
    if (!on && n == 0) t = QStringLiteral("Recording is off. Press Record, then let the machine run.");
    char line[256];
    for (int k = (int)hit.size() - 1; k >= 0; --k) {
        a7800debug_trace_format(&buf[hit[k]], line, sizeof line);
        t += QString::fromUtf8(line);
        if (k) t += QLatin1Char('\n');
    }
    setText(m_hText[idx], t, true);
    m_hNote[idx]->setText(on ? QStringLiteral("recording, %1 instructions").arg(n)
                             : n ? QStringLiteral("recording off, %1 instructions").arg(n)
                                 : QStringLiteral("recording off"));
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
void DebuggerWindow::refreshFujinet(bool force)
{
    static a7800debug_fuji_packet p[TRACE_MAX];
    static char state[4096];
    const int n = a7800debug_fujinet_trace(m_dbg, p, TRACE_MAX);
    if (!a7800session_is_running(m_session)) {
        m_fujiNote->setText(QStringLiteral("The console is off."));
    } else if (!a7800session_fujinet_running(m_session) && !a7800session_fujinet_external(m_session)) {
        m_fujiNote->setText(QStringLiteral("FujiNet is off (Preferences ▸ Enable FujiNet)."));
    } else {
        char status[160];
        a7800session_cart_status(m_session, status, sizeof status);
        m_fujiNote->setText(QStringLiteral("%1 FujiNet at %2, cartridge %3.")
                                .arg(a7800session_fujinet_external(m_session) ? QStringLiteral("External")
                                                                              : QStringLiteral("Built-in"))
                                .arg(QString::fromUtf8(a7800session_boip_endpoint(m_session)))
                                .arg(QString::fromUtf8(status)));
    }
    state[0] = '\0';
    a7800debug_fujinet_state(m_dbg, state, int(sizeof state));
    setText(m_fujiState, stripControl(state));

    if (!force && n == m_fujiCount && (n == 0 || p[n - 1].serial == m_fujiLast)) return;
    const bool follow = force || m_fujiCount < 0 || atEnd(m_fujiText);
    m_fujiCount = n;
    m_fujiLast = n > 0 ? p[n - 1].serial : 0;
    QString t;
    for (int i = 0; i < n; ++i) {
        if (i) t += QLatin1Char('\n');
        t += QString::fromUtf8(p[i].text);
        if (p[i].repeat > 1) t += QStringLiteral(" ×%1").arg(p[i].repeat);
        t += QStringLiteral("\n     ");
        for (int k = 0; k < p[i].nbytes && k < int(sizeof p[i].data); k++) t += hx(p[i].data[k], 2) + QLatin1Char(' ');
        if (p[i].nbytes > int(sizeof p[i].data)) t += QStringLiteral("…");
    }
    if (n == 0) t = QStringLiteral("(no FujiBus packets since the trace was cleared)");
    setText(m_fujiText, t, follow);
}

/* One tab; live is the twice-a-second refresh while the machine runs, which
 * leaves out what only changes at a stop. */
void DebuggerWindow::refreshPage(int page, bool live)
{
    switch (page) {
    case TAB_DISASM: if (!live) refreshDisasm(); break;
    case TAB_MEMORY: refreshMem(); break;
    case TAB_VIDEO: refreshVideo(std::exchange(m_videoForce, false)); break;
    case TAB_BREAKS: refreshBps(); break;
    case TAB_WATCHES: refreshWatches(false); break;
    case TAB_CART: refreshCart(); break;
    case TAB_TRACE: refreshHist(0, false); break;
    case TAB_HISTORY: if (!live) refreshHist(1, false); break;
    case TAB_SOUND: refreshSound(); break;
    case TAB_SYMBOLS: refreshSymbols(false); break;
    case TAB_FUJINET: refreshFujinet(false); break;
    default: break;
    }
}

/* The registers show everywhere; then the tab showing and any in a window of
 * its own. */
void DebuggerWindow::refreshAll()
{
    refreshStatus();
    refreshCpu();
    refreshPage(currentTab(), false);
    for (int t = 0; t < TAB_COUNT; ++t)
        if (m_popped[t]) refreshPage(t, false);
}

/* A page's number, whichever place it holds in the tab widget now. */
int DebuggerWindow::currentTab() const
{
    QWidget *w = m_tabs->currentWidget();
    return w ? w->property("tab").toInt() : -1;
}

/* On screen: the tab widget's page, or a window of its own. */
bool DebuggerWindow::tabVisible(int tab) const
{
    return currentTab() == tab || m_popped[tab] != nullptr;
}

/* A new stop brings every view to the spot, whatever the reader had
 * scrolled to: the Disassembly to the PC, Memory to the range it marks (or
 * the PC), the Trace and History to their newest line, the Video tab to a
 * redraw. */
void DebuggerWindow::recentreOnStop()
{
    m_followPc->setChecked(true);
    m_disasm->verticalScrollBar()->setValue(0);
    m_hSeen[0] = m_hSeen[1] = -1;
    m_videoForce = true;
    refreshAll();
    a7800debug_cpu c;
    a7800debug_cpu_get(m_dbg, &c);
    scrollMemory(m_hl ? m_hlLo : (uint16_t)c.pc);
    for (QPlainTextEdit *view : m_hText)
        view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
}

/* After anything that may have moved the engine: the console notes a new
 * stop, then every view follows it. */
void DebuggerWindow::resync()
{
    const bool stopped = a7800debug_is_stopped(m_dbg) != 0;
    const uint64_t serial = a7800debug_stop_serial(m_dbg);
    const bool newStop = stopped && serial != m_seenSerial;
    if (newStop) {
        char reason[160];
        int addr;
        reason[0] = '\0';
        a7800debug_stop_reason(m_dbg, reason, sizeof reason, &addr);
        /* a step already says where it is */
        if (reason[0] && strncmp(reason, "step", 4) != 0)
            consoleAppend(QStringLiteral("— %1\n").arg(stripControl(reason)));
    }
    m_seenSerial = serial;
    m_wasStopped = stopped;
    m_seenGen = a7800debug_generation(m_dbg);
    m_runningTicks = 0;
    if (newStop) recentreOnStop();
    else refreshAll();
}

/* From inside a breakpoint row's own signal: the table rebuilds after. */
void DebuggerWindow::syncLater()
{
    QTimer::singleShot(0, this, [this] { resync(); });
}

/* A step takes microseconds: a moment's wait for the stop keeps the window
 * from flashing "Running" in between. */
void DebuggerWindow::settleAndSync()
{
    for (int i = 0; i < 40 && !a7800debug_is_stopped(m_dbg); ++i) QThread::msleep(1);
    resync();
}

/* The engine reports a stop through the generation and the stop serial, so
 * this polls them: a refresh when they moved, and the live values twice a
 * second while the machine runs. */
void DebuggerWindow::tick()
{
    if (!isVisible()) return;
    const unsigned gen = a7800debug_generation(m_dbg);
    const bool stopped = a7800debug_is_stopped(m_dbg) != 0;
    if (gen != m_seenGen || stopped != m_wasStopped || a7800debug_stop_serial(m_dbg) != m_seenSerial) {
        resync();
    } else if (tabVisible(TAB_WATCHES) && a7800watch_generation() != m_wtGen) {
        refreshWatches(false);          /* the list changed elsewhere (the console) */
    } else if (!stopped && ++m_runningTicks >= 4) {
        m_runningTicks = 0;
        refreshStatus();
        refreshCpu();
        refreshPage(currentTab(), true);
        for (int t = 0; t < TAB_COUNT; ++t)
            if (m_popped[t]) refreshPage(t, true);
    }
}

/* ---- the console ---------------------------------------------------------- */

/* Onto the transcript, which follows its end while the reader is there. */
void DebuggerWindow::consoleAppend(const QString &text)
{
    const bool follow = atEnd(m_conOut);
    QTextCursor cur(m_conOut->document());
    cur.movePosition(QTextCursor::End);
    cur.insertText(text);
    if (follow) m_conOut->verticalScrollBar()->setValue(m_conOut->verticalScrollBar()->maximum());
}

void DebuggerWindow::runConsole()
{
    static char out[65536];
    const QString cmd = m_conIn->text().trimmed();
    if (cmd.isEmpty()) return;
    if (m_history.isEmpty() || m_history.last() != cmd) m_history << cmd;
    m_historyPos = (int)m_history.size();
    /* a command brings the reader back down */
    m_conOut->verticalScrollBar()->setValue(m_conOut->verticalScrollBar()->maximum());
    consoleAppend(QStringLiteral("> %1\n").arg(cmd));
    m_conIn->clear();

    out[0] = '\0';
    a7800debug_command(m_dbg, cmd.toUtf8().constData(), out, sizeof out);
    QString text = stripControl(out);
    if (!text.isEmpty()) consoleAppend(text.endsWith(QLatin1Char('\n')) ? text : text + QLatin1Char('\n'));
    settleAndSync();
}

/* Tab: the engine completes the word at the end of the line. One match
 * replaces it; several are listed and the line extends to what they share. */
void DebuggerWindow::completeConsole()
{
    char comps[4096];
    const QString text = m_conIn->text();
    comps[0] = '\0';
    const int n = a7800debug_completions(m_dbg, text.toUtf8().constData(), comps, sizeof comps);
    if (n <= 0) return;
    const QStringList words = stripControl(comps).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (words.isEmpty()) return;
    const int sp = text.lastIndexOf(QLatin1Char(' '));
    const QString head = text.left(sp + 1);
    if (words.size() == 1) {
        m_conIn->setText(head + words[0] + QLatin1Char(' '));
        return;
    }
    QString common = words[0];
    for (const QString &w : words)
        while (!w.startsWith(common)) common.chop(1);
    consoleAppend(words.join(QStringLiteral("  ")) + QLatin1Char('\n'));
    if (common.size() > text.size() - sp - 1) m_conIn->setText(head + common);
}

/* Messages land in the console and, since it may not be the tab showing,
 * the status bar. */
void DebuggerWindow::say(const QString &msg)
{
    consoleAppend(msg + QLatin1Char('\n'));
    message(msg);
}

void DebuggerWindow::message(const QString &msg)
{
    statusBar()->showMessage(msg, 6000);
}

/* ---- the rest of the handlers ----------------------------------------------- */

void DebuggerWindow::toggleRun()
{
    if (a7800debug_is_stopped(m_dbg)) {
        a7800debug_resume(m_dbg);
        resync();
    } else {
        a7800debug_stop(m_dbg);
        settleAndSync();
    }
}

void DebuggerWindow::stepAnd(const std::function<void()> &fn)
{
    if (!a7800debug_is_stopped(m_dbg)) {
        message(QStringLiteral("Stop the machine first"));
        return;
    }
    fn();
    settleAndSync();
}

void DebuggerWindow::runToCursor()
{
    if (m_selected < 0) {
        message(QStringLiteral("Select a line in the Disassembly to run to"));
        return;
    }
    stepAnd([this] { a7800debug_run_to(m_dbg, (uint16_t)m_selected); });
}

void DebuggerWindow::toggleBreakpoint(int addr)
{
    a7800debug_breakpoint_toggle(m_dbg, (uint16_t)addr);
    resync();
}

void DebuggerWindow::toggleSelected()
{
    if (m_selected < 0) {
        message(QStringLiteral("Select a line in the Disassembly first"));
        return;
    }
    toggleBreakpoint(m_selected);
}

/* The toolbar's Break at: a symbol or a hex address, an execute breakpoint
 * there (one already set stays). */
void DebuggerWindow::breakAt()
{
    const QString text = m_breakAt->text().trimmed();
    if (text.isEmpty()) return;
    long addr;
    QString why;
    if (!addressOf(text, &addr, &why)) {
        message(why);
        return;
    }
    if (a7800debug_breakpoint_check(m_dbg, (uint16_t)addr))
        message(QStringLiteral("A breakpoint is already set at $%1").arg(hx((unsigned)addr, 4)));
    else {
        a7800debug_breakpoint_toggle(m_dbg, (uint16_t)addr);
        message(QStringLiteral("Breakpoint at $%1").arg(hx((unsigned)addr, 4)));
    }
    m_breakAt->clear();
    resync();
}

/* A number typed into an entry: a label first, then bare digits as hex (as in
 * the sibling debuggers), then whatever the engine evaluates ($4000, 0x4000,
 * label+2).
 * When it is none of those, the reason goes to *why, or to the status bar
 * without. */
bool DebuggerWindow::numberOf(const QString &text, long *out, QString *why)
{
    static const QRegularExpression hexOnly(QStringLiteral("^[0-9A-Fa-f]+$"));
    const QString t = text.trimmed();
    const QByteArray utf8 = t.toUtf8();
    long v = 0;
    QString reason;
    uint16_t lo, hi;
    const int label = a7800debug_label_address(m_dbg, utf8.constData());
    if (label >= 0) {
        v = label;
    } else if (hexOnly.match(t).hasMatch()) {
        bool ok;
        v = t.toLong(&ok, 16);
        if (!ok) reason = QStringLiteral("Not a number");
    } else if (a7800sym_parse_range(m_dbg, utf8.constData(), &lo, &hi) && lo == hi) {
        v = lo;
    } else {
        reason = QStringLiteral("Not a number ($4000, or a label): %1").arg(t);
    }
    if (reason.isEmpty()) {
        *out = v;
        return true;
    }
    if (why) *why = reason;
    else message(reason);
    return false;
}

/* The same, as an address: 0 to $FFFF. */
bool DebuggerWindow::addressOf(const QString &text, long *out, QString *why)
{
    long v;
    if (!numberOf(text, &v, why)) return false;
    if (v < 0 || v > 0xFFFF) {
        const QString reason = QStringLiteral("Out of range ($0000-$FFFF)");
        if (why) *why = reason;
        else message(reason);
        return false;
    }
    *out = v;
    return true;
}

/* A breakpoint's range from its two entries. The start may itself be a
 * range (lo-hi); the end is optional. */
bool DebuggerWindow::rangeOf(const QString &startText, const QString &endText, uint16_t *lo, uint16_t *hi,
                             QString *why)
{
    long first = 0, last = 0;
    const QString start = startText.trimmed(), end = endText.trimmed();
    if (end.isEmpty()) {
        uint16_t from, to;
        if (!a7800sym_parse_range(m_dbg, start.toUtf8().constData(), &from, &to)) {
            *why = QStringLiteral("Not an address, label or range: %1").arg(start);
            return false;
        }
        first = from;
        last = to;
    } else {
        if (!addressOf(start, &first, why) || !addressOf(end, &last, why)) return false;
    }
    if (last < first) {
        *why = QStringLiteral("The end is before the start");
        return false;
    }
    *lo = (uint16_t)first;
    *hi = (uint16_t)last;
    return true;
}

/* Edits go in only while the machine is stopped. A register entry shows hex
 * digits, so bare digits there are hex; anything else is an expression. */
void DebuggerWindow::setReg(int i)
{
    QLineEdit *e = m_reg[i];
    long v = 0;
    QString why;
    const bool ok = numberOf(e->text(), &v, &why);
    if (!a7800debug_is_stopped(m_dbg)) message(QStringLiteral("Stop the machine first"));
    else if (!ok) message(why);
    else a7800debug_cpu_set(m_dbg, kRegs[i].id, (int)v);
    e->clearFocus();
    resync();
}

void DebuggerWindow::setFlag(int id, bool on)
{
    if (!a7800debug_is_stopped(m_dbg)) message(QStringLiteral("Stop the machine first"));
    else a7800debug_cpu_set(m_dbg, id, on ? 1 : 0);
    resync();
}

/* Shows a range in the dump, from just above it, marked and scrolled to. */
void DebuggerWindow::showInMemory(uint16_t lo, uint16_t hi)
{
    m_hl = true;
    m_hlLo = lo;
    m_hlHi = hi;
    refreshMem();
    scrollMemory(lo);
}

/* Opens an address in the Disassembly (the tab, or its window), the PC no
 * longer followed, the line selected. */
void DebuggerWindow::showInDisassembly(uint16_t addr)
{
    if (m_popped[TAB_DISASM]) m_popped[TAB_DISASM]->activateWindow();
    else m_tabs->setCurrentWidget(m_pages[TAB_DISASM]);
    m_followPc->setChecked(false);
    m_selected = addr;
    m_disasmTop = (uint16_t)a7800debug_row_address(m_dbg, addr, -(visibleRows() / 3));
    refreshDisasm();
}

/* Go to: a label, an address, an expression or a range (lo-hi), which is
 * marked. Bare digits are hex, as everywhere in the entries. */
void DebuggerWindow::memGoto()
{
    const QString text = m_memGoto->text().trimmed();
    if (text.isEmpty()) return;
    uint16_t lo, hi;
    if (!a7800sym_parse_range(m_dbg, text.toUtf8().constData(), &lo, &hi)) {
        m_hl = false;
        refreshMem();
        message(QStringLiteral("Not an address, label or range: %1").arg(text));
        return;
    }
    showInMemory(lo, hi);
}

/* Find Next / Previous from the marked range (or the top of the view); the
 * hit is marked. */
void DebuggerWindow::memSearch(bool backward)
{
    uint8_t pat[A7800SEARCH_MAX], mask = 0xFF;
    int n = 0, flags = 0;
    if (!a7800search_parse(m_srchKind->currentIndex(), m_srchEntry->text().toUtf8().constData(), pat, &n, &mask,
                         &flags) || n <= 0) {
        m_srchNote->setText(QStringLiteral("Type hex bytes (3E 5A) or some text."));
        return;
    }
    uint16_t from = 0, found = 0;
    if (m_hl) from = backward ? m_hlLo : (uint16_t)(m_hlLo + 1);
    const int r = a7800debug_search_next(m_dbg, pat, n, mask, flags, from, backward ? 1 : 0, &found);
    if (r < 0) {
        m_srchNote->setText(QStringLiteral("Stop the machine first"));
        message(QStringLiteral("Stop the machine first"));
    } else if (r == 0) {
        m_srchNote->setText(QStringLiteral("Not found"));
        message(QStringLiteral("Not found"));
    } else {
        m_srchNote->setText(QStringLiteral("Found at $%1").arg(hx(found, 4)));
        showInMemory(found, (uint16_t)(found + n - 1));
    }
}

void DebuggerWindow::memWrite()
{
    long a, v;
    if (!addressOf(m_memAddr->text(), &a)) return;
    if (!numberOf(m_memVal->text(), &v)) return;
    if (v < 0 || v > 0xFF) {
        message(QStringLiteral("The value is a byte ($00-$FF)"));
        return;
    }
    a7800debug_write(m_dbg, (uint16_t)a, (uint8_t)v);
    resync();
}

void DebuggerWindow::jumpTo()
{
    long a;
    if (!addressOf(m_jump->text(), &a)) return;
    m_followPc->setChecked(false);
    m_selected = (int)a;
    m_disasmTop = (uint16_t)a7800debug_row_address(m_dbg, (uint16_t)a, -(visibleRows() / 3));
    refreshDisasm();
}

/* The start takes an address, range or label; the end is optional; the
 * condition is an expression the engine checks. */
void DebuggerWindow::addBreakpoint()
{
    const int type = bpTypeAt(m_bpType->currentIndex());
    const QString start = m_bpStart->text().trimmed(), end = m_bpEnd->text().trimmed();
    if (start.isEmpty()) return;
    uint16_t lo = 0, hi = 0;
    QString why;
    if (!rangeOf(start, end, &lo, &hi, &why)) {
        m_bpMsg->setText(why);
        return;
    }
    const QByteArray cond = m_bpCond->text().trimmed().toUtf8();
    if (a7800debug_breakpoint_add(m_dbg, type, lo, hi, cond.constData()) < 0) {
        m_bpMsg->setText(QStringLiteral("The condition does not parse"));
        return;
    }
    m_bpMsg->clear();
    m_bpStart->clear();
    m_bpEnd->clear();
    m_bpCond->clear();
    resync();
}

/* A symbol file the user picks, or (fromFile false) the <cart>.sym/.map/.noi
 * beside the opened cartridge. */
void DebuggerWindow::loadSymbols(bool fromFile)
{
    QString path;
    if (fromFile) {
        const QString cart = QString::fromUtf8(a7800session_cart_path(m_session));
        path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Load Symbols"), cart.isEmpty() ? QString() : QFileInfo(cart).absolutePath(),
            QStringLiteral("Symbol files (*.dbg *.lbl *.sym *.labels *.txt);;All files (*)"));
        if (path.isEmpty()) return;
    }
    char msg[512] = "";
    const QByteArray p = path.toLocal8Bit();
    a7800debug_load_symbols(m_dbg, fromFile ? p.constData() : nullptr, msg, sizeof msg);
    say(msg[0] ? stripControl(msg) : QStringLiteral("No symbols found"));
    refreshSymbols(true);
    resync();
}

void DebuggerWindow::clearSymbols()
{
    a7800debug_clear_symbols(m_dbg);
    say(QStringLiteral("Symbols cleared (the built-in ones stay)"));
    refreshSymbols(true);
    resync();
}

void DebuggerWindow::save(const char *kind, const QString &title, const QString &file)
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save %1").arg(title), file);
    if (path.isEmpty()) return;
    char msg[512] = "";
    a7800debug_save(m_dbg, kind, path.toLocal8Bit().constData(), msg, sizeof msg);
    say(msg[0] ? stripControl(msg) : QStringLiteral("Saved %1").arg(path));
}

/* ---- tabs in windows of their own ----------------------------------------- */

namespace {
/* A tab's window: closing it docks the tab again. */
class PopWindow : public QWidget {
public:
    std::function<void()> onClose;
protected:
    void closeEvent(QCloseEvent *e) override
    {
        if (onClose) onClose();
        QWidget::closeEvent(e);
    }
};
} // namespace

void DebuggerWindow::popOut(int tab)
{
    if (tab < 0 || tab >= TAB_COUNT || m_popped[tab] || m_tabs->count() <= 1) return;     /* one tab always stays */
    QWidget *page = m_pages[tab];
    m_tabs->removeTab(m_tabs->indexOf(page));

    auto *win = new PopWindow;
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->setObjectName(QStringLiteral("debugger-pop-%1").arg(QLatin1String(kTabs[tab].name)));
    win->setWindowTitle(QStringLiteral("Atari 7800 Debugger - %1").arg(QLatin1String(kTabs[tab].label)));
    win->setProperty("a7800HelpContext", QStringLiteral("debugger/%1").arg(QLatin1String(kTabs[tab].help)));
    auto *layout = new QVBoxLayout(win);
    auto *bar = new QHBoxLayout;
    auto *dockBtn = new QPushButton(QStringLiteral("Dock"));
    dockBtn->setToolTip(QStringLiteral("Put this tab back in the debugger"));
    bar->addWidget(dockBtn);
    bar->addStretch(1);
    bar->addWidget(a7800HelpButton(QString()));
    layout->addLayout(bar);
    layout->addWidget(page, 1);
    page->show();
    win->resize(760, 520);
    m_popped[tab] = win;
    static_cast<PopWindow *>(win)->onClose = [this, tab] {
        if (!m_closing) dock(tab);
    };
    connect(dockBtn, &QPushButton::clicked, win, &QWidget::close);
    /* the stepping keys work in the window too */
    new QShortcut(QKeySequence(Qt::Key_F1), win, [win] { a7800HelpFor(win); });
    new QShortcut(QKeySequence(Qt::Key_F5), win, [this] { toggleRun(); });
    new QShortcut(QKeySequence(Qt::Key_F7), win, [this] { stepAnd([this] { a7800debug_step(m_dbg); }); });
    new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F7), win, [this] { stepAnd([this] { a7800debug_frame(m_dbg); }); });
    new QShortcut(QKeySequence(Qt::Key_F8), win, [this] { stepAnd([this] { a7800debug_step_over(m_dbg); }); });
    new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F8), win, [this] { stepAnd([this] { a7800debug_step_out(m_dbg); }); });
    if (isVisible()) win->show();
    if (tab == TAB_VIDEO) m_videoForce = true;
    if (tab == TAB_HISTORY) m_hSeen[1] = -1;
    refreshPage(tab, false);
}

void DebuggerWindow::dock(int tab)
{
    QWidget *win = m_popped[tab];
    if (!win) return;
    QWidget *page = m_pages[tab];
    m_popped[tab] = nullptr;
    int pos = 0;
    for (int t = 0; t < tab; ++t)       /* back where it was in the order */
        if (!m_popped[t]) ++pos;
    page->setParent(nullptr);
    m_tabs->insertTab(pos, page, QString::fromUtf8(kTabs[tab].label).replace(QLatin1Char('&'), QLatin1String("&&")));
    m_tabs->setCurrentIndex(pos);
    if (tab == TAB_VIDEO) m_videoForce = true;
    if (tab == TAB_HISTORY) m_hSeen[1] = -1;
    refreshPage(tab, false);
}

/* ---- events --------------------------------------------------------------- */

bool DebuggerWindow::eventFilter(QObject *obj, QEvent *e)
{
    /* Tab completes; Up and Down walk the history */
    if (obj == m_conIn && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Tab) {
            completeConsole();
            return true;
        }
        if (ke->key() == Qt::Key_Up || ke->key() == Qt::Key_Down) {
            if (ke->key() == Qt::Key_Up && m_historyPos > 0) --m_historyPos;
            else if (ke->key() == Qt::Key_Down && m_historyPos < m_history.size()) ++m_historyPos;
            m_conIn->setText(m_historyPos < m_history.size() ? m_history.at(m_historyPos) : QString());
            return true;
        }
    }
    if (m_disasm && obj == m_disasm->viewport()) {
        /* A click selects a line (Run to Cursor); a click on its left edge,
         * or a double-click, toggles its breakpoint. A label has no line of
         * its own, so there is nothing else to hit. */
        if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonDblClick) {
            auto *me = static_cast<QMouseEvent *>(e);
            if (me->button() != Qt::LeftButton) return true;
            const QTextCursor cur = m_disasm->cursorForPosition(me->position().toPoint());
            const int line = cur.blockNumber();
            if (line < 0 || line >= (int)m_lineAddr.size()) return true;
            const bool gutter = cur.positionInBlock() <= 2;
            const bool dbl = e->type() == QEvent::MouseButtonDblClick;
            m_selected = m_lineAddr[(size_t)line];
            if (gutter ? !dbl : dbl) toggleBreakpoint(m_lineAddr[(size_t)line]);
            else refreshDisasm();
            return true;
        }
        if (e->type() == QEvent::MouseButtonRelease || e->type() == QEvent::MouseMove) return true;
        if (e->type() == QEvent::Wheel) {
            m_wheel += static_cast<QWheelEvent *>(e)->angleDelta().y();
            const int rows = -m_wheel / 40;     /* a notch is three rows */
            if (rows != 0) {
                m_wheel += rows * 40;
                m_followPc->setChecked(false);
                m_disasmTop = (uint16_t)a7800debug_row_address(m_dbg, m_disasmTop, rows);
                refreshDisasm();
            }
            return true;
        }
        if (e->type() == QEvent::Resize && isVisible()) refreshDisasm();
    }
    return QMainWindow::eventFilter(obj, e);
}

/* These keys are the debugger's here, taken as plain key presses ahead of
 * any application-wide shortcut. */
bool DebuggerWindow::event(QEvent *e)
{
    if (e->type() == QEvent::ShortcutOverride && isDebuggerKey(static_cast<QKeyEvent *>(e))) {
        e->accept();
        return true;
    }
    return QMainWindow::event(e);
}

void DebuggerWindow::keyPressEvent(QKeyEvent *e)
{
    if (!isDebuggerKey(e)) {
        QMainWindow::keyPressEvent(e);
        return;
    }
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    /* held, the steps repeat; the others do not */
    if (e->isAutoRepeat() && (e->key() == Qt::Key_F5 || e->key() == Qt::Key_F9 || e->key() == Qt::Key_F12)) return;
    switch (e->key()) {
    case Qt::Key_F5: toggleRun(); break;
    case Qt::Key_F7: stepAnd(shift ? std::function<void()>([this] { a7800debug_frame(m_dbg); })
                                   : std::function<void()>([this] { a7800debug_step(m_dbg); })); break;
    case Qt::Key_F8: stepAnd(shift ? std::function<void()>([this] { a7800debug_step_out(m_dbg); })
                                   : std::function<void()>([this] { a7800debug_step_over(m_dbg); })); break;
    case Qt::Key_F9: toggleSelected(); break;
    default: hide(); break;     /* F12 */
    }
}

/* Shown: the engine attaches, which stops the machine, as on every sibling.
 * The tabs have no size until the window is up, so the first refresh waits
 * for the event loop. A restore from the taskbar is not a show. */
void DebuggerWindow::showEvent(QShowEvent *e)
{
    QMainWindow::showEvent(e);
    m_timer.start(100);
    if (e->spontaneous()) return;
    for (QWidget *win : m_popped)
        if (win) win->show();
    a7800debug_attach(m_dbg);
    static bool historyOnce = true;     /* A7800_HISTORY: record from the first show */
    if (historyOnce && qEnvironmentVariableIsSet("A7800_HISTORY")) a7800debug_trace_enable(m_dbg, 1);
    historyOnce = false;
    m_wasStopped = false;
    QTimer::singleShot(0, this, [this] {
        if (!isVisible()) return;
        if (m_tabs->currentIndex() == TAB_CONSOLE) m_conIn->setFocus();
        settleAndSync();
    });
}

/* Hidden by F12, the close button or the main window: the engine goes away
 * and the machine runs on. The close button is a plain close, which only
 * hides the window (it is not deleted, so it comes back as it was);
 * ignoring the close instead would make QApplication's quit, which closes
 * every window first, give up whenever it reached this one. A minimised
 * window's spontaneous hide is not a detach. */
void DebuggerWindow::hideEvent(QHideEvent *e)
{
    m_timer.stop();
    if (!e->spontaneous()) {
        for (QWidget *win : m_popped)
            if (win) win->hide();
        a7800debug_detach(m_dbg);
    }
    QMainWindow::hideEvent(e);
}
