/*
 * SettingsDialog -- see SettingsDialog.h.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSlider>
#include <QStandardItemModel>
#include <QTimer>
#include <QVBoxLayout>
#include <vector>

namespace {

QCheckBox *check(const char *text, const char *tip, a7800session *s, const char *key, int def)
{
    auto *b = new QCheckBox(QString::fromUtf8(text));
    b->setToolTip(QString::fromUtf8(tip));
    b->setChecked(a7800session_get_int(s, key, def) != 0);
    return b;
}

bool commit(a7800session *s, const char *key, int def, int value)
{
    if (a7800session_get_int(s, key, def) == value) return false;
    a7800session_set_int(s, key, value);
    return true;
}

/* A console's BIOS choices: None, then each of MAME's BIOSes for that
 * console, the ones not imported greyed out. The item data is the BIOS
 * index (-1 for none). */
QComboBox *biosBox(a7800session *s, int region)
{
    auto *box = new QComboBox;
    box->addItem(QStringLiteral("None (start the cartridge directly)"), -1);
    for (int i = 0; i < a7800session_bios_count(); ++i) {
        const a7800_bios_info *b = a7800session_bios_info(i);
        if (b->region != region) continue;
        const bool have = a7800session_bios_available(s, i) != 0;
        box->addItem(have ? QString::fromUtf8(b->desc)
                          : QStringLiteral("%1 (not imported)").arg(QString::fromUtf8(b->desc)), i);
        if (!have) {
            auto *model = qobject_cast<QStandardItemModel *>(box->model());
            if (model) model->item(box->count() - 1)->setEnabled(false);
        }
    }
    const int cur = box->findData(a7800session_bios(s, region));
    box->setCurrentIndex(cur >= 0 ? cur : 0);
    return box;
}

/* "Auto" says what it resolved to for the running game. */
void labelAuto(QComboBox *box, a7800session *s, int port)
{
    const char *now = a7800_ctrl_type_name(a7800session_port_detected(s, port));
    box->setItemText(A7800_CTRL_AUTO, QStringLiteral("Auto (now: %1)").arg(QString::fromUtf8(now ? now : "?")));
}

} // namespace

