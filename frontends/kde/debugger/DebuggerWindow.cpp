/*
 * DebuggerWindow -- see DebuggerWindow.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DebuggerWindow.h"

#include <QFileDialog>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QKeyEvent>
#include <QMenu>
#include <QPixmap>
#include <QPointer>
#include <QScrollBar>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextCursor>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "../FujiNetWindows.h"

#define DISASM_WINDOW 48

namespace {

QPlainTextEdit *monoView(bool editable)
{
    auto *v = new QPlainTextEdit;
    v->setReadOnly(!editable);
    QFont f = v->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    v->setFont(f);
    v->setLineWrapMode(QPlainTextEdit::NoWrap);
    return v;
}

QString stripControl(const char *s)
{
    QString out;
    for (; *s; ++s)
        if ((unsigned char)*s >= 0x20 || *s == '\n' || *s == '\t') out += QChar(*s);
    return out;
}

bool parseNum(const QString &t, long *out)
{
    QString s = t.trimmed();
    bool ok = false;
    long v = 0;
    if (s.startsWith('$')) v = s.mid(1).toLong(&ok, 16);
    else if (s.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) v = s.mid(2).toLong(&ok, 16);
    else if (s.startsWith('#')) v = s.mid(1).toLong(&ok, 10);
    else v = s.toLong(&ok, 16);
    if (ok) *out = v;
    return ok;
}

QString hx(unsigned v, int digits)
{
    return QStringLiteral("%1").arg(v, digits, 16, QLatin1Char('0')).toUpper();
}

QPointer<DebuggerWindow> s_win;

} // namespace

void DebuggerWindow::showFor(QWidget *parent, nessession *session)
{
    if (!s_win) s_win = new DebuggerWindow(session, parent);
    s_win->show();
    s_win->raise();
    s_win->activateWindow();
    /* Opening the debugger stops the machine, as on every sibling. */
    nesdebug_attach(s_win->m_dbg);
    s_win->refreshAll();
}

void DebuggerWindow::toggleFor(QWidget *parent, nessession *session)
{
    if (s_win && s_win->isVisible()) s_win->hide();
    else showFor(parent, session);
}

DebuggerWindow::DebuggerWindow(nessession *session, QWidget *parent)
    : QMainWindow(parent, Qt::Window), m_session(session), m_dbg(nessession_debugger(session))
{
    setWindowTitle(QStringLiteral("Debugger"));
    resize(1100, 780);
    m_ppuPx.resize(NESDEBUG_VIEW_MAX_PIXELS);

    auto *central = new QWidget;
    auto *root = new QVBoxLayout(central);
    root->addWidget(buildToolbar());
    auto *tabs = new QTabWidget;
    tabs->addTab(buildPrompt(), QStringLiteral("Prompt"));
    tabs->addTab(buildCpu(), QStringLiteral("CPU && RAM"));
    tabs->addTab(buildDisasm(), QStringLiteral("Disassembly"));
    tabs->addTab(buildPpu(), QStringLiteral("PPU"));
    tabs->addTab(buildApu(), QStringLiteral("APU && Input"));
    tabs->addTab(buildBreaks(), QStringLiteral("Breakpoints"));
    tabs->addTab(buildCart(), QStringLiteral("Cart"));
    if (qEnvironmentVariableIsSet("NES_DEBUGGER_TAB"))
        tabs->setCurrentIndex(qEnvironmentVariableIntValue("NES_DEBUGGER_TAB"));
    root->addWidget(tabs, 1);
    setCentralWidget(central);

    connect(&m_timer, &QTimer::timeout, this, &DebuggerWindow::tick);
    m_timer.start(100);
}

/* ---- building ------------------------------------------------------------- */

void DebuggerWindow::stepAnd(void (*fn)(nesdebug *))
{
    fn(m_dbg);
    refreshAll();
}

QWidget *DebuggerWindow::buildToolbar()
{
    auto *bar = new QWidget;
    auto *h = new QHBoxLayout(bar);
    h->setContentsMargins(0, 0, 0, 0);
    auto add = [&](const QString &label, auto fn) {
        auto *b = new QPushButton(label);
        b->setFocusPolicy(Qt::NoFocus);
        connect(b, &QPushButton::clicked, this, fn);
        h->addWidget(b);
        return b;
    };
    m_runBtn = add(QStringLiteral("Stop (F5)"), [this] {
        if (nesdebug_is_stopped(m_dbg)) nesdebug_resume(m_dbg); else nesdebug_stop(m_dbg);
        refreshAll();
    });
    add(QStringLiteral("Step (F7)"), [this] { stepAnd(nesdebug_step); });
    add(QStringLiteral("Step Over (F8)"), [this] { stepAnd(nesdebug_step_over); });
    add(QStringLiteral("Step Out (⇧F8)"), [this] { stepAnd(nesdebug_step_out); });
    add(QStringLiteral("Scanline+1"), [this] { nesdebug_scanline(m_dbg, 1); refreshAll(); });
    add(QStringLiteral("Frame+1"), [this] { nesdebug_frame(m_dbg, 1); refreshAll(); });
    m_status = new QLabel;
    m_status->setStyleSheet(QStringLiteral("color: gray;"));
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    h->addWidget(m_status, 1);
    return bar;
}

