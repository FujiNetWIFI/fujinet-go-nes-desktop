/*
 * SettingsDialog -- see SettingsDialog.h.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <vector>

namespace {

QCheckBox *check(const char *text, const char *tip, nessession *s, const char *key, int def)
{
    auto *b = new QCheckBox(QString::fromUtf8(text));
    b->setToolTip(QString::fromUtf8(tip));
    b->setChecked(nessession_get_int(s, key, def) != 0);
    return b;
}

bool commit(nessession *s, const char *key, int def, int value)
{
    if (nessession_get_int(s, key, def) == value) return false;
    nessession_set_int(s, key, value);
    return true;
}

} // namespace

bool SettingsDialog::run(QWidget *parent, nessession *session)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("Preferences"));
    dlg.setMinimumWidth(520);
    auto *outer = new QVBoxLayout(&dlg);

    /* Machine (restart) */
    auto *machine = new QGroupBox(QStringLiteral("Machine (applied by restarting the session)"));
    auto *mform = new QFormLayout(machine);
    auto *region = new QComboBox;
    for (int i = 0; nes_region_name(i); ++i) region->addItem(QString::fromUtf8(nes_region_name(i)));
    region->setCurrentIndex(nessession_get_int(session, "region", NES_REGION_AUTO));
    region->setToolTip(QStringLiteral("Auto follows each image's header (NTSC unless it says PAL)"));
    mform->addRow(QStringLiteral("Region"), region);
    outer->addWidget(machine);

    /* Picture (live) */
    auto *picture = new QGroupBox(QStringLiteral("Picture"));
    auto *picForm = new QFormLayout(picture);
    auto *aspect = new QComboBox;
    aspect->addItems({ QStringLiteral("TV (8:7 pixels, 4:3 picture)"), QStringLiteral("Square pixels") });
    aspect->setCurrentIndex(nessession_get_int(session, "aspect", 0) ? 1 : 0);
    picForm->addRow(QStringLiteral("Aspect"), aspect);
    QCheckBox *smooth = check("Smooth scaling", "Filter the picture when scaling it; off keeps the pixels sharp", session, "smooth", 0);
    picForm->addRow(smooth);
    QObject::connect(aspect, &QComboBox::currentIndexChanged, &dlg,
                     [=](int idx) { nessession_set_int(session, "aspect", idx); });
    QObject::connect(smooth, &QCheckBox::toggled, &dlg,
                     [=](bool on) { nessession_set_int(session, "smooth", on ? 1 : 0); });
    outer->addWidget(picture);

    /* Controllers (live) */
    auto *ports = new QGroupBox(QStringLiteral("Controllers (applied immediately, even to a game booted over FujiNet)"));
    auto *pform = new QFormLayout(ports);
    QComboBox *portBox[2];
    for (int port = 0; port < 2; ++port) {
        portBox[port] = new QComboBox;
        for (int i = 0; nes_ctrl_type_name(i); ++i)
            portBox[port]->addItem(QString::fromUtf8(nes_ctrl_type_name(i)));
        portBox[port]->setCurrentIndex(nessession_port_type(session, port));
        pform->addRow(port ? QStringLiteral("Player 2") : QStringLiteral("Player 1"), portBox[port]);
        QObject::connect(portBox[port], &QComboBox::currentIndexChanged, &dlg, [=](int idx) {
            nessession_set_port_type(session, port, idx);
        });
    }
    QCheckBox *aj = check("Analog sticks drive the D-pad", "The left stick as the controller's cross, as well as the D-pad", session, "analog_joystick", 1);
    QObject::connect(aj, &QCheckBox::toggled, &dlg, [=](bool on) { nessession_set_analog(session, on ? 1 : 0); });
    pform->addRow(aj);
    outer->addWidget(ports);

    /* Gamepads: one row per pad, refreshed as they come and go */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads (assigned to players in connection order unless chosen here)"));
    auto *padForm = new QFormLayout(pads);
    std::vector<QWidget *> padRows;
    unsigned seenGen = nessession_gamepad_generation(session) + 1;
    auto rebuildPads = [&, padForm, session]() mutable {
        for (QWidget *w : padRows) { padForm->removeRow(w); }
        padRows.clear();
        const int n = nessession_gamepad_count(session);
        if (n == 0) {
            auto *l = new QLabel(QStringLiteral("No gamepads connected — plug one in, it is picked up as it appears"));
            padForm->addRow(l);
            padRows.push_back(l);
            return;
        }
        for (int i = 0; i < n && i < 8; ++i) {
            char name[128];
            nessession_gamepad_name(session, i, name, sizeof name);
            auto *box = new QComboBox;
            box->addItems({ QStringLiteral("Automatic"), QStringLiteral("Player 1"), QStringLiteral("Player 2") });
            box->setCurrentIndex(nessession_gamepad_assignment(session, i) + 1);
            const int eff = nessession_gamepad_effective_port(session, i);
            box->setToolTip(eff >= 0 ? QStringLiteral("Driving player %1").arg(eff + 1)
                                     : QStringLiteral("Driving no player"));
            QObject::connect(box, &QComboBox::currentIndexChanged, box, [=](int idx) {
                nessession_gamepad_assign(session, i, idx - 1);
            });
            padForm->addRow(QString::fromUtf8(name), box);
            padRows.push_back(box);
        }
    };
    auto *padTimer = new QTimer(&dlg);
    QObject::connect(padTimer, &QTimer::timeout, &dlg, [&, session]() mutable {
        const unsigned gen = nessession_gamepad_generation(session);
        if (gen != seenGen) { seenGen = gen; rebuildPads(); }
    });
    padTimer->start(1000);
    seenGen = nessession_gamepad_generation(session);
    rebuildPads();
    outer->addWidget(pads);

    /* Audio (live) */
    auto *audio = new QGroupBox(QStringLiteral("Audio"));
    auto *audioForm = new QFormLayout(audio);
    auto *volume = new QSlider(Qt::Horizontal);
    volume->setRange(0, 100);
    volume->setValue(nessession_get_int(session, "volume", 100));
    QObject::connect(volume, &QSlider::valueChanged, &dlg, [=](int v) { nessession_set_volume(session, v); });
    audioForm->addRow(QStringLiteral("Volume"), volume);
    outer->addWidget(audio);

    /* Host (restart) */
    auto *host = new QGroupBox(QStringLiteral("Host (applied by restarting the session)"));
    auto *hform = new QVBoxLayout(host);
    QCheckBox *fuji = check("Enable FujiNet", "Run the in-process FujiNet the cartridge dials into. Off means no network and a link-down CONFIG client.", session, "enable_fujinet", 1);
    QCheckBox *snd = check("Audio", "Open the system audio device", session, "enable_audio", 1);
    QCheckBox *gp = check("Gamepads", "Poll USB/Bluetooth gamepads", session, "enable_gamepad", 1);
    hform->addWidget(fuji); hform->addWidget(snd); hform->addWidget(gp);
    outer->addWidget(host);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    outer->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return false;

    bool changed = false;
    changed |= commit(session, "region", NES_REGION_AUTO, region->currentIndex());
    changed |= commit(session, "enable_fujinet", 1, fuji->isChecked() ? 1 : 0);
    changed |= commit(session, "enable_audio", 1, snd->isChecked() ? 1 : 0);
    changed |= commit(session, "enable_gamepad", 1, gp->isChecked() ? 1 : 0);
    return changed;
}