bool SettingsDialog::run(QWidget *parent, a7800session *session)
{
    QDialog dlg(parent);
    dlg.setObjectName(QStringLiteral("prefs"));
    dlg.setWindowTitle(QStringLiteral("Preferences"));
    dlg.setMinimumWidth(560);
    auto *outer = new QVBoxLayout(&dlg);

    /* The console (a new one when OK is pressed) */
    auto *machine = new QGroupBox(QStringLiteral("Console (a change starts a new console when you press OK)"));
    auto *mform = new QFormLayout(machine);
    auto *region = new QComboBox;
    for (int i = 0; a7800_region_name(i); ++i) region->addItem(QString::fromUtf8(a7800_region_name(i)));
    region->setCurrentIndex(a7800session_region(session));
    region->setToolTip(QStringLiteral("Auto follows each cartridge's header (NTSC unless it says PAL)"));
    mform->addRow(QStringLiteral("TV system"), region);
    QComboBox *bios[2] = { biosBox(session, A7800_REGION_NTSC), biosBox(session, A7800_REGION_PAL) };
    mform->addRow(QStringLiteral("BIOS (NTSC console)"), bios[0]);
    mform->addRow(QStringLiteral("BIOS (PAL console)"), bios[1]);
    auto *note = new QLabel(QStringLiteral("No BIOS is needed. Machine ▸ Import BIOS... adds your own."));
    note->setToolTip(QStringLiteral("With a BIOS chosen, the console shows the Atari logo and starts "
                                    "the games it accepts itself"));
    note->setStyleSheet(QStringLiteral("color: gray;"));
    mform->addRow(note);
    auto *hsc = new QCheckBox(QStringLiteral("High Score Cart"));
    hsc->setChecked(a7800session_hsc(session) != 0);
    if (a7800session_hsc_available(session)) {
        hsc->setToolTip(QStringLiteral("Games that support it keep their high scores (saved through FujiNet)"));
    } else {
        hsc->setEnabled(false);
        hsc->setToolTip(QStringLiteral("Import the High Score Cart's ROM first (Machine ▸ Import BIOS...)"));
    }
    mform->addRow(hsc);
    outer->addWidget(machine);

    /* Picture (live) */
    auto *picture = new QGroupBox(QStringLiteral("Picture"));
    auto *picForm = new QFormLayout(picture);
    auto *aspect = new QComboBox;
    aspect->addItems({ QStringLiteral("TV (a 4:3 picture)"), QStringLiteral("Square pixels") });
    aspect->setCurrentIndex(a7800session_get_int(session, "aspect", 0) ? 1 : 0);
    picForm->addRow(QStringLiteral("Aspect"), aspect);
    QCheckBox *smooth = check("Smooth scaling", "Filter the picture when scaling it; off keeps the pixels sharp", session, "smooth", 0);
    picForm->addRow(smooth);
    QObject::connect(aspect, &QComboBox::currentIndexChanged, &dlg,
                     [=](int idx) { a7800session_set_int(session, "aspect", idx); });
    QObject::connect(smooth, &QCheckBox::toggled, &dlg,
                     [=](bool on) { a7800session_set_int(session, "smooth", on ? 1 : 0); });
    outer->addWidget(picture);

    /* Controllers (live) */
    auto *ports = new QGroupBox(QStringLiteral("Controllers (applied immediately, even to a game booted over FujiNet)"));
    auto *pform = new QFormLayout(ports);
    for (int port = 0; port < 2; ++port) {
        auto *box = new QComboBox;
        for (int i = 0; a7800_ctrl_type_name(i); ++i)
            box->addItem(QString::fromUtf8(a7800_ctrl_type_name(i)));
        labelAuto(box, session, port);
        box->setCurrentIndex(a7800session_port_type(session, port));
        box->setToolTip(QStringLiteral("Auto plugs in what the running game wants: the light gun for the "
                                       "light-gun titles, a ProLine joystick otherwise"));
        pform->addRow(port ? QStringLiteral("Player 2") : QStringLiteral("Player 1"), box);
        QObject::connect(box, &QComboBox::currentIndexChanged, &dlg, [=](int idx) {
            a7800session_set_port_type(session, port, idx);
        });
    }
    QCheckBox *aj = check("Analog sticks drive the joystick", "A gamepad's left stick moves the joystick, as well as its D-pad", session, "analog_joystick", 1);
    QObject::connect(aj, &QCheckBox::toggled, &dlg, [=](bool on) { a7800session_set_analog(session, on ? 1 : 0); });
    pform->addRow(aj);
    outer->addWidget(ports);

    /* Gamepads: one row per pad, refreshed as they come and go */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads (assigned to players in connection order unless chosen here)"));
    auto *padForm = new QFormLayout(pads);
    std::vector<QWidget *> padRows;
    unsigned seenGen = a7800session_gamepad_generation(session) + 1;
    auto rebuildPads = [&, padForm, session]() mutable {
        for (QWidget *w : padRows) { padForm->removeRow(w); }
        padRows.clear();
        const int n = a7800session_gamepad_count(session);
        if (n == 0) {
            auto *l = new QLabel(QStringLiteral("No gamepads connected — plug one in, it is picked up as it appears"));
            padForm->addRow(l);
            padRows.push_back(l);
            return;
        }
        for (int i = 0; i < n && i < 8; ++i) {
            char name[128];
            a7800session_gamepad_name(session, i, name, sizeof name);
            auto *box = new QComboBox;
            box->addItems({ QStringLiteral("Automatic"), QStringLiteral("Player 1"), QStringLiteral("Player 2") });
            box->setCurrentIndex(a7800session_gamepad_assignment(session, i) + 1);
            const int eff = a7800session_gamepad_effective_port(session, i);
            box->setToolTip(eff >= 0 ? QStringLiteral("Driving player %1").arg(eff + 1)
                                     : QStringLiteral("Driving no player"));
            QObject::connect(box, &QComboBox::currentIndexChanged, box, [=](int idx) {
                a7800session_gamepad_assign(session, i, idx - 1);
            });
            padForm->addRow(QString::fromUtf8(name), box);
            padRows.push_back(box);
        }
    };
    auto *padTimer = new QTimer(&dlg);
    QObject::connect(padTimer, &QTimer::timeout, &dlg, [&, session]() mutable {
        const unsigned gen = a7800session_gamepad_generation(session);
        if (gen != seenGen) { seenGen = gen; rebuildPads(); }
    });
    padTimer->start(1000);
    seenGen = a7800session_gamepad_generation(session);
    rebuildPads();
    outer->addWidget(pads);

    /* Audio (live) */
    auto *audio = new QGroupBox(QStringLiteral("Audio"));
    auto *audioForm = new QFormLayout(audio);
    auto *volume = new QSlider(Qt::Horizontal);
    volume->setRange(0, 100);
    volume->setValue(a7800session_get_int(session, "volume", 100));
    QObject::connect(volume, &QSlider::valueChanged, &dlg, [=](int v) { a7800session_set_volume(session, v); });
    audioForm->addRow(QStringLiteral("Volume"), volume);
    outer->addWidget(audio);

    /* Host (restart) */
    auto *host = new QGroupBox(QStringLiteral("Host (applied by restarting the session)"));
    auto *hform = new QVBoxLayout(host);
    QCheckBox *fuji = check("Enable FujiNet", "Run the in-process FujiNet the cartridge dials into. Off means no network and a link-down CONFIG client.", session, "enable_fujinet", 1);
    QCheckBox *snd = check("Audio", "Open the system audio device", session, "enable_audio", 1);
    QCheckBox *gp = check("Gamepads", "Poll USB/Bluetooth gamepads", session, "enable_gamepad", 1);
    hform->addWidget(fuji); hform->addWidget(snd); hform->addWidget(gp);
    outer->addWidget(host);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    outer->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return false;

    /* The console: each of these is a new console, so only what changed */
    if (region->currentIndex() != a7800session_region(session))
        a7800session_set_region(session, region->currentIndex());
    const int regions[2] = { A7800_REGION_NTSC, A7800_REGION_PAL };
    for (int i = 0; i < 2; ++i) {
        const int want = bios[i]->currentData().toInt();
        if (want != a7800session_bios(session, regions[i]))
            a7800session_set_bios(session, regions[i], want);
    }
    if (hsc->isEnabled() && hsc->isChecked() != (a7800session_hsc(session) != 0))
        a7800session_set_hsc(session, hsc->isChecked() ? 1 : 0);

    bool changed = false;
    changed |= commit(session, "enable_fujinet", 1, fuji->isChecked() ? 1 : 0);
    changed |= commit(session, "enable_audio", 1, snd->isChecked() ? 1 : 0);
    changed |= commit(session, "enable_gamepad", 1, gp->isChecked() ? 1 : 0);
    return changed;
}
