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
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFileDialog>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
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
    "NES cartridges (*.nes *.NES);;All files (*)";

MainWindow::MainWindow(nessession *session, QWidget *parent)
    : QMainWindow(parent), m_session(session)
{
    setWindowTitle(QStringLiteral("FujiNet Go NES"));
    /* 292x240 (8:7 pixels) at 3x. */
    resize(876, 720 + 60);
    setAcceptDrops(true);

    m_display = new DisplayWidget(session, this);
    setCentralWidget(m_display);

    m_dot = new QLabel(QStringLiteral("●"));
    m_status = new QLabel(QStringLiteral("Starting..."));
    statusBar()->addWidget(m_dot);
    statusBar()->addWidget(m_status);
    m_kbd = new QLabel;
    statusBar()->addPermanentWidget(m_kbd);
    m_display->installEventFilter(this);

    buildMenus();
    applyPicture();

    connect(&m_statusTimer, &QTimer::timeout, this, &MainWindow::updateStatus);
    m_statusTimer.start(1000);
    updateStatus();
    /* The gamepad thread cannot call into Qt; it posts system actions and
     * this timer takes them. */
    connect(&m_sysactTimer, &QTimer::timeout, this, &MainWindow::drainSysactions);
    m_sysactTimer.start(100);
    /* keyboard mode flips inside nessession_key (Scroll Lock) or from a
     * settings change; follow it on the same beat */
    connect(&m_sysactTimer, &QTimer::timeout, this, &MainWindow::syncKeyboard);
    syncKeyboard();
    /* Gamepads come and go on their own thread; say so when they do. */
    m_padGen = nessession_gamepad_generation(session);
    connect(&m_padTimer, &QTimer::timeout, this, &MainWindow::pollGamepads);
    m_padTimer.start(250);

    if (qEnvironmentVariableIsSet("NES_OPEN_CONTROLLERS"))
        toggleControllers();
    if (qEnvironmentVariableIsSet("NES_OPEN_DEBUGGER"))
        DebuggerWindow::toggleFor(this, session);
    if (qEnvironmentVariableIsSet("NES_OPEN_KEYBOARD")) {
        if (!m_controllers) m_controllers = new ControllersWindow(m_session, this);
        m_controllers->showKeyboard();
    }
    if (qEnvironmentVariableIsSet("NES_OPEN_SETTINGS"))
        QTimer::singleShot(0, this, &MainWindow::showSettings);
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
        nessession_eject(m_session);
        statusBar()->showMessage(QStringLiteral("Cartridge ejected — back to CONFIG"), 4000);
    });
    machine->addAction(QStringLiteral("&Import Cartridge to SD..."), this, [this] {
        const QString f = QFileDialog::getOpenFileName(
            this, QStringLiteral("Import Cartridge to SD"), QString(), QString::fromUtf8(kCartFilter));
        if (f.isEmpty()) return;
        char dest[1024];
        if (nessession_import_cart_to_sd(m_session, f.toLocal8Bit().constData(), dest, sizeof dest) != 0)
            QMessageBox::warning(this, QStringLiteral("Import failed"),
                                 QString::fromUtf8(nessession_last_error(m_session)));
        else
            statusBar()->showMessage(QStringLiteral("%1 is on the SD host — boot it from the CONFIG client")
                                     .arg(QFileInfo(QString::fromUtf8(dest)).fileName()), 6000);
    });
    machine->addSeparator();
    /* Backspace is the console's RESET binding (remappable); shown, not
     * claimed as a shortcut, so the binding stays the one that acts. */
    machine->addAction(QStringLiteral("Reset &Game\tBackspace"), this, [this] {
        nessession_reset_game(m_session);
        statusBar()->showMessage(QStringLiteral("Reset"), 2000);
    });
    machine->addAction(QStringLiteral("Reset to &CONFIG"), QKeySequence(Qt::CTRL | Qt::Key_R), this,
                       [this] { runSysaction(NES_SYSACT_RESET_CONFIG); });
    machine->addSeparator();
    /* Scroll Lock is handled by the core inside nessession_key; shown as
     * the hint only */
    m_kbdModeAction = machine->addAction(QStringLiteral("&Keyboard Mode\tScroll Lock"));
    m_kbdModeAction->setCheckable(true);
    m_kbdModeAction->setToolTip(QStringLiteral("Typed keys go to the expansion-port keyboard"));
    connect(m_kbdModeAction, &QAction::triggered, this, [this](bool on) {
        nessession_set_keyboard_mode(m_session, on ? 1 : 0);
        syncKeyboard();
    });
    m_tapeMenu = machine->addMenu(QStringLiteral("&Data Recorder"));
    m_tapeMenu->addAction(QStringLiteral("&Play Tape..."), this, [this] { tapeAction(NES_TAPE_PLAYING); });
    m_tapeMenu->addAction(QStringLiteral("&Record Tape..."), this, [this] { tapeAction(NES_TAPE_RECORDING); });
    m_tapeStopAction = m_tapeMenu->addAction(QStringLiteral("&Stop"), this, [this] { tapeAction(NES_TAPE_IDLE); });
    machine->addSeparator();
    machine->addAction(QStringLiteral("&Preferences..."), QKeySequence(Qt::CTRL | Qt::Key_Comma), this,
                       &MainWindow::showSettings);
    machine->addSeparator();
    machine->addAction(QStringLiteral("&Quit"), QKeySequence::Quit, this, [this] { close(); });

    QMenu *view = menuBar()->addMenu(QStringLiteral("&View"));
    m_controllersAction = view->addAction(QStringLiteral("&Controllers"), QKeySequence(Qt::Key_F9), this,
                                          &MainWindow::toggleControllers);
    m_debuggerAction = view->addAction(QStringLiteral("&Debugger"), QKeySequence(Qt::Key_F12), this,
                    [this] { DebuggerWindow::toggleFor(this, m_session); });
    view->addSeparator();
    m_squareAction = view->addAction(QStringLiteral("S&quare Pixels"));
    m_squareAction->setCheckable(true);
    connect(m_squareAction, &QAction::triggered, this, [this](bool on) {
        nessession_set_int(m_session, "aspect", on ? 1 : 0);
        applyPicture();
    });
    m_smoothAction = view->addAction(QStringLiteral("&Smooth Scaling"));
    m_smoothAction->setCheckable(true);
    connect(m_smoothAction, &QAction::triggered, this, [this](bool on) {
        nessession_set_int(m_session, "smooth", on ? 1 : 0);
        applyPicture();
    });
    m_fullscreenAction = view->addAction(QStringLiteral("&Fullscreen"), QKeySequence(Qt::Key_F11), this, [this] {
        if (isFullScreen()) showNormal(); else showFullScreen();
    });

    QMenu *fuji = menuBar()->addMenu(QStringLiteral("&FujiNet"));
    fuji->addAction(QStringLiteral("&Web UI"), this, [this] {
        if (!nessession_fujinet_running(m_session)) {
            statusBar()->showMessage(QStringLiteral("FujiNet is not running"), 4000);
            return;
        }
        fujinet_config_show(this, m_session);
    });
    fuji->addAction(QStringLiteral("Console &Log"), this, [this] { fujinet_log_show(this, m_session); });

    QMenu *help = menuBar()->addMenu(QStringLiteral("&Help"));
    help->addAction(QStringLiteral("&About FujiNet Go NES"), this, &MainWindow::showAbout);
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, QStringLiteral("About FujiNet Go NES"),
        QStringLiteral("<b>FujiNet Go NES</b> %1<br><br>"
                       "An NES with a built-in FujiNet.<br>"
                       "The emulator is MesenCE (GPL-3.0-or-later), from Mesen by Sour "
                       "and its contributors, with the FujiNet NES cartridge.<br><br>"
                       "Copyright © 2026 Thomas Cherryhomes — GPL-3.0-or-later<br>"
                       "<a href=\"https://fujinet.online/\">fujinet.online</a>")
            .arg(QStringLiteral(NES_VERSION_STRING)));
}

