/*
 * Debugger window (Qt6 Widgets) over MesenCE's own debugger engine, via
 * core/include/nesdebug.h and nesvidtab.h. Mirrors the GNOME one tab for
 * tab, and follows the Adam, Coleco, MSX and INTV KDE debuggers (the
 * family's newest): twelve tabs with the editable register strip under every
 * one.
 *
 * Tabs: the Console (the engine's command line, with a history and Tab
 * completion), the Disassembly (click a line to select it for Run to Cursor;
 * click its left edge, double-click it or press F9 for its breakpoint),
 * Memory (the CPU bus, the PPU bus, OAM, palette RAM, the cartridge's PRG and
 * CHR and WRAM; Go to, Poke, search), Breakpoints (execute, read, write and
 * access in the CPU, PPU, palette and OAM spaces, with an expression
 * condition), Watches, Cart & I/O (the mapper and the FujiNet mailbox, the
 * memory map, the controllers, the expansion port and the PPU's state), Trace
 * (live) and History (what ran before the stop), Video (VideoTab over the
 * core's nesvidtab.h), Sound (the 2A03's five channels), Symbols and FujiNet
 * (the FujiBus packets on the cartridge's link). The toolbar carries the
 * stepping controls, symbol loading, Save and Pop Out; the family's keys apply
 * (F5 run/stop, F7 step, F8 step over, Shift+F8 step out, Shift+F7 frame, F9
 * breakpoint, F12 close).
 *
 * Showing the window attaches the engine, which stops the machine (as on every
 * sibling); hiding it detaches, which lets the machine run on. The engine
 * serialises its calls and blocks during a power cycle, so while the machine
 * runs this window reads only what is cheap and safe (the stop snapshot, the
 * trace ring, the FujiNet log, the Video tab's captured frames); everything
 * else is read at a stop.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DebuggerWindow.h"
#include "../help/HelpWindow.h"
#include "Accent.h"
#include "VideoTab.h"

#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollBar>
#include <QShortcut>
#include <QShowEvent>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTextBlock>
#include <QTextCursor>
#include <QThread>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

extern "C" {
#include "nesmemtools.h"
}

#define DISASM_WINDOW 64        /* lines fetched; as many show as fit */
#define MEM_ROWS 256            /* 4K of memory, 16 bytes a row */
#define BP_MAX 256
#define TRACE_MAX 512
#define CONSOLE_LINES 5000
#define HIST_FETCH 4000         /* ring entries scanned for a Trace / History view at a stop */
#define LIVE_FETCH 512          /* ... and while the machine runs (a big read pauses it briefly) */
#define HIST_SHOWN 400          /* matching lines shown */

