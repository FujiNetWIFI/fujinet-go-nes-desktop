/*
 * ControllersWindow -- see ControllersWindow.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ControllersWindow.h"

#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QVBoxLayout>

#include "../FujiNetWindows.h"
#include "../KeyForward.h"

/* ---- PadButton ------------------------------------------------------------ */

PadButton::PadButton(const QString &face, int target, QWidget *parent)
    : QPushButton(face, parent), m_target(target), m_face(face)
{
    setFocusPolicy(Qt::NoFocus);
}

void PadButton::setHeld(bool held)
{
    if (held == m_held) return;
    m_held = held;
    setStyleSheet(held ? QStringLiteral("background: %1; color: white;").arg(nesAccentColor().name())
                       : QString());
}

void PadButton::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && !m_down) {
        m_down = true;
        emit pressedTarget(m_target);
    }
    QPushButton::mousePressEvent(e);
}

void PadButton::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_down) {
        m_down = false;
        emit releasedTarget(m_target);
    }
    QPushButton::mouseReleaseEvent(e);
}

/* Dragging off a button must release it. */
void PadButton::leaveEvent(QEvent *e)
{
    if (m_down) {
        m_down = false;
        emit releasedTarget(m_target);
    }
    QPushButton::leaveEvent(e);
}

/* ---- ControllersWindow ------------------------------------------------------ */

/* Fixed button sizes, wide enough for a Map-mode label ("Right Shift /
 * Start"), so the panel is the same shape in both modes. */
static constexpr int kButtonWidth = 112;
static constexpr int kButtonHeight = 40;

ControllersWindow::ControllersWindow(nessession *session, QWidget *parent)
    /* A dialog-type window, so a tiling compositor floats it: it is a
     * panel of fixed-size buttons. */
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint
                      | Qt::CustomizeWindowHint), m_session(session)
{
    setWindowTitle(QStringLiteral("Controllers"));
    setFocusPolicy(Qt::StrongFocus);

    auto *root = new QVBoxLayout(this);
    auto *ports = new QHBoxLayout;
    ports->addWidget(buildController(0));
    ports->addWidget(buildController(1));
    root->addLayout(ports);

    /* The console's own buttons: RESET is a machine control like any other;
     * Reset to CONFIG is the power switch of a FujiNet cartridge. */
    auto *console = new QGroupBox(QStringLiteral("Console"));
    auto *crow = new QHBoxLayout(console);
    crow->addStretch();
    PadButton *reset = control(QStringLiteral("Reset Game"), NES_TARGET_SWITCH(NES_SW_RESET));
    PadButton *config = control(QStringLiteral("Reset to CONFIG"), NES_TARGET_SYSACT(NES_SYSACT_RESET_CONFIG));
    const int cw = qMax(kButtonWidth, config->fontMetrics().horizontalAdvance(config->face()) + 24);
    reset->setFixedWidth(cw);
    config->setFixedWidth(cw);
    crow->addWidget(reset);
    crow->addWidget(config);
    crow->addStretch();
    root->addWidget(console);

    /* Gamepads: one row per pad, rebuilt as they come and go */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads (assigned to players in connection order unless chosen here)"));
    m_padForm = new QFormLayout(pads);
    root->addWidget(pads);

    auto *maprow = new QHBoxLayout;
    m_mapButton = new QPushButton(QStringLiteral("Map"));
    m_mapButton->setFocusPolicy(Qt::NoFocus);
    connect(m_mapButton, &QPushButton::clicked, this, [this] { setMapState(m_mapState == -2 ? -1 : -2); });
    auto *defaults = new QPushButton(QStringLiteral("Defaults"));
    defaults->setFocusPolicy(Qt::NoFocus);
    connect(defaults, &QPushButton::clicked, this, [this] {
        nessession_bindings_reset(m_session);
        refreshLabels();
    });
    m_hint = new QLabel;
    m_hint->setStyleSheet(QStringLiteral("color: gray;"));
    maprow->addWidget(m_mapButton);
    maprow->addWidget(defaults);
    maprow->addWidget(m_hint, 1);
    root->addLayout(maprow);

    connect(&m_captureTimer, &QTimer::timeout, this, &ControllersWindow::pollCapture);
    connect(&m_pollTimer, &QTimer::timeout, this, &ControllersWindow::poll);
    rebuildPads();
    setMapState(-2);
}

PadButton *ControllersWindow::control(const QString &face, int target)
{
    auto *b = new PadButton(face, target);
    b->setFixedSize(kButtonWidth, kButtonHeight);
    connect(b, &PadButton::pressedTarget, this, &ControllersWindow::onPressed);
    connect(b, &PadButton::releasedTarget, this, &ControllersWindow::onReleased);
    m_controls.push_back(b);
    return b;
}

