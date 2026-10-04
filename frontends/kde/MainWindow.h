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
    bool eventFilter(QObject *o, QEvent *e) override;

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
    void syncKeyboard();
    void tapeAction(int action);
    bool captureKey(QEvent *e);

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
    /* the plain-F-key shortcuts, cleared while keyboard mode owns the keys */
    QAction *m_controllersAction = nullptr;
    QAction *m_debuggerAction = nullptr;
    QAction *m_fullscreenAction = nullptr;
    QAction *m_kbdModeAction = nullptr;
    QMenu *m_tapeMenu = nullptr;
    QAction *m_tapeStopAction = nullptr;
    QLabel *m_kbd = nullptr;
    bool m_captured = false;
    unsigned m_padGen = 0;
    bool m_sysactDown[NES_SYSACT_COUNT] = {};
};