QWidget *DebuggerWindow::buildPrompt()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    m_promptOut = monoView(false);
    m_promptOut->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_promptOut->setPlainText(QStringLiteral("Debugger prompt. Type 'help' for every command.\n"));
    auto *row = new QHBoxLayout;
    m_promptIn = new QLineEdit;
    m_promptIn->setPlaceholderText(QStringLiteral("command (help, step, break, bpw, print, mem, poke, runto, disasm, cart ...) — Tab completes"));
    m_promptIn->installEventFilter(this);
    connect(m_promptIn, &QLineEdit::returnPressed, this, &DebuggerWindow::runPrompt);
    auto *sym = new QPushButton(QStringLiteral("Load symbols..."));
    connect(sym, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Load symbols"), QString(),
            QStringLiteral("Symbol files (*.dbg *.lbl *.mlb *.sym *.labels);;All files (*)"));
        if (path.isEmpty()) return;
        char msg[512];
        nesdebug_load_symbols(m_dbg, path.toLocal8Bit().constData(), msg, sizeof msg);
        appendPrompt(QString::fromUtf8(msg) + QLatin1Char('\n'));
        refreshAll();
    });
    auto *save = new QPushButton(QStringLiteral("Save..."));
    auto *menu = new QMenu(save);
    static const struct { const char *kind, *title; } saves[] = {
        { "dis", "Disassembly ($8000-$FFFF)" }, { "prg", "PRG SRAM" }, { "chr", "CHR SRAM" },
        { "ram", "Console RAM" }, { "wram", "WRAM" } };
    for (const auto &s : saves) {
        const char *kind = s.kind;
        const char *title = s.title;
        menu->addAction(QString::fromUtf8(title), this, [this, kind, title] {
            const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save %1").arg(QString::fromUtf8(title)));
            if (path.isEmpty()) return;
            char msg[512];
            nesdebug_save(m_dbg, kind, path.toLocal8Bit().constData(), msg, sizeof msg);
            appendPrompt(stripControl(msg) + QLatin1Char('\n'));
        });
    }
    save->setMenu(menu);
    row->addWidget(m_promptIn, 1);
    row->addWidget(sym);
    row->addWidget(save);
    v->addWidget(m_promptOut, 1);
    v->addLayout(row);
    return w;
}