QWidget *ControllersWindow::buildController(int port)
{
    auto *box = new QGroupBox(port ? QStringLiteral("Player 2") : QStringLiteral("Player 1"));
    auto *v = new QVBoxLayout(box);

    m_type[port] = new QComboBox;
    for (int i = 0; nes_ctrl_type_name(i); ++i)
        m_type[port]->addItem(QString::fromUtf8(nes_ctrl_type_name(i)));
    m_type[port]->setCurrentIndex(nessession_port_type(m_session, port));
    connect(m_type[port], &QComboBox::currentIndexChanged, this,
            [this, port](int idx) { nessession_set_port_type(m_session, port, idx); });
    auto *typeRow = new QHBoxLayout;
    typeRow->addWidget(new QLabel(QStringLiteral("Plugged in:")));
    typeRow->addWidget(m_type[port], 1);
    v->addLayout(typeRow);

    /* The controller's own layout: the cross on the left, Select and Start
     * in the middle, B and A on the right with their turbo twins above. */
    auto *grid = new QGridLayout;
    grid->setSpacing(6);
    grid->setSizeConstraint(QLayout::SetFixedSize);
    grid->addWidget(control(QStringLiteral("Up"), NES_TARGET_PORT(port, NES_ACT_UP)), 0, 1);
    grid->addWidget(control(QStringLiteral("Left"), NES_TARGET_PORT(port, NES_ACT_LEFT)), 1, 0);
    grid->addWidget(control(QStringLiteral("Right"), NES_TARGET_PORT(port, NES_ACT_RIGHT)), 1, 2);
    grid->addWidget(control(QStringLiteral("Down"), NES_TARGET_PORT(port, NES_ACT_DOWN)), 2, 1);
    grid->addWidget(control(QStringLiteral("Select"), NES_TARGET_PORT(port, NES_ACT_SELECT)), 3, 1);
    grid->addWidget(control(QStringLiteral("Start"), NES_TARGET_PORT(port, NES_ACT_START)), 3, 2);
    grid->addWidget(control(QStringLiteral("Turbo B"), NES_TARGET_PORT(port, NES_ACT_TURBO_B)), 0, 3);
    grid->addWidget(control(QStringLiteral("Turbo A"), NES_TARGET_PORT(port, NES_ACT_TURBO_A)), 0, 4);
    grid->addWidget(control(QStringLiteral("B"), NES_TARGET_PORT(port, NES_ACT_B)), 1, 3);
    grid->addWidget(control(QStringLiteral("A"), NES_TARGET_PORT(port, NES_ACT_A)), 1, 4);
    auto *gridRow = new QHBoxLayout;
    gridRow->addStretch();
    gridRow->addLayout(grid);
    gridRow->addStretch();
    v->addLayout(gridRow);
    return box;
}

void ControllersWindow::rebuildPads()
{
    for (QWidget *w : m_padRows) m_padForm->removeRow(w);
    m_padRows.clear();
    const int n = nessession_gamepad_count(m_session);
    if (n == 0) {
        auto *l = new QLabel(QStringLiteral("No gamepads connected — plug one in, it is picked up as it appears"));
        l->setStyleSheet(QStringLiteral("color: gray;"));
        m_padForm->addRow(l);
        m_padRows.push_back(l);
        return;
    }
    for (int i = 0; i < n && i < 8; ++i) {
        char name[128];
        nessession_gamepad_name(m_session, i, name, sizeof name);
        auto *box = new QComboBox;
        box->addItems({ QStringLiteral("Automatic"), QStringLiteral("Player 1"), QStringLiteral("Player 2") });
        box->setCurrentIndex(nessession_gamepad_assignment(m_session, i) + 1);
        const int eff = nessession_gamepad_effective_port(m_session, i);
        box->setToolTip(eff >= 0 ? QStringLiteral("Driving player %1").arg(eff + 1)
                                 : QStringLiteral("Driving no player"));
        connect(box, &QComboBox::currentIndexChanged, this, [this, i](int idx) {
            nessession_gamepad_assign(m_session, i, idx - 1);
        });
        m_padForm->addRow(QString::fromUtf8(name), box);
        m_padRows.push_back(box);
    }
}

void ControllersWindow::onPressed(int target)
{
    if (m_mapState == -1) { setMapState(target); return; }
    if (m_mapState >= 0) return;
    m_mouseHeld = target;
    if (target >= NES_TARGET_SYSACT(0)) return;   /* fires on release */
    nessession_press(m_session, target, 1);
}

void ControllersWindow::onReleased(int target)
{
    if (m_mapState != -2) return;
    if (m_mouseHeld == target) m_mouseHeld = -1;
    if (target >= NES_TARGET_SYSACT(0)) {
        nessession_sysaction(m_session, target - NES_TARGET_SYSACT(0));
        return;
    }
    nessession_press(m_session, target, 0);
}

