/*
 * MainWindow -- see MainWindow.h.
 *
 * Plain Qt6 Widgets, deliberately not KDE Frameworks: it picks up Breeze
 * through the platform theme anyway, and staying framework-free keeps this
 * frontend usable outside a KDE session.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "MainWindow.h"

#include <QApplication>
#include <QDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFileDialog>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPixmap>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QUrl>

#include "DisplayWidget.h"
#include "FujiNetWindows.h"
#include "KeyForward.h"
#include "SettingsDialog.h"
#include "debugger/DebuggerWindow.h"
#include "controllers/ControllersWindow.h"

static const char *const kCartFilter =
    "Atari 7800 cartridges (*.a78 *.A78 *.bin *.BIN *.zip *.ZIP *.7z);;All files (*)";
static const char *const kRomFilter =
    "BIOS and High Score Cart images (*.bin *.rom *.u7 *.a78 *.zip *.7z);;All files (*)";

MainWindow::MainWindow(a7800session *session, QWidget *parent)
    : QMainWindow(parent), m_session(session)
{
    setWindowTitle(QStringLiteral("FujiNet Go Atari 7800"));
    /* 320x224 shown at 4:3, at 3x. */
    resize(896, 672 + 60);
    setAcceptDrops(true);

    m_display = new DisplayWidget(session, this);
    setCentralWidget(m_display);

    m_dot = new QLabel(QStringLiteral("●"));
    m_status = new QLabel(QStringLiteral("Starting..."));
    statusBar()->addWidget(m_dot);
    statusBar()->addWidget(m_status);
    m_console = new QLabel;
    m_console->setStyleSheet(QStringLiteral("color: gray;"));
    statusBar()->addPermanentWidget(m_console);

    buildMenus();
    applyPicture();

    connect(&m_statusTimer, &QTimer::timeout, this, &MainWindow::updateStatus);
    m_statusTimer.start(1000);
    updateStatus();
    /* The gamepad thread cannot call into Qt; it posts system actions and
     * this timer takes them. The difficulty switches follow the keyboard,
     * a gamepad or the Controllers window on the same beat. */
    connect(&m_sysactTimer, &QTimer::timeout, this, &MainWindow::drainSysactions);
    connect(&m_sysactTimer, &QTimer::timeout, this, &MainWindow::syncSwitches);
    m_sysactTimer.start(100);
    /* Gamepads come and go on their own thread; say so when they do. */
    m_padGen = a7800session_gamepad_generation(session);
    connect(&m_padTimer, &QTimer::timeout, this, &MainWindow::pollGamepads);
    m_padTimer.start(250);

    if (qEnvironmentVariableIsSet("A7800_OPEN_CONTROLLERS"))
        toggleControllers();
    /* once the session has started (main starts it after building this
     * window), so opening it stops a running machine */
    if (qEnvironmentVariableIsSet("A7800_OPEN_DEBUGGER"))
        QTimer::singleShot(0, this, [this] { DebuggerWindow::showFor(this, m_session); });
    if (qEnvironmentVariableIsSet("A7800_OPEN_SETTINGS"))
        QTimer::singleShot(0, this, &MainWindow::showSettings);
    /* For the smoke tests: picture every open window, then quit. */
    const QString shot = qEnvironmentVariable("A7800_SCREENSHOT");
    if (!shot.isEmpty()) {
        bool ok = false;
        int delay = qEnvironmentVariableIntValue("A7800_SCREENSHOT_DELAY_MS", &ok);
        if (!ok) delay = 3000;
        QTimer::singleShot(delay, this, [this, shot] { screenshotAndQuit(shot); });
    }
}