QWidget *DebuggerWindow::buildCpu()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *regs = new QHBoxLayout;
    static const char *const names[6] = { "PC", "SP", "A", "X", "Y", "P" };
    static const int regIds[6] = { NES_REG_PC, NES_REG_SP, NES_REG_A, NES_REG_X, NES_REG_Y, NES_REG_PS };
    for (int i = 0; i < 6; ++i) {
        regs->addWidget(new QLabel(QString::fromUtf8(names[i])));
        m_reg[i] = new QLineEdit;
        m_reg[i]->setMaxLength(5);
        m_reg[i]->setMaximumWidth(64);
        connect(m_reg[i], &QLineEdit::returnPressed, this, [this, i] {
            long val;
            if (parseNum(m_reg[i]->text(), &val)) nesdebug_cpu_set(m_dbg, regIds[i], (int)val);
            m_reg[i]->clearFocus();
            refreshAll();
        });
        regs->addWidget(m_reg[i]);
    }
    regs->addStretch();
    v->addLayout(regs);
    auto *flags = new QHBoxLayout;
    static const char *const fnames[6] = { "N", "V", "D", "I", "Z", "C" };
    static const int flagIds[6] = { NES_FLAG_N, NES_FLAG_V, NES_FLAG_D, NES_FLAG_I, NES_FLAG_Z, NES_FLAG_C };
    for (int i = 0; i < 6; ++i) {
        m_flag[i] = new QCheckBox(QString::fromUtf8(fnames[i]));
        connect(m_flag[i], &QCheckBox::clicked, this, [this, i](bool on) {
            if (nesdebug_is_stopped(m_dbg)) nesdebug_cpu_set(m_dbg, flagIds[i], on);
            refreshAll();
        });
        flags->addWidget(m_flag[i]);
    }
    m_cycles = new QLabel;
    m_cycles->setStyleSheet(QStringLiteral("color: gray;"));
    flags->addWidget(m_cycles, 1);
    v->addLayout(flags);
    v->addWidget(new QLabel(QStringLiteral("Console RAM ($0000-$07FF)")));
    m_ram = monoView(false);
    v->addWidget(m_ram, 1);
    auto *edit = new QHBoxLayout;
    m_ramAddr = new QLineEdit; m_ramAddr->setPlaceholderText(QStringLiteral("$0300")); m_ramAddr->setMaximumWidth(80);
    m_ramVal = new QLineEdit; m_ramVal->setPlaceholderText(QStringLiteral("$00")); m_ramVal->setMaximumWidth(60);
    auto write = [this] {
        long a, val;
        if (parseNum(m_ramAddr->text(), &a) && parseNum(m_ramVal->text(), &val))
            nesdebug_write(m_dbg, (uint16_t)a, (uint8_t)val);
        refreshAll();
    };
    connect(m_ramVal, &QLineEdit::returnPressed, this, write);
    connect(m_ramAddr, &QLineEdit::returnPressed, this, write);
    edit->addWidget(new QLabel(QStringLiteral("Write address (any: RAM, WRAM, cartridge SRAM)")));
    edit->addWidget(m_ramAddr);
    edit->addWidget(new QLabel(QStringLiteral("value")));
    edit->addWidget(m_ramVal);
    edit->addStretch();
    v->addLayout(edit);
    return w;
}

QWidget *DebuggerWindow::buildDisasm()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    m_followPc = new QCheckBox(QStringLiteral("Follow PC"));
    m_followPc->setChecked(true);
    connect(m_followPc, &QCheckBox::toggled, this, [this](bool on) { if (on) refreshDisasm(); });
    m_jump = new QLineEdit;
    m_jump->setPlaceholderText(QStringLiteral("$address or label"));
    m_jump->setMaximumWidth(160);
    connect(m_jump, &QLineEdit::returnPressed, this, [this] { jumpTo(m_jump->text()); });
    row->addWidget(m_followPc);
    row->addWidget(new QLabel(QStringLiteral("Jump to")));
    row->addWidget(m_jump);
    row->addWidget(new QLabel(QStringLiteral("Click a line to toggle its breakpoint; scroll to browse")));
    row->addStretch();
    v->addLayout(row);
    m_disasm = monoView(false);
    m_disasm->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_disasm->viewport()->installEventFilter(this);
    v->addWidget(m_disasm, 1);
    return w;
}

QWidget *DebuggerWindow::buildPpu()
{
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    auto *left = new QVBoxLayout;
    m_ppuText = monoView(false);
    m_ppuText->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    left->addWidget(m_ppuText, 1);
    left->addWidget(new QLabel(QStringLiteral("OAM")));
    m_oam = monoView(false);
    left->addWidget(m_oam, 1);
    h->addLayout(left, 1);

    auto *right = new QVBoxLayout;
    auto *sel = new QHBoxLayout;
    m_ppuView = new QComboBox;
    m_ppuView->addItems({ QStringLiteral("Nametables"), QStringLiteral("Patterns"),
                          QStringLiteral("Sprites"), QStringLiteral("Palette") });
    m_ppuPalette = new QSpinBox;
    m_ppuPalette->setRange(0, 7);
    m_ppuPalette->setPrefix(QStringLiteral("palette "));
    m_ppuPalette->setToolTip(QStringLiteral("0-3 background, 4-7 sprites (Patterns view)"));
    connect(m_ppuView, &QComboBox::currentIndexChanged, this, [this](int) { refreshPpu(); });
    connect(m_ppuPalette, &QSpinBox::valueChanged, this, [this](int) { refreshPpu(); });
    sel->addWidget(m_ppuView);
    sel->addWidget(m_ppuPalette);
    sel->addStretch();
    right->addLayout(sel);
    m_ppuPic = new QLabel;
    m_ppuPic->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_ppuPic->setMinimumSize(512, 480);
    right->addWidget(m_ppuPic, 1);
    h->addLayout(right);
    return w;
}

QWidget *DebuggerWindow::buildApu()
{
    m_apu = monoView(false);
    m_apu->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    return m_apu;
}

