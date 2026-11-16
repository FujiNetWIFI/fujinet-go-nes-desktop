/*
 * debugger_test -- the Debugger window (debugger/DebuggerWindow.cpp) driven
 * with synthetic events on the offscreen platform against a running NES
 * session whose cartridge is a hand-assembled loop (core/tests/test_engine_rom.h:
 * no ROM file, nothing from the FujiNet runtime): the twelve tabs and their
 * order, the NES_DEBUGGER_TAB names and the older aliases, the Help contexts,
 * the minimum size, attach/detach with show and hide, F12, the stepping keys
 * and buttons, the console (transcript, Tab completion, history), the
 * disassembly (the PC's line, click / double-click / F9 for a breakpoint,
 * selection and Run to Cursor, the wheel, the jump box), the register strip,
 * Memory (spaces, Go to, Poke, search), Breakpoints (every type and space, an
 * expression condition, hits), Watches, Trace and History, Video, Sound,
 * Symbols, Cart & I/O, FujiNet, pop-out and dock, the env hooks, F12 and the
 * View menu from the main window and a session restart. A small program of
 * NOP-padded instructions is written into the console RAM and stepped through
 * the window.
 *
 * DBG_TEST_SHOTS=<dir> also grabs a PNG of each tab (DBG_TEST_LIGHT=1 /
 * DBG_TEST_DARK=1: in that scheme).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QOpenGLContext>
#include <QTemporaryDir>
#include <QtTest>

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStatusBar>
#include <QStyleHints>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBlock>
#include <QToolButton>

#include "FujiNetWindows.h"
#include "DisplayWidget.h"
#include "MainWindow.h"
#include "debugger/Accent.h"
#include "debugger/DebuggerWindow.h"
#include "debugger/VideoTab.h"

extern "C" {
#include "nesdebug.h"
#include "nesmemtools.h"
#include "nessession.h"
#include "test_engine_rom.h"
}

namespace {

/* The test cartridge (test_engine_rom.h), the loop at $C00F (the NMI enable is
 * patched out of the image so a step is never taken by a vblank):
 *   C00F  LDA #$10
 *   C011  STA $2000      ; a PPU register write
 *   C014  INC $0300      ; read-modify-write of RAM
 *   C017  LDA $0300
 *   C01A  STA $0301
 *   C01D  JSR $C030      ; palette $3F01, nametable, OAM through the PPU ports
 *   C020  JMP $C00F      */
const int kLoop = 0xC00F;

int pcOf(nesdebug *dbg)
{
    nesdebug_cpu cpu;
    nesdebug_cpu_get(dbg, &cpu);
    return cpu.pc;
}

/* In the console RAM at $0400 (the cart's own loop uses $0300), NOP-padded so
 * a backward disassembly guess is stable:
 * NOP x3, LDA #$12, NOP, STA $0500, NOP x3. */
const uint8_t kProg[] = { 0xEA, 0xEA, 0xEA, 0xA9, 0x12, 0xEA, 0x8D, 0x00, 0x05, 0xEA, 0xEA, 0xEA };
const int kStart = 0x0400;

QStringList expectedTabs()
{
    return { QStringLiteral("Console"), QStringLiteral("Disassembly"), QStringLiteral("Memory"),
             QStringLiteral("Breakpoints"), QStringLiteral("Watches"), QStringLiteral("Cart & I/O"),
             QStringLiteral("Trace"), QStringLiteral("History"), QStringLiteral("Video"),
             QStringLiteral("Sound"), QStringLiteral("Symbols"), QStringLiteral("FujiNet") };
}

} // namespace

class DebuggerTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    nessession *m_s = nullptr;
    nesdebug *m_d = nullptr;
    QMainWindow *m_win = nullptr;
    QTabWidget *m_tabs = nullptr;
    QString m_shots;
    QByteArray m_cartPath;
    nessession_start_opts m_opts = {};

    static QMainWindow *findWindow()
    {
        for (QWidget *top : QApplication::topLevelWidgets())
            if (top->objectName() == QLatin1String("debugger")) return qobject_cast<QMainWindow *>(top);
        return nullptr;
    }
    /* the window is kept when closed: hide it (which detaches the engine) and
     * delete it, so showFor() builds a fresh one that reads the env hooks */
    void discardWindow()
    {
        if (m_win) {
            m_win->hide();
            m_win->deleteLater();
        }
        m_win = nullptr;
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void openWindow()
    {
        DebuggerWindow::showFor(nullptr, m_s);
        m_win = findWindow();
        QVERIFY(m_win);
        QVERIFY(QTest::qWaitForWindowExposed(m_win));
        m_tabs = m_win->findChild<QTabWidget *>();
        QVERIFY(m_tabs);
        QTRY_VERIFY_WITH_TIMEOUT(nesdebug_is_attached(m_d), 2000);
        waitStopped();
    }
    QString tabText(int idx) const { return m_tabs->tabText(idx).replace(QStringLiteral("&&"), QStringLiteral("&")); }

    QPushButton *button(const QString &textStart)
    {
        for (QPushButton *btn : m_win->findChildren<QPushButton *>())
            if (btn->text().startsWith(textStart)) return btn;
        return nullptr;
    }
    QLineEdit *entry(const QString &placeholderStart)
    {
        for (QLineEdit *edit : m_win->findChildren<QLineEdit *>())
            if (edit->placeholderText().startsWith(placeholderStart)) return edit;
        return nullptr;
    }
    QLineEdit *entryIn(int tab, const QString &placeholderStart)   /* in one tab's own page */
    {
        for (QLineEdit *edit : m_tabs->widget(tab)->findChildren<QLineEdit *>())
            if (edit->placeholderText().startsWith(placeholderStart)) return edit;
        return nullptr;
    }
    QPushButton *addButton(int tab)   /* the tab's own Add */
    {
        for (QPushButton *btn : m_tabs->widget(tab)->findChildren<QPushButton *>())
            if (btn->text() == QLatin1String("Add")) return btn;
        return nullptr;
    }
    QPlainTextEdit *disasmView()
    {
        for (QPlainTextEdit *view : m_win->findChildren<QPlainTextEdit *>())
            if (view->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff) return view;
        return nullptr;
    }
    QPlainTextEdit *consoleView() { return m_tabs->widget(0)->findChild<QPlainTextEdit *>(); }
    QString consoleText()
    {
        auto *view = consoleView();
        return view ? view->toPlainText() : QString();
    }
    QLabel *statusLabel()
    {
        for (QLabel *label : m_win->findChildren<QLabel *>())
            if (label->text().startsWith(QLatin1String("Stopped")) || label->text() == QLatin1String("Running")) return label;
        return nullptr;
    }
    QCheckBox *check(const QString &text)
    {
        for (QCheckBox *box : m_win->findChildren<QCheckBox *>())
            if (box->text() == text) return box;
        return nullptr;
    }
    QList<QLineEdit *> regFields()
    {
        QList<QLineEdit *> out;
        for (QLineEdit *edit : m_win->findChildren<QLineEdit *>())
            if (edit->toolTip().startsWith(QLatin1String("Enter sets it"))) out << edit;
        return out;
    }
    QTableWidget *bpTable() { return m_tabs->widget(3)->findChild<QTableWidget *>(); }
    nesdebug_cpu cpuNow()
    {
        nesdebug_cpu cpu;
        nesdebug_cpu_get(m_d, &cpu);
        return cpu;
    }
    uint8_t peek(int space, uint32_t addr)
    {
        uint8_t byte = 0;
        nesdebug_read_space(m_d, space, addr, &byte, 1);
        return byte;
    }
    void setPc(int address) { setReg("pc", address); }
    /* To an instruction of the loop by single steps from the top: the debugger
     * then stands on a real instruction (a PC written while stopped is not one
     * it has seen, which a Step Over or Step Out would take for another). */
    void runTo(int target)
    {
        setPc(kLoop);
        for (int tries = 0; tries < 12 && pcOf(m_d) != target; ++tries) {
            const uint64_t before = nesdebug_stop_serial(m_d);
            nesdebug_step(m_d);
            QTRY_VERIFY_WITH_TIMEOUT(nesdebug_stop_serial(m_d) != before && nesdebug_is_stopped(m_d), 2000);
        }
        QCOMPARE(pcOf(m_d), target);
    }
    void setReg(const char *name, long value)
    {
        char err[100] = "";
        QVERIFY2(nesdebug_register_set(m_d, name, value, err, sizeof err), err);
    }

    void waitStopped()
    {
        QTRY_VERIFY_WITH_TIMEOUT(nesdebug_is_stopped(m_d), 4000);
        /* the window follows within its poll */
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel() && statusLabel()->text().startsWith(QLatin1String("Stopped")), 3000);
    }
    void shot(const QString &name)
    {
        if (!m_shots.isEmpty()) m_win->grab().save(m_shots + QLatin1Char('/') + name + QStringLiteral(".png"));
    }

    /* The block of the disassembly that shows an address, or -1. */
    int lineOf(QPlainTextEdit *view, int addr)
    {
        const QString want = QStringLiteral("%1").arg(addr, 4, 16, QLatin1Char('0')).toUpper();
        int count = 0;
        for (QTextBlock block = view->document()->begin(); block.isValid(); block = block.next(), ++count)
            if (block.text().mid(3, 4) == want) return count;
        return -1;
    }
    QPoint lineAt(QPlainTextEdit *view, int line, int column)
    {
        QTextCursor cursor(view->document()->findBlockByNumber(line));
        cursor.setPosition(cursor.block().position() + column);
        const QRect rect = view->cursorRect(cursor);
        return QPoint(rect.left() + 1, rect.center().y());
    }
    void clickLine(int addr, int column, bool dbl = false)
    {
        QPlainTextEdit *view = disasmView();
        const int line = lineOf(view, addr);
        QVERIFY2(line >= 0, qPrintable(QStringLiteral("line for %1").arg(addr, 0, 16)));
        const QPoint point = lineAt(view, line, column);
        if (dbl) QTest::mouseDClick(view->viewport(), Qt::LeftButton, {}, point);
        else QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, point);
    }
    void typeInto(QLineEdit *edit, const QString &text)
    {
        edit->clear();
        QTest::keyClicks(edit, text);
        QTest::keyClick(edit, Qt::Key_Return);
    }
    /* The program in console RAM, started: no interrupt in the way, the stack
     * as it was. */
    bool loadProgram()
    {
        uint8_t back[sizeof kProg];
        char err[200] = "";
        for (size_t offset = 0; offset < sizeof kProg; ++offset) nesdebug_write(m_d, uint16_t(kStart + offset), kProg[offset]);
        if (nesdebug_read(m_d, kStart, back, int(sizeof kProg)) != int(sizeof kProg) || memcmp(back, kProg, sizeof kProg))
            return false;
        return nesdebug_register_set(m_d, "pc", kStart, err, sizeof err) && pcOf(m_d) == kStart;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        qputenv("SDL_AUDIODRIVER", "dummy");
        if (qEnvironmentVariableIsSet("DBG_TEST_SHOTS")) m_shots = qEnvironmentVariable("DBG_TEST_SHOTS");
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        if (qEnvironmentVariableIsSet("DBG_TEST_LIGHT"))
            QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
        if (qEnvironmentVariableIsSet("DBG_TEST_DARK"))
            QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
#endif
        nessession_paths paths = {};
        const QByteArray dir = m_dir.path().toUtf8();
        paths.config_dir = dir.constData();
        paths.data_dir = dir.constData();
        paths.fujinet_lib = "";
        m_s = nessession_new(&paths);
        QVERIFY(m_s);
        m_d = nessession_debugger(m_s);
        QVERIFY(m_d);

        const QString cart = m_dir.path() + QStringLiteral("/loop.nes");
        QVERIFY(engine_rom_write(cart.toLocal8Bit().constData()) > 0);
        {
            /* the loop's LDA #$90 becomes LDA #$10: no NMI to break into a step. $2000 is written $10 each time. */
            QFile image(cart);
            QVERIFY(image.open(QIODevice::ReadWrite));
            QVERIFY(image.seek(16 + 0x10));
            QByteArray data(1, char(0x10));
            QCOMPARE(image.write(data), qint64(1));
        }
        m_cartPath = cart.toUtf8();
        nessession_default_opts(m_s, &m_opts);
        m_opts.cart_path = m_cartPath.constData();
        m_opts.enable_fujinet = 0;
        m_opts.enable_audio = 0;
        m_opts.enable_gamepad = 0;
        QCOMPARE(nessession_start(m_s, &m_opts), 0);

        /* the window opens on the tab NES_DEBUGGER_TAB names (the older "ppu" is the Video tab) */
        qputenv("NES_DEBUGGER_TAB", "ppu");
        DebuggerWindow::showFor(nullptr, m_s);
        qunsetenv("NES_DEBUGGER_TAB");
        m_win = findWindow();
        QVERIFY(m_win);
        QVERIFY(QTest::qWaitForWindowExposed(m_win));
        m_tabs = m_win->findChild<QTabWidget *>();
        QVERIFY(m_tabs);
        QCOMPARE(m_tabs->currentIndex(), 8);
        waitStopped();
    }

    void cleanupTestCase()
    {
        delete m_win;       /* detaches */
        nessession_stop(m_s);
        nessession_free(m_s);
    }

    void init()
    {
        if (!m_win->isVisible()) DebuggerWindow::toggleFor(nullptr, m_s);
        QTRY_VERIFY_WITH_TIMEOUT(nesdebug_is_attached(m_d), 2000);
        if (!nesdebug_is_stopped(m_d)) nesdebug_stop(m_d);
        waitStopped();
        nesdebug_breakpoint_clear(m_d);
        /* a stop lands on an instruction of the loop; put it at the top with the stack it had */
        setPc(kLoop);
        setReg("sp", 0xFF);
        QCheckBox *follow = check(QStringLiteral("Follow PC"));
        if (follow && !follow->isChecked()) follow->click();
    }

    void tabsInOrder()
    {
        QStringList names;
        for (int idx = 0; idx < m_tabs->count(); ++idx) names << tabText(idx);
        QCOMPARE(names, expectedTabs());
        QCOMPARE(m_tabs->currentIndex(), 8);   /* "ppu" is the Video tab */
        QVERIFY(m_win->findChild<VideoTab *>() != nullptr);
        QVERIFY(nesdebug_is_attached(m_d));
        QVERIFY(nesdebug_is_stopped(m_d));
        /* the register strip is under every tab, not in one */
        const QList<QLineEdit *> regs = regFields();
        QCOMPARE(regs.size(), 6);
        for (QLineEdit *edit : regs) QVERIFY(!m_tabs->isAncestorOf(edit));
        /* Run is the accent while stopped, whichever text it wears */
        QPushButton *run = button(QStringLiteral("Run (F5)"));
        QVERIFY(run);
        QVERIFY2(run->styleSheet().contains(nesDebugAccentFill(m_win).name()), qPrintable(run->styleSheet() + nesDebugAccentFill(m_win).name()));
        QTest::mouseClick(run, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!nesdebug_is_stopped(m_d), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(run->text() == QLatin1String("Stop (F5)"), 2000);
        QVERIFY(run->styleSheet().isEmpty());
        QCOMPARE(statusLabel()->text(), QStringLiteral("Running"));
        QTest::mouseClick(run, Qt::LeftButton);
        waitStopped();
        QVERIFY(run->styleSheet().contains(nesDebugAccentFill(m_win).name()));
        QVERIFY(run->styleSheet().contains(nesDebugAccentFillInk(m_win).name()));
    }

    void tabsByName()
    {
        /* each name the env hook takes, in a window of its own */
        discardWindow();
        struct { const char *name; int index; } cases[] = {
            { "prompt", 0 }, { "cpu", 2 }, { "io", 5 }, { "apu", 9 }, { "CART", 5 }, { "3", 3 },
            { "Sound", 9 }, { "console", 0 }, { "hist", 7 }, { "sym", 10 }, { "fuji", 11 }, { "disassembly", 1 } };
        for (const auto &one : cases) {
            qputenv("NES_DEBUGGER_TAB", one.name);
            DebuggerWindow::showFor(nullptr, m_s);
            qunsetenv("NES_DEBUGGER_TAB");
            QMainWindow *win = findWindow();
            QVERIFY(win);
            QVERIFY2(win->findChild<QTabWidget *>()->currentIndex() == one.index, one.name);
            m_win = win;
            discardWindow();
        }
        openWindow();
    }

    void helpContexts()
    {
        static const char *const kContexts[] = { "console", "disassembly", "memory", "breakpoints", "watches", "cart",
                                                 "trace", "history", "video", "sound", "symbols", "fujinet" };
        for (int idx = 0; idx < 12; ++idx) {
            m_tabs->setCurrentIndex(idx);
            QCOMPARE(m_win->property("nesHelpContext").toString(),
                     QStringLiteral("debugger/%1").arg(QLatin1String(kContexts[idx])));
        }
    }

    void fitsASmallScreen()
    {
        QVERIFY2(m_win->minimumSizeHint().height() <= 800,
                 qPrintable(QStringLiteral("minimum height %1").arg(m_win->minimumSizeHint().height())));
        QVERIFY2(m_win->minimumSizeHint().width() <= 950,
                 qPrintable(QStringLiteral("minimum width %1").arg(m_win->minimumSizeHint().width())));
    }

    /* the built-in register names fill the list the first time the tab opens */
    void symbolsFillOnFirstOpen()
    {
        m_tabs->setCurrentIndex(10);
        QListWidget *list = m_tabs->widget(10)->findChild<QListWidget *>();
        QVERIFY(list);
        QTRY_VERIFY_WITH_TIMEOUT(list->count() > 0, 2000);
        QLineEdit *filter = m_tabs->widget(10)->findChild<QLineEdit *>();
        QVERIFY(filter);
        const int all = list->count();
        filter->setText(QStringLiteral("PPUCTRL"));
        QVERIFY(list->count() > 0 && list->count() < all);
        QVERIFY(list->item(0)->text().contains(QStringLiteral("PPUCTRL"), Qt::CaseInsensitive));
        /* Break at toggles an execute breakpoint on the selected symbol */
        list->setCurrentRow(0);
        const unsigned addr = list->item(0)->data(Qt::UserRole).toUInt();
        QPushButton *breakAt = nullptr;
        for (QPushButton *btn : m_tabs->widget(10)->findChildren<QPushButton *>())
            if (btn->text() == QLatin1String("Break at")) breakAt = btn;
        QVERIFY(breakAt);
        breakAt->click();
        QVERIFY(nesdebug_breakpoint_check(m_d, uint16_t(addr)));
        breakAt->click();
        QVERIFY(!nesdebug_breakpoint_check(m_d, uint16_t(addr)));
        filter->clear();
        QCOMPARE(list->count(), all);
    }

    void popOutAndDock()
    {
        m_tabs->setCurrentIndex(9);
        QPushButton *pop = button(QStringLiteral("Pop Out"));
        QVERIFY(pop);
        pop->click();
        QCOMPARE(m_tabs->count(), 11);
        QWidget *window = nullptr;
        for (QWidget *top : QApplication::topLevelWidgets())
            if (top->windowTitle().endsWith(QLatin1String("Sound"))) window = top;
        QVERIFY(window);
        for (QPushButton *btn : window->findChildren<QPushButton *>())
            if (btn->text() == QLatin1String("Dock")) btn->click();
        QCOMPARE(m_tabs->count(), 12);
        QCOMPARE(tabText(9), QStringLiteral("Sound"));
    }

    void stepKeys()
    {
        QCOMPARE(pcOf(m_d), kLoop);
        QTest::keyClick(m_win, Qt::Key_F7);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0xC011, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(statusLabel()->text().left(13), QStringLiteral("Stopped: step"), 2000);
        /* F8 steps over the JSR (reached by stepping: the debugger then knows the instruction it stands on) */
        runTo(0xC01D);
        QTest::keyClick(m_win, Qt::Key_F8);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0xC020, 3000);
        /* F7 into the subroutine, and the button steps out of it */
        runTo(0xC01D);
        QTest::keyClick(m_win, Qt::Key_F7);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0xC030, 2000);
        QTest::mouseClick(button(QStringLiteral("Step Out")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0xC020, 3000);
        /* Frame: Shift+F7 lands on a frame boundary */
        const nesdebug_cpu before = cpuNow();
        QTest::keyClick(m_win, Qt::Key_F7, Qt::ShiftModifier);
        QTRY_VERIFY_WITH_TIMEOUT(nesdebug_is_stopped(m_d) && cpuNow().frame > before.frame, 3000);
        QTRY_VERIFY2_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("frame")), qPrintable(statusLabel()->text()), 2000);
        /* stepping while running is refused with a word, not silently */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!nesdebug_is_stopped(m_d), 2000);
        QTest::keyClick(m_win, Qt::Key_F7);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Stop the machine")) ||
                statusLabel()->text() == QLatin1String("Running"));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
    }

    void consoleCommands()
    {
        m_tabs->setCurrentIndex(0);
        QLineEdit *input = entry(QStringLiteral("command"));
        QPlainTextEdit *out = consoleView();
        QVERIFY(input && out);
        typeInto(input, QStringLiteral("regs"));
        QVERIFY(out->toPlainText().contains(QStringLiteral("> regs")));
        QVERIFY(input->text().isEmpty());
        typeInto(input, QStringLiteral("print $10 + 2"));
        QTRY_VERIFY(consoleText().contains(QStringLiteral("$0012")) || consoleText().contains(QStringLiteral("18")));
        /* a command that moves the machine moves the window */
        typeInto(input, QStringLiteral("set pc $C011"));
        QCOMPARE(pcOf(m_d), 0xC011);
        typeInto(input, QStringLiteral("break $C014"));
        QVERIFY(nesdebug_breakpoint_check(m_d, 0xC014));
        typeInto(input, QStringLiteral("run"));
        QTRY_VERIFY_WITH_TIMEOUT(out->toPlainText().contains(QStringLiteral("— breakpoint: exec $C014")), 3000);
        waitStopped();
        QCOMPARE(pcOf(m_d), 0xC014);
        QVERIFY(statusLabel()->text().contains(QStringLiteral("breakpoint: exec $C014")));
        /* the video command reaches the Video tab's controller */
        typeInto(input, QStringLiteral("video"));
        QTRY_VERIFY_WITH_TIMEOUT(consoleText().contains(QStringLiteral("PPUCTRL")), 3000);
        /* Tab completes a command, lists several, and Up recalls */
        input->clear();
        QTest::keyClicks(input, QStringLiteral("reg"));
        QTest::keyClick(input, Qt::Key_Tab);
        QCOMPARE(input->text(), QStringLiteral("regs "));
        input->clear();
        QTest::keyClicks(input, QStringLiteral("b"));
        QTest::keyClick(input, Qt::Key_Tab);
        QVERIFY(out->toPlainText().contains(QStringLiteral("break")));
        QVERIFY(out->toPlainText().contains(QStringLiteral("bpw")));
        QVERIFY(input->text().startsWith(QLatin1Char('b')));
        input->clear();
        QTest::keyClick(input, Qt::Key_Up);
        QCOMPARE(input->text(), QStringLiteral("video"));
        QTest::keyClick(input, Qt::Key_Up);
        QCOMPARE(input->text(), QStringLiteral("run"));
        QTest::keyClick(input, Qt::Key_Down);
        QCOMPARE(input->text(), QStringLiteral("video"));
        input->clear();
        shot(QStringLiteral("console"));
    }

    void disassemblyHighlights()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *view = disasmView();
        QVERIFY(view);
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(view, kLoop) >= 0, 2000);
        const int pcLine = lineOf(view, kLoop);
        QCOMPARE(view->document()->findBlockByNumber(pcLine).text().left(2), QStringLiteral(" >"));
        /* follow-PC keeps it about a third of the way down */
        const int rows = view->document()->blockCount();
        QVERIFY(pcLine >= rows / 3 - 2 && pcLine <= rows / 3 + 2);
        /* the PC's line: the accent fill (the light variant on a dark theme), applied last */
        auto sels = view->extraSelections();
        QVERIFY(!sels.isEmpty());
        const auto &pcLineSel = sels.last();
        QCOMPARE(pcLineSel.cursor.blockNumber(), pcLine);
        QVERIFY(pcLineSel.format.boolProperty(QTextFormat::FullWidthSelection));
        QCOMPARE(pcLineSel.format.background().color(), nesDebugAccentFill(view));
        QCOMPARE(pcLineSel.format.foreground().color(), nesDebugAccentFillInk(view));
        if (!nesDebugDarkTheme(view)) {
            QCOMPARE(pcLineSel.format.background().color(), nesAccentColor());
            QCOMPARE(pcLineSel.format.foreground().color(), QColor(Qt::white));
        }

        /* a breakpoint's dot: filled and red, hollow and grey when disabled */
        nesdebug_breakpoint_toggle(m_d, 0xC014);
        QTRY_VERIFY_WITH_TIMEOUT(view->document()->findBlockByNumber(lineOf(view, 0xC014)).text().startsWith(
                                     QStringLiteral("●")), 2000);
        auto dotColor = [&](int line) {
            QColor color;
            for (const auto &sel : view->extraSelections())
                if (sel.cursor.blockNumber() == line && sel.cursor.hasSelection() &&
                    sel.cursor.selectionStart() == sel.cursor.block().position() &&
                    sel.cursor.selectionEnd() == sel.cursor.block().position() + 1)
                    color = sel.format.foreground().color();
            return color;
        };
        QCOMPARE(dotColor(lineOf(view, 0xC014)), QColor(0xe0, 0x1b, 0x24));
        nesdebug_breakpoint bpt;
        QCOMPARE(nesdebug_breakpoint_list(m_d, &bpt, 1), 1);
        nesdebug_breakpoint_enable(m_d, bpt.id, 0);
        QTRY_COMPARE_WITH_TIMEOUT(dotColor(lineOf(view, 0xC014)), QColor(0x9a, 0x99, 0x96), 2000);
        shot(QStringLiteral("disassembly"));
    }

    void clickSelectsAndGutterToggles()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *view = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(view, 0xC017) >= 0, 2000);
        /* nothing selected: Run to Cursor and F9 say so */
        QTest::mouseClick(button(QStringLiteral("Run to Cursor")), Qt::LeftButton);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Select a line")));
        /* a click on the text selects (underlined, bold) and sets no breakpoint */
        clickLine(0xC017, 30);
        QVERIFY(!nesdebug_breakpoint_check(m_d, 0xC017));
        {
            bool underlined = false;
            for (const auto &sel : view->extraSelections())
                if (sel.cursor.blockNumber() == lineOf(view, 0xC017) && sel.format.fontUnderline() &&
                    sel.format.fontWeight() == QFont::Bold)
                    underlined = true;
            QVERIFY(underlined);
        }
        /* moving to another line moves the selection; no caret-toggled breakpoints */
        clickLine(0xC01A, 30);
        QVERIFY(!nesdebug_breakpoint_check(m_d, 0xC017));
        QVERIFY(!nesdebug_breakpoint_check(m_d, 0xC01A));
        /* the left edge toggles, a double-click toggles, F9 toggles the selection */
        clickLine(0xC014, 0);
        QVERIFY(nesdebug_breakpoint_check(m_d, 0xC014));
        clickLine(0xC014, 0);
        QVERIFY(!nesdebug_breakpoint_check(m_d, 0xC014));
        clickLine(0xC01D, 30, true);
        QVERIFY(nesdebug_breakpoint_check(m_d, 0xC01D));
        nesdebug_breakpoint_toggle(m_d, 0xC01D);
        clickLine(0xC020, 30);
        QTest::keyClick(m_win, Qt::Key_F9);
        QVERIFY(nesdebug_breakpoint_check(m_d, 0xC020));
        QTest::keyClick(m_win, Qt::Key_F9);
        QVERIFY(!nesdebug_breakpoint_check(m_d, 0xC020));
        nesdebug_breakpoint bpt[4];
        QCOMPARE(nesdebug_breakpoint_list(m_d, bpt, 4), 0);
    }

    void runToCursor()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *view = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(view, 0xC017) >= 0, 2000);
        clickLine(0xC017, 30);
        QTest::mouseClick(button(QStringLiteral("Run to Cursor")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0xC017, 3000);
        /* the selection survives the stop, drawn under the PC's fill */
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(view, 0xC017) >= 0, 2000);
    }

    void wheelJumpAndFollow()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *view = disasmView();
        QCheckBox *follow = check(QStringLiteral("Follow PC"));
        QVERIFY(follow && follow->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(view, kLoop) >= 0, 2000);
        const QPoint point = view->viewport()->rect().center();
        QWheelEvent wheel(point, view->viewport()->mapToGlobal(point), QPoint(), QPoint(0, -120), Qt::NoButton, {},
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(view->viewport(), &wheel);
        QVERIFY(!follow->isChecked());
        QVERIFY(lineOf(view, kLoop) < 0 || lineOf(view, kLoop) < view->document()->blockCount() / 3 - 1);
        /* the jump box: bare digits are hex here, a label wins first, then expressions */
        QLineEdit *jump = entry(QStringLiteral("$address"));
        QVERIFY(jump);
        typeInto(jump, QStringLiteral("C030"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(view, 0xC030) >= 0, 2000);
        nesdebug_set_label(m_d, 0xC017, "back");
        typeInto(jump, QStringLiteral("back"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(view, 0xC017) >= 0, 2000);
        QVERIFY(view->toPlainText().contains(QStringLiteral("back")));
        typeInto(jump, QStringLiteral("$C030+3"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(view, 0xC033) >= 0, 2000);
        typeInto(jump, QStringLiteral("nosuchlabel"));
        QVERIFY(!m_win->statusBar()->currentMessage().isEmpty());
        /* Follow PC again returns to the PC */
        QTest::mouseClick(follow, Qt::LeftButton, {}, QPoint(4, follow->height() / 2));
        QVERIFY(follow->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(view, kLoop) >= 0, 2000);
        nesdebug_set_label(m_d, 0xC017, nullptr);
    }

    /* Symbols > Next to the cartridge: loop.lbl beside loop.nes */
    void symbolsNextToTheCartridge()
    {
        m_tabs->setCurrentIndex(0);
        QAction *next = nullptr;
        for (QAction *action : m_win->findChildren<QAction *>())
            if (action->text() == QLatin1String("Next to the cartridge")) next = action;
        QVERIFY(next);
        next->trigger();                    /* no file yet: said, not silent */
        QVERIFY(consoleView()->toPlainText().trimmed().split(QLatin1Char('\n')).size() >= 2);
        QCOMPARE(nesdebug_label_address(m_d, "afterwards"), -1);
        QFile sym(m_dir.path() + QStringLiteral("/loop.lbl"));
        QVERIFY(sym.open(QIODevice::WriteOnly));
        sym.write("al C:C020 .afterwards\n");
        sym.close();
        next->trigger();
        QCOMPARE(nesdebug_label_address(m_d, "afterwards"), 0xC020);
        QVERIFY(consoleView()->toPlainText().contains(QStringLiteral("1 labels loaded")));
        /* the Symbols tab shows it; activating the row shows it in the Disassembly */
        m_tabs->setCurrentIndex(10);
        QListWidget *list = m_tabs->widget(10)->findChild<QListWidget *>();
        QTRY_VERIFY_WITH_TIMEOUT(!list->findItems(QStringLiteral("C020  afterwards"), Qt::MatchContains).isEmpty(), 2000);
        list->setCurrentItem(list->findItems(QStringLiteral("C020  afterwards"), Qt::MatchContains).first());
        emit list->itemActivated(list->currentItem());
        QCOMPARE(m_tabs->currentIndex(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(disasmView(), 0xC020) >= 0, 2000);
        QVERIFY(disasmView()->toPlainText().contains(QStringLiteral("afterwards")));
        m_tabs->setCurrentIndex(10);
        /* Clear loaded symbols (in the toolbar's menu) drops it again */
        QAction *clear = nullptr;
        for (QAction *action : m_win->findChildren<QAction *>())
            if (action->text() == QLatin1String("Clear loaded symbols")) clear = action;
        QVERIFY(clear);
        clear->trigger();
        QCOMPARE(nesdebug_label_address(m_d, "afterwards"), -1);
        QTRY_VERIFY_WITH_TIMEOUT(list->findItems(QStringLiteral("C020  afterwards"), Qt::MatchContains).isEmpty(), 2000);
        QFile::remove(m_dir.path() + QStringLiteral("/loop.lbl"));
    }

    void registerStrip()
    {
        /* under every tab: the Disassembly and the Breakpoints show it as well */
        for (int tab : { 0, 1, 3, 11 }) {
            m_tabs->setCurrentIndex(tab);
            QVERIFY(regFields().first()->isVisible());
        }
        m_tabs->setCurrentIndex(2);
        QList<QLineEdit *> regs = regFields();
        QCOMPARE(regs.size(), 6);                  /* PC SP A X Y P; the flags are the boxes */
        QTRY_COMPARE_WITH_TIMEOUT(regs[0]->text(), QStringLiteral("C00F"), 2000);   /* PC */
        QCOMPARE(regs[1]->text(), QStringLiteral("FF"));
        typeInto(regs[0], QStringLiteral("C017"));
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0xC017, 2000);
        typeInto(regs[2], QStringLiteral("5a"));    /* A */
        QCOMPARE(cpuNow().a, 0x5A);
        /* an expression, the register taken from another */
        typeInto(regs[3], QStringLiteral("a + 1"));
        QCOMPARE(cpuNow().x, 0x5B);
        typeInto(regs[4], QStringLiteral("$21"));
        QCOMPARE(cpuNow().y, 0x21);
        /* a value too wide for the register is refused and leaves it alone */
        typeInto(regs[4], QStringLiteral("1FF"));
        QCOMPARE(cpuNow().y, 0x21);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("holds")));
        typeInto(regs[1], QStringLiteral("F0"));
        QCOMPARE(cpuNow().sp, 0xF0);
        /* P edits all the flags at once; the boxes follow */
        typeInto(regs[5], QStringLiteral("85"));    /* N Z... 0x85 = N, I(4)? bits 7,2,0 */
        QCOMPARE(cpuNow().ps & 0xC7, 0x85);
        QTRY_VERIFY_WITH_TIMEOUT(check(QStringLiteral("N"))->isChecked() && check(QStringLiteral("C"))->isChecked(), 2000);
        /* the flags */
        QCheckBox *carry = check(QStringLiteral("C"));
        QVERIFY(carry);
        nesdebug_cpu_set(m_d, NES_FLAG_C, 0);
        QTRY_VERIFY_WITH_TIMEOUT(!carry->isChecked(), 2000);
        QTest::mouseClick(carry, Qt::LeftButton, {}, QPoint(6, carry->height() / 2));
        QCOMPARE(cpuNow().c, 1);
        QCheckBox *zero = check(QStringLiteral("Z"));
        QVERIFY(zero);
        nesdebug_cpu_set(m_d, NES_FLAG_Z, 0);
        QTRY_VERIFY_WITH_TIMEOUT(!zero->isChecked(), 2000);
        QTest::mouseClick(zero, Qt::LeftButton, {}, QPoint(6, zero->height() / 2));
        QCOMPARE(cpuNow().z, 1);
        QTest::mouseClick(zero, Qt::LeftButton, {}, QPoint(6, zero->height() / 2));
        QCOMPARE(cpuNow().z, 0);
        nesdebug_cpu_set(m_d, NES_FLAG_C, 0);
        /* the beam note and the lines */
        QLabel *beam = nullptr;
        for (QLabel *label : m_win->findChildren<QLabel *>())
            if (label->text().startsWith(QLatin1String("cycle "))) beam = label;
        QVERIFY(beam);
        QVERIFY2(beam->text().contains(QRegularExpression(QStringLiteral("cycle \\d+  line \\d+  dot \\d+  frame \\d+"))),
                 qPrintable(beam->text()));
        /* edits need a stopped machine */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!nesdebug_is_stopped(m_d), 2000);
        typeInto(regs[0], QStringLiteral("C000"));
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Stop the machine")));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        shot(QStringLiteral("registers"));
    }

    void memorySpacesPokeAndSearch()
    {
        m_tabs->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QComboBox *space = m_tabs->widget(2)->findChild<QComboBox *>();
        QVERIFY(space);
        QStringList spaces;
        for (int idx = 0; idx < space->count(); ++idx) spaces << space->itemText(idx);
        QCOMPARE(spaces, (QStringList{ QStringLiteral("CPU"), QStringLiteral("PPU"), QStringLiteral("OAM"),
                                       QStringLiteral("Palette"), QStringLiteral("PRG"), QStringLiteral("CHR"),
                                       QStringLiteral("WRAM") }));
        QLineEdit *addr = entryIn(2, QStringLiteral("address"));
        QLineEdit *val = entryIn(2, QStringLiteral("value"));
        QVERIFY(addr && val);
        addr->setText(QStringLiteral("0310"));       /* hex */
        typeInto(val, QStringLiteral("5a"));
        QVERIFY2(peek(NESDEBUG_SPACE_CPU, 0x310) == 0x5A, qPrintable(m_win->statusBar()->currentMessage()));
        addr->setText(QStringLiteral("$0311"));
        typeInto(val, QStringLiteral("$A5"));
        QCOMPARE((int)peek(NESDEBUG_SPACE_CPU, 0x311), 0xA5);
        nesdebug_write(m_d, 0x312, 0x11);
        addr->setText(QStringLiteral("0312"));
        typeInto(val, QStringLiteral("1FF"));        /* not a byte: refused */
        QCOMPARE((int)peek(NESDEBUG_SPACE_CPU, 0x312), 0x11);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("byte")));
        /* Go to marks the row in the accent */
        QLineEdit *gotoBox = entryIn(2, QStringLiteral("$8000, label"));
        QVERIFY(gotoBox);
        typeInto(gotoBox, QStringLiteral("C000"));
        auto *mem = m_tabs->widget(2)->findChildren<QPlainTextEdit *>().last();
        QVERIFY(mem->toPlainText().contains(QStringLiteral("$C000: ")));
        QVERIFY(mem->toPlainText().contains(QStringLiteral("78 D8 A2 FF 9A")));
        bool marked = false;
        for (const auto &sel : mem->extraSelections())
            marked |= sel.format.background().color() == nesDebugAccentFill(mem) &&
                      sel.format.foreground().color() == nesDebugAccentFillInk(mem);
        QVERIFY(marked);
        /* the rows are labelled with the region they are in */
        QVERIFY2(mem->toPlainText().contains(QStringLiteral("; ")), "region names");
        /* search finds the program's LDA #$90 / STA $2000 */
        typeInto(gotoBox, QStringLiteral("0000"));
        QLineEdit *find = entryIn(2, QStringLiteral("bytes or text"));
        QVERIFY(find);
        typeInto(find, QStringLiteral("A9 10 8D 00 20"));
        QLabel *note = nullptr;
        for (QLabel *label : m_tabs->widget(2)->findChildren<QLabel *>())
            if (label->text().startsWith(QLatin1String("Found")) || label->text().startsWith(QLatin1String("Not found"))) note = label;
        QVERIFY(note);
        QVERIFY2(note->text().contains(QRegularExpression(QStringLiteral("Found at \\$[8C]005"))), qPrintable(note->text()));
        /* text search, and a miss */
        auto *kind = m_tabs->widget(2)->findChildren<QComboBox *>().last();
        QVERIFY(kind);
        kind->setCurrentIndex(1);
        typeInto(find, QStringLiteral("zzzzqq"));
        QVERIFY2(note->text().startsWith(QLatin1String("Not found")), qPrintable(note->text()));
        kind->setCurrentIndex(0);

        /* the PPU space: the palette entry the loop writes through $2007, poked and searched */
        space->setCurrentIndex(space->findText(QStringLiteral("Palette")));
        QTRY_VERIFY_WITH_TIMEOUT(mem->toPlainText().contains(QStringLiteral("$0000: ")), 2000);
        QVERIFY(mem->toPlainText().contains(QStringLiteral("background palettes")));
        addr->setText(QStringLiteral("1E"));
        typeInto(val, QStringLiteral("2C"));
        QCOMPARE((int)peek(NESDEBUG_SPACE_PALETTE, 0x1E), 0x2C);
        space->setCurrentIndex(space->findText(QStringLiteral("OAM")));
        QTRY_VERIFY_WITH_TIMEOUT(mem->toPlainText().contains(QStringLiteral("sprites 0-3")), 2000);
        addr->setText(QStringLiteral("10"));
        typeInto(val, QStringLiteral("77"));
        QCOMPARE((int)peek(NESDEBUG_SPACE_OAM, 0x10), 0x77);
        typeInto(find, QStringLiteral("77"));
        QVERIFY2(note->text().contains(QStringLiteral("Found at $0010")), qPrintable(note->text()));
        space->setCurrentIndex(space->findText(QStringLiteral("PPU")));
        QTRY_VERIFY_WITH_TIMEOUT(mem->toPlainText().contains(QStringLiteral("pattern table 0")), 2000);
        typeInto(gotoBox, QStringLiteral("2000-2003"));
        QVERIFY(mem->toPlainText().contains(QStringLiteral("$2000: ")));
        QVERIFY(mem->toPlainText().contains(QStringLiteral("nametable 0")));
        shot(QStringLiteral("memory"));
        /* PRG: the cartridge's own image, however large */
        space->setCurrentIndex(space->findText(QStringLiteral("PRG")));
        QTRY_VERIFY2_WITH_TIMEOUT(mem->toPlainText().contains(QStringLiteral("$000000: ")), qPrintable(mem->toPlainText().left(200)), 2000);
        /* the arrows page by 4K, clamped to the space */
        QToolButton *down = nullptr;
        for (QToolButton *arrow : m_tabs->widget(2)->findChildren<QToolButton *>())
            if (arrow->arrowType() == Qt::DownArrow) down = arrow;
        QVERIFY(down);
        down->click();
        QVERIFY(mem->toPlainText().contains(QStringLiteral("$001000: ")));
        for (int idx = 0; idx < 200; ++idx) down->click();
        QVERIFY(mem->toPlainText().contains(QStringLiteral("$07FFF0: ")));
        /* CHR and WRAM answer too */
        space->setCurrentIndex(space->findText(QStringLiteral("WRAM")));
        QTRY_VERIFY_WITH_TIMEOUT(mem->toPlainText().contains(QStringLiteral("$0000: ")), 2000);
        space->setCurrentIndex(0);
    }

    void breakpointsTab()
    {
        m_tabs->setCurrentIndex(3);
        QTableWidget *table = bpTable();
        QVERIFY(table);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QCOMPARE(table->rowCount(), 0);
        QLineEdit *start = entry(QStringLiteral("start"));
        QLineEdit *end = entry(QStringLiteral("end"));
        QLineEdit *cond = entry(QStringLiteral("condition"));
        QVERIFY(start && end && cond);
        const QList<QComboBox *> combos = m_tabs->currentWidget()->findChildren<QComboBox *>();
        QCOMPARE(combos.size(), 2);
        QComboBox *type = combos[0], *space = combos[1];
        QCOMPARE(type->count(), 4);
        QCOMPARE(type->itemText(0), QStringLiteral("Execute"));
        QCOMPARE(type->itemText(3), QStringLiteral("Access"));
        QStringList spaces;
        for (int idx = 0; idx < space->count(); ++idx) spaces << space->itemText(idx);
        QCOMPARE(spaces, (QStringList{ QStringLiteral("CPU"), QStringLiteral("PPU"), QStringLiteral("Palette"),
                                       QStringLiteral("OAM") }));
        QPushButton *add = addButton(3);
        QPushButton *clear = nullptr;
        for (QPushButton *btn : m_tabs->currentWidget()->findChildren<QPushButton *>())
            if (btn->text() == QLatin1String("Clear All")) clear = btn;
        QLabel *msg = nullptr;
        for (QLabel *label : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (label->styleSheet().contains(QLatin1String("e01b24"))) msg = label;
        QVERIFY(add && clear && msg);

        /* exec at a hex address */
        typeInto(start, QStringLiteral("C014"));
        QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 1, 2000);
        QCOMPARE(table->item(0, 1)->text(), QStringLiteral("exec"));
        QCOMPARE(table->item(0, 2)->text(), QStringLiteral("CPU"));
        QCOMPARE(table->item(0, 3)->text(), QStringLiteral("$C014"));
        QCOMPARE(table->item(0, 0)->checkState(), Qt::Checked);
        QVERIFY(start->text().isEmpty());
        /* a write breakpoint over a range, with a condition on the byte and the old one */
        type->setCurrentText(QStringLiteral("Write"));
        start->setText(QStringLiteral("0301"));
        end->setText(QStringLiteral("$0302"));
        cond->setText(QStringLiteral("value != old"));
        QTest::mouseClick(add, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 2, 2000);
        QCOMPARE(table->item(1, 1)->text(), QStringLiteral("write"));
        QCOMPARE(table->item(1, 3)->text(), QStringLiteral("$0301-$0302"));
        QCOMPARE(table->item(1, 4)->text(), QStringLiteral("value != old"));
        /* errors are shown, not swallowed */
        cond->setText(QStringLiteral("nosuchname == 1"));
        start->setText(QStringLiteral("0700"));
        end->clear();
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(!msg->text().isEmpty());
        QCOMPARE(table->rowCount(), 2);
        /* old only exists in CPU space; execute only there */
        space->setCurrentIndex(space->findText(QStringLiteral("PPU")));
        cond->setText(QStringLiteral("old == 1"));
        start->setText(QStringLiteral("2000"));
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(!msg->text().isEmpty());
        QCOMPARE(table->rowCount(), 2);
        msg->clear();
        cond->clear();
        type->setCurrentText(QStringLiteral("Execute"));
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(!msg->text().isEmpty());
        QCOMPARE(table->rowCount(), 2);
        msg->clear();
        /* a space of its own: palette bytes are $00-$1F, OAM bytes $00-$FF, shown with two digits */
        type->setCurrentText(QStringLiteral("Write"));
        space->setCurrentIndex(space->findText(QStringLiteral("Palette")));
        start->setText(QStringLiteral("01"));
        QTest::mouseClick(add, Qt::LeftButton);
        space->setCurrentIndex(space->findText(QStringLiteral("OAM")));
        type->setCurrentText(QStringLiteral("Access"));
        start->setText(QStringLiteral("0"));
        end->setText(QStringLiteral("3"));
        QTest::mouseClick(add, Qt::LeftButton);
        space->setCurrentIndex(space->findText(QStringLiteral("PPU")));
        type->setCurrentText(QStringLiteral("Write"));
        start->setText(QStringLiteral("2000"));
        end->setText(QStringLiteral("23FF"));
        QTest::mouseClick(add, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 5, 2000);
        QVERIFY2(msg->text().isEmpty(), qPrintable(msg->text()));
        QCOMPARE(table->item(2, 2)->text(), QStringLiteral("Palette"));
        QCOMPARE(table->item(2, 3)->text(), QStringLiteral("$01"));
        QCOMPARE(table->item(3, 1)->text(), QStringLiteral("access"));
        QCOMPARE(table->item(3, 2)->text(), QStringLiteral("OAM"));
        QCOMPARE(table->item(3, 3)->text(), QStringLiteral("$00-$03"));
        QCOMPARE(table->item(4, 2)->text(), QStringLiteral("PPU"));
        QCOMPARE(table->item(4, 3)->text(), QStringLiteral("$2000-$23FF"));
        nesdebug_breakpoint list[8];
        QCOMPARE(nesdebug_breakpoint_list(m_d, list, 8), 5);
        QCOMPARE(list[3].type, int(NESDEBUG_BP_ACCESS));
        QCOMPARE(list[3].space, int(NESDEBUG_SPACE_OAM));
        /* an edited condition goes to the engine */
        table->item(1, 4)->setText(QStringLiteral("value > 1"));
        QTRY_VERIFY_WITH_TIMEOUT((nesdebug_breakpoint_list(m_d, list, 8),
                                  QString::fromUtf8(list[1].condition) == QLatin1String("value > 1")), 2000);
        for (int bpid : { list[2].id, list[3].id, list[4].id }) nesdebug_breakpoint_remove(m_d, bpid);
        QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 2, 2000);
        start->clear();
        end->clear();
        msg->clear();
        type->setCurrentText(QStringLiteral("Execute"));
        space->setCurrentIndex(0);

        /* the exec breakpoint stops the run, with its hits counted */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(nesdebug_is_stopped(m_d) && pcOf(m_d) == 0xC014, 3000);
        QTRY_VERIFY2_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("exec $C014")), qPrintable(statusLabel()->text()), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(table->item(0, 5) && table->item(0, 5)->text().toInt() >= 1, 2000);
        /* Clear Hits zeroes the counts */
        QPushButton *hits = button(QStringLiteral("Clear Hits"));
        QVERIFY(hits);
        QTest::mouseClick(hits, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(table->item(0, 5)->text(), QStringLiteral("0"), 2000);
        /* unticking it lets the run pass; the write breakpoint (the byte after INC differs) stops it instead */
        table->item(0, 0)->setCheckState(Qt::Unchecked);
        QCOMPARE(nesdebug_breakpoint_list(m_d, list, 8), 2);
        QCOMPARE(list[0].enabled, 0);
        QCOMPARE(list[1].enabled, 1);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(nesdebug_is_stopped(m_d) && statusLabel()->text().contains(QLatin1String("wrote $0301")),
                                 4000);
        QVERIFY2(statusLabel()->text().contains(QLatin1String("(was $")), qPrintable(statusLabel()->text()));
        QTRY_VERIFY_WITH_TIMEOUT(consoleText().contains(QStringLiteral("wrote $0301")), 2000);
        shot(QStringLiteral("breakpoints"));
        /* the row's delete button */
        auto *removeBtn = qobject_cast<QToolButton *>(table->cellWidget(1, 6));
        QVERIFY(removeBtn);
        QTest::mouseClick(removeBtn, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 1, 2000);
        QCOMPARE(nesdebug_breakpoint_list(m_d, list, 8), 1);
        QTest::mouseClick(clear, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 0, 2000);
        QCOMPARE(nesdebug_breakpoint_list(m_d, list, 8), 0);
    }

    /* a write breakpoint on a PPU register with a condition on both bytes, stopping in the core's words:
     * the loop's STA $2000 stores $10 over $10 */
    void breakpointOnAPpuRegister()
    {
        m_tabs->setCurrentIndex(3);
        QLineEdit *addr = entry(QStringLiteral("start ("));
        QLineEdit *cond = entry(QStringLiteral("condition"));
        QVERIFY(addr && cond);
        addr->setText(QStringLiteral("$2000"));
        cond->setText(QStringLiteral("value == $10 && old == $10"));
        QComboBox *type = m_tabs->widget(3)->findChildren<QComboBox *>().first();
        type->setCurrentIndex(2);   /* Write */
        addButton(3)->click();
        nesdebug_breakpoint bps[4];
        QCOMPARE(nesdebug_breakpoint_list(m_d, bps, 4), 1);
        QCOMPARE(bps[0].type, int(NESDEBUG_BP_WRITE));
        QCOMPARE(QString::fromUtf8(bps[0].condition), QStringLiteral("value == $10 && old == $10"));
        /* a condition that does not parse is refused */
        addr->setText(QStringLiteral("$2001"));
        cond->setText(QStringLiteral("value =="));
        addButton(3)->click();
        QCOMPARE(nesdebug_breakpoint_list(m_d, bps, 4), 1);
        type->setCurrentIndex(0);
        /* run the loop: it stops before the store, the byte in the core's words */
        nesdebug_resume(m_d);
        nesdebug_snapshot snap;
        QTRY_VERIFY_WITH_TIMEOUT((nesdebug_stop_snapshot(m_d, &snap), snap.paused), 4000);
        QVERIFY2(QString::fromUtf8(snap.text).contains(QStringLiteral("wrote $2000 = $10 (was $10)")), snap.text);
        QTRY_VERIFY_WITH_TIMEOUT(button(QStringLiteral("Run (F5)")) != nullptr, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(consoleText().contains(QStringLiteral("wrote $2000 = $10")), 2000);
        nesdebug_breakpoint_clear(m_d);
    }

    void watchesAndBreakOnChange()
    {
        neswatch_clear();
        m_tabs->setCurrentIndex(4);
        QLineEdit *entryBox = m_tabs->widget(4)->findChild<QLineEdit *>();
        QVERIFY(entryBox);
        entryBox->setText(QStringLiteral("$0300"));
        QTest::keyClick(entryBox, Qt::Key_Return);
        QCOMPARE(neswatch_count(), 1);
        QListWidget *list = m_tabs->widget(4)->findChild<QListWidget *>();
        QVERIFY(list);
        QTRY_COMPARE(list->count(), 1);
        QVERIFY(list->item(0)->text().contains(QStringLiteral("0300")));
        list->setCurrentRow(0);
        for (QPushButton *btn : m_tabs->widget(4)->findChildren<QPushButton *>())
            if (btn->text() == QLatin1String("Break on Change")) btn->click();
        nesdebug_breakpoint bps[4];
        QTRY_COMPARE(nesdebug_breakpoint_list(m_d, bps, 4), 1);
        QCOMPARE(QString::fromUtf8(bps[0].condition), QStringLiteral("value != old"));
        QCOMPARE(bps[0].type, int(NESDEBUG_BP_WRITE));
        /* Memory shows the watch, marked */
        for (QPushButton *btn : m_tabs->widget(4)->findChildren<QPushButton *>())
            if (btn->text() == QLatin1String("Memory")) btn->click();
        QCOMPARE(m_tabs->currentIndex(), 2);
        m_tabs->setCurrentIndex(4);
        /* Remove */
        for (QPushButton *btn : m_tabs->widget(4)->findChildren<QPushButton *>())
            if (btn->text() == QLatin1String("Remove")) btn->click();
        QCOMPARE(neswatch_count(), 0);
        QTRY_COMPARE(list->count(), 0);
        nesdebug_breakpoint_clear(m_d);
        neswatch_clear();
    }

    void traceAndHistory()
    {
        m_tabs->setCurrentIndex(7);
        QPushButton *record = nullptr;
        for (QPushButton *btn : m_tabs->widget(7)->findChildren<QPushButton *>())
            if (btn->text() == QLatin1String("Record")) record = btn;
        QVERIFY(record);
        record->click();
        QVERIFY(nesdebug_history_enabled(m_d));
        QVERIFY(loadProgram());
        for (int idx = 0; idx < 4; ++idx) {
            nesdebug_step(m_d);
            QTest::qWait(60);
        }
        QTRY_VERIFY_WITH_TIMEOUT(nesdebug_history_count(m_d) >= 4, 3000);
        m_tabs->setCurrentIndex(0);
        m_tabs->setCurrentIndex(7);
        auto *text = m_tabs->widget(7)->findChild<QPlainTextEdit *>();
        QVERIFY(text);
        QTRY_VERIFY_WITH_TIMEOUT(text->toPlainText().contains(QStringLiteral("040")), 3000);
        /* the filter: branches only leaves the NOPs out */
        auto *filter = m_tabs->widget(7)->findChild<QComboBox *>();
        QVERIFY(filter);
        filter->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(!text->toPlainText().contains(QStringLiteral("NOP")), 3000);
        filter->setCurrentIndex(0);
        QTRY_VERIFY_WITH_TIMEOUT(text->toPlainText().contains(QStringLiteral("NOP")), 3000);
        record->click();
        QVERIFY(!nesdebug_history_enabled(m_d));
        nesdebug_history_clear(m_d);
        /* Trace is live: Record on, run, instructions arrive */
        m_tabs->setCurrentIndex(6);
        QPushButton *traceRec = nullptr;
        for (QPushButton *btn : m_tabs->widget(6)->findChildren<QPushButton *>())
            if (btn->text() == QLatin1String("Record")) traceRec = btn;
        QVERIFY(traceRec);
        traceRec->click();
        QVERIFY(nesdebug_trace_enabled(m_d));
        setPc(kLoop);
        setReg("sp", 0xFF);
        nesdebug_resume(m_d);
        auto *live = m_tabs->widget(6)->findChild<QPlainTextEdit *>();
        QTRY_VERIFY_WITH_TIMEOUT(live->toPlainText().contains(QStringLiteral("JSR")), 4000);
        traceRec->click();
        QVERIFY(!nesdebug_trace_enabled(m_d));
        nesdebug_trace_clear(m_d);
        nesdebug_stop(m_d);
        waitStopped();
    }

    void videoSoundAndCart()
    {
        m_tabs->setCurrentIndex(8);
        VideoTab *video = m_win->findChild<VideoTab *>();
        QVERIFY(video);
        video->refresh(true);
        QVERIFY(video->findChild<QComboBox *>() != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(video->findChild<QTabWidget *>() != nullptr, 1000);
        shot(QStringLiteral("video"));
        /* Show in Memory from a pinned pixel is a signal the window turns into the Memory tab */
        m_tabs->setCurrentIndex(9);
        QTableWidget *sound = m_tabs->widget(9)->findChild<QTableWidget *>();
        QVERIFY(sound);
        QTRY_VERIFY_WITH_TIMEOUT(sound->item(0, 0) && sound->item(0, 0)->text() == QLatin1String("Pulse 1"), 3000);
        QCOMPARE(sound->rowCount(), 5);
        QCOMPARE(sound->item(2, 0)->text(), QStringLiteral("Triangle"));
        QCOMPARE(sound->item(4, 0)->text(), QStringLiteral("DMC"));
        shot(QStringLiteral("sound"));
        m_tabs->setCurrentIndex(5);
        QString ioText;
        QTRY_VERIFY_WITH_TIMEOUT(!m_tabs->widget(5)->findChildren<QPlainTextEdit *>().isEmpty(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!m_tabs->widget(5)->findChildren<QPlainTextEdit *>().first()->toPlainText().isEmpty(), 2000);
        for (QPlainTextEdit *view : m_tabs->widget(5)->findChildren<QPlainTextEdit *>()) ioText += view->toPlainText();
        QVERIFY2(ioText.contains(QStringLiteral("memory map")), qPrintable(ioText));
        QVERIFY2(ioText.contains(QStringLiteral("PRG slots")), qPrintable(ioText));
        QVERIFY(ioText.contains(QStringLiteral("Controllers")));
        QVERIFY(ioText.contains(QStringLiteral("Expansion port")));
        QVERIFY(ioText.contains(QStringLiteral("PPU")));
        shot(QStringLiteral("cart"));
        /* FujiNet: empty without the runtime, and says so */
        m_tabs->setCurrentIndex(11);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        bool note = false;
        for (QLabel *label : m_tabs->currentWidget()->findChildren<QLabel *>())
            note |= label->text().contains(QLatin1String("FujiNet is not running"));
        QVERIFY(note);
        shot(QStringLiteral("fujinet"));
        m_tabs->setCurrentIndex(0);
    }

    /* NES_DEBUGGER_POPOUT peels a tab off at the start */
    void popoutEnv()
    {
        discardWindow();
        qputenv("NES_DEBUGGER_POPOUT", "sound");
        DebuggerWindow::showFor(nullptr, m_s);
        qunsetenv("NES_DEBUGGER_POPOUT");
        m_win = findWindow();
        QVERIFY(m_win);
        m_tabs = m_win->findChild<QTabWidget *>();
        QCOMPARE(m_tabs->count(), 11);
        QWidget *window = nullptr;
        for (QWidget *top : QApplication::topLevelWidgets())
            if (top->windowTitle().endsWith(QLatin1String("Sound"))) window = top;
        QVERIFY(window);
        discardWindow();
        for (QWidget *top : QApplication::topLevelWidgets())
            if (top->windowTitle().endsWith(QLatin1String("Sound"))) top->close();
        openWindow();
    }

    /* the video env hooks reach the tab */
    void videoEnv()
    {
        discardWindow();
        qputenv("NES_DEBUGGER_TAB", "video");
        qputenv("NES_DEBUGGER_VIDEO_SECTION", "registers");
        qputenv("NES_DEBUGGER_VIDEO_VIEW", "decoded");
        qputenv("NES_DEBUGGER_VIDEO_SOURCE", "patterns");
        qputenv("NES_DEBUGGER_VIDEO_ZOOM", "2");
        DebuggerWindow::showFor(nullptr, m_s);
        qunsetenv("NES_DEBUGGER_TAB");
        qunsetenv("NES_DEBUGGER_VIDEO_SECTION");
        qunsetenv("NES_DEBUGGER_VIDEO_VIEW");
        qunsetenv("NES_DEBUGGER_VIDEO_SOURCE");
        qunsetenv("NES_DEBUGGER_VIDEO_ZOOM");
        m_win = findWindow();
        QVERIFY(m_win);
        QVERIFY(QTest::qWaitForWindowExposed(m_win));
        m_tabs = m_win->findChild<QTabWidget *>();
        QCOMPARE(m_tabs->currentIndex(), 8);
        VideoTab *video = m_win->findChild<VideoTab *>();
        QVERIFY(video);
        waitStopped();
        QTest::qWait(300);
        const QList<QComboBox *> combos = video->findChildren<QComboBox *>();
        QVERIFY(!combos.isEmpty());
        QCOMPARE(combos[0]->currentText(), QStringLiteral("Decoded"));
        QCOMPARE(combos[1]->currentText(), QStringLiteral("Patterns"));
        shot(QStringLiteral("video-env"));
        discardWindow();
        openWindow();
    }

    /* NES_DEBUGGER_WATCH, _WATCH_BREAK, _MEM_GOTO and _HISTORY act in a fresh window */
    void envHooks()
    {
        nesdebug_breakpoint_clear(m_d);
        neswatch_clear();
        discardWindow();
        qputenv("NES_DEBUGGER_TAB", "watches");
        qputenv("NES_DEBUGGER_WATCH", "$0300");
        qputenv("NES_DEBUGGER_WATCH_BREAK", "$0300");
        qputenv("NES_DEBUGGER_MEM_GOTO", "$0300");
        qputenv("NES_DEBUGGER_HISTORY", "1");
        DebuggerWindow::showFor(nullptr, m_s);
        qunsetenv("NES_DEBUGGER_TAB");
        qunsetenv("NES_DEBUGGER_WATCH");
        qunsetenv("NES_DEBUGGER_WATCH_BREAK");
        qunsetenv("NES_DEBUGGER_MEM_GOTO");
        qunsetenv("NES_DEBUGGER_HISTORY");
        m_win = findWindow();
        QVERIFY(m_win);
        m_tabs = m_win->findChild<QTabWidget *>();
        QTRY_COMPARE_WITH_TIMEOUT(neswatch_count(), 1, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(nesdebug_history_enabled(m_d), 3000);
        nesdebug_breakpoint bps[4];
        QTRY_COMPARE_WITH_TIMEOUT(nesdebug_breakpoint_list(m_d, bps, 4), 1, 3000);
        QCOMPARE(QString::fromUtf8(bps[0].condition), QStringLiteral("value != old"));
        /* MEM_GOTO put its address in the Memory view (the tab stays the one asked for) */
        QCOMPARE(m_tabs->currentIndex(), 4);
        m_tabs->setCurrentIndex(2);
        auto *dump = m_tabs->widget(2)->findChildren<QPlainTextEdit *>().last();
        QTRY_VERIFY_WITH_TIMEOUT(dump->toPlainText().contains(QStringLiteral("$0200:")), 3000);
        nesdebug_history_enable(m_d, 0);
        nesdebug_breakpoint_clear(m_d);
        neswatch_clear();
    }

    /* DBG_TEST_SHOTS=<dir>: a PNG of every tab */
    void shots()
    {
        if (m_shots.isEmpty()) QSKIP("DBG_TEST_SHOTS not set");
        for (int idx = 0; idx < m_tabs->count(); ++idx) {
            m_tabs->setCurrentIndex(idx);
            QTest::qWait(250);
            QVERIFY(m_win->grab().save(QStringLiteral("%1/tab%2.png").arg(m_shots).arg(idx)));
        }
    }

    /* The main window's F12 (display focused: its ShortcutOverride lets the
     * chord through to the View menu's action) and the View menu entry. */
    void f12FromTheMainWindow()
    {
        /* the display is a QRhiWidget; with no GPU device (CI's offscreen Qt)
         * the window swaps in the software display, so ask for whichever
         * is there */
        QVERIFY(QFile::exists(QStringLiteral(":/shaders/display.vert.qsb")));
        QVERIFY(QFile::exists(QStringLiteral(":/shaders/display.frag.qsb")));
        MainWindow main(m_s);
        main.show();
        QVERIFY(QTest::qWaitForWindowExposed(&main));
        main.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&main));
        QTest::qWait(300);   /* let a queued renderFailed swap the display */
        QWidget *display = main.centralWidget();
        QVERIFY(display);
        display->setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(QApplication::focusWidget() == display, 2000);
        QCOMPARE(QApplication::focusWidget(), display);
        QVERIFY(m_win->isVisible());
        QTest::keyClick(display, Qt::Key_F12);
        QVERIFY(!m_win->isVisible());
        QVERIFY(!nesdebug_is_attached(m_d));
        main.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&main));
        QTest::keyClick(display, Qt::Key_F12);
        QVERIFY(m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(nesdebug_is_attached(m_d), 2000);
        waitStopped();
        /* the menu entry does the same */
        QAction *entry = nullptr;
        for (QAction *action : main.findChildren<QAction *>())
            if (action->text() == QLatin1String("&Debugger") || action->text() == QLatin1String("Debugger")) entry = action;
        QVERIFY(entry);
        QCOMPARE(entry->shortcut(), QKeySequence(Qt::Key_F12));
        entry->trigger();
        QVERIFY(!m_win->isVisible());
        entry->trigger();
        QVERIFY(m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(nesdebug_is_attached(m_d), 2000);
        waitStopped();
    }

    /* A power cycle or media change replaces the console, which has no debugger:
     * the open window attaches again, the new machine stops at its first
     * instruction and the breakpoints set before are still there. */
    void sessionRestart()
    {
        m_tabs->setCurrentIndex(1);
        nesdebug_breakpoint_toggle(m_d, 0xC014);
        nessession_stop(m_s);
        QCOMPARE(nessession_start(m_s, &m_opts), 0);
        QTRY_VERIFY_WITH_TIMEOUT(nesdebug_is_attached(m_d) && nesdebug_is_stopped(m_d), 5000);
        QVERIFY(nesdebug_breakpoint_check(m_d, 0xC014));       /* kept */
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().startsWith(QLatin1String("Stopped")), 3000);
        QPlainTextEdit *view = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(view->toPlainText().contains(QStringLiteral("> %1").arg(pcOf(m_d), 4, 16, QLatin1Char('0')).toUpper()), 3000);
        /* and it runs from there to the breakpoint */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(nesdebug_is_stopped(m_d) && pcOf(m_d) == 0xC014, 5000);
        QTRY_VERIFY2_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("exec $C014")), qPrintable(statusLabel()->text()), 3000);
    }

    void f12HidesAndDetaches()
    {
        QVERIFY(m_win->isVisible());
        QTest::keyClick(m_win, Qt::Key_F12);
        QVERIFY(!m_win->isVisible());
        QVERIFY(!nesdebug_is_attached(m_d));
        const nesdebug_cpu before = cpuNow();
        Q_UNUSED(before);
        QTest::qWait(300);
        /* the main window's F12 is toggleFor: it shows the window and attaches (stopping) */
        DebuggerWindow::toggleFor(nullptr, m_s);
        QVERIFY(m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(nesdebug_is_attached(m_d), 2000);
        waitStopped();
        DebuggerWindow::toggleFor(nullptr, m_s);
        QVERIFY(!m_win->isVisible());
        QVERIFY(!nesdebug_is_attached(m_d));
        DebuggerWindow::toggleFor(nullptr, m_s);
        QTRY_VERIFY_WITH_TIMEOUT(nesdebug_is_attached(m_d), 2000);
        waitStopped();
    }
};

QTEST_MAIN(DebuggerTest)
#include "debugger_test.moc"