void MainWindow::showSettings()
{
    const bool restart = SettingsDialog::run(this, m_session);
    applyPicture();
    if (restart) restartSession();
}

void MainWindow::applyPicture()
{
    const int aspect = nessession_get_int(m_session, "aspect", 0);
    const bool smooth = nessession_get_int(m_session, "smooth", 0) != 0;
    m_display->setAspect(aspect);
    m_display->setSmooth(smooth);
    if (m_squareAction) m_squareAction->setChecked(aspect != 0);
    if (m_smoothAction) m_smoothAction->setChecked(smooth);
}

void MainWindow::pollGamepads()
{
    const unsigned gen = nessession_gamepad_generation(m_session);
    if (gen == m_padGen) return;
    m_padGen = gen;
    char msg[160];
    if (nessession_gamepad_last_event(m_session, msg, sizeof msg) > 0)
        statusBar()->showMessage(QString::fromUtf8(msg), 4000);
}

void MainWindow::restartSession()
{
    nessession_start_opts o;
    nessession_settings_flush(m_session);
    nessession_default_opts(m_session, &o);
    nessession_stop(m_session);
    if (nessession_start(m_session, &o) != 0) {
        QMessageBox::warning(this, QStringLiteral("Restart failed"),
                             QString::fromUtf8(nessession_last_error(m_session)));
        return;
    }
    statusBar()->showMessage(QStringLiteral("Machine options applied (session restarted)"), 5000);
}

