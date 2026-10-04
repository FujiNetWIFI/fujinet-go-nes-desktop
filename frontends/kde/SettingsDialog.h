/*
 * SettingsDialog -- the Preferences dialog. Controller types, the analog
 * stick switch, the picture and the volume apply live; the region and the
 * host options need a restart, which the caller does when run() says one of
 * those changed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QDialog>

extern "C" {
#include "nessession.h"
}

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    /* Returns true if a restart option changed. */
    static bool run(QWidget *parent, nessession *session);
};
