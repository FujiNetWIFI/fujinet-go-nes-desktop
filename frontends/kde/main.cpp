/*
 * FujiNet Go NES -- the KDE (Qt6 Widgets) frontend.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QIcon>

#include "MainWindow.h"
#include "nessession.h"

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("fujinet-go-nes-kde"));
    app.setApplicationDisplayName(QStringLiteral("FujiNet Go NES"));
    app.setDesktopFileName(QStringLiteral("online.fujinet.go.nes.kde"));
    /* The installed icon is named after the desktop-entry id; running out
     * of the build tree, the in-tree artwork stands in. */
    {
        QIcon icon = QIcon::fromTheme(QStringLiteral("online.fujinet.go.nes.kde"));
        if (icon.isNull()) {
            QIcon::setThemeSearchPaths(QIcon::themeSearchPaths()
                                       << QStringLiteral(NES_SOURCE_ICON_DIR));
            icon = QIcon::fromTheme(QStringLiteral("fujinet-go-nes"));
        }
        if (!icon.isNull()) app.setWindowIcon(icon);
    }

    nessession *session = nessession_new(nullptr);
    if (!session) {
        qCritical("Could not create the session (unusable config/data dirs?)");
        return 1;
    }

    MainWindow win(session);

    nessession_start_opts opts;
    nessession_default_opts(session, &opts);
    if (argc > 1) opts.cart_path = argv[1];

    if (nessession_start(session, &opts) != 0)
        qWarning("%s", nessession_last_error(session));
    win.show();

    const int rc = app.exec();
    nessession_stop(session);
    nessession_free(session);
    return rc;
}