namespace {

enum { TAB_CONSOLE, TAB_DISASM, TAB_MEMORY, TAB_BREAKS, TAB_WATCHES, TAB_CART, TAB_TRACE, TAB_HISTORY,
       TAB_VIDEO, TAB_SOUND, TAB_SYMBOLS, TAB_FUJINET, TAB_COUNT };

/* The tabs, in order: the names NES_DEBUGGER_TAB and NES_DEBUGGER_POPOUT take, and their
 * labels (which they take too); the Help context of each is its name. */
const struct { const char *name, *label; } kTabs[TAB_COUNT] = {
    { "console", "Console" }, { "disassembly", "Disassembly" }, { "memory", "Memory" },
    { "breakpoints", "Breakpoints" }, { "watches", "Watches" }, { "cart", "Cart & I/O" },
    { "trace", "Trace" }, { "history", "History" }, { "video", "Video" }, { "sound", "Sound" },
    { "symbols", "Symbols" }, { "fujinet", "FujiNet" },
};

/* The older names of the tabs that were folded in or renamed. */
const struct { const char *name; int tab; } kTabAliases[] = {
    { "prompt", TAB_CONSOLE }, { "cpu", TAB_MEMORY }, { "ppu", TAB_VIDEO }, { "apu", TAB_SOUND },
    { "input", TAB_CART }, { "io", TAB_CART },
};

/* The register entries, in the order shown, and the name the engine's
 * expression grammar knows each by (nesdebug_register_set). */
const struct { const char *name, *engine; int digits; int limit; } kRegs[6] = {
    { "PC", "pc", 4, 0xFFFF }, { "SP", "sp", 2, 0xFF }, { "A", "a", 2, 0xFF },
    { "X", "x", 2, 0xFF },     { "Y", "y", 2, 0xFF },   { "P", "ps", 2, 0xFF },
};
enum { REG_PC = 0 };

/* The flags: their bit in P. */
const struct { const char *name, *engine; int mask; } kFlags[6] = {
    { "N", "n", 0x80 }, { "V", "v", 0x40 }, { "D", "d", 0x08 }, { "I", "i", 0x04 }, { "Z", "z", 0x02 }, { "C", "c", 0x01 },
};

/* The memory spaces the Memory tab and the breakpoints offer, in the order shown. */
const int kMemSpaces[7] = { NESDEBUG_SPACE_CPU, NESDEBUG_SPACE_PPU, NESDEBUG_SPACE_OAM, NESDEBUG_SPACE_PALETTE,
                            NESDEBUG_SPACE_PRG, NESDEBUG_SPACE_CHR, NESDEBUG_SPACE_WRAM };
const int kBpSpaces[4] = { NESDEBUG_SPACE_CPU, NESDEBUG_SPACE_PPU, NESDEBUG_SPACE_PALETTE, NESDEBUG_SPACE_OAM };

const QString kGray = QStringLiteral("color: gray;");

/* The accent: the Run button while stopped, the PC's line, a marked row. On a
 * dark theme the light variant with black ink (Accent.h). */
QTextCharFormat accentFormat(const QWidget *wid)
{
    QTextCharFormat fmt;
    fmt.setBackground(nesDebugAccentFill(wid));
    fmt.setForeground(nesDebugAccentFillInk(wid));
    return fmt;
}

void setMono(QWidget *widget)
{
    QFont font = widget->font();
    font.setFamily(QStringLiteral("monospace"));
    font.setStyleHint(QFont::TypeWriter);
    widget->setFont(font);
}

QPlainTextEdit *monoView()
{
    auto *view = new QPlainTextEdit;
    view->setReadOnly(true);
    setMono(view);
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    return view;
}

/* Engine text for a view: control bytes dropped. */
QString stripControl(const char *chars)
{
    const QString source = QString::fromUtf8(chars);
    QString out;
    out.reserve(source.size());
    for (const QChar letter : source)
        if (letter.unicode() >= 0x20 || letter == QLatin1Char('\n') || letter == QLatin1Char('\t')) out += letter;
    return out;
}

QString hexText(unsigned value, int digits)
{
    return QStringLiteral("%1").arg(value, digits, 16, QLatin1Char('0')).toUpper();
}

QLabel *titleLabel(const QString &text)
{
    auto *label = new QLabel(text);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}

QLabel *hintLabel(const QString &text)
{
    auto *label = new QLabel(text);
    label->setStyleSheet(kGray);
    label->setWordWrap(true);
    return label;
}

QLineEdit *smallEntry(const QString &placeholder, int chars)
{
    auto *entry = new QLineEdit;
    entry->setPlaceholderText(placeholder);
    entry->setFixedWidth(entry->fontMetrics().averageCharWidth() * chars + 16);
    return entry;
}

/* A register entry exactly wide enough for its hex digits. */
void setDigits(QLineEdit *entry, int digits)
{
    entry->setFixedWidth(entry->fontMetrics().horizontalAdvance(QString(digits, QLatin1Char('0'))) + 16);
}

/* Whether a view's reader is at its end (a log then follows new lines). */
bool atEnd(QPlainTextEdit *view)
{
    return view->verticalScrollBar()->value() >= view->verticalScrollBar()->maximum() - 1;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
void setText(QPlainTextEdit *view, const QString &text, bool follow = false)
{
    if (view->toPlainText() == text) {
        if (follow) view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
        return;
    }
    const int top = view->verticalScrollBar()->value(), left = view->horizontalScrollBar()->value();
    view->setPlainText(text);
    view->verticalScrollBar()->setValue(follow ? view->verticalScrollBar()->maximum() : top);
    view->horizontalScrollBar()->setValue(left);
}

/* A whole line of a view in a format: the accent fill, a mark. */
QTextEdit::ExtraSelection lineSelection(QPlainTextEdit *view, int line, const QTextCharFormat &fmt)
{
    QTextEdit::ExtraSelection sel;
    sel.cursor = QTextCursor(view->document()->findBlockByNumber(line));
    sel.format = fmt;
    sel.format.setProperty(QTextFormat::FullWidthSelection, true);
    return sel;
}

/* A status line that never widens the window. It is clipped, not elided: the
 * stop reason ("wrote $2000 = $90 (was $10) at $C011") is read whole in the
 * tooltip and the Breakpoints tab, and never turns into "...". */
class StatusLabel : public QLabel {
public:
    StatusLabel() { setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); }
    QSize minimumSizeHint() const override { return QSize(40, QLabel::minimumSizeHint().height()); }
};

/* NES_DEBUGGER_TAB and NES_DEBUGGER_POPOUT: a page number, or a tab's name or label
 * (any case), or the start of one, or of a word of a label ("memory"), or an
 * older name (prompt, cpu, ppu, apu ...). */
int tabFromEnv(const char *variable = "NES_DEBUGGER_TAB")
{
    const QString tab = qEnvironmentVariable(variable).trimmed();
    if (tab.isEmpty()) return -1;
    bool isNum = false;
    const int number = tab.toInt(&isNum);
    if (isNum) return number >= 0 && number < TAB_COUNT ? number : -1;
    for (const auto &alias : kTabAliases)
        if (tab.compare(QString::fromUtf8(alias.name), Qt::CaseInsensitive) == 0) return alias.tab;
    for (int pass = 0; pass < 3; ++pass)
        for (int tabIdx = 0; tabIdx < TAB_COUNT; ++tabIdx) {
            const QString name = QString::fromUtf8(kTabs[tabIdx].name), label = QString::fromUtf8(kTabs[tabIdx].label);
            if (pass == 0 && name.startsWith(tab, Qt::CaseInsensitive)) return tabIdx;
            if (pass == 1 && label.startsWith(tab, Qt::CaseInsensitive)) return tabIdx;
            if (pass == 2) {
                const QStringList words = label.split(QLatin1Char(' '));
                for (int wordPos = 1; wordPos < words.size(); ++wordPos)
                    if (words[wordPos].startsWith(tab, Qt::CaseInsensitive)) return tabIdx;
            }
        }
    return -1;
}

/* The label a tab shows: "&" starts a mnemonic in tab text, so it is doubled. */
QString tabLabel(int tab)
{
    return QString::fromUtf8(kTabs[tab].label).replace(QLatin1Char('&'), QLatin1String("&&"));
}

/* The family's keys: F5 run/stop, F7 step, Shift+F7 frame, F8 step over,
 * Shift+F8 step out, F9 breakpoint, F12 close. */
bool isDebuggerKey(const QKeyEvent *evt)
{
    const Qt::KeyboardModifiers mods = evt->modifiers() & ~Qt::KeypadModifier;
    switch (evt->key()) {
    case Qt::Key_F5: case Qt::Key_F9: case Qt::Key_F12: return mods == Qt::NoModifier;
    case Qt::Key_F7: case Qt::Key_F8: return mods == Qt::NoModifier || mods == Qt::ShiftModifier;
    default: return false;
    }
}

/* A tone as a note: "440.0 Hz  A4 (+3 cents)". */
QString noteName(double hertz)
{
    static const char *const kNotes[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    if (hertz < 8.0) return QStringLiteral("-");
    const double midi = 69.0 + 12.0 * std::log2(hertz / 440.0);
    const long note = std::lround(midi);
    return QStringLiteral("%1 Hz  %2%3 (%4 cents)")
        .arg(hertz, 0, 'f', 1)
        .arg(QLatin1String(kNotes[((note % 12) + 12) % 12]))
        .arg(note / 12 - 1)
        .arg(std::lround((midi - double(note)) * 100.0));
}

QString onOff(int value) { return value ? QStringLiteral("on") : QStringLiteral("off"); }

/* What the PPU bus holds at an address, for the Memory tab's region column. */
QString ppuRegionName(uint32_t addr)
{
    if (addr < 0x1000) return QStringLiteral("pattern table 0");
    if (addr < 0x2000) return QStringLiteral("pattern table 1");
    if (addr < 0x3000) return QStringLiteral("nametable %1").arg((addr - 0x2000) / 0x400);
    if (addr < 0x3F00) return QStringLiteral("nametable mirror");
    return QStringLiteral("palette");
}

/* A tab's window: closing it docks the tab again. The family's keys work in
 * it too, handed to the debugger. */
class PopWindow : public QWidget {
public:
    std::function<void()> onClose;
    std::function<bool(QKeyEvent *)> onKey;
protected:
    void closeEvent(QCloseEvent *evt) override
    {
        if (onClose) onClose();
        QWidget::closeEvent(evt);
    }
    bool event(QEvent *evt) override
    {
        if (evt->type() == QEvent::ShortcutOverride && isDebuggerKey(static_cast<QKeyEvent *>(evt))) {
            evt->accept();
            return true;
        }
        return QWidget::event(evt);
    }
    void keyPressEvent(QKeyEvent *evt) override
    {
        if (!onKey || !onKey(evt)) QWidget::keyPressEvent(evt);
    }
};

QPointer<DebuggerWindow> s_win;

/* The frame button and Shift+F7: one frame; the scanline button: one line. */
void frameOne(nesdebug *dbg)
{
    nesdebug_frame(dbg, 1);
}

void scanlineOne(nesdebug *dbg)
{
    nesdebug_scanline(dbg, 1);
}

} // namespace

void DebuggerWindow::showFor(QWidget *parent, nessession *session)
{
    if (!s_win) s_win = new DebuggerWindow(session, parent);
    /* showing attaches, which stops the machine (showEvent) */
    s_win->show();
    s_win->raise();
    s_win->activateWindow();
}

void DebuggerWindow::toggleFor(QWidget *parent, nessession *session)
{
    if (s_win && s_win->isVisible()) s_win->hide();
    else showFor(parent, session);
}

DebuggerWindow::DebuggerWindow(nessession *session, QWidget *parent)
    : QMainWindow(parent, Qt::Window), m_session(session), m_dbg(nessession_debugger(session))
{
    setObjectName(QStringLiteral("debugger"));
    setWindowTitle(QStringLiteral("Debugger"));
    resize(1200, 800);

    auto *central = new QWidget;
    auto *root = new QVBoxLayout(central);
    root->addWidget(buildToolbar());
    m_tabs = new QTabWidget;
    m_tabs->setUsesScrollButtons(true);
    QWidget *pages[TAB_COUNT];
    pages[TAB_CONSOLE] = buildConsole();
    pages[TAB_DISASM] = buildDisasm();
    pages[TAB_MEMORY] = buildMemory();
    pages[TAB_BREAKS] = buildBreaks();
    pages[TAB_WATCHES] = buildWatches();
    pages[TAB_CART] = buildCart();
    pages[TAB_TRACE] = buildTrace(0);
    pages[TAB_HISTORY] = buildTrace(1);
    pages[TAB_VIDEO] = buildVideo();
    pages[TAB_SOUND] = buildSound();
    pages[TAB_SYMBOLS] = buildSymbols();
    pages[TAB_FUJINET] = buildFujinet();
    for (int idx = 0; idx < TAB_COUNT; ++idx) {
        pages[idx]->setProperty("tab", idx);     /* each page keeps its identity when tabs move */
        m_pages[idx] = pages[idx];
        m_tabs->addTab(pages[idx], tabLabel(idx));
    }
    const int tab = tabFromEnv();
    if (tab >= 0) m_tabs->setCurrentIndex(tab);
    /* Help's contexts (neshelp.h), one per tab */
    auto setHelpContext = [this]() {
        const int page = currentTab();
        if (page >= 0 && page < TAB_COUNT)
            setProperty("nesHelpContext", QStringLiteral("debugger/%1").arg(QLatin1String(kTabs[page].name)));
    };
    setHelpContext();
    new QShortcut(QKeySequence(Qt::Key_F1), this, [this] { nesHelpFor(this); });
    connect(m_tabs, &QTabWidget::currentChanged, this, [this, setHelpContext](int) {
        setHelpContext();
        /* the view has no size until it shows: fill it now */
        refreshTab(currentTab(), false);
        if (currentTab() == TAB_CONSOLE) m_conIn->setFocus();
    });
    root->addWidget(m_tabs, 1);
    root->addWidget(buildRegs());
    setCentralWidget(central);
    statusBar();

    m_envWatch = qgetenv("NES_DEBUGGER_WATCH");
    m_envWatchBreak = qgetenv("NES_DEBUGGER_WATCH_BREAK");
    m_envMemGoto = qgetenv("NES_DEBUGGER_MEM_GOTO");
    m_envHistory = qEnvironmentVariableIsSet("NES_DEBUGGER_HISTORY");
    const int popTab = tabFromEnv("NES_DEBUGGER_POPOUT");
    if (popTab >= 0) popOut(popTab);     /* for captures: peel a tab off at the start */

    /* the timer runs only while the window is shown (showEvent/hideEvent) */
    connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
}

DebuggerWindow::~DebuggerWindow()
{
    m_closing = true;
    if (nesdebug_is_attached(m_dbg)) nesdebug_detach(m_dbg);
    for (QWidget *&win : m_popped)
        if (win) {
            QWidget *popped = win;
            win = nullptr;
            popped->close();
        }
}

/* ---- building ------------------------------------------------------------- */

QWidget *DebuggerWindow::buildToolbar()
{
    /* a tool bar, so a narrow window folds the buttons that do not fit into its extension menu */
    auto *bar = new QToolBar;
    bar->setMovable(false);
    auto add = [&](const QString &label, const QString &tip, std::function<void()> action) {
        auto *button = new QPushButton(label);
        button->setFocusPolicy(Qt::NoFocus);
        button->setToolTip(tip);
        connect(button, &QPushButton::clicked, this, action);
        bar->addWidget(button);
        return button;
    };
    m_runBtn = add(QStringLiteral("Stop (F5)"), QStringLiteral("Run, or stop before the next instruction"),
                   [this] { toggleRun(); });
    add(QStringLiteral("Step (F7)"), QStringLiteral("One instruction"), [this] { stepAnd(nesdebug_step); });
    add(QStringLiteral("Step Over (F8)"), QStringLiteral("Over a JSR"), [this] { stepAnd(nesdebug_step_over); });
    add(QStringLiteral("Step Out (⇧F8)"), QStringLiteral("On to the RTS or RTI"),
        [this] { stepAnd(nesdebug_step_out); });
    add(QStringLiteral("Run to Cursor"), QStringLiteral("On to the line selected in the Disassembly"),
        [this] { runToCursor(); });
    add(QStringLiteral("Scanline+1"), QStringLiteral("On to the next scanline"), [this] { stepAnd(scanlineOne); });
    add(QStringLiteral("Frame+1 (⇧F7)"), QStringLiteral("On to the end of the frame"),
        [this] { stepAnd(frameOne); });
    auto *spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bar->addWidget(spacer);

    auto *sym = new QPushButton(QStringLiteral("Symbols"));
    sym->setFocusPolicy(Qt::NoFocus);
    sym->setToolTip(QStringLiteral("Labels from an ld65 -Ln / VICE label file, a ca65 .dbg or a Mesen .mlb"));
    auto *symMenu = new QMenu(sym);
    symMenu->addAction(QStringLiteral("Symbol file…"), this, [this] { loadSymbols(true); });
    symMenu->addAction(QStringLiteral("Next to the cartridge"), this, [this] { loadSymbols(false); });
    symMenu->addAction(QStringLiteral("Clear loaded symbols"), this, [this] {
        nesdebug_clear_symbols(m_dbg);
        refreshSymbols(true);
        resync();
    });
    sym->setMenu(symMenu);
    bar->addWidget(sym);

    auto *saveBtn = new QPushButton(QStringLiteral("Save"));
    saveBtn->setFocusPolicy(Qt::NoFocus);
    auto *menu = new QMenu(saveBtn);
    menu->addAction(QStringLiteral("Disassembly ($8000-$FFFF)"), this,
                    [this] { save("dis", QStringLiteral("Disassembly"), QStringLiteral("disassembly.asm")); });
    menu->addAction(QStringLiteral("PRG SRAM"), this,
                    [this] { save("prg", QStringLiteral("PRG SRAM"), QStringLiteral("prg.bin")); });
    menu->addAction(QStringLiteral("CHR SRAM"), this,
                    [this] { save("chr", QStringLiteral("CHR SRAM"), QStringLiteral("chr.bin")); });
    menu->addAction(QStringLiteral("Console RAM"), this,
                    [this] { save("ram", QStringLiteral("Console RAM"), QStringLiteral("ram.bin")); });
    menu->addAction(QStringLiteral("WRAM"), this,
                    [this] { save("wram", QStringLiteral("WRAM"), QStringLiteral("wram.bin")); });
    saveBtn->setMenu(menu);
    bar->addWidget(saveBtn);
    add(QStringLiteral("Pop Out"), QStringLiteral("Move the tab that shows into a window of its own; Dock puts it back"),
        [this] { popOut(currentTab()); });
    bar->addWidget(nesHelpButton(QString()));

    /* the status line sits in the status bar: the toolbar has no room for it */
    m_status = new StatusLabel;
    m_status->setAlignment(Qt::AlignLeft);
    statusBar()->addWidget(m_status, 1);
    return bar;
}

/* The 6502's registers, under every tab. Enter sets one (only while stopped);
 * a flag is a check box. The second row says where the beam is. */
QWidget *DebuggerWindow::buildRegs()
{
    auto *box = new QGroupBox(QStringLiteral("Registers"));
    auto *rows = new QVBoxLayout(box);
    rows->setSpacing(2);
    auto *regs = new QHBoxLayout;
    for (int idx = 0; idx < 6; ++idx) {
        auto *nameLabel = new QLabel(QString::fromUtf8(kRegs[idx].name));
        nameLabel->setStyleSheet(kGray);
        regs->addWidget(nameLabel);
        m_reg[idx] = new QLineEdit;
        setMono(m_reg[idx]);
        m_reg[idx]->setMaxLength(24);
        setDigits(m_reg[idx], kRegs[idx].digits);
        m_reg[idx]->setToolTip(QStringLiteral("Enter sets it (a bare number is hex; an expression such as a + 1 works; "
                                              "only while stopped)"));
        connect(m_reg[idx], &QLineEdit::returnPressed, this, [this, idx] { setReg(idx); });
        regs->addWidget(m_reg[idx]);
        regs->addSpacing(6);
    }
    regs->addSpacing(10);
    for (int idx = 0; idx < 6; ++idx) {
        m_flag[idx] = new QCheckBox(QString::fromUtf8(kFlags[idx].name));
        m_flag[idx]->setToolTip(QStringLiteral("Flag %1 of P (only while stopped)").arg(QString::fromUtf8(kFlags[idx].name)));
        connect(m_flag[idx], &QCheckBox::clicked, this, [this, idx](bool enabled) { setFlag(idx, enabled); });
        regs->addWidget(m_flag[idx]);
    }
    regs->addSpacing(14);
    m_irqNote = new QLabel(QStringLiteral("IRQ"));
    m_nmiNote = new QLabel(QStringLiteral("NMI"));
    m_irqNote->setToolTip(QStringLiteral("The IRQ line is pending"));
    m_nmiNote->setToolTip(QStringLiteral("An NMI is pending"));
    regs->addWidget(m_irqNote);
    regs->addWidget(m_nmiNote);
    regs->addStretch(1);
    rows->addLayout(regs);
    /* where the machine is: CPU cycles since power on and the PPU's beam */
    m_beamNote = new QLabel;
    m_beamNote->setToolTip(QStringLiteral("CPU cycles since power on, and where the PPU's beam is"));
    setMono(m_beamNote);
    rows->addWidget(m_beamNote);
    return box;
}

QWidget *DebuggerWindow::buildConsole()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    m_conOut = monoView();
    m_conOut->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_conOut->setMaximumBlockCount(CONSOLE_LINES);
    m_conOut->setPlainText(QStringLiteral("Debugger console. Type 'help' for every command. Numbers: $C000 or 0xC000 "
                                          "for hex; a bare number is decimal in an expression and hex where a "
                                          "command takes an address (unless it names a label).\n"));
    m_conIn = new QLineEdit;
    setMono(m_conIn);
    m_conIn->setPlaceholderText(QStringLiteral("command (help, step, break, bpw, print, mem, poke, video, sound ...) — Tab completes, Up/Down recall"));
    m_conIn->installEventFilter(this);
    connect(m_conIn, &QLineEdit::returnPressed, this, [this] { runConsole(); });
    layout->addWidget(m_conOut, 1);
    layout->addWidget(m_conIn);
    return page;
}