QWidget *DebuggerWindow::buildBreaks()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    m_bpType = new QComboBox;
    m_bpType->addItems({ QStringLiteral("Execute"), QStringLiteral("Read"), QStringLiteral("Write"),
                         QStringLiteral("Read/Write") });
    m_bpRange = new QLineEdit;
    m_bpRange->setPlaceholderText(QStringLiteral("$C000 or $0200-$02FF or label"));
    m_bpRange->setMaximumWidth(220);
    m_bpCond = new QLineEdit;
    m_bpCond->setPlaceholderText(QStringLiteral("condition (optional Mesen expression, e.g. a == $FF)"));
    auto *add = new QPushButton(QStringLiteral("Add"));
    auto doAdd = [this] {
        static const int types[4] = { NESDEBUG_BP_EXEC, NESDEBUG_BP_READ, NESDEBUG_BP_WRITE,
                                      NESDEBUG_BP_READ | NESDEBUG_BP_WRITE };
        const QString text = m_bpRange->text().trimmed();
        if (text.isEmpty()) return;
        const int dash = text.indexOf(QLatin1Char('-'));
        auto addrOf = [this](const QString &s, long *out) {
            const int lbl = nesdebug_label_address(m_dbg, s.trimmed().toUtf8().constData());
            if (lbl >= 0) { *out = lbl; return true; }
            return parseNum(s, out);
        };
        long a, b;
        if (!addrOf(dash > 0 ? text.left(dash) : text, &a)) { m_status->setText(QStringLiteral("Bad address")); return; }
        b = a;
        if (dash > 0 && !addrOf(text.mid(dash + 1), &b)) { m_status->setText(QStringLiteral("Bad address")); return; }
        const QByteArray cond = m_bpCond->text().trimmed().toUtf8();
        if (nesdebug_breakpoint_add(m_dbg, types[m_bpType->currentIndex()], (uint16_t)a, (uint16_t)b,
                                    cond.constData()) < 0) {
            m_status->setText(QStringLiteral("The condition does not parse"));
            return;
        }
        m_bpRange->clear();
        m_bpCond->clear();
        refreshAll();
    };
    connect(add, &QPushButton::clicked, this, doAdd);
    connect(m_bpRange, &QLineEdit::returnPressed, this, doAdd);
    connect(m_bpCond, &QLineEdit::returnPressed, this, doAdd);
    row->addWidget(m_bpType);
    row->addWidget(m_bpRange);
    row->addWidget(m_bpCond, 1);
    row->addWidget(add);
    v->addLayout(row);

    m_bpTable = new QTableWidget(0, 4);
    m_bpTable->setHorizontalHeaderLabels({ QStringLiteral("On"), QStringLiteral("Type"),
                                           QStringLiteral("Address"), QStringLiteral("Condition") });
    m_bpTable->horizontalHeader()->setStretchLastSection(true);
    m_bpTable->verticalHeader()->setVisible(false);
    m_bpTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_bpTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(m_bpTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_bpFilling || item->column() != 0) return;
        nesdebug_breakpoint_enable(m_dbg, item->data(Qt::UserRole).toInt(), item->checkState() == Qt::Checked);
    });
    v->addWidget(m_bpTable, 1);

    auto *btns = new QHBoxLayout;
    auto *remove = new QPushButton(QStringLiteral("Remove"));
    connect(remove, &QPushButton::clicked, this, [this] {
        const int r = m_bpTable->currentRow();
        if (r < 0) return;
        nesdebug_breakpoint_remove(m_dbg, m_bpTable->item(r, 0)->data(Qt::UserRole).toInt());
        refreshAll();
    });
    auto *clear = new QPushButton(QStringLiteral("Clear all"));
    connect(clear, &QPushButton::clicked, this, [this] {
        nesdebug_breakpoint_clear(m_dbg);
        refreshAll();
    });
    btns->addWidget(remove);
    btns->addWidget(clear);
    btns->addStretch();
    v->addLayout(btns);
    return w;
}

QWidget *DebuggerWindow::buildCart()
{
    m_cart = monoView(false);
    m_cart->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    return m_cart;
}

/* ---- refresh -------------------------------------------------------------- */

void DebuggerWindow::refreshStatus()
{
    char reason[160];
    int addr;
    const bool stopped = nesdebug_is_stopped(m_dbg) != 0;
    nesdebug_stop_reason(m_dbg, reason, sizeof reason, &addr);
    m_status->setText(stopped ? QStringLiteral("Stopped%1%2").arg(reason[0] ? ": " : "", QString::fromUtf8(reason))
                              : QStringLiteral("Running"));
    m_runBtn->setText(stopped ? QStringLiteral("Run (F5)") : QStringLiteral("Stop (F5)"));
    m_runBtn->setStyleSheet(stopped ? QStringLiteral("background: %1; color: white;").arg(nesAccentColor().name()) : QString());
}

