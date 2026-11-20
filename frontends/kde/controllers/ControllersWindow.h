/*
 * The Controllers window (F9): both NES controllers drawn as what is plugged
 * in (an NES-001 pad or an empty socket) over the console's front strip
 * (RESET), from nessession_controls_layout; each port's type, the gamepads
 * with their assignments, and Map mode for rebinding. A second tab carries
 * the expansion port: the attached keyboard (Family BASIC or Subor) drawn
 * from nessession_keyboard_skin_layout, clickable, its held keys lit. Hidden
 * at start; a fixed-size floating window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <vector>

#include "nessession.h"

class ControllersStage;
class ControllersHint;
class KeyboardStage;

class ControllersWindow : public QWidget {
public:
    explicit ControllersWindow(nessession *session, QWidget *parent = nullptr);

    /* Shows the window on the Keyboard tab and raises it. */
    void showKeyboard();

protected:
    bool event(QEvent *evt) override;
    void keyPressEvent(QKeyEvent *evt) override;
    void keyReleaseEvent(QKeyEvent *evt) override;
    void showEvent(QShowEvent *evt) override;
    void hideEvent(QHideEvent *evt) override;
    void changeEvent(QEvent *evt) override;
    /* Tab is a key of the console's, not focus traversal. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    QWidget *buildKeyboardPage();
    void setMapState(int state);
    void bound(const QString &what, const QString &stolen);
    void pollCapture();
    void pollSlow();
    void syncTypes();
    void rebuildPads();
    void refreshPadPlayers();
    void releaseEverything();

    nessession *m_session;
    QTabWidget *m_tabs = nullptr;
    ControllersStage *m_stage = nullptr;
    KeyboardStage *m_keyboard = nullptr;
    QComboBox *m_kbdType = nullptr;
    QLabel *m_kbdNote = nullptr;
    QComboBox *m_type[2] = {};
    QWidget *m_padArea = nullptr;
    QVBoxLayout *m_padBox = nullptr;
    std::vector<QWidget *> m_padRows;
    std::vector<QLabel *> m_padEff;       /* each pad's "plays Player N" */
    unsigned m_padGen = ~0u;
    QPushButton *m_mapButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    QPushButton *m_sysButton[NES_SYSACT_COUNT] = {};
    ControllersHint *m_hint = nullptr;
    QTimer m_captureTimer;
    QTimer m_heldTimer;
    QTimer m_slowTimer;
    uint32_t m_swallow = 0;               /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a target, >= 0 waiting for a key or
     * gamepad button for that target. */
    int m_mapState = -2;
};