/* The dump: a space chooser, Go to, a page back or on, Poke, and the search bar. */
QWidget *DebuggerWindow::buildMemory()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    auto *nav = new QHBoxLayout;
    nav->addWidget(new QLabel(QStringLiteral("Space")));
    m_memSpaceBox = new QComboBox;
    for (int space : kMemSpaces) m_memSpaceBox->addItem(QString::fromUtf8(nesdebug_space_name(space)), space);
    m_memSpaceBox->setToolTip(QStringLiteral(
        "CPU: the 64K bus as mapped now. PPU: pattern tables and nametables. OAM: sprite RAM. Palette: palette RAM.\n"
        "PRG, CHR and WRAM: the cartridge's own memory."));
    connect(m_memSpaceBox, &QComboBox::currentIndexChanged, this, [this](int) { memSpaceChanged(); });
    nav->addWidget(m_memSpaceBox);
    nav->addSpacing(8);
    nav->addWidget(new QLabel(QStringLiteral("Go to")));
    m_memGoto = smallEntry(QStringLiteral("$8000, label or lo-hi"), 22);
    connect(m_memGoto, &QLineEdit::returnPressed, this, [this] { memGoto(); });
    nav->addWidget(m_memGoto);
    for (int dir = -1; dir <= 1; dir += 2) {
        auto *arrow = new QToolButton;
        arrow->setArrowType(dir < 0 ? Qt::UpArrow : Qt::DownArrow);
        arrow->setToolTip(dir < 0 ? QStringLiteral("Back 4K") : QStringLiteral("On 4K"));
        connect(arrow, &QToolButton::clicked, this, [this, dir] {
            const int size = nesdebug_space_size(m_dbg, memSpace());
            const long top = long(m_memTop) + dir * MEM_ROWS * 16;
            m_memTop = uint32_t(qBound<long>(0L, top, long(qMax(0, size - 1) & ~0xF)));
            refreshMem();
        });
        nav->addWidget(arrow);
    }
    nav->addSpacing(12);
    nav->addWidget(new QLabel(QStringLiteral("Poke")));
    m_memAddr = smallEntry(QStringLiteral("address or label"), 16);
    m_memVal = smallEntry(QStringLiteral("value"), 10);
    m_memVal->setToolTip(QStringLiteral("A byte: hex, or an expression. Only while stopped; ROM ignores it."));
    auto *pokeBtn = new QPushButton(QStringLiteral("Poke"));
    connect(m_memAddr, &QLineEdit::returnPressed, this, [this] { memPoke(); });
    connect(m_memVal, &QLineEdit::returnPressed, this, [this] { memPoke(); });
    connect(pokeBtn, &QPushButton::clicked, this, [this] { memPoke(); });
    nav->addWidget(m_memAddr);
    nav->addWidget(m_memVal);
    nav->addWidget(pokeBtn);
    nav->addStretch(1);
    layout->addLayout(nav);

    auto *search = new QHBoxLayout;
    search->addWidget(new QLabel(QStringLiteral("Search")));
    m_srchKind = new QComboBox;
    m_srchKind->addItems({ QStringLiteral("Hex bytes"), QStringLiteral("Text") });
    m_srchKind->setToolTip(QStringLiteral("Hex bytes (A9 01) or text, one byte per character"));
    search->addWidget(m_srchKind);
    m_srchEntry = new QLineEdit;
    m_srchEntry->setPlaceholderText(QStringLiteral("bytes or text"));
    connect(m_srchEntry, &QLineEdit::returnPressed, this, [this] { memSearch(false); });
    search->addWidget(m_srchEntry, 1);
    auto *next = new QPushButton(QStringLiteral("Find Next"));
    auto *prev = new QPushButton(QStringLiteral("Find Previous"));
    connect(next, &QPushButton::clicked, this, [this] { memSearch(false); });
    connect(prev, &QPushButton::clicked, this, [this] { memSearch(true); });
    search->addWidget(next);
    search->addWidget(prev);
    m_srchNote = new QLabel;
    m_srchNote->setStyleSheet(kGray);
    search->addWidget(m_srchNote);
    layout->addLayout(search);

    m_memTitle = titleLabel(QStringLiteral("Memory (the 64K as the CPU sees it)"));
    layout->addWidget(m_memTitle);
    m_mem = monoView();
    layout->addWidget(m_mem, 1);
    return page;
}

QWidget *DebuggerWindow::buildDisasm()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *row = new QHBoxLayout;
    m_followPc = new QCheckBox(QStringLiteral("Follow PC"));
    m_followPc->setChecked(true);
    connect(m_followPc, &QCheckBox::clicked, this, [this] { refreshDisasm(); });
    m_jump = smallEntry(QStringLiteral("$address or label"), 20);
    connect(m_jump, &QLineEdit::returnPressed, this, [this] { jumpTo(); });
    row->addWidget(m_followPc);
    row->addSpacing(8);
    row->addWidget(new QLabel(QStringLiteral("Jump to")));
    row->addWidget(m_jump);
    row->addSpacing(8);
    row->addWidget(hintLabel(QStringLiteral("Click a line to select it (Run to Cursor); click its left edge, "
                                            "double-click it or press F9 for a breakpoint; scroll to browse")), 1);
    layout->addLayout(row);
    m_disasm = monoView();
    /* no scrollbar: the wheel scrolls by instructions, as many lines as fit */
    m_disasm->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_disasm->setFocusPolicy(Qt::NoFocus);
    m_disasm->viewport()->setCursor(Qt::ArrowCursor);
    m_disasm->viewport()->installEventFilter(this);
    layout->addWidget(m_disasm, 1);
    return page;
}

QWidget *DebuggerWindow::buildBreaks()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    auto *top = new QHBoxLayout;
    top->addWidget(hintLabel(QStringLiteral("Execute stops before the instruction runs; a CPU read or write stops "
                                            "before the instruction that makes the access; the PPU, palette and OAM "
                                            "spaces stop where the CPU is. Breakpoints are kept while the debugger is "
                                            "closed, but only act while it is open.")), 1);
    layout->addLayout(top);

    m_bpTable = new QTableWidget(0, 7);
    m_bpTable->setHorizontalHeaderLabels({ QStringLiteral("On"), QStringLiteral("Type"), QStringLiteral("Space"),
                                           QStringLiteral("Range"), QStringLiteral("Condition"), QStringLiteral("Hits"),
                                           QString() });
    m_bpTable->verticalHeader()->setVisible(false);
    m_bpTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_bpTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_bpTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_bpTable->setEditTriggers(QAbstractItemView::DoubleClicked);     /* only the Condition cells are editable */
    m_bpTable->setFocusPolicy(Qt::NoFocus);
    connect(m_bpTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_bpFilling) return;
        if (item->column() == 4) {
            /* an edited condition: the engine checks it, and an empty one removes it */
            char err[200] = "";
            const QByteArray text = item->text().trimmed().toUtf8();
            if (nesdebug_breakpoint_set_condition(m_dbg, item->data(Qt::UserRole).toInt(), text.constData(), err, sizeof err))
                m_bpMsg->clear();
            else
                m_bpMsg->setText(err[0] ? stripControl(err) : QStringLiteral("The condition could not be set"));
            m_bpSig.clear();
            syncLater();
            return;
        }
        if (item->column() != 0) return;
        nesdebug_breakpoint_enable(m_dbg, item->data(Qt::UserRole).toInt(), item->checkState() == Qt::Checked);
        syncLater();
    });
    layout->addWidget(m_bpTable, 1);
    m_bpEmpty = hintLabel(QStringLiteral("No breakpoints. Click the left edge of a disassembly line, or add one below."));
    layout->addWidget(m_bpEmpty);

    auto *row = new QHBoxLayout;
    m_bpType = new QComboBox;
    m_bpType->addItems({ QStringLiteral("Execute"), QStringLiteral("Read"), QStringLiteral("Write"),
                         QStringLiteral("Access") });
    m_bpType->setToolTip(QStringLiteral("Access is a read or a write of the range (one entry, one hit count). "
                                        "Execute is for the CPU space only."));
    m_bpSpace = new QComboBox;
    for (int space : kBpSpaces) m_bpSpace->addItem(QString::fromUtf8(nesdebug_space_name(space)), space);
    m_bpSpace->setToolTip(QStringLiteral("CPU: the 64K bus. PPU: pattern tables and nametables ($0000-$3EFF). "
                                         "Palette: palette RAM ($00-$1F). OAM: sprite RAM ($00-$FF)."));
    m_bpStart = smallEntry(QStringLiteral("start ($C000 or label)"), 22);
    m_bpEnd = smallEntry(QStringLiteral("end (optional)"), 16);
    m_bpCond = new QLineEdit;
    m_bpCond->setPlaceholderText(QStringLiteral("condition (optional): value != old, a == 5, [$0300] > 3"));
    m_bpCond->setToolTip(QStringLiteral(
        "A Mesen expression: numbers ($C000), the 6502 registers (a x y sp ps pc), labels, [addr] (a byte) and "
        "{addr} (a word), frame, cycle, scanline, and for an access value, address and, in CPU space, old (the byte a "
        "write replaced). Empty stops every time."));
    auto *add = new QPushButton(QStringLiteral("Add"));
    auto *clearHits = new QPushButton(QStringLiteral("Clear Hits"));
    auto *clear = new QPushButton(QStringLiteral("Clear All"));
    connect(add, &QPushButton::clicked, this, [this] { addBreakpoint(); });
    connect(clearHits, &QPushButton::clicked, this, [this] {
        nesdebug_breakpoint_clear_hits(m_dbg);
        resync();
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        nesdebug_breakpoint_clear(m_dbg);
        resync();
    });
    for (QLineEdit *edit : { m_bpStart, m_bpEnd, m_bpCond })
        connect(edit, &QLineEdit::returnPressed, this, [this] { addBreakpoint(); });
    row->addWidget(m_bpType);
    row->addWidget(m_bpSpace);
    row->addWidget(m_bpStart);
    row->addWidget(m_bpEnd);
    row->addWidget(m_bpCond, 1);
    row->addWidget(add);
    row->addWidget(clearHits);
    row->addWidget(clear);
    layout->addLayout(row);

    m_bpMsg = new QLabel;
    m_bpMsg->setWordWrap(true);
    m_bpMsg->setStyleSheet(QStringLiteral("color: #e01b24;"));
    layout->addWidget(m_bpMsg);
    m_bpStop = new QLabel;
    m_bpStop->setStyleSheet(kGray);
    layout->addWidget(m_bpStop);
    return page;
}

QWidget *DebuggerWindow::buildWatches()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *form = new QHBoxLayout;
    form->addWidget(new QLabel(QStringLiteral("Watch")));
    m_wtEntry = new QLineEdit;
    m_wtEntry->setPlaceholderText(QStringLiteral("address, label, expression or first-last (CPU bus)"));
    connect(m_wtEntry, &QLineEdit::returnPressed, this, [this] { addWatch(); });
    form->addWidget(m_wtEntry, 1);
    auto *add = new QPushButton(QStringLiteral("Add"));
    connect(add, &QPushButton::clicked, this, [this] { addWatch(); });
    form->addWidget(add);
    layout->addLayout(form);

    m_wtList = new QListWidget;
    setMono(m_wtList);
    layout->addWidget(m_wtList, 1);

    auto *foot = new QHBoxLayout;
    foot->addStretch();
    auto *mem = new QPushButton(QStringLiteral("Memory"));
    mem->setToolTip(QStringLiteral("Show the selected watch in the Memory tab"));
    connect(mem, &QPushButton::clicked, this, [this] {
        uint16_t first, last;
        if (m_wtList->currentRow() >= 0 && neswatch_range(m_dbg, m_wtList->currentRow(), &first, &last)) {
            showTab(TAB_MEMORY);
            showInMemory(NESDEBUG_SPACE_CPU, first, last);
        }
    });
    auto *brk = new QPushButton(QStringLiteral("Break on Change"));
    brk->setToolTip(QStringLiteral("A write breakpoint on it with the condition value != old"));
    connect(brk, &QPushButton::clicked, this, [this] {
        uint16_t first, last;
        if (m_wtList->currentRow() < 0 || !neswatch_range(m_dbg, m_wtList->currentRow(), &first, &last)) return;
        char err[200] = "";
        if (nesdebug_breakpoint_add_in(m_dbg, NESDEBUG_SPACE_CPU, NESDEBUG_BP_WRITE, first, last, "value != old", err,
                                       sizeof err) >= 0)
            message(QStringLiteral("Breaking when it changes"));
        else
            message(err[0] ? stripControl(err) : QStringLiteral("The breakpoint could not be added"));
        syncLater();
    });
    auto *remove = new QPushButton(QStringLiteral("Remove"));
    connect(remove, &QPushButton::clicked, this, [this] {
        if (m_wtList->currentRow() >= 0) {
            neswatch_remove(m_wtList->currentRow());
            syncLater();
        }
    });
    foot->addWidget(mem);
    foot->addWidget(brk);
    foot->addWidget(remove);
    layout->addLayout(foot);
    return page;
}

/* The cartridge (mapper, banks, the FujiNet mailbox) and the CPU memory map
 * above, the I/O below (controllers, expansion port, the PPU's state). */
QWidget *DebuggerWindow::buildCart()
{
    m_cartText = monoView();
    m_ioText = monoView();
    m_cartText->setLineWrapMode(QPlainTextEdit::WidgetWidth);      /* the device list is long */
    auto *split = new QSplitter(Qt::Vertical);
    split->addWidget(m_cartText);
    split->addWidget(m_ioText);
    split->setChildrenCollapsible(false);
    split->setSizes({ 330, 270 });
    return split;
}

/* Trace (0) and History (1): a Record switch, a filter, a note and the text.
 * Trace follows the machine live; History is what ran before the stop. */
QWidget *DebuggerWindow::buildTrace(int which)
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *bar = new QHBoxLayout;

    m_hRec[which] = new QPushButton(QStringLiteral("Record"));
    m_hRec[which]->setCheckable(true);
    m_hRec[which]->setStyleSheet(QStringLiteral("QPushButton:checked { background: %1; color: %2; }")
                                     .arg(nesDebugAccentFill(this).name(), nesDebugAccentFillInk(this).name()));
    m_hRec[which]->setToolTip(QStringLiteral("Record every instruction the CPU runs into Mesen's 30000-row log "
                                             "(the machine runs a little slower)"));
    connect(m_hRec[which], &QPushButton::toggled, this, [this, which](bool enabled) {
        if (m_hSync) return;
        if (which == 0) nesdebug_trace_enable(m_dbg, enabled);
        else nesdebug_history_enable(m_dbg, enabled);
        syncLater();
    });
    m_hFilter[which] = new QComboBox;
    m_hFilter[which]->addItems({ QStringLiteral("All instructions"), QStringLiteral("Branches only"),
                                 QStringLiteral("In range") });
    m_hRange[which] = new QLineEdit;
    m_hRange[which]->setPlaceholderText(QStringLiteral("range: lo-hi or a label"));
    auto changed = [this] {
        m_hSeen[0] = m_hSeen[1] = ~0ull;
        syncLater();
    };
    connect(m_hFilter[which], &QComboBox::currentIndexChanged, this, changed);
    connect(m_hRange[which], &QLineEdit::returnPressed, this, changed);
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this, which] {
        if (which == 0) nesdebug_trace_clear(m_dbg);
        else nesdebug_history_clear(m_dbg);
        m_hSeen[0] = m_hSeen[1] = ~0ull;
        syncLater();
    });
    m_hNote[which] = new QLabel;
    m_hNote[which]->setStyleSheet(kGray);
    m_hNote[which]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    bar->addWidget(m_hRec[which]);
    bar->addWidget(m_hFilter[which]);
    bar->addWidget(m_hRange[which], 1);
    bar->addWidget(clear);
    bar->addWidget(m_hNote[which], 1);
    layout->addLayout(bar);
    m_hText[which] = monoView();
    layout->addWidget(m_hText[which], 1);
    return page;
}