void DebuggerWindow::refreshCpu()
{
    nesdebug_cpu c;
    nesdebug_cpu_get(m_dbg, &c);
    const int vals[6] = { c.pc, c.sp, c.a, c.x, c.y, c.ps };
    for (int i = 0; i < 6; ++i)
        if (!m_reg[i]->hasFocus())
            m_reg[i]->setText(hx((unsigned)vals[i], i == 0 ? 4 : 2));
    const int flags[6] = { c.n, c.v, c.d, c.i, c.z, c.c };
    for (int i = 0; i < 6; ++i) m_flag[i]->setChecked(flags[i] != 0);
    m_cycles->setText(QStringLiteral("cycles %1   scanline %2   dot %3   frame %4%5%6")
                          .arg(c.total_cycles).arg(c.scanline).arg(c.dot).arg(c.frame)
                          .arg(c.irq ? QStringLiteral("   IRQ") : QString(),
                               c.nmi ? QStringLiteral("   NMI") : QString()));
}

void DebuggerWindow::refreshRam()
{
    uint8_t ram[2048];
    nesdebug_ram_get(m_dbg, ram);
    QString text = QStringLiteral("        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");
    for (int row = 0; row < 128; ++row) {
        text += QStringLiteral("$%1: ").arg(hx(row * 16, 4));
        for (int col = 0; col < 16; ++col)
            text += hx(ram[row * 16 + col], 2) + QLatin1Char(' ');
        text += QLatin1Char('\n');
    }
    const int scroll = m_ram->verticalScrollBar()->value();
    m_ram->setPlainText(text);
    m_ram->verticalScrollBar()->setValue(scroll);
}

void DebuggerWindow::refreshDisasm()
{
    static nesdebug_line lines[DISASM_WINDOW];
    if (m_followPc->isChecked()) {
        nesdebug_cpu c;
        nesdebug_cpu_get(m_dbg, &c);
        /* the PC a third of the way down */
        m_disasmTop = (uint16_t)nesdebug_row_address(m_dbg, (uint16_t)c.pc, -(DISASM_WINDOW / 3));
    }
    int pcLine = -1;
    const int n = nesdebug_disassemble(m_dbg, m_disasmTop, lines, DISASM_WINDOW, &pcLine);
    m_lineAddr.clear();
    QString text;
    char buf[200];
    for (int i = 0; i < n; ++i) {
        m_lineAddr.push_back(lines[i].address);
        snprintf(buf, sizeof buf, "%c%c %04X  %-9s %-12s %s", lines[i].has_breakpoint ? '*' : ' ',
                 lines[i].is_pc ? '>' : ' ', lines[i].address, lines[i].bytes, lines[i].label, lines[i].disasm);
        text += QString::fromUtf8(buf);
        if (lines[i].comment[0]) text += QStringLiteral("  ") + QString::fromUtf8(lines[i].comment);
        text += QLatin1Char('\n');
    }
    if (n == 0) text = QStringLiteral("(no disassembly: the debugger is not attached)\n");
    m_disasm->setPlainText(text);
    if (pcLine >= 0) {
        QTextCursor cur(m_disasm->document()->findBlockByNumber(pcLine));
        cur.select(QTextCursor::LineUnderCursor);
        QTextCharFormat fmt;
        fmt.setBackground(nesAccentColor());
        fmt.setForeground(Qt::white);
        cur.setCharFormat(fmt);
    }
}