void MainWindow::toggleControllers()
{
    if (!m_controllers) m_controllers = new ControllersWindow(m_session, this);
    if (m_controllers->isVisible()) m_controllers->hide();
    else m_controllers->show();
}

void MainWindow::syncKeyboard()
{
    const int kbd = nessession_keyboard(m_session);
    const bool captured = nessession_keyboard_captures(m_session);
    {
        QSignalBlocker block(m_kbdModeAction);
        m_kbdModeAction->setEnabled(kbd != NES_KBD_NONE);
        m_kbdModeAction->setChecked(kbd != NES_KBD_NONE && nessession_keyboard_mode(m_session));
    }
    const int tape = nessession_tape_state(m_session);
    m_tapeMenu->setEnabled(kbd == NES_KBD_FAMILY_BASIC);
    m_tapeStopAction->setEnabled(tape != NES_TAPE_IDLE);
    QStringList parts;
    if (captured) parts << QStringLiteral("KBD");
    if (tape == NES_TAPE_PLAYING) parts << QStringLiteral("PLAY");
    else if (tape == NES_TAPE_RECORDING) parts << QStringLiteral("REC");
    m_kbd->setText(parts.join(QStringLiteral("  ")));
    m_kbd->setToolTip(captured ? QStringLiteral("Keyboard mode: typed keys go to the %1 (Scroll Lock to leave)")
                                     .arg(QString::fromUtf8(nes_keyboard_name(kbd)))
                               : QString());
    if (captured == m_captured) return;
    m_captured = captured;
    /* while the keyboard owns the keys, F9/F11/F12 are its keys too */
    m_controllersAction->setShortcut(captured ? QKeySequence() : QKeySequence(Qt::Key_F9));
    m_debuggerAction->setShortcut(captured ? QKeySequence() : QKeySequence(Qt::Key_F12));
    m_fullscreenAction->setShortcut(captured ? QKeySequence() : QKeySequence(Qt::Key_F11));
}

void MainWindow::tapeAction(int action)
{
    int rc = 0;
    if (action == NES_TAPE_IDLE) {
        rc = nessession_tape_stop(m_session);
    } else {
        const QString dir = QString::fromUtf8(nessession_tapes_path(m_session));
        const QString filter = QStringLiteral("Family BASIC tapes (*.fbt);;All files (*)");
        const QString f = action == NES_TAPE_PLAYING
            ? QFileDialog::getOpenFileName(this, QStringLiteral("Play Tape"), dir, filter)
            : QFileDialog::getSaveFileName(this, QStringLiteral("Record Tape"), dir + QStringLiteral("/untitled.fbt"), filter);
        if (f.isEmpty()) return;
        const QByteArray path = f.toLocal8Bit();
        rc = action == NES_TAPE_PLAYING ? nessession_tape_play(m_session, path.constData())
                                        : nessession_tape_record(m_session, path.constData());
        if (rc == 0)
            statusBar()->showMessage(QStringLiteral("%1 %2").arg(
                action == NES_TAPE_PLAYING ? QStringLiteral("Playing") : QStringLiteral("Recording"),
                QFileInfo(f).fileName()), 4000);
    }
    if (rc != 0)
        QMessageBox::warning(this, QStringLiteral("Data Recorder"),
                             QString::fromUtf8(nessession_last_error(m_session)));
    else if (action == NES_TAPE_IDLE)
        statusBar()->showMessage(QStringLiteral("Tape stopped"), 3000);
    syncKeyboard();
}

void MainWindow::runSysaction(int sa)
{
    switch (sa) {
    case NES_SYSACT_RESET_CONFIG:
        nessession_sysaction(m_session, sa);
        statusBar()->showMessage(QStringLiteral("Back to the FujiNet CONFIG client"), 4000);
        break;
    case NES_SYSACT_PAUSE:
        DebuggerWindow::showFor(this, m_session);
        break;
    default: break;
    }
}

void MainWindow::drainSysactions()
{
    int sa;
    while (nessession_sysaction_take(m_session, &sa)) runSysaction(sa);
}

