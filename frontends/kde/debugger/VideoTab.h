/*
 * The debugger's Video tab (Qt6 Widgets): a thin view over core/include/nesvidtab.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QList>
#include <QWidget>

extern "C" {
#include "nesvidtab.h"
}

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QTabWidget;
class QTableWidget;

class VideoPicture;

class VideoTab : public QWidget
{
    Q_OBJECT
public:
    explicit VideoTab(nessession *session, QWidget *parent = nullptr);
    ~VideoTab() override;

    /* Take a snapshot and redraw what changed (force: everything). Cheap to call every tick. */
    void refresh(bool force);

signals:
    /* "Show in Memory" on the inspected pixel: a nesdebug_space and an address in it */
    void showInMemory(int space, unsigned address);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    friend class VideoPicture;
    void refreshView(unsigned what);
    void updatePixelSection();
    void fillRegs();
    void fillColours();
    void fillLines();
    void fillTiming();
    void fillSprites();
    void note(const QString &text);
    void breakOnPixel();
    void breakOnRegister();
    void updateBreakEnabled();
    void updateSourceControls();

    nessession *m_session = nullptr;
    nesvid_tab *m_tab = nullptr;
    QComboBox *m_view = nullptr, *m_source = nullptr, *m_zoom = nullptr;
    QSpinBox *m_palette = nullptr;
    QList<QCheckBox *> m_overlays;
    QPushButton *m_breakBtn = nullptr, *m_memBtn = nullptr, *m_regBreakBtn = nullptr;
    QLabel *m_status = nullptr, *m_note = nullptr;
    VideoPicture *m_pic = nullptr;
    QScrollArea *m_scroll = nullptr;
    QTabWidget *m_sections = nullptr;
    QLabel *m_pixelText = nullptr, *m_timingText = nullptr;
    QPlainTextEdit *m_spriteText = nullptr;
    QTableWidget *m_regs = nullptr, *m_lines = nullptr, *m_swatches = nullptr;
    QProgressBar *m_beam = nullptr;
    QString m_regsSig, m_linesSig, m_spriteSig;
    bool m_updating = false;
};
