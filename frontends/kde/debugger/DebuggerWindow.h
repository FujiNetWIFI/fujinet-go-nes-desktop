/*
 * Debugger window (Qt6 Widgets) over MesenCE's own debugger engine, via
 * core/include/nesdebug.h and nesvidtab.h. The family's tabbed design (the
 * Adam, Coleco, MSX and INTV KDE debuggers): a QTabWidget under a toolbar,
 * the editable register strip under every tab, and the FujiNet tab (every
 * FujiBus packet on the cartridge's link) last.
 *
 * The engine is attached while the window is shown (which stops the machine,
 * as on every sibling) and detached when it is hidden, which lets the
 * machine run on. The window is never deleted by its close button: it is
 * hidden, and comes back as it was.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <cstdint>
#include <vector>

extern "C" {
#include "nesdebug.h"
#include "nessession.h"
}

class VideoTab;

class DebuggerWindow : public QMainWindow {
    Q_OBJECT
public:
    /* Shows the window (attaching, so the machine stops). */
    static void showFor(QWidget *parent, nessession *session);
    /* F12: shows it, or hides it (detaching: the machine runs on) when it is already up. */
    static void toggleFor(QWidget *parent, nessession *session);

protected:
    bool event(QEvent *evt) override;
    void keyPressEvent(QKeyEvent *evt) override;
    void showEvent(QShowEvent *evt) override;
    void hideEvent(QHideEvent *evt) override;
    bool eventFilter(QObject *obj, QEvent *evt) override;

