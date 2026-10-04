/*
 * The emulator display: a QWidget that pulls frames from the session.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QImage>
#include <QTimer>
#include <QWidget>
#include <cstdint>
#include <vector>

#include "nessession.h"

class DisplayWidget : public QWidget {
    Q_OBJECT
public:
    explicit DisplayWidget(nessession *session, QWidget *parent = nullptr);
    /* "aspect" setting: 0 = TV (8:7 pixels), 1 = square pixels. */
    void setAspect(int aspect);
    void setSmooth(bool smooth);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    void tick();

    nessession *m_session;
    QImage m_image;
    std::vector<uint32_t> m_fb;
    int m_height = 0;
    uint64_t m_serial = 0;
    QTimer m_timer;
    int m_aspect = 0;
    bool m_smooth = false;
};