void DebuggerWindow::refreshPpu()
{
    nesdebug_ppu p;
    nesdebug_ppu_get(m_dbg, &p);
    QString s;
    s += QStringLiteral("PPUCTRL   $%1   NMI %2   sprites 8x%3   BG $%4   SPR $%5   VRAM +%6\n")
        .arg(hx(p.ctrl, 2), p.nmi_on_vblank ? "on" : "off", p.sprite_size_16 ? "16" : "8",
             p.bg_table_1000 ? "1000" : "0000", p.spr_table_1000 ? "1000" : "0000", p.increment_32 ? "32" : "1");
    s += QStringLiteral("PPUMASK   $%1   BG %2 (left %3)   SPR %4 (left %5)%6\n")
        .arg(hx(p.mask, 2), p.show_bg ? "on" : "off", p.show_bg_left ? "on" : "off",
             p.show_spr ? "on" : "off", p.show_spr_left ? "on" : "off", p.grayscale ? "   grayscale" : "");
    s += QStringLiteral("PPUSTATUS $%1   vblank %2   sprite 0 hit %3   overflow %4\n")
        .arg(hx(p.status, 2)).arg(p.vblank).arg(p.sprite0_hit).arg(p.sprite_overflow);
    s += QStringLiteral("OAMADDR   $%1\n\n").arg(hx(p.oam_addr, 2));
    s += QStringLiteral("v $%1   t $%2   fine X %3   write toggle %4\n")
        .arg(hx(p.vram_addr, 4), hx(p.tmp_addr, 4)).arg(p.fine_x).arg(p.write_toggle);
    s += QStringLiteral("scanline %1   dot %2   frame %3\nmirroring %4\n")
        .arg(p.scanline).arg(p.dot).arg(p.frame).arg(QString::fromUtf8(p.mirroring));
    m_ppuText->setPlainText(s);

    nesdebug_sprite oam[64];
    nesdebug_oam_get(m_dbg, oam);
    QString o = QStringLiteral(" #   X   Y  tile attr\n");
    for (int i = 0; i < 64; ++i)
        o += QStringLiteral("%1  %2  %3  $%4  $%5%6%7  pal %8\n")
            .arg(i, 2).arg(oam[i].x, 3).arg(oam[i].y, 3).arg(hx(oam[i].tile, 2), hx(oam[i].attr, 2))
            .arg((oam[i].attr & 0x40) ? QStringLiteral(" H") : QStringLiteral("  "),
                 (oam[i].attr & 0x80) ? QStringLiteral("V") : QStringLiteral(" "))
            .arg(4 + (oam[i].attr & 3));
    const int scroll = m_oam->verticalScrollBar()->value();
    m_oam->setPlainText(o);
    m_oam->verticalScrollBar()->setValue(scroll);

    const int view = m_ppuView->currentIndex();
    m_ppuPalette->setEnabled(view == NESDEBUG_VIEW_PATTERNS);
    int w = 0, h = 0;
    if (nesdebug_ppu_view(m_dbg, view, m_ppuPalette->value(), m_ppuPx.data(), &w, &h) && w > 0 && h > 0) {
        QImage img(reinterpret_cast<const uchar *>(m_ppuPx.data()), w, h, w * 4, QImage::Format_RGB32);
        /* 2x nearest; the 512x480 nametables are already full size */
        const int scale = w > 256 ? 1 : 2;
        m_ppuPic->setPixmap(QPixmap::fromImage(img.scaled(w * scale, h * scale, Qt::IgnoreAspectRatio,
                                                          Qt::FastTransformation)));
    }
}

void DebuggerWindow::refreshApu()
{
    nesdebug_apu a;
    nesdebug_apu_get(m_dbg, &a);
    QString s;
    s += QStringLiteral("Pulse 1    %1   period %2   volume %3   duty %4\n")
        .arg(a.pulse1_enabled ? "on " : "off").arg(a.pulse1_period, 4).arg(a.pulse1_volume, 2).arg(a.pulse1_duty);
    s += QStringLiteral("Pulse 2    %1   period %2   volume %3   duty %4\n")
        .arg(a.pulse2_enabled ? "on " : "off").arg(a.pulse2_period, 4).arg(a.pulse2_volume, 2).arg(a.pulse2_duty);
    s += QStringLiteral("Triangle   %1   period %2\n").arg(a.triangle_enabled ? "on " : "off").arg(a.triangle_period, 4);
    s += QStringLiteral("Noise      %1   period %2   volume %3\n")
        .arg(a.noise_enabled ? "on " : "off").arg(a.noise_period, 4).arg(a.noise_volume, 2);
    s += QStringLiteral("DMC        %1   bytes left %2\n").arg(a.dmc_enabled ? "on " : "off").arg(a.dmc_bytes_left);
    s += QStringLiteral("IRQs       frame counter %1   DMC %2\n\n").arg(a.frame_irq ? "enabled" : "off", a.dmc_irq ? "enabled" : "off");
    static const char *const names[8] = { "A", "B", "Select", "Start", "Up", "Down", "Left", "Right" };
    for (int port = 0; port < 2; ++port) {
        s += QStringLiteral("Player %1   $%2  ").arg(port + 1).arg(hx(a.pad[port], 2));
        for (int b = 0; b < 8; ++b)
            if (a.pad[port] & (1u << b)) s += QLatin1Char(' ') + QString::fromUtf8(names[b]);
        s += QLatin1Char('\n');
    }
    m_apu->setPlainText(s);
}