private:
    explicit DebuggerWindow(nessession *session, QWidget *parent);
    ~DebuggerWindow() override;
    QWidget *buildToolbar();
    QWidget *buildRegs();
    QWidget *buildConsole();
    QWidget *buildDisasm();
    QWidget *buildMemory();
    QWidget *buildBreaks();
    QWidget *buildWatches();
    QWidget *buildCart();
    QWidget *buildTrace(int which);
    QWidget *buildVideo();
    QWidget *buildSound();
    QWidget *buildSymbols();
    QWidget *buildFujinet();

    void refreshStatus(const nesdebug_snapshot &snap);
    void refreshRegs();
    void refreshMem();
    void refreshDisasm();
    void refreshBps();
    void refreshWatches();
    void refreshCart();
    void refreshTrace(int which, bool force);
    void refreshVideo(bool force);
    void refreshSound();
    void refreshSymbols(bool force);
    void refreshFujinet(bool force);
    void refreshTab(int tab, bool live);
    void refreshAll();
    void refreshRegions();
    void resync();
    void syncLater();
    void settleAndSync();
    void tick();
    void firstShow();

    void consoleAppend(const QString &text);
    void runConsole();
    void completeConsole();
    void say(const QString &msg);
    void message(const QString &msg);

    void toggleRun();
    void stepAnd(void (*func)(nesdebug *));
    void applyRegister(const char *name, long value, const QString &okMessage);
    bool handleKey(QKeyEvent *evt);
    void runToCursor();
    void toggleBreakpoint(int addr);
    void toggleSelected();
    void addBreakpoint();
    void addWatch();
    void setReg(int idx);
    void setFlag(int idx, bool enabled);
    void breakAtSymbol();
    void memGoto();
    void memPoke();
    void memSearch(bool backward);
    void memSpaceChanged();
    void showInMemory(int space, uint32_t first, uint32_t last);
    void jumpTo();
    void jumpToAddress(int address);
    void loadSymbols(bool fromFile);
    void save(const char *kind, const QString &title, const QString &file);
    void popOut(int tab);
    void dock(int tab);
    int currentTab() const;
    bool tabVisible(int tab) const;
    int visibleRows() const;
    bool addressOf(const QString &text, long *out);
    bool numberOf(const QString &text, long *out, QString *why);
    bool memAddressOf(const QString &text, long *out, QString *why);
    int memSpace() const;
    int memDigits() const;
    void setEntry(QLineEdit *edit, const QString &text);
    void showTab(int tab);

    nessession *m_session;
    nesdebug *m_dbg;
    uint64_t m_seenSerial = ~0ull;
    unsigned m_seenGen = 0;
    bool m_wasStopped = false;
    bool m_shownOnce = false;
    QByteArray m_envWatch, m_envWatchBreak, m_envMemGoto;   /* the capture variables, read at construction */
    bool m_envHistory = false;
    int m_runningTicks = 0;
    QTimer m_timer;
    QTabWidget *m_tabs = nullptr;

    QLabel *m_status = nullptr;
    QPushButton *m_runBtn = nullptr;

    /* the register strip */
    QLineEdit *m_reg[6] = {};
    QCheckBox *m_flag[6] = {};
    QLabel *m_beamNote = nullptr, *m_irqNote = nullptr, *m_nmiNote = nullptr;

    /* console */
    QPlainTextEdit *m_conOut = nullptr;
    QLineEdit *m_conIn = nullptr;
    QStringList m_history;
    int m_historyPos = 0;

    /* memory */
    QPlainTextEdit *m_mem = nullptr;
    QComboBox *m_memSpaceBox = nullptr;
    QLabel *m_memTitle = nullptr;
    QLineEdit *m_memGoto = nullptr, *m_memAddr = nullptr, *m_memVal = nullptr;
    QComboBox *m_srchKind = nullptr;
    QLineEdit *m_srchEntry = nullptr;
    QLabel *m_srchNote = nullptr;
    uint32_t m_memTop = 0;
    std::vector<nesdebug_region> m_regions;      /* the CPU memory map, fetched once per stop */
    uint64_t m_regionSerial = ~0ull;
    bool m_memHl = false;               /* a range is highlighted */
    uint32_t m_memHlLo = 0, m_memHlHi = 0;
    int m_memHlSpace = 0;
    bool m_memSwitching = false;

    /* disassembly */
    QPlainTextEdit *m_disasm = nullptr;
    QCheckBox *m_followPc = nullptr;
    QLineEdit *m_jump = nullptr;
    uint16_t m_disasmTop = 0;
    int m_selected = -1;                /* the selected line's address, or -1 */
    int m_wheel = 0;                    /* wheel travel not yet a row */
    std::vector<uint16_t> m_lineAddr;

    /* breakpoints */
    QTableWidget *m_bpTable = nullptr;
    QString m_bpSig;                    /* what the table shows, to rebuild on change */
    bool m_bpFilling = false;
    QLabel *m_bpEmpty = nullptr, *m_bpMsg = nullptr, *m_bpStop = nullptr;
    QComboBox *m_bpType = nullptr, *m_bpSpace = nullptr;
    QLineEdit *m_bpStart = nullptr, *m_bpEnd = nullptr, *m_bpCond = nullptr;

    /* watches */
    QLineEdit *m_wtEntry = nullptr;
    QListWidget *m_wtList = nullptr;

    /* Cart & I/O */
    QPlainTextEdit *m_cartText = nullptr, *m_ioText = nullptr;

    /* Trace (0, live) and History (1, while stopped): one ring */
    QPushButton *m_hRec[2] = {};
    QComboBox *m_hFilter[2] = {};
    QLineEdit *m_hRange[2] = {};
    QPlainTextEdit *m_hText[2] = {};
    QLabel *m_hNote[2] = {};
    uint64_t m_hSeen[2] = { ~0ull, ~0ull };
    bool m_hSync = false;

    /* video, sound, symbols */
    VideoTab *m_video = nullptr;
    bool m_videoForce = true;
    QTableWidget *m_soundTable = nullptr;
    QLabel *m_soundNote = nullptr;
    QLineEdit *m_symFilter = nullptr;
    QListWidget *m_symList = nullptr;
    QLabel *m_symCount = nullptr;
    unsigned m_symGen = ~0u;
    QString m_symShownFilter;

    /* fujinet */
    QLabel *m_fujiNote = nullptr;
    QPlainTextEdit *m_fujiText = nullptr, *m_fujiState = nullptr;
    uint64_t m_fujiLast = 0;
    int m_fujiCount = -1;

    /* tabs peeled off into windows of their own */
    QWidget *m_pages[16] = {};
    QWidget *m_popped[16] = {};
    bool m_closing = false;
};