QWidget *DebuggerWindow::buildVideo()
{
    m_video = new VideoTab(m_session);
    /* the inspector's "Show in Memory": a PPU, palette or OAM address */
    connect(m_video, &VideoTab::showInMemory, this, [this](int space, unsigned address) {
        showTab(TAB_MEMORY);
        showInMemory(space, address, address);
    });
    return m_video;
}

/* The 2A03's five channels as a table; the frame counter and $4015 under it. */
QWidget *DebuggerWindow::buildSound()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    m_soundTable = new QTableWidget(5, 9);
    m_soundTable->setHorizontalHeaderLabels({ QStringLiteral("Channel"), QStringLiteral("On"), QStringLiteral("Pitch"),
                                              QStringLiteral("Period"), QStringLiteral("Volume"), QStringLiteral("Duty / mode"),
                                              QStringLiteral("Length"), QStringLiteral("Envelope"), QStringLiteral("Sweep / linear / IRQ") });
    m_soundTable->verticalHeader()->hide();
    m_soundTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_soundTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_soundTable->setFocusPolicy(Qt::NoFocus);
    m_soundTable->horizontalHeader()->setStretchLastSection(true);
    setMono(m_soundTable);
    layout->addWidget(m_soundTable, 1);
    m_soundNote = new QLabel;
    setMono(m_soundNote);
    m_soundNote->setWordWrap(true);
    layout->addWidget(m_soundNote);
    return page;
}

QWidget *DebuggerWindow::buildSymbols()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *bar = new QHBoxLayout;
    bar->addWidget(new QLabel(QStringLiteral("Filter")));
    m_symFilter = new QLineEdit;
    connect(m_symFilter, &QLineEdit::textChanged, this, [this] { refreshSymbols(false); });
    bar->addWidget(m_symFilter, 1);
    m_symCount = new QLabel;
    bar->addWidget(m_symCount);
    layout->addLayout(bar);
    m_symList = new QListWidget;
    setMono(m_symList);
    m_symList->setToolTip(QStringLiteral("Enter or double-click shows it in the Disassembly"));
    connect(m_symList, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        const unsigned addr = item->data(Qt::UserRole).toUInt();
        showTab(TAB_DISASM);
        jumpToAddress(int(addr));
    });
    layout->addWidget(m_symList, 1);
    auto *foot = new QHBoxLayout;
    foot->addStretch();
    auto *memory = new QPushButton(QStringLiteral("Memory"));
    memory->setToolTip(QStringLiteral("Show the selected symbol in the Memory tab"));
    connect(memory, &QPushButton::clicked, this, [this] {
        if (QListWidgetItem *item = m_symList->currentItem()) {
            const unsigned addr = item->data(Qt::UserRole).toUInt();
            showTab(TAB_MEMORY);
            showInMemory(NESDEBUG_SPACE_CPU, addr, addr);
        }
    });
    auto *breakAt = new QPushButton(QStringLiteral("Break at"));
    breakAt->setToolTip(QStringLiteral("Toggle an execute breakpoint at the selected symbol"));
    connect(breakAt, &QPushButton::clicked, this, [this] { breakAtSymbol(); });
    foot->addWidget(memory);
    foot->addWidget(breakAt);
    layout->addLayout(foot);
    return page;
}

/* The packets left (a note and Clear over them), the cartridge's link
 * right. */
QWidget *DebuggerWindow::buildFujinet()
{
    auto *split = new QSplitter(Qt::Horizontal);
    auto *left = new QWidget;
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->addWidget(titleLabel(QStringLiteral("FujiBus packets (SLIP-decoded)")));
    auto *row = new QHBoxLayout;
    m_fujiNote = new QLabel;
    m_fujiNote->setStyleSheet(kGray);
    m_fujiNote->setWordWrap(true);
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] {
        nesdebug_fujinet_trace_clear(m_dbg);
        refreshFujinet(true);
    });
    row->addWidget(m_fujiNote, 1);
    row->addWidget(clear);
    leftLayout->addLayout(row);
    m_fujiText = monoView();
    leftLayout->addWidget(m_fujiText, 1);
    left->setMinimumWidth(560);

    auto *right = new QWidget;
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->addWidget(titleLabel(QStringLiteral("The cartridge's link")));
    m_fujiState = monoView();
    rightLayout->addWidget(m_fujiState, 1);
    right->setMinimumWidth(320);
    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setChildrenCollapsible(false);
    return split;
}

/* ---- refresh -------------------------------------------------------------- */

void DebuggerWindow::refreshStatus(const nesdebug_snapshot &snap)
{
    const bool stopped = snap.paused != 0;
    const QString text = stopped ? QStringLiteral("Stopped: %1").arg(stripControl(snap.text)) : QStringLiteral("Running");
    if (m_status->text() != text) {
        m_status->setText(text);
        m_status->setToolTip(text);
        m_status->update();
    }
    /* the registers are read at a stop: while the machine runs they are the last stop's, shown grey */
    for (QLineEdit *edit : m_reg) edit->setStyleSheet(stopped ? QString() : kGray);
    if (!stopped) m_beamNote->setText(QStringLiteral("running: the registers are read when it stops"));
    m_runBtn->setText(stopped ? QStringLiteral("Run (F5)") : QStringLiteral("Stop (F5)"));
    /* the action on offer wears the accent: Run while stopped */
    m_runBtn->setStyleSheet(stopped ? QStringLiteral("background: %1; color: %2;")
                                          .arg(nesDebugAccentFill(this).name(), nesDebugAccentFillInk(this).name())
                                    : QString());
}

/* The refresh never overwrites a value being typed. */
void DebuggerWindow::setEntry(QLineEdit *edit, const QString &text)
{
    if (edit->hasFocus()) return;
    if (edit->text() != text) edit->setText(text);
}

void DebuggerWindow::refreshRegs()
{
    nesdebug_cpu cpu;
    nesdebug_cpu_get(m_dbg, &cpu);
    const unsigned values[6] = { unsigned(cpu.pc), unsigned(cpu.sp), unsigned(cpu.a), unsigned(cpu.x), unsigned(cpu.y),
                                 unsigned(cpu.ps) };
    for (int idx = 0; idx < 6; ++idx) setEntry(m_reg[idx], hexText(values[idx] & unsigned(kRegs[idx].limit), kRegs[idx].digits));
    const int flags[6] = { cpu.n, cpu.v, cpu.d, cpu.i, cpu.z, cpu.c };
    for (int idx = 0; idx < 6; ++idx) m_flag[idx]->setChecked(flags[idx] != 0);
    const QString pending = QStringLiteral("color: %1; font-weight: bold;").arg(nesDebugAccentTextColor(m_irqNote).name());
    m_irqNote->setStyleSheet(cpu.irq ? pending : kGray);
    m_nmiNote->setStyleSheet(cpu.nmi ? pending : kGray);
    m_beamNote->setText(QStringLiteral("cycle %1  line %2  dot %3  frame %4")
                            .arg(qulonglong(cpu.total_cycles)).arg(cpu.scanline).arg(cpu.dot).arg(cpu.frame));
}

int DebuggerWindow::memSpace() const
{
    const QVariant space = m_memSpaceBox ? m_memSpaceBox->currentData() : QVariant();
    return space.isValid() ? space.toInt() : int(NESDEBUG_SPACE_CPU);
}

/* The hex digits of an address in the space showing. */
int DebuggerWindow::memDigits() const
{
    return nesdebug_space_size(m_dbg, memSpace()) > 0x10000 ? 6 : 4;
}

/* The dump from m_memTop in the space chosen, each row labelled once with
 * the region it is in (CPU: the map as of the last stop; PPU: the table);
 * a highlighted range (Go to, a watch, a search hit) in the accent. */
void DebuggerWindow::refreshMem()
{
    static uint8_t mem[MEM_ROWS * 16];
    const int space = memSpace();
    const int size = nesdebug_space_size(m_dbg, space);
    m_memTitle->setText(QStringLiteral("Memory: %1 space%2").arg(QString::fromUtf8(nesdebug_space_name(space)),
        space == NESDEBUG_SPACE_CPU ? QStringLiteral(" (the 64K as the CPU sees it)") : QString()));
    if (size <= 0) {
        setText(m_mem, QStringLiteral("(%1 has no memory now: %2)")
                           .arg(QString::fromUtf8(nesdebug_space_name(space)),
                                nesdebug_is_attached(m_dbg) ? QStringLiteral("this cartridge has none")
                                                            : QStringLiteral("the debugger is not attached")));
        m_mem->setExtraSelections({});
        return;
    }
    const int digits = size > 0x10000 ? 6 : 4;
    if (m_memTop >= uint32_t(size)) m_memTop = uint32_t(size - 1) & ~0xFu;
    const int count = qMin(MEM_ROWS * 16, size - int(m_memTop));
    memset(mem, 0, sizeof mem);
    nesdebug_read_space(m_dbg, space, m_memTop, mem, count);
    const int rows = (count + 15) / 16;

    QString text;
    text.reserve(rows * 90);
    text += QString(digits + 3, QLatin1Char(' '));
    for (int col = 0; col < 16; ++col) text += hexText((unsigned)col, 1) + QStringLiteral("  ");
    char region[64];
    QString lastRegion;
    for (int row = 0; row < rows; ++row) {
        const uint8_t *bytes = mem + row * 16;
        const uint32_t addr = m_memTop + uint32_t(row) * 16;
        const int inRow = qMin(16, count - row * 16);
        text += QStringLiteral("\n$%1: ").arg(hexText(addr, digits));
        for (int col = 0; col < 16; ++col) text += col < inRow ? hexText(bytes[col], 2) + QLatin1Char(' ') : QStringLiteral("   ");
        text += QLatin1Char(' ');
        for (int col = 0; col < inRow; ++col) text += QLatin1Char(bytes[col] >= 0x20 && bytes[col] < 0x7F ? (char)bytes[col] : '.');
        QString name;
        if (space == NESDEBUG_SPACE_CPU) {
            if (!m_regions.empty() && nesmem_region_name(m_regions.data(), (int)m_regions.size(), (uint16_t)addr, region, sizeof region) > 0)
                name = QString::fromUtf8(region);
        } else if (space == NESDEBUG_SPACE_PPU) {
            name = ppuRegionName(addr);
        } else if (space == NESDEBUG_SPACE_OAM) {
            name = QStringLiteral("sprites %1-%2").arg(addr / 4).arg(addr / 4 + 3);
        } else if (space == NESDEBUG_SPACE_PALETTE) {
            name = addr < 0x10 ? QStringLiteral("background palettes") : QStringLiteral("sprite palettes");
        }
        if (!name.isEmpty() && name != lastRegion) text += QStringLiteral("  ; ") + name;
        lastRegion = name;
    }
    setText(m_mem, text);

    /* the bytes Go to found stay marked across refreshes */
    QList<QTextEdit::ExtraSelection> sel;
    if (m_memHl && m_memHlSpace == space) {
        for (int row = 0; row < rows; ++row) {
            const QTextBlock block = m_mem->document()->findBlockByNumber(1 + row);
            if (!block.isValid()) break;
            for (int col = 0; col < 16; ++col) {
                const uint32_t addr = m_memTop + uint32_t(row * 16 + col);
                if (addr < m_memHlLo || addr > m_memHlHi) continue;
                QTextEdit::ExtraSelection mark;
                mark.cursor = QTextCursor(m_mem->document());
                mark.cursor.setPosition(block.position() + digits + 3 + col * 3);
                mark.cursor.setPosition(block.position() + digits + 3 + col * 3 + 2, QTextCursor::KeepAnchor);
                mark.format = accentFormat(m_mem);
                sel << mark;
            }
        }
    }
    m_mem->setExtraSelections(sel);
}

/* The disassembly lines that fit the view. */
int DebuggerWindow::visibleRows() const
{
    const int line = m_disasm->fontMetrics().lineSpacing();
    const int height = m_disasm->viewport()->height() - 2 * (int)m_disasm->document()->documentMargin();
    if (line <= 0 || height <= 0) return 32;
    return qBound(4, height / line, DISASM_WINDOW);
}