void DebuggerWindow::refreshBps()
{
    nesdebug_breakpoint bps[256];
    const int n = nesdebug_breakpoint_list(m_dbg, bps, 256);
    m_bpFilling = true;
    m_bpTable->setRowCount(n);
    for (int i = 0; i < n; ++i) {
        auto *on = new QTableWidgetItem;
        on->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        on->setCheckState(bps[i].enabled ? Qt::Checked : Qt::Unchecked);
        on->setData(Qt::UserRole, bps[i].id);
        m_bpTable->setItem(i, 0, on);
        QString type;
        if (bps[i].type & NESDEBUG_BP_EXEC) type += QStringLiteral("exec ");
        if (bps[i].type & NESDEBUG_BP_READ) type += QStringLiteral("read ");
        if (bps[i].type & NESDEBUG_BP_WRITE) type += QStringLiteral("write");
        m_bpTable->setItem(i, 1, new QTableWidgetItem(type.trimmed()));
        QString range = QStringLiteral("$") + hx(bps[i].start, 4);
        if (bps[i].end != bps[i].start) range += QStringLiteral("-$") + hx(bps[i].end, 4);
        m_bpTable->setItem(i, 2, new QTableWidgetItem(range));
        m_bpTable->setItem(i, 3, new QTableWidgetItem(QString::fromUtf8(bps[i].condition)));
    }
    m_bpFilling = false;
}

void DebuggerWindow::refreshCart()
{
    nesdebug_cart c;
    char info[256];
    nesdebug_cart_get(m_dbg, &c);
    nesdebug_cart_info(m_dbg, info, sizeof info);
    QString s = QString::fromUtf8(info) + QStringLiteral("\n\n");
    if (!c.present) { m_cart->setPlainText(s); return; }
    s += QStringLiteral("Link          %1%2\n").arg(c.link_up ? "up" : "down",
                                                     c.busy ? QStringLiteral(" (transaction in flight)") : QString());
    if (c.link_error[0]) s += QStringLiteral("Last error    %1\n").arg(QString::fromUtf8(c.link_error));
    s += QStringLiteral("Mailbox       %1\n").arg(c.mailbox_live ? "live" : "closed");
    s += QStringLiteral("SRAM          %1%2\n").arg(c.sram_enabled ? "enabled" : "off",
        c.loading ? QStringLiteral(", loading %1%").arg(c.load_pct) : QString());
    s += QStringLiteral("Image         %1\n").arg(c.booted_image ? "a booted game" : "CONFIG");
    s += QStringLiteral("Mapper        %1%2\n").arg(c.mapper)
        .arg(c.mapper_name[0] ? QStringLiteral(" (%1)").arg(QString::fromUtf8(c.mapper_name)) : QString());
    s += QStringLiteral("PRG slots     %1 %2 %3 %4\n").arg(hx(c.prg_slot[0], 2), hx(c.prg_slot[1], 2), hx(c.prg_slot[2], 2), hx(c.prg_slot[3], 2));
    s += QStringLiteral("CHR slots    ");
    for (int i = 0; i < 8; ++i) s += QLatin1Char(' ') + hx(c.chr_slot[i], 3);
    s += QStringLiteral("\nMirroring     %1\n").arg(QString::fromUtf8(c.mirroring));
    s += QStringLiteral("WRAM          %1%2\n").arg(c.wram_enabled ? "on" : "off", c.wram_protected ? " (write-protected)" : "");
    s += QStringLiteral("CHR           %1\n").arg(c.chr_writable ? "RAM (writable)" : "ROM (protected)");
    s += QStringLiteral("IRQ           %1   line %2   latch %3   counter %4\n\n")
        .arg(c.irq_enabled ? "enabled" : "off").arg(c.irq_line).arg(c.irq_latch).arg(c.irq_counter);
    s += QStringLiteral("ACKSEQ $%1   STATUS $%2   ERR %3\n").arg(hx(c.ackseq, 2), hx(c.status, 2)).arg(c.last_error);
    s += QStringLiteral("BOOT state $%1   %2%   error %3\n").arg(hx(c.boot_state, 2)).arg(c.boot_pct).arg(c.boot_err);
    s += QStringLiteral("RMW dummy writes dropped %1   mailbox queue %2\n").arg(c.diag_rmw).arg(c.queue_depth);
    m_cart->setPlainText(s);
}

void DebuggerWindow::refreshAll()
{
    refreshStatus(); refreshCpu(); refreshRam(); refreshDisasm(); refreshPpu(); refreshApu();
    refreshBps(); refreshCart();
}

