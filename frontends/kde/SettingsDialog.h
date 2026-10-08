/*
 * SettingsDialog -- the Preferences dialog. Controller types, the analog
 * stick switch, the picture and the volume apply live; the console (TV
 * system, BIOS, High Score Cart) is rebuilt when OK is pressed; the host
 * options need a restart, which the caller does when run() says one of
 * those changed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QDialog>

extern "C" {
#include "a7800session.h"
}

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    /* Returns true if a restart option changed. */
    static bool run(QWidget *parent, a7800session *session);
};