void MainWindow::updateStatus()
{
    QString text;
    bool on = false;
    const QString cart = QString::fromUtf8(nessession_cart_path(m_session));
    if (!nessession_is_running(m_session)) {
        text = QStringLiteral("Stopped");
    } else {
        char st[160];
        nessession_cart_status(m_session, st, sizeof st);
        on = nessession_cart_link_up(m_session) == 1;
        text = QStringLiteral("FujiNet: %1").arg(QString::fromUtf8(st));
        if (!cart.isEmpty())
            text = QStringLiteral("%1 — %2").arg(QFileInfo(cart).fileName(), text);
        else if (!nessession_cart_booted_game(m_session))
            text = QStringLiteral("CONFIG — %1").arg(text);
    }
    m_status->setText(text);
    m_dot->setStyleSheet(on ? QStringLiteral("color: %1;").arg(nesAccentColor().name())
                            : QStringLiteral("color: gray;"));
}

void MainWindow::openCart(const QString &path)
{
    if (nessession_load_cart(m_session, path.toLocal8Bit().constData()) != 0) {
        QMessageBox::warning(this, QStringLiteral("Could not open"),
                             QString::fromUtf8(nessession_last_error(m_session)));
        return;
    }
    statusBar()->showMessage(QStringLiteral("Running %1").arg(QFileInfo(path).fileName()), 4000);
}

void MainWindow::loadMedia(const QString &path)
{
    if (nessession_media_is_cartridge(path.toLocal8Bit().constData())) {
        openCart(path);
        return;
    }
    char dest[1024];
    if (nessession_import_media(m_session, path.toLocal8Bit().constData(), dest, sizeof dest) != 0) {
        QMessageBox::warning(this, QStringLiteral("Import failed"),
                             QString::fromUtf8(nessession_last_error(m_session)));
        return;
    }
    statusBar()->showMessage(QStringLiteral("Copied to FujiNet's SD folder — mount it from the CONFIG client"), 6000);
}

void MainWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat()) return;
    if (nessession_keyboard_captures(m_session)) {
        /* keyboard mode: every key is the emulated keyboard's (Scroll Lock
         * included -- the core toggles the mode on it). A Ctrl accelerator
         * that matched a menu item never gets here. */
        const uint32_t ks = nesKeysymFromQt(e);
        if (ks) nessession_key(m_session, ks, 1);
        syncKeyboard();
        return;
    }
    if (e->key() == Qt::Key_F9) { toggleControllers(); return; }
    if (e->key() == Qt::Key_F12) { DebuggerWindow::toggleFor(this, m_session); return; }
    if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) { QMainWindow::keyPressEvent(e); return; }

    const uint32_t ks = nesKeysymFromQt(e);
    if (!ks) { QMainWindow::keyPressEvent(e); return; }

    const int sa = nessession_key_sysaction(m_session, ks);
    if (sa >= 0) {
        if (!m_sysactDown[sa]) { m_sysactDown[sa] = true; runSysaction(sa); }
        return;
    }
    const int used = nessession_key(m_session, ks, 1);
    if (ks == NES_KEYSYM_SCROLL_LOCK) syncKeyboard();
    if (!used) QMainWindow::keyPressEvent(e);
}

/* Keyboard mode: Tab must not move focus and a plain-key shortcut must not
 * fire; take those keys as ordinary key presses. Ctrl accelerators stay. */
bool MainWindow::captureKey(QEvent *e)
{
    if (e->type() != QEvent::ShortcutOverride && e->type() != QEvent::KeyPress) return false;
    auto *k = static_cast<QKeyEvent *>(e);
    if (!nessession_keyboard_captures(m_session) || (k->modifiers() & Qt::ControlModifier)) return false;
    if (e->type() == QEvent::ShortcutOverride) { e->accept(); return true; }
    if (k->key() == Qt::Key_Tab || k->key() == Qt::Key_Backtab) { keyPressEvent(k); return true; }
    return false;
}

bool MainWindow::eventFilter(QObject *o, QEvent *e)
{
    if (o == m_display && captureKey(e)) return true;
    return QMainWindow::eventFilter(o, e);
}

void MainWindow::keyReleaseEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat()) return;
    const uint32_t ks = nesKeysymFromQt(e);
    if (!ks) { QMainWindow::keyReleaseEvent(e); return; }
    const int sa = nessession_key_sysaction(m_session, ks);
    if (sa >= 0) { m_sysactDown[sa] = false; return; }
    if (!nessession_key(m_session, ks, 0)) QMainWindow::keyReleaseEvent(e);
}

bool MainWindow::event(QEvent *e)
{
    if (captureKey(e)) return true;
    if (e->type() == QEvent::WindowDeactivate) {
        nessession_release_all(m_session);
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

void MainWindow::closeEvent(QCloseEvent *e)
{
    m_statusTimer.stop();
    m_sysactTimer.stop();
    m_padTimer.stop();
    QMainWindow::closeEvent(e);
}
