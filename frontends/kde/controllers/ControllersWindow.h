/*
 * The Controllers window: both NES controllers on screen (they press the
 * machine's buttons and light up with whatever the keyboard, a gamepad or
 * the mouse is holding), the console's RESET and Reset to CONFIG, each
 * port's controller type and gamepad assignment, and Map mode for rebinding
 * any control to a key or a gamepad button. A second tab carries the
 * expansion port: the attached keyboard drawn from the core's layout,
 * clickable, its held keys lit.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QWidget>
#include <vector>

#include "nessession.h"

/* A button that reports press and release, not "clicked": a controller
 * button is HELD, since the machine samples it once per frame. */
class PadButton : public QPushButton {
    Q_OBJECT
public:
    PadButton(const QString &face, int target, QWidget *parent = nullptr);
    int target() const { return m_target; }
    QString face() const { return m_face; }
    void setHeld(bool held);
signals:
    void pressedTarget(int target);
    void releasedTarget(int target);
protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;
private:
    int m_target;
    QString m_face;
    bool m_down = false;
    bool m_held = false;
};

/* The expansion-port keyboard, drawn from nessession_keyboard_layout in key
 * units; a key is HELD while the mouse is down on it. */
class KeyboardWidget : public QWidget {
public:
    explicit KeyboardWidget(nessession *session, QWidget *parent = nullptr);
    void setType(int type);
    QSize sizeHint() const override;
protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
private:
    QRectF keyRect(int i) const;
    void releaseMouse();
    nessession *m_session;
    int m_type = NES_KBD_NONE;
    const nes_kbd_key *m_keys = nullptr;
    int m_count = 0;
    float m_cols = 0, m_rows = 0;
    int m_mouseKey = -1;       /* the key index the mouse holds, or -1 */
};

class ControllersWindow : public QWidget {
    Q_OBJECT
public:
    explicit ControllersWindow(nessession *session, QWidget *parent = nullptr);
    void showKeyboard();

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;

private:
    QWidget *buildController(int port);
    QWidget *buildKeyboard();
    PadButton *control(const QString &face, int target);
    void onPressed(int target);
    void onReleased(int target);
    void setMapState(int state);
    void refreshLabels();
    void pollCapture();
    void poll();
    void rebuildPads();

    nessession *m_session;
    QTabWidget *m_tabs = nullptr;
    KeyboardWidget *m_keyboard = nullptr;
    QComboBox *m_kbdType = nullptr;
    QLabel *m_kbdNote = nullptr;
    std::vector<PadButton *> m_controls;
    QPushButton *m_mapButton = nullptr;
    QLabel *m_hint = nullptr;
    QComboBox *m_type[2] = { nullptr, nullptr };
    QFormLayout *m_padForm = nullptr;
    std::vector<QWidget *> m_padRows;
    unsigned m_padGen = ~0u;
    int m_mouseHeld = -1;      /* the target the mouse is holding, if any */
    QTimer m_captureTimer;
    QTimer m_pollTimer;
    /* -2 idle, -1 armed and waiting for a target, >=0 waiting for a key or
     * gamepad button. */
    int m_mapState = -2;
};