void MainWindow::buildMenus()
{
    QMenu *machine = menuBar()->addMenu(QStringLiteral("&Machine"));
    machine->addAction(QStringLiteral("&Open Cartridge..."), QKeySequence(Qt::CTRL | Qt::Key_O), this, [this] {
        const QString f = QFileDialog::getOpenFileName(
            this, QStringLiteral("Open Cartridge"), QString(), QString::fromUtf8(kCartFilter));
        if (!f.isEmpty()) openCart(f);
    });
    machine->addAction(QStringLiteral("&Eject Cartridge"), this, [this] {
        a7800session_eject(m_session);
        statusBar()->showMessage(QStringLiteral("Cartridge ejected — back to CONFIG"), 4000);
    });
    machine->addAction(QStringLiteral("&Import Cartridge to SD..."), this, [this] {
        const QString f = QFileDialog::getOpenFileName(
            this, QStringLiteral("Import Cartridge to SD"), QString(), QString::fromUtf8(kCartFilter));
        if (f.isEmpty()) return;
        char dest[1024];
        if (a7800session_import_cart_to_sd(m_session, f.toLocal8Bit().constData(), dest, sizeof dest) != 0)
            QMessageBox::warning(this, QStringLiteral("Import failed"),
                                 QString::fromUtf8(a7800session_last_error(m_session)));
        else
            statusBar()->showMessage(QStringLiteral("%1 is on the SD host — boot it from the CONFIG client")
                                     .arg(QFileInfo(QString::fromUtf8(dest)).fileName()), 6000);
    });
    machine->addAction(QStringLiteral("Import &BIOS..."), this, &MainWindow::importBios);
    machine->addSeparator();
    machine->addAction(QStringLiteral("&Power Cycle"), this, [this] {
        a7800session_power_cycle(m_session);
        statusBar()->showMessage(QStringLiteral("Power cycled"), 2000);
    });
    /* Escape is the binding (remappable); Ctrl+R is the menu's own. */
    machine->addAction(QStringLiteral("Reboot to &CONFIG"), QKeySequence(Qt::CTRL | Qt::Key_R), this,
                       [this] { runSysaction(A7800_SYSACT_REBOOT_CONFIG); });
    machine->addSeparator();
    machine->addAction(QStringLiteral("&Quit"), QKeySequence::Quit, this, [this] { close(); });

    /* F1-F3 are the switches' key bindings (remappable): shown, not claimed
     * as shortcuts, so one key press never acts twice. The difficulty
     * switches have no default binding; Alt+L / Alt+R are theirs. */
    QMenu *console = menuBar()->addMenu(QStringLiteral("&Console"));
    console->addAction(QStringLiteral("&Select\tF1"), this,
                       [this] { a7800session_switch_pulse(m_session, A7800_SW_SELECT); });
    console->addAction(QStringLiteral("&Reset\tF2"), this,
                       [this] { a7800session_switch_pulse(m_session, A7800_SW_RESET); });
    console->addAction(QStringLiteral("&Pause\tF3"), this,
                       [this] { a7800session_switch_pulse(m_session, A7800_SW_PAUSE); });
    console->addSeparator();
    for (int which = 0; which < 2; ++which) {
        const int sw = which ? A7800_SW_RIGHT_DIFF : A7800_SW_LEFT_DIFF;
        m_diffAction[which] = console->addAction(
            which ? QStringLiteral("R&ight Difficulty A") : QStringLiteral("&Left Difficulty A"),
            QKeySequence(Qt::ALT | (which ? Qt::Key_R : Qt::Key_L)));
        m_diffAction[which]->setCheckable(true);
        m_diffAction[which]->setToolTip(QStringLiteral("A (pro) when checked, B (novice) when not"));
        connect(m_diffAction[which], &QAction::triggered, this, [this, sw](bool on) {
            a7800session_switch_set(m_session, sw, on ? 1 : 0);
        });
    }
    syncSwitches();

    QMenu *view = menuBar()->addMenu(QStringLiteral("&View"));
    view->addAction(QStringLiteral("&Controllers"), QKeySequence(Qt::Key_F9), this,
                    &MainWindow::toggleControllers);
    view->addAction(QStringLiteral("&Debugger"), QKeySequence(Qt::Key_F12), this,
                    [this] { DebuggerWindow::toggleFor(this, m_session); });
    view->addSeparator();
    m_tvAction = view->addAction(QStringLiteral("&TV Aspect (4:3)"));
    m_tvAction->setCheckable(true);
    connect(m_tvAction, &QAction::triggered, this, [this](bool on) {
        a7800session_set_int(m_session, "aspect", on ? 0 : 1);
        applyPicture();
    });
    m_smoothAction = view->addAction(QStringLiteral("&Smooth Scaling"));
    m_smoothAction->setCheckable(true);
    connect(m_smoothAction, &QAction::triggered, this, [this](bool on) {
        a7800session_set_int(m_session, "smooth", on ? 1 : 0);
        applyPicture();
    });
    view->addAction(QStringLiteral("&Fullscreen"), QKeySequence(Qt::Key_F11), this, [this] {
        if (isFullScreen()) showNormal(); else showFullScreen();
    });

    QMenu *fuji = menuBar()->addMenu(QStringLiteral("&FujiNet"));
    fuji->addAction(QStringLiteral("FujiNet &Web UI"), this, [this] {
        if (!a7800session_fujinet_running(m_session)) {
            statusBar()->showMessage(QStringLiteral("FujiNet is not running"), 4000);
            return;
        }
        fujinet_config_show(this, m_session);
    });
    fuji->addAction(QStringLiteral("Console &Log"), this, [this] { fujinet_log_show(this, m_session); });

    QMenu *settings = menuBar()->addMenu(QStringLiteral("&Settings"));
    settings->addAction(QStringLiteral("&Preferences..."), QKeySequence(Qt::CTRL | Qt::Key_Comma), this,
                        &MainWindow::showSettings);

    QMenu *help = menuBar()->addMenu(QStringLiteral("&Help"));
    help->addAction(QStringLiteral("&About FujiNet Go Atari 7800"), this, &MainWindow::showAbout);
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, QStringLiteral("About FujiNet Go Atari 7800"),
        QStringLiteral("<b>FujiNet Go Atari 7800</b> %1<br><br>"
                       "An Atari 7800 with a built-in FujiNet.<br>"
                       "The emulator is MAME's Atari 7800 (GPL-2.0-or-later; its Atari 7800 "
                       "driver BSD-3-Clause), from the MAME team, with the FujiNet Atari 7800 "
                       "cartridge. No BIOS is needed or included.<br><br>"
                       "Copyright © 2026 Thomas Cherryhomes — GPL-3.0-or-later<br>"
                       "<a href=\"https://fujinet.online/\">fujinet.online</a>")
            .arg(QStringLiteral(A7800_VERSION_STRING)));
}