void DebuggerWindow::refreshDisasm()
{
    static nesdebug_line lines[DISASM_WINDOW];
    const int rows = visibleRows();
    if (m_followPc->isChecked()) {
        nesdebug_cpu cpu;
        nesdebug_cpu_get(m_dbg, &cpu);
        /* the PC a third of the way down */
        m_disasmTop = (uint16_t)nesdebug_row_address(m_dbg, (uint16_t)cpu.pc, -(rows / 3));
    }
    int pcLine = -1;
    const int count = nesdebug_disassemble(m_dbg, m_disasmTop, lines, rows, &pcLine);
    /* the label column is as wide as the longest label shown, within reason */
    int labelWidth = 0;
    for (int row = 0; row < count; ++row) labelWidth = qMax(labelWidth, (int)strlen(lines[row].label));
    labelWidth = qMin(labelWidth, 24);
    m_lineAddr.clear();
    QString text;
    char buf[256];
    for (int row = 0; row < count; ++row) {
        m_lineAddr.push_back(lines[row].address);
        char label[48];
        snprintf(label, sizeof label, "%.*s", labelWidth, lines[row].label);
        /* the gutter: a breakpoint dot, then the PC's arrow */
        snprintf(buf, sizeof buf, "%c %04X  %-9.9s %-*s %-22s", lines[row].is_pc ? '>' : ' ', lines[row].address,
                 lines[row].bytes, labelWidth, label, lines[row].disasm);
        if (row) text += QLatin1Char('\n');
        text += lines[row].has_breakpoint ? QStringLiteral("●") : QStringLiteral(" ");
        text += QString::fromUtf8(buf);
        if (lines[row].comment[0]) text += QStringLiteral("; ") + QString::fromUtf8(lines[row].comment);
    }
    if (count == 0) text = QStringLiteral("(no disassembly: no machine is running)");
    m_disasm->setPlainText(stripControl(text.toUtf8().constData()));
    m_disasm->verticalScrollBar()->setValue(0);

    QList<QTextEdit::ExtraSelection> sel;
    for (int row = 0; row < count; ++row) {
        /* the gutter: a breakpoint red, a disabled one grey */
        if (lines[row].has_breakpoint) {
            QTextEdit::ExtraSelection mark;
            mark.cursor = QTextCursor(m_disasm->document()->findBlockByNumber(row));
            mark.cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
            if (lines[row].breakpoint_enabled) {
                mark.format.setForeground(QColor(0xe0, 0x1b, 0x24));
                mark.format.setFontWeight(QFont::Bold);
            } else {
                mark.format.setForeground(QColor(0x9a, 0x99, 0x96));
            }
            sel << mark;
        }
        /* the selected line underlined */
        if (lines[row].address == m_selected && row != pcLine) {
            QTextEdit::ExtraSelection mark;
            mark.cursor = QTextCursor(m_disasm->document()->findBlockByNumber(row));
            mark.cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            mark.format.setFontUnderline(true);
            mark.format.setFontWeight(QFont::Bold);
            sel << mark;
        }
    }
    /* the PC's line in the accent colour, last so its ink wins */
    if (pcLine >= 0) {
        QTextCharFormat pcFormat = accentFormat(m_disasm);
        if (lines[pcLine].address == m_selected) pcFormat.setFontUnderline(true);
        sel << lineSelection(m_disasm, pcLine, pcFormat);
    }
    m_disasm->setExtraSelections(sel);
}

void DebuggerWindow::refreshBps()
{
    static nesdebug_breakpoint bps[BP_MAX];
    const int count = nesdebug_breakpoint_list(m_dbg, bps, BP_MAX);

    /* what stopped the machine, in the core's words */
    nesdebug_snapshot snap;
    nesdebug_stop_snapshot(m_dbg, &snap);
    m_bpStop->setText(snap.paused ? QStringLiteral("Stopped: %1").arg(stripControl(snap.text)) : QStringLiteral("Running."));

    /* rebuilt only when it changed (each step refreshes), and never while a
     * row's own button is being handled */
    QString sig;
    for (int idx = 0; idx < count; ++idx)
        sig += QStringLiteral("%1 %2 %3 %4 %5 %6 %7 %8\n").arg(bps[idx].id).arg(bps[idx].enabled).arg(bps[idx].hits)
                   .arg(bps[idx].type).arg(bps[idx].space).arg(bps[idx].start).arg(bps[idx].end)
                   .arg(QString::fromUtf8(bps[idx].condition));
    if (sig == m_bpSig && m_bpTable->rowCount() == count) return;
    m_bpSig = sig;

    QFont mono = m_bpTable->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    m_bpFilling = true;
    m_bpTable->setRowCount(count);
    for (int idx = 0; idx < count; ++idx) {
        auto *enabledItem = new QTableWidgetItem;
        enabledItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        enabledItem->setCheckState(bps[idx].enabled ? Qt::Checked : Qt::Unchecked);
        enabledItem->setData(Qt::UserRole, bps[idx].id);
        enabledItem->setToolTip(QStringLiteral("Enabled"));
        m_bpTable->setItem(idx, 0, enabledItem);
        QStringList type;
        if (bps[idx].type == NESDEBUG_BP_ACCESS) {
            type << QStringLiteral("access");
        } else {
            if (bps[idx].type & NESDEBUG_BP_EXEC) type << QStringLiteral("exec");
            if (bps[idx].type & NESDEBUG_BP_READ) type << QStringLiteral("read");
            if (bps[idx].type & NESDEBUG_BP_WRITE) type << QStringLiteral("write");
        }
        auto *typeItem = new QTableWidgetItem(type.join(QLatin1Char('/')));
        char desc[256];
        nesbp_describe(m_dbg, &bps[idx], desc, sizeof desc);
        typeItem->setToolTip(stripControl(desc));
        m_bpTable->setItem(idx, 1, typeItem);
        m_bpTable->setItem(idx, 2, new QTableWidgetItem(QString::fromUtf8(nesdebug_space_name(bps[idx].space))));
        /* palette and OAM addresses are a byte */
        const int digits = (bps[idx].space == NESDEBUG_SPACE_PALETTE || bps[idx].space == NESDEBUG_SPACE_OAM) ? 2 : 4;
        QString range = QStringLiteral("$") + hexText(bps[idx].start, digits);
        if (bps[idx].end != bps[idx].start) range += QStringLiteral("-$") + hexText(bps[idx].end, digits);
        m_bpTable->setItem(idx, 3, new QTableWidgetItem(range));
        auto *condItem = new QTableWidgetItem(stripControl(bps[idx].condition));
        condItem->setData(Qt::UserRole, bps[idx].id);
        condItem->setToolTip(QStringLiteral("Double-click to change the condition (empty: stop every time)"));
        m_bpTable->setItem(idx, 4, condItem);
        m_bpTable->setItem(idx, 5, new QTableWidgetItem(QString::number(bps[idx].hits)));
        for (int col = 1; col < 6; ++col) {
            m_bpTable->item(idx, col)->setFont(mono);
            if (col != 4) m_bpTable->item(idx, col)->setFlags(Qt::ItemIsEnabled);
        }
        auto *removeBtn = new QToolButton;
        removeBtn->setIcon(QIcon::fromTheme(QStringLiteral("edit-delete"), QIcon::fromTheme(QStringLiteral("user-trash"))));
        if (removeBtn->icon().isNull()) removeBtn->setText(QStringLiteral("Remove"));
        removeBtn->setAutoRaise(true);
        removeBtn->setToolTip(QStringLiteral("Remove"));
        const int bpid = bps[idx].id;
        connect(removeBtn, &QToolButton::clicked, this, [this, bpid] {
            nesdebug_breakpoint_remove(m_dbg, bpid);
            syncLater();
        });
        m_bpTable->setCellWidget(idx, 6, removeBtn);
    }
    m_bpFilling = false;
    m_bpEmpty->setVisible(count == 0);
}

void DebuggerWindow::refreshWatches()
{
    const int count = neswatch_count();
    const int keep = m_wtList->currentRow();
    QStringList rows;
    for (int idx = 0; idx < count; ++idx) {
        char line[256];
        neswatch_format(m_dbg, idx, line, sizeof line);
        rows << stripControl(line);
    }
    /* unchanged: leave the list alone (a click in it must not be lost) */
    bool same = m_wtList->count() == count;
    for (int idx = 0; same && idx < count; ++idx) same = m_wtList->item(idx)->text() == rows[idx];
    if (same) return;
    m_wtList->clear();
    m_wtList->addItems(rows);
    if (keep >= 0 && keep < count) m_wtList->setCurrentRow(keep);
}

/* The cartridge, its memory map and the I/O, from the engine's text (valid
 * while stopped). The FujiNet cartridge's own link is on the FujiNet tab. */
void DebuggerWindow::refreshCart()
{
    static char text[8192];
    nesdebug_region map[32];
    QString top;

    text[0] = '\0';
    nesdebug_cart_text(m_dbg, text, int(sizeof text));
    top += stripControl(text) + QLatin1Char('\n');
    const int regions = nesdebug_memory_map(m_dbg, map, 32);
    top += QStringLiteral("\nCPU memory map\n");
    if (!regions) top += QStringLiteral("  Stop the machine to read its memory map.\n");
    for (int idx = 0; idx < regions; ++idx)
        top += QStringLiteral("  %1-%2  %3%4  %5\n")
                   .arg(hexText(map[idx].low, 4), hexText(map[idx].high, 4))
                   .arg(map[idx].readable ? QLatin1Char('r') : QLatin1Char('-'))
                   .arg(map[idx].writable ? QLatin1Char('w') : QLatin1Char('-'))
                   .arg(QString::fromUtf8(map[idx].name));
    setText(m_cartText, top);

    /* the I/O: the engine's text repeats the cartridge first, which is above */
    text[0] = '\0';
    nesdebug_io_text(m_dbg, text, int(sizeof text));
    QString ioLines = stripControl(text);
    const int controllers = ioLines.indexOf(QStringLiteral("Controllers"));
    if (controllers > 0) ioLines = ioLines.mid(controllers);
    setText(m_ioText, ioLines.isEmpty() ? QStringLiteral("No machine running.") : ioLines);
}

/* Trace (which 0: the newest instructions, live) and History (1: what ran
 * before the stop). Both read the one ring; the filter and the note are the
 * tab's own. */
void DebuggerWindow::refreshTrace(int which, bool force)
{
    static nesdebug_trace_entry buf[HIST_FETCH];
    const bool live = which == 0;
    const bool stopped = nesdebug_is_stopped(m_dbg) != 0;
    const bool traceOn = nesdebug_trace_enabled(m_dbg) != 0, histOn = nesdebug_history_enabled(m_dbg) != 0;
    const bool recording = live ? traceOn : histOn;
    const uint64_t total = live ? nesdebug_trace_total(m_dbg) : nesdebug_history_count(m_dbg);
    uint16_t first = 0, last = 0xFFFF;
    int filter = NESDEBUG_TRACE_ALL;

    m_hSync = true;      /* the two Record buttons follow the engine */
    m_hRec[0]->setChecked(traceOn);
    m_hRec[1]->setChecked(histOn);
    m_hSync = false;

    if (!force && total == m_hSeen[which] && (live || stopped)) return;
    m_hSeen[which] = total;
    if (!live && !stopped) {
        m_hNote[which]->setText(QStringLiteral("Stop the machine to read its history."));
        return;
    }
    switch (m_hFilter[which]->currentIndex()) {
    case 1: filter = NESDEBUG_TRACE_BRANCHES; break;
    case 2:
        if (nessym_parse_range(m_dbg, m_hRange[which]->text().toUtf8().constData(), &first, &last)) filter = NESDEBUG_TRACE_RANGE;
        break;
    default: break;
    }
    QList<int> hit;
    int fetched = 0;
    if (live) {
        /* a big read pauses a running machine for a moment: a few hundred rows */
        fetched = nesdebug_trace_read(m_dbg, buf, stopped ? HIST_FETCH : LIVE_FETCH);       /* newest first */
        for (int idx = 0; idx < fetched && hit.size() < HIST_SHOWN; ++idx)
            if (nesdebug_trace_match(&buf[idx], filter, first, last)) hit.append(idx);
    } else {
        fetched = nesdebug_history_copy(m_dbg, buf, HIST_FETCH);              /* oldest first */
        for (int idx = fetched - 1; idx >= 0 && hit.size() < HIST_SHOWN; --idx)   /* the newest matches, scanning back */
            if (nesdebug_history_match(&buf[idx], filter, first, last)) hit.append(idx);
    }
    QString text;
    if (!recording && fetched == 0)
        text = QStringLiteral("%1 is off. Press Record, then let the machine run.").arg(live ? QStringLiteral("Trace") : QStringLiteral("History"));
    for (int pos = hit.size() - 1; pos >= 0; --pos) {      /* oldest first */
        char line[320];
        if (live) nesdebug_trace_format(&buf[hit[pos]], line, sizeof line);
        else nesdebug_history_format(&buf[hit[pos]], line, sizeof line);
        text += stripControl(line);
        text += QLatin1Char('\n');
    }
    setText(m_hText[which], text, true);
    m_hNote[which]->setText(QStringLiteral("%1%2 instructions recorded, last %3 shown")
                                .arg(recording ? QStringLiteral("Recording. ") : QString()).arg(total).arg(hit.size()));
}

void DebuggerWindow::refreshVideo(bool force)
{
    if (m_video) m_video->refresh(force || m_videoForce);
    m_videoForce = false;
}