void DebuggerWindow::tick()
{
    if (!isVisible()) return;
    const unsigned gen = nesdebug_generation(m_dbg);
    const bool stopped = nesdebug_is_stopped(m_dbg) != 0;
    if (gen != m_seenGen || stopped != m_wasStopped) {
        m_seenGen = gen; m_wasStopped = stopped;
        refreshAll();
    } else if (!stopped && ++m_runningTicks >= 5) {
        m_runningTicks = 0;
        refreshStatus(); refreshCpu(); refreshPpu(); refreshApu(); refreshCart();
    }
}

/* ---- the prompt and the rest ----------------------------------------------- */

void DebuggerWindow::appendPrompt(const QString &text)
{
    m_promptOut->moveCursor(QTextCursor::End);
    m_promptOut->insertPlainText(text);
    m_promptOut->verticalScrollBar()->setValue(m_promptOut->verticalScrollBar()->maximum());
}

void DebuggerWindow::runPrompt()
{
    static char out[65536];
    const QString cmd = m_promptIn->text().trimmed();
    if (cmd.isEmpty()) return;
    appendPrompt(QStringLiteral("> %1\n").arg(cmd));
    nesdebug_command(m_dbg, cmd.toUtf8().constData(), out, sizeof out);
    appendPrompt(stripControl(out) + QLatin1Char('\n'));
    m_promptIn->clear();
    refreshAll();
}

void DebuggerWindow::jumpTo(const QString &text)
{
    long a;
    int addr = nesdebug_label_address(m_dbg, text.trimmed().toUtf8().constData());
    if (addr < 0 && parseNum(text, &a)) addr = (int)(a & 0xFFFF);
    if (addr < 0) { m_status->setText(QStringLiteral("No such address or label")); return; }
    m_followPc->setChecked(false);
    m_disasmTop = (uint16_t)nesdebug_row_address(m_dbg, (uint16_t)addr, -(DISASM_WINDOW / 3));
    refreshDisasm();
}

bool DebuggerWindow::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_promptIn && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Tab) {
            const QString text = m_promptIn->text();
            const int sp = text.lastIndexOf(QLatin1Char(' '));
            const QString word = sp >= 0 ? text.mid(sp + 1) : text;
            char comps[4096];
            const int n = nesdebug_completions(m_dbg, word.toUtf8().constData(), comps, sizeof comps);
            if (n == 1) {
                QString c = QString::fromUtf8(comps).section(QLatin1Char('\n'), 0, 0);
                m_promptIn->setText(text.left(sp + 1) + c + QLatin1Char(' '));
            } else if (n > 1) {
                appendPrompt(QString::fromUtf8(comps));
            }
            return true;
        }
    }
    if (m_disasm && obj == m_disasm->viewport()) {
        if (e->type() == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(e);
            const int line = m_disasm->cursorForPosition(me->pos()).blockNumber();
            if (line >= 0 && line < (int)m_lineAddr.size()) {
                nesdebug_breakpoint_toggle(m_dbg, m_lineAddr[line]);
                refreshDisasm();
                refreshBps();
            }
            return true;
        }
        if (e->type() == QEvent::Wheel) {
            auto *we = static_cast<QWheelEvent *>(e);
            const int rows = -we->angleDelta().y() / 40;
            if (rows != 0) {
                m_followPc->setChecked(false);
                m_disasmTop = (uint16_t)nesdebug_row_address(m_dbg, m_disasmTop, rows);
                refreshDisasm();
            }
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, e);
}

void DebuggerWindow::keyPressEvent(QKeyEvent *e)
{
    switch (e->key()) {
    case Qt::Key_F5:
        if (nesdebug_is_stopped(m_dbg)) nesdebug_resume(m_dbg); else nesdebug_stop(m_dbg);
        refreshAll(); return;
    case Qt::Key_F7: stepAnd(nesdebug_step); return;
    case Qt::Key_F8:
        stepAnd((e->modifiers() & Qt::ShiftModifier) ? nesdebug_step_out : nesdebug_step_over);
        return;
    case Qt::Key_F12: hide(); return;
    default: QMainWindow::keyPressEvent(e);
    }
}

void DebuggerWindow::closeEvent(QCloseEvent *e)
{
    hide();
    e->ignore();
}

/* Hidden by F12, the close button or the main window: the engine goes
 * away and the machine runs on. */
void DebuggerWindow::hideEvent(QHideEvent *e)
{
    nesdebug_detach(m_dbg);
    QMainWindow::hideEvent(e);
}