void MainWindow::showSettings()
{
    const bool restart = SettingsDialog::run(this, m_session);
    applyPicture();
    updateStatus();
    if (restart) restartSession();
}

void MainWindow::applyPicture()
{
    const int aspect = a7800session_get_int(m_session, "aspect", 0);
    const bool smooth = a7800session_get_int(m_session, "smooth", 0) != 0;
    m_display->setAspect(aspect);
    m_display->setSmooth(smooth);
    if (m_tvAction) m_tvAction->setChecked(aspect == 0);
    if (m_smoothAction) m_smoothAction->setChecked(smooth);
}

/* The difficulty switches' check marks follow whoever flipped them. */
void MainWindow::syncSwitches()
{
    for (int which = 0; which < 2; ++which) {
        if (!m_diffAction[which]) continue;
        const int on = a7800session_switch_get(m_session, which ? A7800_SW_RIGHT_DIFF : A7800_SW_LEFT_DIFF);
        if (m_diffAction[which]->isChecked() != (on != 0)) {
            QSignalBlocker block(m_diffAction[which]);
            m_diffAction[which]->setChecked(on != 0);
        }
    }
}

void MainWindow::pollGamepads()
{
    const unsigned gen = a7800session_gamepad_generation(m_session);
    if (gen == m_padGen) return;
    m_padGen = gen;
    char msg[160];
    if (a7800session_gamepad_last_event(m_session, msg, sizeof msg) > 0)
        statusBar()->showMessage(QString::fromUtf8(msg), 4000);
}