/* The 2A03's five channels, from the engine (stopped only). */
void DebuggerWindow::refreshSound()
{
    static nesdebug_sound snd;
    if (!nesdebug_is_stopped(m_dbg)) {
        m_soundNote->setText(QStringLiteral("Stop the machine to read the sound channels."));
        return;
    }
    if (!nesdebug_sound_get(m_dbg, &snd)) {
        m_soundNote->setText(QStringLiteral("No machine running."));
        return;
    }
    auto cell = [this](int row, int col, const QString &text) {
        auto *item = m_soundTable->item(row, col);
        if (!item) {
            item = new QTableWidgetItem;
            m_soundTable->setItem(row, col, item);
        }
        item->setText(text);
    };
    /* a channel that is on and in the range of hearing has a pitch; a silent one's period is only a register */
    auto audible = [](const nesdebug_voice &voice) { return voice.enabled && voice.hz >= 8.0 && voice.hz <= 20000.0; };
    auto envelope = [](const nesdebug_voice &voice) {
        return QStringLiteral("%1 %2/%3%4").arg(voice.env_constant ? QStringLiteral("const") : QStringLiteral("decay"))
            .arg(voice.env_volume).arg(voice.env_counter).arg(voice.env_loop ? QStringLiteral(" loop") : QString());
    };
    auto length = [](const nesdebug_voice &voice) {
        return QStringLiteral("%1%2").arg(voice.length).arg(voice.length_halt ? QStringLiteral(" halt") : QString());
    };
    struct { const char *name; const nesdebug_voice *voice; } pulses[2] = { { "Pulse 1", &snd.pulse1 }, { "Pulse 2", &snd.pulse2 } };
    for (int row = 0; row < 2; ++row) {
        const nesdebug_voice &voice = *pulses[row].voice;
        cell(row, 0, QString::fromUtf8(pulses[row].name));
        cell(row, 1, onOff(voice.enabled));
        cell(row, 2, audible(voice) ? noteName(voice.hz) : QStringLiteral("-"));
        cell(row, 3, QString::number(voice.period));
        cell(row, 4, QString::number(voice.volume));
        cell(row, 5, QStringLiteral("duty %1").arg(voice.duty));
        cell(row, 6, length(voice));
        cell(row, 7, envelope(voice));
        cell(row, 8, voice.sweep_enabled ? QStringLiteral("on%1 period %2 shift %3")
                                               .arg(voice.sweep_negate ? QStringLiteral(" neg") : QString())
                                               .arg(voice.sweep_period).arg(voice.sweep_shift)
                                         : QStringLiteral("off"));
    }
    cell(2, 0, QStringLiteral("Triangle"));
    cell(2, 1, onOff(snd.triangle.enabled));
    cell(2, 2, audible(snd.triangle) ? noteName(snd.triangle.hz) : QStringLiteral("-"));
    cell(2, 3, QString::number(snd.triangle.period));
    cell(2, 4, QString::number(snd.triangle.volume));
    cell(2, 5, QStringLiteral("-"));
    cell(2, 6, length(snd.triangle));
    cell(2, 7, QStringLiteral("-"));
    cell(2, 8, QStringLiteral("linear %1/%2").arg(snd.triangle.linear).arg(snd.triangle.linear_reload));
    cell(3, 0, QStringLiteral("Noise"));
    cell(3, 1, onOff(snd.noise.enabled));
    cell(3, 2, snd.noise.enabled && snd.noise.hz > 0 ? QStringLiteral("%1 Hz").arg(snd.noise.hz, 0, 'f', 1) : QStringLiteral("-"));
    cell(3, 3, QString::number(snd.noise.period));
    cell(3, 4, QString::number(snd.noise.volume));
    cell(3, 5, snd.noise.noise_mode ? QStringLiteral("short") : QStringLiteral("long"));
    cell(3, 6, length(snd.noise));
    cell(3, 7, envelope(snd.noise));
    cell(3, 8, QStringLiteral("-"));
    cell(4, 0, QStringLiteral("DMC"));
    cell(4, 1, onOff(snd.dmc.enabled));
    cell(4, 2, QStringLiteral("%1 samples/s").arg(snd.dmc.rate_hz, 0, 'f', 1));
    cell(4, 3, QString::number(snd.dmc.period));
    cell(4, 4, QString::number(snd.dmc.volume));
    cell(4, 5, snd.dmc.loop ? QStringLiteral("loop") : QStringLiteral("once"));
    cell(4, 6, QStringLiteral("%1 left").arg(snd.dmc.bytes_left));
    cell(4, 7, QStringLiteral("-"));
    cell(4, 8, snd.dmc.irq ? QStringLiteral("irq") : QStringLiteral("no irq"));
    m_soundTable->resizeColumnsToContents();
    m_soundNote->setText(QStringLiteral("$4015 reads $%1 (channels running)   frame counter: %2-step, IRQ %3   DMC IRQ %4   APU clock %5 Hz\n"
                                        "DMC sample: $%6 now, from $%7, %8 bytes, %9 left.   "
                                        "Pulse and noise volume is the envelope as output now (0-15); the DMC level is 0-127.")
                             .arg(hexText(snd.status, 2), snd.five_step ? QStringLiteral("5") : QStringLiteral("4"),
                                  onOff(snd.frame_irq), onOff(snd.dmc_irq)).arg(snd.cpu_hz, 0, 'f', 0)
                             .arg(hexText(snd.dmc.address, 4), hexText(snd.dmc.start_address, 4)).arg(snd.dmc.length).arg(snd.dmc.bytes_left));
}

/* Only rebuilt when the list or the filter changed: a real game's label
 * file can be a few hundred entries. force: whatever the tab shows, on first
 * open too. */
void DebuggerWindow::refreshSymbols(bool force)
{
    const unsigned generation = nesdebug_symbols_generation(m_dbg);
    const QString filter = m_symFilter->text();
    if (!force && generation == m_symGen && filter == m_symShownFilter) return;
    m_symGen = generation;
    m_symShownFilter = filter;
    const int total = nesdebug_symbols_count(m_dbg);
    std::vector<int> found((size_t)qMax(total, 1));
    const int shown = nesdebug_symbols_filter(m_dbg, filter.toUtf8().constData(), found.data(), (int)found.size());
    m_symList->clear();
    for (int idx = 0; idx < shown; ++idx) {
        uint16_t addr;
        char name[96];
        if (!nesdebug_symbols_get(m_dbg, found[(size_t)idx], &addr, name, sizeof name)) continue;
        auto *item = new QListWidgetItem(QStringLiteral("%1  %2").arg(hexText(addr, 4), QString::fromUtf8(name)));
        item->setData(Qt::UserRole, addr);
        m_symList->addItem(item);
    }
    m_symCount->setText(QStringLiteral("%1 symbol%2%3").arg(shown).arg(shown == 1 ? QString() : QStringLiteral("s"))
                            .arg(filter.isEmpty() ? QString() : QStringLiteral(" (filtered)")));
}

/* The note and the link's state always; the log only when it changed (a new
 * packet, or a repeat, which moves the newest serial on). */
void DebuggerWindow::refreshFujinet(bool force)
{
    static nesdebug_fuji_packet packets[TRACE_MAX];
    static char state[2048];
    const int count = nesdebug_fujinet_trace(m_dbg, packets, TRACE_MAX);
    if (!nessession_is_running(m_session))
        m_fujiNote->setText(QStringLiteral("The NES is off."));
    else if (nessession_fujinet_external(m_session))
        m_fujiNote->setText(QStringLiteral("External FujiNet at %1; the cartridge is %2.")
                                .arg(QString::fromUtf8(nessession_boip_endpoint(m_session)))
                                .arg(nessession_cart_link_up(m_session) == 1 ? QStringLiteral("connected")
                                                                              : QStringLiteral("not connected")));
    else if (!nessession_fujinet_running(m_session))
        m_fujiNote->setText(QStringLiteral("FujiNet is not running (Preferences ▸ Host ▸ Enable FujiNet)."));
    else
        m_fujiNote->setText(nessession_cart_link_up(m_session) == 1
                                ? QStringLiteral("FujiNet is on; the cartridge is connected.")
                                : QStringLiteral("FujiNet is on; the cartridge is not connected."));
    state[0] = '\0';
    nesdebug_fujinet_state(m_dbg, state, int(sizeof state));
    setText(m_fujiState, stripControl(state));

    if (!force && count == m_fujiCount && (count == 0 || packets[count - 1].serial == m_fujiLast)) return;
    const bool follow = force || m_fujiCount < 0 || atEnd(m_fujiText);
    m_fujiCount = count;
    m_fujiLast = count > 0 ? packets[count - 1].serial : 0;
    QString text;
    for (int idx = 0; idx < count; ++idx) {
        if (idx) text += QLatin1Char('\n');
        text += QString::fromUtf8(packets[idx].text);
        if (packets[idx].repeat > 1) text += QStringLiteral(" ×%1").arg(packets[idx].repeat);
        text += QStringLiteral("\n     ");
        for (int byte = 0; byte < packets[idx].nbytes && byte < int(sizeof packets[idx].data); byte++)
            text += hexText(packets[idx].data[byte], 2) + QLatin1Char(' ');
        if (packets[idx].nbytes > int(sizeof packets[idx].data)) text += QStringLiteral("…");
    }
    if (count == 0) text = QStringLiteral("(no FujiBus packets since the trace was cleared)");
    setText(m_fujiText, text, follow);
}

/* One tab; live is the refresh while the machine runs, which leaves out
 * everything that reads the engine (it is read at a stop). */
void DebuggerWindow::refreshTab(int tab, bool live)
{
    switch (tab) {
    case TAB_DISASM: if (!live) refreshDisasm(); break;
    case TAB_MEMORY: if (!live) { refreshRegions(); refreshMem(); } break;
    case TAB_BREAKS: refreshBps(); break;
    case TAB_WATCHES: if (!live) refreshWatches(); break;
    case TAB_CART: if (!live) refreshCart(); break;
    case TAB_TRACE: refreshTrace(0, !live); break;
    case TAB_HISTORY: if (!live) refreshTrace(1, true); break;
    case TAB_VIDEO: refreshVideo(!live); break;
    case TAB_SOUND: if (!live) refreshSound(); break;
    case TAB_SYMBOLS: if (!live) refreshSymbols(false); break;     /* it fills when its generation first differs */
    case TAB_FUJINET: refreshFujinet(false); break;
    default: break;
    }
}

/* The memory map labels the CPU rows of Memory: read for the stop that is current. */
void DebuggerWindow::refreshRegions()
{
    nesdebug_snapshot snap;
    nesdebug_stop_snapshot(m_dbg, &snap);
    if (!m_regions.empty() && m_regionSerial == snap.serial && snap.paused) return;
    nesdebug_region map[32];
    const int count = nesdebug_memory_map(m_dbg, map, 32);
    m_regions.assign(map, map + qMax(0, count));
    m_regionSerial = snap.serial;
}

/* Every tab that is on screen (the one showing, and any peeled off). A
 * running machine is only asked the cheap things. */
void DebuggerWindow::refreshAll()
{
    nesdebug_snapshot snap;
    nesdebug_stop_snapshot(m_dbg, &snap);
    refreshStatus(snap);
    if (snap.paused) refreshRegs();
    for (int tab = 0; tab < TAB_COUNT; ++tab)
        if (tabVisible(tab)) refreshTab(tab, !snap.paused);
}

/* After anything that may have moved the engine: the console notes a new
 * stop, then the tabs showing follow. */
void DebuggerWindow::resync()
{
    nesdebug_snapshot snap;
    nesdebug_stop_snapshot(m_dbg, &snap);
    /* a step is its own answer; anything else that stopped the machine is news */
    if (snap.paused && snap.serial != m_seenSerial && snap.stop.reason != NESDEBUG_STOP_STEP && snap.text[0])
        consoleAppend(QStringLiteral("— %1\n").arg(stripControl(snap.text)));
    m_wasStopped = snap.paused != 0;
    m_seenSerial = snap.serial;
    m_seenGen = nesdebug_generation(m_dbg);
    m_runningTicks = 0;
    refreshAll();
}

/* From inside a breakpoint row's own signal: the table rebuilds after. */
void DebuggerWindow::syncLater()
{
    QTimer::singleShot(0, this, [this] { resync(); });
}

/* A step takes microseconds: a moment's wait for the stop keeps the window
 * from flashing "Running" in between. */
void DebuggerWindow::settleAndSync()
{
    for (int tries = 0; tries < 40 && !nesdebug_is_stopped(m_dbg); ++tries) QThread::msleep(1);
    resync();
}

/* The window follows the engine through the stop snapshot (cheap, and safe
 * while the machine runs), so this polls it: a refresh when it moved, and the
 * cheap live tabs while the machine runs. */
void DebuggerWindow::tick()
{
    if (!isVisible()) return;
    /* A power cycle or a new cartridge replaces the console, and the new one has no
     * debugger: attach again (which stops it at its first instruction, as opening the
     * window does) rather than leave an open window looking at nothing. */
    if (!nesdebug_is_attached(m_dbg)) {
        if (!nessession_is_running(m_session)) return;
        nesdebug_attach(m_dbg);
        m_wasStopped = false;
        m_seenSerial = ~0ull;
        m_regions.clear();
        QTimer::singleShot(0, this, [this] { settleAndSync(); });
        return;
    }
    nesdebug_snapshot snap;
    nesdebug_stop_snapshot(m_dbg, &snap);
    const unsigned gen = nesdebug_generation(m_dbg);
    const bool stopped = snap.paused != 0;
    if (snap.serial != m_seenSerial || gen != m_seenGen || stopped != m_wasStopped) {
        resync();
        return;
    }
    if (stopped) return;
    ++m_runningTicks;
    /* a running machine publishes a frame, a trace record and a packet all the time */
    if (m_runningTicks % 2 == 0 && tabVisible(TAB_VIDEO)) refreshVideo(false);
    if (m_runningTicks % 4 == 0 && tabVisible(TAB_TRACE)) refreshTrace(0, false);
    if (m_runningTicks % 5 == 0 && tabVisible(TAB_FUJINET)) refreshFujinet(false);
}

/* ---- the console ---------------------------------------------------------- */

/* Onto the transcript, which follows its end while the reader is there. */
void DebuggerWindow::consoleAppend(const QString &text)
{
    const bool follow = atEnd(m_conOut);
    QTextCursor cur(m_conOut->document());
    cur.movePosition(QTextCursor::End);
    cur.insertText(text);
    if (follow) m_conOut->verticalScrollBar()->setValue(m_conOut->verticalScrollBar()->maximum());
}

