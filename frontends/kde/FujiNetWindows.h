/*
 * The FujiNet console log window and the FujiNet configuration window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QWidget>

extern "C" {
#include "a7800session.h"
}

/* Shows (raising an existing one) the FujiNet console log window. */
void fujinet_log_show(QWidget *parent, a7800session *session);
/* Opens the FujiNet web UI in the system browser. Not embedded: the web UI's
 * Google and OneDrive Authorize buttons open the provider's consent page in a
 * new tab, and both providers refuse OAuth from an embedded web view. */
void fujinet_config_show(QWidget *parent, a7800session *session);

/* The accent colour, for highlights. */
QColor a7800AccentColor();