void ControllersWindow::setMapState(int state)
{
    m_mapState = state;
    if (state == -2) {
        m_captureTimer.stop();
        nessession_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Map"));
        m_mapButton->setStyleSheet(QString());
        m_hint->setText(QString());
    } else if (state == -1) {
        m_captureTimer.stop();
        nessession_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Cancel"));
        m_mapButton->setStyleSheet(QStringLiteral("background: %1; color: white;").arg(nesAccentColor().name()));
        m_hint->setText(QStringLiteral("Click a control to remap"));
    } else {
        m_hint->setText(QStringLiteral("Press a key or gamepad button for %1")
                            .arg(QString::fromUtf8(nes_target_name(state))));
        nessession_gamepad_capture_begin(m_session);
        m_captureTimer.start(50);
    }
    refreshLabels();
}

void ControllersWindow::pollCapture()
{
    int button;
    if (m_mapState < 0) { m_captureTimer.stop(); return; }
    if (nessession_gamepad_capture_poll(m_session, &button)) {
        char stolen[128];
        nessession_binding_set_button(m_session, m_mapState, button, stolen, sizeof stolen);
        const QString msg = stolen[0]
            ? QStringLiteral("Bound %1 (was %2)").arg(QString::fromUtf8(nes_pad_button_name(button)), QString::fromUtf8(stolen))
            : QString();
        setMapState(-1);
        if (!msg.isEmpty()) m_hint->setText(msg);
    }
}

void ControllersWindow::refreshLabels()
{
    for (PadButton *b : m_controls) {
        if (m_mapState != -2) {
            const nes_binding bind = nessession_binding_get(m_session, b->target());
            char key[32];
            nessession_keysym_name(bind.keysym, key, sizeof key);
            QString text = key[0] ? QString::fromUtf8(key) : QStringLiteral("—");
            if (bind.button != NES_PAD_BTN_NONE)
                text += QStringLiteral(" / ") + QString::fromUtf8(nes_pad_button_name(bind.button));
            b->setText(text);
            b->setToolTip(QString::fromUtf8(nes_target_name(b->target())));
            b->setHeld(b->target() == m_mapState);
        } else {
            b->setText(b->face());
            b->setToolTip(QString());
            b->setHeld(false);
        }
    }
}

/* What the machine sees held, from every source, lit in the accent colour;
 * the gamepad list follows hot-plugging; the type combos follow a change
 * made in Preferences. */
void ControllersWindow::poll()
{
    const unsigned gen = nessession_gamepad_generation(m_session);
    if (gen != m_padGen) { m_padGen = gen; rebuildPads(); }
    for (int port = 0; port < 2; ++port) {
        const int t = nessession_port_type(m_session, port);
        if (m_type[port]->currentIndex() != t) {
            QSignalBlocker block(m_type[port]);
            m_type[port]->setCurrentIndex(t);
        }
    }
    if (m_mapState != -2) return;
    const unsigned held[2] = { nessession_buttons_held(m_session, 0), nessession_buttons_held(m_session, 1) };
    for (PadButton *b : m_controls) {
        const int t = b->target();
        bool on = t == m_mouseHeld;
        if (t < 2 * NES_ACT_PER_PORT)
            on = on || (held[t / NES_ACT_PER_PORT] & (1u << (t % NES_ACT_PER_PORT)));
        b->setHeld(on);
    }
}

void ControllersWindow::showEvent(QShowEvent *e)
{
    poll();
    m_pollTimer.start(50);
    QWidget::showEvent(e);
}

void ControllersWindow::hideEvent(QHideEvent *e)
{
    m_pollTimer.stop();
    setMapState(-2);
    QWidget::hideEvent(e);
}

void ControllersWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat()) return;
    const uint32_t ks = nesKeysymFromQt(e);
    if (m_mapState >= 0) {
        if (!ks) return;
        char stolen[128], name[32];
        nessession_binding_set_key(m_session, m_mapState, ks, stolen, sizeof stolen);
        nessession_keysym_name(ks, name, sizeof name);
        setMapState(-1);
        if (stolen[0])
            m_hint->setText(QStringLiteral("Bound %1 (was %2)").arg(QString::fromUtf8(name), QString::fromUtf8(stolen)));
        return;
    }
    if (m_mapState == -1) return;
    if (e->key() == Qt::Key_F9) { hide(); return; }
    if (!ks) { QWidget::keyPressEvent(e); return; }
    const int sa = nessession_key_sysaction(m_session, ks);
    if (sa >= 0) { nessession_sysaction(m_session, sa); return; }
    if (!nessession_key(m_session, ks, 1)) QWidget::keyPressEvent(e);
}

void ControllersWindow::keyReleaseEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat() || m_mapState != -2) return;
    const uint32_t ks = nesKeysymFromQt(e);
    if (!ks || !nessession_key(m_session, ks, 0)) QWidget::keyReleaseEvent(e);
}