void DebuggerWindow::runConsole()
{
    static char out[65536];
    const QString cmd = m_conIn->text().trimmed();
    if (cmd.isEmpty()) return;
    if (m_history.isEmpty() || m_history.last() != cmd) m_history << cmd;
    m_historyPos = (int)m_history.size();
    /* a command brings the reader back down */
    m_conOut->verticalScrollBar()->setValue(m_conOut->verticalScrollBar()->maximum());
    consoleAppend(QStringLiteral("> %1\n").arg(cmd));
    m_conIn->clear();

    out[0] = '\0';
    nesdebug_command(m_dbg, cmd.toUtf8().constData(), out, sizeof out);
    QString text = stripControl(out);
    if (!text.isEmpty()) consoleAppend(text.endsWith(QLatin1Char('\n')) ? text : text + QLatin1Char('\n'));
    settleAndSync();
}

/* Tab: the engine completes the word at the end of the line. One match
 * replaces it; several are listed and the line extends to what they share. */
void DebuggerWindow::completeConsole()
{
    char comps[4096];
    const QString text = m_conIn->text();
    const int space = text.lastIndexOf(QLatin1Char(' '));
    const QString word = text.mid(space + 1);
    comps[0] = '\0';
    const int count = nesdebug_completions(m_dbg, word.toUtf8().constData(), comps, sizeof comps);
    if (count <= 0) return;
    const QStringList words = stripControl(comps).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (words.isEmpty()) return;
    const QString head = text.left(space + 1);
    if (words.size() == 1) {
        m_conIn->setText(head + words[0] + QLatin1Char(' '));
        return;
    }
    QString common = words[0];
    for (const QString &each : words)
        while (!each.startsWith(common)) common.chop(1);
    consoleAppend(words.join(QStringLiteral("  ")) + QLatin1Char('\n'));
    if (common.size() > word.size()) m_conIn->setText(head + common);
}

/* Messages land in the console and, since it may not be the tab showing,
 * the status bar. */
void DebuggerWindow::say(const QString &msg)
{
    consoleAppend(msg + QLatin1Char('\n'));
    message(msg);
}

void DebuggerWindow::message(const QString &msg)
{
    statusBar()->showMessage(msg, 6000);
}

/* ---- the rest of the handlers ----------------------------------------------- */

void DebuggerWindow::toggleRun()
{
    if (nesdebug_is_stopped(m_dbg)) {
        nesdebug_resume(m_dbg);
        resync();
    } else {
        nesdebug_stop(m_dbg);
        settleAndSync();
    }
}

void DebuggerWindow::stepAnd(void (*func)(nesdebug *))
{
    if (!nesdebug_is_stopped(m_dbg)) {
        message(QStringLiteral("Stop the machine first"));
        return;
    }
    func(m_dbg);
    settleAndSync();
}

void DebuggerWindow::runToCursor()
{
    if (m_selected < 0) {
        message(QStringLiteral("Select a line in the Disassembly to run to"));
        return;
    }
    if (!nesdebug_is_stopped(m_dbg)) {
        message(QStringLiteral("Stop the machine first"));
        return;
    }
    nesdebug_run_to(m_dbg, (uint16_t)m_selected);
    settleAndSync();
}

void DebuggerWindow::toggleBreakpoint(int addr)
{
    nesdebug_breakpoint_toggle(m_dbg, (uint16_t)addr);
    resync();
}

void DebuggerWindow::toggleSelected()
{
    if (m_selected < 0) {
        message(QStringLiteral("Select a line in the Disassembly first"));
        return;
    }
    toggleBreakpoint(m_selected);
}

/* A number the way the prompt takes it: a bare token of hex digits is hex (as
 * the register and value entries always were), anything else goes to the
 * engine's expression evaluator ($C000, 49152, a label, a + 1 ...). */
bool DebuggerWindow::numberOf(const QString &text, long *out, QString *why)
{
    static const QRegularExpression hexOnly(QStringLiteral("^[0-9A-Fa-f]{1,8}$"));
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        if (why) *why = QStringLiteral("Type a value");
        return false;
    }
    if (hexOnly.match(trimmed).hasMatch()) {
        *out = trimmed.toLong(nullptr, 16);
        return true;
    }
    char err[160] = "";
    if (nesdebug_eval(m_dbg, trimmed.toUtf8().constData(), out, err, int(sizeof err)) == 0) return true;
    if (why) *why = stripControl(err);
    return false;
}

/* A label, or a number as numberOf reads it. */
bool DebuggerWindow::addressOf(const QString &text, long *out)
{
    const int lbl = nesdebug_label_address(m_dbg, text.trimmed().toUtf8().constData());
    if (lbl >= 0) { *out = lbl; return true; }
    return numberOf(text, out, nullptr);
}

/* An address in the Memory tab's space: labels name CPU addresses only. */
bool DebuggerWindow::memAddressOf(const QString &text, long *out, QString *why)
{
    if (memSpace() == NESDEBUG_SPACE_CPU && addressOf(text, out)) return true;
    return memSpace() != NESDEBUG_SPACE_CPU && numberOf(text, out, why);
}

/* One register or flag through the engine's grammar: it says if it will not
 * take the value (running, out of range). */
void DebuggerWindow::applyRegister(const char *name, long value, const QString &okMessage)
{
    char err[200] = "";
    if (nesdebug_register_set(m_dbg, name, value, err, sizeof err))
        message(err[0] ? okMessage + QStringLiteral(": ") + stripControl(err) : okMessage);
    else
        message(err[0] ? stripControl(err) : QStringLiteral("The register could not be written"));
}

/* Edits go in only while the machine is stopped. */
void DebuggerWindow::setReg(int idx)
{
    long val = 0;
    QString why;
    QLineEdit *edit = m_reg[idx];
    if (!nesdebug_is_stopped(m_dbg)) {
        message(QStringLiteral("Stop the machine first"));
    } else if (!numberOf(edit->text(), &val, &why)) {
        message(why);
    } else if (val < 0 || val > kRegs[idx].limit) {
        message(QStringLiteral("%1 holds $0-$%2").arg(QString::fromUtf8(kRegs[idx].name), hexText(unsigned(kRegs[idx].limit), kRegs[idx].digits)));
    } else {
        applyRegister(kRegs[idx].engine, val,
                      idx == REG_PC ? QStringLiteral("PC set") : QStringLiteral("%1 set").arg(QString::fromUtf8(kRegs[idx].name)));
    }
    edit->clearFocus();
    resync();
}

void DebuggerWindow::setFlag(int idx, bool enabled)
{
    if (!nesdebug_is_stopped(m_dbg)) message(QStringLiteral("Stop the machine first"));
    else applyRegister(kFlags[idx].engine, enabled ? 1 : 0, QStringLiteral("Flag set"));
    resync();
}

/* Shows a range of a space in the Memory tab, highlighted, on a row boundary
 * and a little above the middle. */
void DebuggerWindow::showInMemory(int space, uint32_t first, uint32_t last)
{
    m_memSwitching = true;
    const int pos = m_memSpaceBox->findData(space);
    if (pos >= 0) m_memSpaceBox->setCurrentIndex(pos);
    m_memSwitching = false;
    const int size = nesdebug_space_size(m_dbg, space);
    m_memHl = true;
    m_memHlSpace = space;
    m_memHlLo = first;
    m_memHlHi = last;
    m_memTop = uint32_t(qBound<long>(0L, long((first > 0x100 ? first - 0x100 : 0) & ~0xFu), long(qMax(0, size - MEM_ROWS * 16))) & ~0xFL);
    if (size > 0 && m_memTop >= uint32_t(size)) m_memTop = uint32_t(size - 1) & ~0xFu;
    refreshMem();
    const int line = 1 + int(first - m_memTop) / 16;
    const int shown = m_mem->viewport()->height() / qMax(1, m_mem->fontMetrics().lineSpacing());
    m_mem->verticalScrollBar()->setValue(qMax(0, line - shown * 3 / 10));
}

/* The space chooser: the dump starts at the top of the new space. */
void DebuggerWindow::memSpaceChanged()
{
    if (m_memSwitching) return;
    m_memTop = 0;
    m_memHl = false;
    m_srchNote->clear();
    m_memGoto->setPlaceholderText(memSpace() == NESDEBUG_SPACE_CPU ? QStringLiteral("$8000, label or lo-hi")
                                                                   : QStringLiteral("$0000 or lo-hi"));
    m_memAddr->setPlaceholderText(memSpace() == NESDEBUG_SPACE_CPU ? QStringLiteral("address or label")
                                                                   : QStringLiteral("address"));
    refreshMem();
}

/* Go to: in the CPU space an address, a label, an expression or lo-hi; in the
 * others a number or lo-hi. */
void DebuggerWindow::memGoto()
{
    const QString text = m_memGoto->text();
    const int space = memSpace();
    uint16_t first = 0, last = 0;
    bool found = false;
    if (space == NESDEBUG_SPACE_CPU) {
        found = nessym_parse_range(m_dbg, text.toUtf8().constData(), &first, &last) != 0;
    } else {
        static const QRegularExpression range(QStringLiteral("^\\s*\\$?([0-9A-Fa-f]+)\\s*(?:-|\\.\\.)\\s*\\$?([0-9A-Fa-f]+)\\s*$"));
        const QRegularExpressionMatch match = range.match(text);
        long lowAddr = 0, highAddr = 0;
        if (match.hasMatch()) {
            lowAddr = match.captured(1).toLong(nullptr, 16);
            highAddr = match.captured(2).toLong(nullptr, 16);
            found = lowAddr <= highAddr;
        } else {
            found = numberOf(text, &lowAddr, nullptr);
            highAddr = lowAddr;
        }
        if (found && (lowAddr < 0 || highAddr < lowAddr || highAddr > 0xFFFFFF)) found = false;
        first = uint16_t(lowAddr);
        last = uint16_t(highAddr);
        if (found) {
            showInMemory(space, uint32_t(lowAddr), uint32_t(highAddr));
            return;
        }
    }
    if (found) {
        showInMemory(space, first, last);
    } else {
        m_memHl = false;
        refreshMem();
        message(QStringLiteral("Not an address ($C000, a label or first-last)"));
    }
}

void DebuggerWindow::memPoke()
{
    long addr, val;
    QString why;
    if (!nesdebug_is_stopped(m_dbg)) {
        message(QStringLiteral("Stop the machine first"));
        return;
    }
    const int space = memSpace();
    const int size = nesdebug_space_size(m_dbg, space);
    if (!memAddressOf(m_memAddr->text(), &addr, nullptr) || addr < 0 || addr >= qMax(size, 1)) {
        message(QStringLiteral("Not an address in the %1 space").arg(QString::fromUtf8(nesdebug_space_name(space))));
        return;
    }
    if (!numberOf(m_memVal->text(), &val, &why) || val < 0 || val > 0xFF) {
        message(why.isEmpty() ? QStringLiteral("The value is a byte ($00-$FF)") : why);
        return;
    }
    const uint8_t byte = uint8_t(val);
    nesdebug_write_space(m_dbg, space, uint32_t(addr), &byte, 1);
    message(QStringLiteral("Wrote $%1 to %2 $%3").arg(hexText((unsigned)val, 2), QString::fromUtf8(nesdebug_space_name(space)),
                                                     hexText((unsigned)addr, memDigits())));
    resync();
}

/* Find Next / Previous: from the highlighted bytes, or the top of the dump. */
void DebuggerWindow::memSearch(bool backward)
{
    uint8_t pat[NESSEARCH_MAX], mask;
    int count = 0, flags = 0;
    uint32_t found;
    const int space = memSpace();
    if (!nessearch_parse(m_srchKind->currentIndex(), m_srchEntry->text().toUtf8().constData(), pat, &count, &mask, &flags)) {
        m_srchNote->setText(QStringLiteral("Type hex bytes (A9 01) or some text."));
        return;
    }
    if (!nesdebug_is_stopped(m_dbg)) {
        m_srchNote->setText(QStringLiteral("Stop the machine to search its memory."));
        return;
    }
    const uint32_t from = (m_memHl && m_memHlSpace == space) ? (backward ? m_memHlLo : m_memHlLo + 1) : m_memTop;
    const int hit = nesdebug_search_next_in(m_dbg, space, pat, count, mask, flags, from, backward ? 1 : 0, &found);
    if (hit > 0) {
        m_srchNote->setText(QStringLiteral("Found at $%1").arg(hexText(found, memDigits())));
        showInMemory(space, found, found + uint32_t(count) - 1);
    } else {
        m_srchNote->setText(hit == 0 ? QStringLiteral("Not found.") : QStringLiteral("Stop the machine to search its memory."));
    }
}

void DebuggerWindow::jumpTo()
{
    long address;
    if (!addressOf(m_jump->text(), &address) || address < 0 || address > 0xFFFF) {
        message(QStringLiteral("No such address or label"));
        return;
    }
    jumpToAddress(int(address));
}

/* The Disassembly on an address: Follow PC off, the line selected. */
void DebuggerWindow::jumpToAddress(int address)
{
    m_followPc->setChecked(false);
    m_selected = address;
    m_disasmTop = (uint16_t)nesdebug_row_address(m_dbg, (uint16_t)address, -(visibleRows() / 3));
    refreshDisasm();
}

/* The space picks the address range; an execute breakpoint is CPU-space only
 * (the engine says so). The end is optional; the condition is an expression
 * the engine checks. */
