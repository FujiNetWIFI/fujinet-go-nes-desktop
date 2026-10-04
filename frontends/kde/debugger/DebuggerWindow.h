/*
 * Debugger window (Qt6 Widgets) over MesenCE's own debugger engine, via
 * core/include/nesdebug.h. Mirrors the GNOME one tab for tab.
 *
 * The engine is attached while the window is shown (which stops the
 * machine, as on every sibling) and detached when it is hidden, which lets
 * the machine run on.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <cstdint>
#include <vector>

extern "C" {
#include "nesdebug.h"
#include "nessession.h"
}

class DebuggerWindow : public QMainWindow {
    Q_OBJECT
public:
    /* Shows the window (attaching, so the machine stops). */
    static void showFor(QWidget *parent, nessession *session);
    /* F12: shows it, or hides it (detaching) when it is already up. */
    static void toggleFor(QWidget *parent, nessession *session);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    explicit DebuggerWindow(nessession *session, QWidget *parent);
    QWidget *buildToolbar();
    QWidget *buildPrompt();
    QWidget *buildCpu();
    QWidget *buildDisasm();
    QWidget *buildPpu();
    QWidget *buildApu();
    QWidget *buildBreaks();
    QWidget *buildCart();

    void refreshAll();
    void refreshStatus();
    void refreshCpu();
    void refreshRam();
    void refreshDisasm();
    void refreshPpu();
    void refreshApu();
    void refreshBps();
    void refreshCart();
    void tick();
    void runPrompt();
    void appendPrompt(const QString &text);
    void jumpTo(const QString &text);
    void stepAnd(void (*fn)(nesdebug *));

    nessession *m_session;
    nesdebug *m_dbg;
    unsigned m_seenGen = 0;
    bool m_wasStopped = false;
    int m_runningTicks = 0;
    QTimer m_timer;

    QLabel *m_status = nullptr;
    QPushButton *m_runBtn = nullptr;

    QPlainTextEdit *m_promptOut = nullptr;
    QLineEdit *m_promptIn = nullptr;

    QLineEdit *m_reg[6] = {};
    QCheckBox *m_flag[6] = {};
    QLabel *m_cycles = nullptr;
    QPlainTextEdit *m_ram = nullptr;
    QLineEdit *m_ramAddr = nullptr, *m_ramVal = nullptr;

    QPlainTextEdit *m_disasm = nullptr;
    QCheckBox *m_followPc = nullptr;
    QLineEdit *m_jump = nullptr;
    uint16_t m_disasmTop = 0;
    std::vector<uint16_t> m_lineAddr;

    QPlainTextEdit *m_ppuText = nullptr;
    QComboBox *m_ppuView = nullptr;
    QSpinBox *m_ppuPalette = nullptr;
    QLabel *m_ppuPic = nullptr;
    QPlainTextEdit *m_oam = nullptr;
    std::vector<uint32_t> m_ppuPx;

    QPlainTextEdit *m_apu = nullptr;

    QTableWidget *m_bpTable = nullptr;
    QComboBox *m_bpType = nullptr;
    QLineEdit *m_bpRange = nullptr, *m_bpCond = nullptr;
    bool m_bpFilling = false;

    QPlainTextEdit *m_cart = nullptr;
};
