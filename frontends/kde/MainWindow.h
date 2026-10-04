/*
 * The main window.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QLabel>
#include <QMainWindow>
#include <QTimer>

#include "nessession.h"

class DisplayWidget;
class ControllersWindow;
class QAction;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(nessession *session, QWidget *parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    bool event(QEvent *e) override;

private:
    void buildMenus();
    void updateStatus();
    void drainSysactions();
    void runSysaction(int sa);
    void openCart(const QString &path);
    void loadMedia(const QString &path);
    void toggleControllers();
    void pollGamepads();
    void applyPicture();
    void showSettings();
    void restartSession();
    void showAbout();

    nessession *m_session;
    DisplayWidget *m_display = nullptr;
    ControllersWindow *m_controllers = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_dot = nullptr;
    QTimer m_statusTimer;
    QTimer m_sysactTimer;
    QTimer m_padTimer;
    QAction *m_squareAction = nullptr;
    QAction *m_smoothAction = nullptr;
    unsigned m_padGen = 0;
    bool m_sysactDown[NES_SYSACT_COUNT] = {};
};