void DebuggerWindow::addBreakpoint()
{
    static const int types[4] = { NESDEBUG_BP_EXEC, NESDEBUG_BP_READ, NESDEBUG_BP_WRITE, NESDEBUG_BP_ACCESS };
    const int sel = qBound(0, m_bpType->currentIndex(), 3);
    const int space = m_bpSpace->currentData().toInt();
    const QString start = m_bpStart->text().trimmed(), end = m_bpEnd->text().trimmed();
    if (start.isEmpty()) return;
    long first, last;
    const bool cpu = space == NESDEBUG_SPACE_CPU;
    const bool parsed = cpu ? addressOf(start, &first) : numberOf(start, &first, nullptr);
    if (!parsed || first < 0 || first > 0xFFFF) {
        m_bpMsg->setText(cpu ? QStringLiteral("Not an address ($C000, or a label)") : QStringLiteral("Not an address in the %1 space")
                                                                                          .arg(QString::fromUtf8(nesdebug_space_name(space))));
        return;
    }
    last = first;
    if (!end.isEmpty() && (!(cpu ? addressOf(end, &last) : numberOf(end, &last, nullptr)) || last < first || last > 0xFFFF)) {
        m_bpMsg->setText(QStringLiteral("The end is not an address at or after the start"));
        return;
    }
    char err[256] = "";
    const QByteArray cond = m_bpCond->text().trimmed().toUtf8();
    if (nesdebug_breakpoint_add_in(m_dbg, space, types[sel], (uint32_t)first, (uint32_t)last, cond.constData(), err,
                                   sizeof err) < 0) {
        m_bpMsg->setText(err[0] ? stripControl(err) : QStringLiteral("The breakpoint could not be added"));
        return;
    }
    m_bpMsg->clear();
    m_bpStart->clear();
    m_bpEnd->clear();
    m_bpCond->clear();
    resync();
}

void DebuggerWindow::addWatch()
{
    if (neswatch_add(m_wtEntry->text().toUtf8().constData()) < 0) {
        message(QStringLiteral("Type an address, label, expression or lo-hi (the list holds %1)").arg(NESWATCH_MAX));
        return;
    }
    m_wtEntry->clear();
    resync();
}

void DebuggerWindow::breakAtSymbol()
{
    QListWidgetItem *item = m_symList->currentItem();
    if (!item) {
        message(QStringLiteral("Select a symbol first"));
        return;
    }
    const uint16_t addr = uint16_t(item->data(Qt::UserRole).toUInt());
    const int now = nesdebug_breakpoint_toggle(m_dbg, addr);
    message(now ? QStringLiteral("Breakpoint at $%1").arg(hexText(addr, 4)) : QStringLiteral("Breakpoint at $%1 removed").arg(hexText(addr, 4)));
    resync();
}

void DebuggerWindow::loadSymbols(bool fromFile)
{
    QString path;
    if (fromFile) {
        const QString cart = QString::fromUtf8(nessession_get_str(m_session, "cart", ""));
        path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Load Symbols"), cart.isEmpty() ? QString() : QFileInfo(cart).absolutePath(),
            QStringLiteral("Symbol files (*.dbg *.lbl *.mlb *.sym *.labels);;All files (*)"));
        if (path.isEmpty()) return;
    }
    char msg[512] = "";
    const QByteArray bytes = path.toLocal8Bit();
    nesdebug_load_symbols(m_dbg, fromFile ? bytes.constData() : nullptr, msg, sizeof msg);
    say(msg[0] ? stripControl(msg) : QStringLiteral("No symbols found"));
    refreshSymbols(true);
    resync();
}

void DebuggerWindow::save(const char *kind, const QString &title, const QString &file)
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save %1").arg(title), file);
    if (path.isEmpty()) return;
    char msg[512] = "";
    nesdebug_save(m_dbg, kind, path.toLocal8Bit().constData(), msg, sizeof msg);
    say(msg[0] ? stripControl(msg) : QStringLiteral("Saved %1").arg(path));
}

/* ---- tabs peeled off into windows of their own --------------------------------- */

/* A page's place in the tab widget is not its identity once tabs can be
 * moved into windows of their own: each carries its original index. */
int DebuggerWindow::currentTab() const
{
    QWidget *page = m_tabs->currentWidget();
    return page ? page->property("tab").toInt() : -1;
}

/* On screen: the tab widget's page, or a window of its own. */
bool DebuggerWindow::tabVisible(int tab) const
{
    return currentTab() == tab || m_popped[tab] != nullptr;
}

/* Brings a tab forward, wherever it lives. */
void DebuggerWindow::showTab(int tab)
{
    if (m_popped[tab]) {
        m_popped[tab]->activateWindow();
        m_popped[tab]->raise();
    } else {
        m_tabs->setCurrentWidget(m_pages[tab]);
    }
}

void DebuggerWindow::popOut(int tab)
{
    if (tab < 0 || tab >= TAB_COUNT || m_popped[tab] || m_tabs->count() <= 1) return;   /* one tab always stays */
    QWidget *page = m_pages[tab];
    m_tabs->removeTab(m_tabs->indexOf(page));

    auto *win = new PopWindow;
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->setWindowTitle(QStringLiteral("Debugger – %1").arg(QString::fromUtf8(kTabs[tab].label)));
    win->setProperty("nesHelpContext", QStringLiteral("debugger/%1").arg(QLatin1String(kTabs[tab].name)));
    auto *layout = new QVBoxLayout(win);
    auto *bar = new QHBoxLayout;
    auto *dockBtn = new QPushButton(QStringLiteral("Dock"));
    dockBtn->setToolTip(QStringLiteral("Put this tab back in the debugger"));
    bar->addWidget(dockBtn);
    bar->addStretch();
    layout->addLayout(bar);
    layout->addWidget(page, 1);
    page->show();
    win->resize(760, 520);
    m_popped[tab] = win;
    win->onClose = [this, tab] {
        if (!m_closing) dock(tab);
    };
    win->onKey = [this](QKeyEvent *evt) { return handleKey(evt); };
    connect(dockBtn, &QPushButton::clicked, win, &QWidget::close);
    new QShortcut(QKeySequence(Qt::Key_F1), win, [win] { nesHelpFor(win); });
    win->show();
    resync();
}

void DebuggerWindow::dock(int tab)
{
    QWidget *win = m_popped[tab];
    if (!win) return;
    QWidget *page = m_pages[tab];
    m_popped[tab] = nullptr;
    int pos = 0;
    for (int before = 0; before < tab; before++)   /* back where it was in the order */
        if (!m_popped[before]) pos++;
    page->setParent(nullptr);
    m_tabs->insertTab(pos, page, tabLabel(tab));
    m_tabs->setCurrentIndex(pos);
    resync();
}

/* ---- events --------------------------------------------------------------- */

bool DebuggerWindow::eventFilter(QObject *obj, QEvent *evt)
{
    /* Tab completes; Up and Down walk the history */
    if (obj == m_conIn && evt->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(evt);
        if (keyEvent->key() == Qt::Key_Tab) {
            completeConsole();
            return true;
        }
        if (keyEvent->key() == Qt::Key_Up || keyEvent->key() == Qt::Key_Down) {
            if (keyEvent->key() == Qt::Key_Up && m_historyPos > 0) --m_historyPos;
            else if (keyEvent->key() == Qt::Key_Down && m_historyPos < m_history.size()) ++m_historyPos;
            m_conIn->setText(m_historyPos < m_history.size() ? m_history.at(m_historyPos) : QString());
            return true;
        }
    }
    if (m_disasm && obj == m_disasm->viewport()) {
        /* A click selects a line (Run to Cursor); a click on its left edge,
         * or a double-click, toggles its breakpoint. A label has no line of
         * its own, so there is nothing else to hit. */
        if (evt->type() == QEvent::MouseButtonPress || evt->type() == QEvent::MouseButtonDblClick) {
            auto *mouseEvent = static_cast<QMouseEvent *>(evt);
            if (mouseEvent->button() != Qt::LeftButton) return true;
            const QTextCursor cur = m_disasm->cursorForPosition(mouseEvent->position().toPoint());
            const int line = cur.blockNumber();
            if (line < 0 || line >= (int)m_lineAddr.size()) return true;
            const bool gutter = cur.positionInBlock() <= 2;
            const bool dbl = evt->type() == QEvent::MouseButtonDblClick;
            m_selected = m_lineAddr[(size_t)line];
            if (gutter ? !dbl : dbl) toggleBreakpoint(m_lineAddr[(size_t)line]);
            else refreshDisasm();
            return true;
        }
        if (evt->type() == QEvent::MouseButtonRelease || evt->type() == QEvent::MouseMove) return true;
        if (evt->type() == QEvent::Wheel) {
            m_wheel += static_cast<QWheelEvent *>(evt)->angleDelta().y();
            const int rows = -m_wheel / 40;     /* a notch is three rows */
            if (rows != 0) {
                m_wheel += rows * 40;
                m_followPc->setChecked(false);
                m_disasmTop = (uint16_t)nesdebug_row_address(m_dbg, m_disasmTop, rows);
                refreshDisasm();
            }
            return true;
        }
        if (evt->type() == QEvent::Resize && isVisible()) refreshDisasm();
    }
    return QMainWindow::eventFilter(obj, evt);
}

/* These keys are the debugger's here, taken as plain key presses ahead of
 * any application-wide shortcut. */
bool DebuggerWindow::event(QEvent *evt)
{
    if (evt->type() == QEvent::ShortcutOverride && isDebuggerKey(static_cast<QKeyEvent *>(evt))) {
        evt->accept();
        return true;
    }
    return QMainWindow::event(evt);
}

/* The family's keys, from this window or one of its peeled-off tabs. */
bool DebuggerWindow::handleKey(QKeyEvent *evt)
{
    if (!isDebuggerKey(evt)) return false;
    const bool shift = evt->modifiers() & Qt::ShiftModifier;
    /* held, the steps repeat; the others do not */
    if (evt->isAutoRepeat() && (evt->key() == Qt::Key_F5 || evt->key() == Qt::Key_F9 || evt->key() == Qt::Key_F12)) return true;
    switch (evt->key()) {
    case Qt::Key_F5: toggleRun(); break;
    case Qt::Key_F7: stepAnd(shift ? frameOne : nesdebug_step); break;
    case Qt::Key_F8: stepAnd(shift ? nesdebug_step_out : nesdebug_step_over); break;
    case Qt::Key_F9: toggleSelected(); break;
    default: hide(); break;     /* F12 */
    }
    return true;
}

void DebuggerWindow::keyPressEvent(QKeyEvent *evt)
{
    if (!handleKey(evt)) QMainWindow::keyPressEvent(evt);
}

/* Shown: the engine attaches, which stops the machine, as on every sibling.
 * The tabs have no size until the window is up, so the first refresh waits
 * for the event loop. A restore from the taskbar is not a show. */
void DebuggerWindow::showEvent(QShowEvent *evt)
{
    QMainWindow::showEvent(evt);
    m_timer.start(100);
    if (evt->spontaneous()) return;
    nesdebug_attach(m_dbg);
    m_wasStopped = false;
    m_seenSerial = ~0ull;
    for (QWidget *win : m_popped)
        if (win) win->show();
    QTimer::singleShot(0, this, [this] {
        if (!isVisible()) return;
        firstShow();
        if (currentTab() == TAB_CONSOLE) m_conIn->setFocus();
        settleAndSync();
    });
}

/* The scripted-capture variables that touch the machine (read at construction),
 * applied once the window is up: NES_DEBUGGER_WATCH adds a watch,
 * NES_DEBUGGER_WATCH_BREAK a write-when-changed breakpoint on it,
 * NES_DEBUGGER_MEM_GOTO shows it in Memory, NES_DEBUGGER_HISTORY records
 * from the start. (NES_DEBUGGER_TAB and NES_DEBUGGER_POPOUT are read at
 * construction; NES_DEBUGGER_VIDEO_* by the Video tab.) */
void DebuggerWindow::firstShow()
{
    if (m_shownOnce) return;
    m_shownOnce = true;
    uint16_t first, last;
    if (!m_envWatch.isEmpty()) neswatch_add(m_envWatch.constData());
    if (!m_envWatchBreak.isEmpty() && nessym_parse_range(m_dbg, m_envWatchBreak.constData(), &first, &last)) {
        char err[200] = "";
        nesdebug_breakpoint_add_in(m_dbg, NESDEBUG_SPACE_CPU, NESDEBUG_BP_WRITE, first, last, "value != old", err, sizeof err);
    }
    if (!m_envMemGoto.isEmpty() && nessym_parse_range(m_dbg, m_envMemGoto.constData(), &first, &last)) {
        showInMemory(NESDEBUG_SPACE_CPU, first, last);
    }
    if (m_envHistory) nesdebug_history_enable(m_dbg, 1);   /* record from the start */
}

/* Hidden by F12, the close button or the main window: the engine goes away
 * and the machine runs on. The close button is a plain close, which only
 * hides the window (it is not deleted, so it comes back as it was); ignoring
 * the close instead would make QApplication's quit, which closes every window
 * first, give up whenever it reached this one. A minimised window's
 * spontaneous hide is not a detach. The peeled-off tabs go with it. */
void DebuggerWindow::hideEvent(QHideEvent *evt)
{
    m_timer.stop();
    if (!evt->spontaneous()) {
        for (QWidget *win : m_popped)
            if (win) win->hide();
        if (!m_closing) nesdebug_detach(m_dbg);
    }
    QMainWindow::hideEvent(evt);
}