void MainWindow::restartSession()
{
    a7800session_start_opts o;
    a7800session_settings_flush(m_session);
    a7800session_default_opts(m_session, &o);
    a7800session_stop(m_session);
    if (a7800session_start(m_session, &o) != 0) {
        QMessageBox::warning(this, QStringLiteral("Restart failed"),
                             QString::fromUtf8(a7800session_last_error(m_session)));
        return;
    }
    statusBar()->showMessage(QStringLiteral("Host options applied (session restarted)"), 5000);
}

void MainWindow::toggleControllers()
{
    if (!m_controllers) m_controllers = new ControllersWindow(m_session, this);
    if (m_controllers->isVisible()) m_controllers->hide();
    else m_controllers->show();
}

void MainWindow::runSysaction(int sa)
{
    switch (sa) {
    case A7800_SYSACT_REBOOT_CONFIG:
        a7800session_reboot_to_config(m_session);
        statusBar()->showMessage(QStringLiteral("Back to the FujiNet CONFIG client"), 4000);
        break;
    case A7800_SYSACT_PAUSE:
        DebuggerWindow::showFor(this, m_session);
        break;
    default: break;
    }
}

void MainWindow::drainSysactions()
{
    int sa;
    while (a7800session_sysaction_take(m_session, &sa)) runSysaction(sa);
}

void MainWindow::updateStatus()
{
    QString text;
    bool on = false;
    const QString cart = QString::fromUtf8(a7800session_cart_path(m_session));
    if (!a7800session_is_running(m_session)) {
        text = QStringLiteral("Stopped");
        m_console->clear();
    } else {
        char st[160];
        a7800session_cart_status(m_session, st, sizeof st);
        on = a7800session_cart_link_up(m_session) == 1;
        text = QStringLiteral("FujiNet: %1").arg(QString::fromUtf8(st));
        if (!cart.isEmpty())
            text = QStringLiteral("%1 — %2").arg(QFileInfo(cart).fileName(), text);
        else if (!a7800session_cart_booted_game(m_session))
            text = QStringLiteral("CONFIG — %1").arg(text);
        /* the console actually running, and whether it has its BIOS */
        const int region = a7800session_running_region(m_session);
        const int bios = a7800session_bios(m_session, region);
        const bool hasBios = bios >= 0 && a7800session_bios_available(m_session, bios);
        m_console->setText(QStringLiteral("%1 · %2").arg(
            region == A7800_REGION_PAL ? QStringLiteral("PAL") : QStringLiteral("NTSC"),
            hasBios ? QStringLiteral("BIOS") : QStringLiteral("no BIOS")));
    }
    m_status->setText(text);
    m_dot->setStyleSheet(on ? QStringLiteral("color: %1;").arg(a7800AccentColor().name())
                            : QStringLiteral("color: gray;"));
}

void MainWindow::openCart(const QString &path)
{
    if (a7800session_load_cart(m_session, path.toLocal8Bit().constData()) != 0) {
        QMessageBox::warning(this, QStringLiteral("Could not open"),
                             QString::fromUtf8(a7800session_last_error(m_session)));
        return;
    }
    statusBar()->showMessage(QStringLiteral("Running %1").arg(QFileInfo(path).fileName()), 4000);
    updateStatus();
}

void MainWindow::importBios()
{
    const QString f = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import BIOS"), QString(), QString::fromUtf8(kRomFilter));
    if (f.isEmpty()) return;
    char what[128];
    if (a7800session_import_rom(m_session, f.toLocal8Bit().constData(), what, sizeof what) != 0) {
        QMessageBox::warning(this, QStringLiteral("Import failed"),
                             QString::fromUtf8(a7800session_last_error(m_session)));
        return;
    }
    statusBar()->showMessage(QStringLiteral("Imported the %1 — choose it in Preferences")
                             .arg(QString::fromUtf8(what)), 6000);
}

/* A dropped file: a cartridge runs, a BIOS or the High Score Cart's ROM is
 * imported, anything else goes to FujiNet's SD folder. */
void MainWindow::loadMedia(const QString &path)
{
    const QByteArray src = path.toLocal8Bit();
    const bool rom = a7800session_media_is_rom(src.constData()) != 0;
    const bool cart = !rom && a7800session_media_is_cartridge(src.constData());
    char dest[1024];
    if (a7800session_import_media(m_session, src.constData(), dest, sizeof dest) != 0) {
        QMessageBox::warning(this, QStringLiteral("Import failed"),
                             QString::fromUtf8(a7800session_last_error(m_session)));
        return;
    }
    if (cart)
        openCart(QString::fromLocal8Bit(dest));
    else if (rom)
        statusBar()->showMessage(QStringLiteral("Imported %1 — choose it in Preferences")
                                 .arg(QFileInfo(path).fileName()), 6000);
    else
        statusBar()->showMessage(QStringLiteral("Copied to FujiNet's SD folder — mount it from the CONFIG client"), 6000);
}

void MainWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat()) return;
    if (e->key() == Qt::Key_F9) { toggleControllers(); return; }
    if (e->key() == Qt::Key_F12) { DebuggerWindow::toggleFor(this, m_session); return; }
    if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) { QMainWindow::keyPressEvent(e); return; }

    const uint32_t ks = a7800KeysymFromQt(e);
    if (!ks) { QMainWindow::keyPressEvent(e); return; }

    const int sa = a7800session_key_sysaction(m_session, ks);
    if (sa >= 0) {
        if (!m_sysactDown[sa]) { m_sysactDown[sa] = true; runSysaction(sa); }
        return;
    }
    if (!a7800session_key(m_session, ks, 1)) QMainWindow::keyPressEvent(e);
}

void MainWindow::keyReleaseEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat()) return;
    const uint32_t ks = a7800KeysymFromQt(e);
    if (!ks) { QMainWindow::keyReleaseEvent(e); return; }
    const int sa = a7800session_key_sysaction(m_session, ks);
    if (sa >= 0) { m_sysactDown[sa] = false; return; }
    if (!a7800session_key(m_session, ks, 0)) QMainWindow::keyReleaseEvent(e);
}

bool MainWindow::event(QEvent *e)
{
    if (e->type() == QEvent::WindowDeactivate) {
        a7800session_release_all(m_session);
        for (bool &d : m_sysactDown) d = false;
    }
    return QMainWindow::event(e);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    const QList<QUrl> urls = e->mimeData()->urls();
    if (urls.isEmpty()) return;
    const QString path = urls.first().toLocalFile();
    if (!path.isEmpty()) loadMedia(path);
}

/* A7800_SCREENSHOT=<file.png>: this window to <file.png>, every other open
 * window of the app to <file>-<name>.png, then quit. The app's own widgets
 * are grabbed, never the screen. */
void MainWindow::screenshotAndQuit(const QString &path)
{
    grab().save(path);
    const QString base = path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive) ? path.chopped(4) : path;
    int n = 0;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (w == this || !w->isVisible()) continue;
        const QString name = w->objectName().isEmpty() ? QStringLiteral("window%1").arg(++n) : w->objectName();
        w->grab().save(QStringLiteral("%1-%2.png").arg(base, name));
    }
    if (auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget())) modal->reject();
    QTimer::singleShot(0, qApp, &QCoreApplication::quit);
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    m_statusTimer.stop();
    m_sysactTimer.stop();
    m_padTimer.stop();
    QMainWindow::closeEvent(e);
}
