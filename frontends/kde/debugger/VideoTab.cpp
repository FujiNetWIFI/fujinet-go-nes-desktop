/*
 * The debugger's Video tab (Qt6 Widgets).
 *
 * Everything that decides what to show lives in the core (nesvidtab.h): this file is widgets.
 * The picture comes from the controller's cached buffer already opaque (0xFFRRGGBB), so
 * Format_RGB32 is safe on KWin; a mouse move redraws only the marks, never re-renders.
 * Only the section showing is filled in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "VideoTab.h"
#include "Accent.h"

extern "C" {
#include "nesvidcap.h"
}

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {

QFont monoFont(QFont base)
{
    base.setFamily(QStringLiteral("monospace"));
    base.setStyleHint(QFont::TypeWriter);
    return base;
}

QLabel *monoLabel()
{
    auto *label = new QLabel;
    label->setFont(monoFont(label->font()));
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    return label;
}

QTableWidget *makeTable(const QStringList &heads)
{
    auto *table = new QTableWidget(0, heads.size());
    table->setHorizontalHeaderLabels(heads);
    table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setFont(monoFont(table->font()));
    return table;
}

QString hex(unsigned value, int digits)
{
    return QStringLiteral("%1").arg(value, digits, 16, QLatin1Char('0')).toUpper();
}

void setCell(QTableWidget *table, int row, int col, const QString &text, bool hot)
{
    auto *item = table->item(row, col);
    if (!item) {
        item = new QTableWidgetItem;
        table->setItem(row, col, item);
    }
    item->setText(text);
    item->setBackground(hot ? QBrush(nesDebugAccentFill(table)) : QBrush());
    item->setForeground(hot ? QBrush(nesDebugAccentFillInk(table)) : QBrush());
}

} // namespace

/* ---- the picture ------------------------------------------------------------ */

class VideoPicture : public QWidget
{
public:
    explicit VideoPicture(VideoTab *owner) : m_owner(owner)
    {
        setMouseTracking(true);
        setMinimumSize(320, 200);
    }
    /* the controller's buffer, or a message when this view cannot be drawn */
    void sync()
    {
        int width = 0, height = 0;
        const uint32_t *pixels = nesvid_tab_picture(m_owner->m_tab, &width, &height);
        if (pixels) {
            /* a view over the controller's buffer: valid until its next refresh */
            m_image = QImage(reinterpret_cast<const uchar *>(pixels), width, height, width * 4, QImage::Format_RGB32);
            m_msg.clear();
            setFixedSize(width, height);
        } else {
            m_image = QImage();
            const char *why = nesvid_tab_why(m_owner->m_tab);
            m_msg = QString::fromUtf8(why ? why : "Nothing to show");
            setMinimumSize(320, 200);
            setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter paint(this);
        paint.fillRect(rect(), QColor(18, 18, 24));
        if (m_image.isNull()) {
            paint.setPen(QColor(140, 140, 148));
            paint.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, m_msg);
            return;
        }
        paint.drawImage(0, 0, m_image);
        int hover[4] = { 0 }, pin[4] = { 0 };
        const int marks = nesvid_tab_marks(m_owner->m_tab, hover, pin);
        if (marks & 1) {
            paint.setPen(QPen(QColor(255, 255, 255, 230), 1));
            paint.drawRect(hover[0] - 1, hover[1] - 1, hover[2] + 1, hover[3] + 1);   /* just outside the object */
        }
        if (marks & 2) {
            paint.setPen(QPen(QColor(255, 217, 26), 2));
            paint.drawRect(pin[0] - 1, pin[1] - 1, pin[2] + 1, pin[3] + 1);
        }
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        const QPoint where = event->position().toPoint();
        if (nesvid_tab_hover(m_owner->m_tab, where.x(), where.y())) {
            update();
            m_owner->updatePixelSection();
        }
    }
    void leaveEvent(QEvent *) override
    {
        if (nesvid_tab_hover(m_owner->m_tab, -1, -1)) {
            update();
            m_owner->updatePixelSection();
        }
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        const QPoint where = event->position().toPoint();
        nesvid_tab_pin(m_owner->m_tab, where.x(), where.y());
        update();
        m_owner->updatePixelSection();
    }
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        const QPoint where = event->position().toPoint();
        nesvid_tab_hover(m_owner->m_tab, where.x(), where.y());
        m_owner->breakOnPixel();
        update();
    }

private:
    VideoTab *m_owner;
    QImage m_image;
    QString m_msg;
};

/* ---- the tab ------------------------------------------------------------------ */

VideoTab::VideoTab(nessession *session, QWidget *parent) : QWidget(parent), m_session(session)
{
    m_tab = nesvid_tab_new(session);

    auto *root = new QVBoxLayout(this);

    /* ---- toolbar: view, Decoded source, palette, zoom, break; then the overlays */
    auto *bar = new QHBoxLayout;
    m_view = new QComboBox;
    for (int idx = 0; idx < NESVID_V_COUNT; ++idx) m_view->addItem(QString::fromUtf8(nesvid_view_name(idx)));
    m_view->setToolTip(QStringLiteral(
        "Display: the last finished frame, as the main window shows it.\n"
        "Decoded: one source (Source) redrawn from the PPU's memory.\n"
        "Beam: the frame as the PPU is drawing it, the lines the beam has not reached yet dimmed (stopped only).\n"
        "Values: each nametable entry as a flat false colour of its value."));
    m_source = new QComboBox;
    for (int idx = 0; idx < NESVID_SRC_COUNT; ++idx) m_source->addItem(QString::fromUtf8(nesvid_source_name(idx)));
    m_source->setToolTip(QStringLiteral("What the Decoded view redraws: the four nametables, the pattern tables, "
                                        "the 64 sprites or the palette"));
    m_palette = new QSpinBox;
    m_palette->setRange(0, 7);
    m_palette->setPrefix(QStringLiteral("palette "));
    m_palette->setToolTip(QStringLiteral("The palette the Patterns source draws in: 0-3 background, 4-7 sprites"));
    m_zoom = new QComboBox;
    int zoomCount = 0;
    const int *zooms = nesvid_zooms(&zoomCount);
    for (int idx = 0; idx < zoomCount; ++idx)
        m_zoom->addItem(zooms[idx] ? QStringLiteral("%1×").arg(zooms[idx]) : QStringLiteral("Fit"), zooms[idx]);
    bar->addWidget(new QLabel(QStringLiteral("View")));
    bar->addWidget(m_view);
    bar->addWidget(new QLabel(QStringLiteral("Source")));
    bar->addWidget(m_source);
    bar->addWidget(m_palette);
    bar->addWidget(new QLabel(QStringLiteral("Zoom")));
    bar->addWidget(m_zoom);
    m_breakBtn = new QPushButton(QStringLiteral("Break on writes"));
    m_breakBtn->setToolTip(QStringLiteral("Breakpoints on the CPU's writes to what the inspected pixel is made from: "
                                          "a nametable entry, an attribute, pattern bytes, palette entries, OAM, "
                                          "or the PPU registers that changed at a band edge"));
    bar->addWidget(m_breakBtn);
    bar->addStretch(1);
    root->addLayout(bar);

    auto *overlays = new QHBoxLayout;
    overlays->addWidget(new QLabel(QStringLiteral("Overlays")));
    int overlayCount = 0;
    const nesvid_overlay_info *overlayInfo = nesvid_overlays(&overlayCount);
    for (int idx = 0; idx < overlayCount; ++idx) {
        auto *box = new QCheckBox(QString::fromUtf8(overlayInfo[idx].label));
        box->setProperty("bit", overlayInfo[idx].bit);
        m_overlays << box;
        overlays->addWidget(box);
    }
    overlays->addStretch(1);
    root->addLayout(overlays);

    m_status = monoLabel();
    root->addWidget(m_status);

    auto *split = new QSplitter(Qt::Horizontal);
    root->addWidget(split, 1);

    /* ---- left: picture */
    auto *left = new QWidget;
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    m_scroll = new QScrollArea;
    m_pic = new VideoPicture(this);
    m_pic->setObjectName(QStringLiteral("videoPicture"));
    m_scroll->setWidget(m_pic);
    m_scroll->setWidgetResizable(false);
    m_scroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    leftLayout->addWidget(m_scroll, 1);
    m_note = new QLabel;
    leftLayout->addWidget(m_note);
    split->addWidget(left);

    /* ---- right: sections, in nesvid_section_name order */
    m_sections = new QTabWidget;
    {
        auto *page = new QWidget;
        auto *pageLayout = new QVBoxLayout(page);
        m_pixelText = monoLabel();
        m_pixelText->setWordWrap(true);
        pageLayout->addWidget(m_pixelText);
        auto *btns = new QHBoxLayout;
        auto *brk = new QPushButton(QStringLiteral("Break on its writes"));
        m_memBtn = new QPushButton(QStringLiteral("Show in Memory"));
        m_memBtn->setToolTip(QStringLiteral("The inspected pixel's bytes in the Memory tab (PPU, palette or OAM space)"));
        auto *unpin = new QPushButton(QStringLiteral("Unpin"));
        btns->addWidget(brk);
        btns->addWidget(m_memBtn);
        btns->addWidget(unpin);
        btns->addStretch(1);
        pageLayout->addLayout(btns);
        pageLayout->addStretch(1);
        connect(brk, &QPushButton::clicked, this, [this] { breakOnPixel(); });
        connect(m_memBtn, &QPushButton::clicked, this, [this] {
            const nesvid_pixel *pixel = nesvid_tab_pixel(m_tab);
            if (pixel && pixel->mem_space >= 0) emit showInMemory(pixel->mem_space, pixel->mem_addr);
        });
        connect(unpin, &QPushButton::clicked, this, [this] {
            nesvid_tab_unpin(m_tab);
            m_pic->update();
            updatePixelSection();
        });
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(page);
        m_sections->addTab(scroll, QString::fromUtf8(nesvid_section_name(NESVID_S_PIXEL)));
    }
    {
        auto *page = new QWidget;
        auto *pageLayout = new QVBoxLayout(page);
        pageLayout->setContentsMargins(0, 0, 0, 0);
        m_regs = makeTable({ QStringLiteral("Name"), QStringLiteral("Value"), QStringLiteral("Meaning") });
        m_regs->setSelectionMode(QAbstractItemView::SingleSelection);
        m_regs->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_regs->setToolTip(QStringLiteral("Double-click a register to break on the CPU's writes to it"));
        pageLayout->addWidget(m_regs, 1);
        auto *row = new QHBoxLayout;
        m_regBreakBtn = new QPushButton(QStringLiteral("Break on writes to the selected register"));
        row->addWidget(m_regBreakBtn);
        row->addStretch(1);
        pageLayout->addLayout(row);
        connect(m_regBreakBtn, &QPushButton::clicked, this, [this] { breakOnRegister(); });
        connect(m_regs, &QTableWidget::cellDoubleClicked, this, [this](int, int) { breakOnRegister(); });
        m_sections->addTab(page, QString::fromUtf8(nesvid_section_name(NESVID_S_REGISTERS)));
    }
    m_swatches = makeTable({ QString(), QStringLiteral("Entry"), QStringLiteral("Colour"), QStringLiteral("RGB"),
                             QStringLiteral("Used"), QStringLiteral("Role") });
    m_swatches->verticalHeader()->setDefaultSectionSize(26);
    m_sections->addTab(m_swatches, QString::fromUtf8(nesvid_section_name(NESVID_S_COLOURS)));
    m_lines = makeTable({ QStringLiteral("Lines"), QStringLiteral("Scroll X"), QStringLiteral("Scroll Y"),
                          QStringLiteral("$2000"), QStringLiteral("$2001"), QStringLiteral("Fine X"),
                          QStringLiteral("VRAM addr"), QStringLiteral("Writes"), QStringLiteral("Mapper") });
    m_sections->addTab(m_lines, QString::fromUtf8(nesvid_section_name(NESVID_S_LINES)));
    {
        auto *page = new QWidget;
        auto *pageLayout = new QVBoxLayout(page);
        m_beam = new QProgressBar;
        m_beam->setRange(0, 1000);
        m_beam->setFormat(QStringLiteral("beam"));
        pageLayout->addWidget(m_beam);
        m_timingText = monoLabel();
        m_timingText->setWordWrap(true);
        pageLayout->addWidget(m_timingText);
        pageLayout->addStretch(1);
        m_sections->addTab(page, QString::fromUtf8(nesvid_section_name(NESVID_S_TIMING)));
    }
    {
        m_spriteText = new QPlainTextEdit;
        m_spriteText->setReadOnly(true);
        m_spriteText->setFont(monoFont(m_spriteText->font()));
        m_spriteText->setLineWrapMode(QPlainTextEdit::NoWrap);
        m_sections->addTab(m_spriteText, QString::fromUtf8(nesvid_section_name(NESVID_S_SPRITES)));
    }
    split->addWidget(m_sections);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    m_sections->setMinimumWidth(480);
    split->setSizes({ 640, 640 });

    /* start from the controller's state (the NES_DEBUGGER_VIDEO_* variables) */
    m_updating = true;
    m_view->setCurrentIndex(nesvid_tab_view(m_tab));
    m_source->setCurrentIndex(nesvid_tab_source(m_tab));
    m_palette->setValue(nesvid_tab_palette(m_tab));
    for (int idx = 0; idx < zoomCount; ++idx)
        if (zooms[idx] == nesvid_tab_zoom(m_tab)) m_zoom->setCurrentIndex(idx);
    for (QCheckBox *box : m_overlays) box->setChecked((nesvid_tab_overlays(m_tab) & box->property("bit").toUInt()) != 0);
    m_sections->setCurrentIndex(nesvid_tab_env_section(m_tab));
    updateSourceControls();
    updateBreakEnabled();
    m_updating = false;

    connect(m_view, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (m_updating || idx < 0) return;
        nesvid_tab_set_view(m_tab, idx);
        updateSourceControls();
        refresh(true);
    });
    connect(m_source, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (m_updating || idx < 0) return;
        nesvid_tab_set_source(m_tab, idx);
        updateSourceControls();
        refresh(true);
    });
    connect(m_palette, &QSpinBox::valueChanged, this, [this](int value) {
        if (m_updating) return;
        nesvid_tab_set_palette(m_tab, value);
        refresh(true);
    });
    connect(m_zoom, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (m_updating || idx < 0) return;
        nesvid_tab_set_zoom(m_tab, m_zoom->itemData(idx).toInt());
        refresh(true);
    });
    for (QCheckBox *box : m_overlays)
        connect(box, &QCheckBox::toggled, this, [this](bool) {
            if (m_updating) return;
            unsigned mask = 0;
            for (QCheckBox *each : m_overlays)
                if (each->isChecked()) mask |= each->property("bit").toUInt();
            nesvid_tab_set_overlays(m_tab, mask);
            refresh(true);
        });
    connect(m_breakBtn, &QPushButton::clicked, this, [this] { breakOnPixel(); });
    connect(m_sections, &QTabWidget::currentChanged, this, [this](int) {
        m_regsSig.clear();
        m_linesSig.clear();
        m_spriteSig.clear();
        refreshView(NESVID_TAB_TABLES);
    });
}

VideoTab::~VideoTab()
{
    nesvid_tab_free(m_tab);
}

/* The source is the Decoded view's; the palette the Patterns source's. */
void VideoTab::updateSourceControls()
{
    const bool decoded = nesvid_tab_view(m_tab) == NESVID_V_DECODED;
    m_source->setEnabled(decoded);
    m_palette->setEnabled(decoded && nesvid_tab_source(m_tab) == NESVID_SRC_PATTERNS);
}

/* Break on writes needs something under the inspected pixel to watch. */
void VideoTab::updateBreakEnabled()
{
    m_breakBtn->setEnabled(nesvid_tab_can_break(m_tab) != 0);
}

void VideoTab::note(const QString &text)
{
    m_note->setText(text);
}

void VideoTab::breakOnPixel()
{
    const nesvid_pixel *pixel = nesvid_tab_pixel(m_tab);
    const int bpid = nesvid_tab_break_on_pixel(m_tab);
    if (bpid >= 0 && pixel && pixel->nbreak > 0) {
        static const char *const kSpace[] = { "CPU", "PPU", "palette", "OAM" };
        const int space = qBound(0, pixel->break_space[0], 3);
        note(QStringLiteral("Break on %1 writes to $%2-$%3%4 (see Breakpoints)")
                 .arg(QString::fromUtf8(kSpace[space]), hex(pixel->break_lo[0], 4),
                      hex(pixel->break_hi[0], 4),
                      pixel->nbreak > 1 ? QStringLiteral(" and %1 more").arg(pixel->nbreak - 1) : QString()));
    } else if (bpid >= 0) {
        note(QStringLiteral("Write breakpoint added (see Breakpoints)"));
    } else {
        note(QStringLiteral("Nothing to break on: hover or pin a tile, a sprite, a pattern or a colour"));
    }
    updatePixelSection();
}

/* The Registers section's selected row: a CPU-space write breakpoint on its address. */
void VideoTab::breakOnRegister()
{
    const int row = m_regs->currentRow();
    if (row < 0) {
        note(QStringLiteral("Select a register first"));
        return;
    }
    const int bpid = nesvid_tab_break_on_register(m_tab, row);
    if (bpid >= 0) note(QStringLiteral("Break on writes to %1 (see Breakpoints)").arg(m_regs->item(row, 0)->text()));
    else note(QStringLiteral("%1 cannot be broken on (PPUCTRL, PPUMASK, OAMADDR, PPUSCROLL and PPUADDR can)")
                  .arg(m_regs->item(row, 0) ? m_regs->item(row, 0)->text() : QStringLiteral("That row")));
}

void VideoTab::updatePixelSection()
{
    const nesvid_pixel *pixel = nesvid_tab_pixel(m_tab);
    updateBreakEnabled();
    m_memBtn->setEnabled(pixel && pixel->mem_space >= 0);
    m_pixelText->setText(pixel ? QString::fromUtf8(pixel->text)
                               : QStringLiteral("Move over the picture to inspect a pixel.\nClick to pin it; "
                                                "double-click to break on writes to it."));
}

void VideoTab::fillRegs()
{
    const nesvid_reg *regs;
    const int count = nesvid_tab_regs(m_tab, &regs);
    QString sig;
    for (int idx = 0; idx < count; ++idx)
        sig += QString::fromUtf8(regs[idx].value) + (regs[idx].age < NESVID_REG_FADE ? QLatin1Char('*') : QLatin1Char('.')) +
               QString::number(regs[idx].writes);
    sig += QString::number(count);
    if (sig == m_regsSig) return;
    m_regsSig = sig;
    const int vpos = m_regs->verticalScrollBar()->value();
    m_regs->setRowCount(count);
    for (int idx = 0; idx < count; ++idx) {
        const bool hot = regs[idx].age < NESVID_REG_FADE || regs[idx].writes > 0;
        QString meaning = QString::fromUtf8(regs[idx].meaning);
        if (regs[idx].writes) {
            meaning += QStringLiteral("  [%1 write%2").arg(regs[idx].writes).arg(regs[idx].writes == 1 ? QString() : QStringLiteral("s"));
            if (regs[idx].first_line >= 0) meaning += QStringLiteral(", first at line %1").arg(regs[idx].first_line);
            meaning += QLatin1Char(']');
        }
        setCell(m_regs, idx, 0, QString::fromUtf8(regs[idx].name), hot);
        setCell(m_regs, idx, 1, QString::fromUtf8(regs[idx].value), hot);
        setCell(m_regs, idx, 2, meaning, hot);
    }
    m_regs->resizeColumnsToContents();
    m_regs->verticalScrollBar()->setValue(vpos);
}

/* The 32 palette RAM entries, each with a swatch and what the frame used it
 * for; the ones the inspected pixel is made of are marked. */
void VideoTab::fillColours()
{
    const nesvid_swatch *swatches;
    const int count = nesvid_tab_swatches(m_tab, &swatches);
    m_swatches->setRowCount(count);
    for (int idx = 0; idx < count; ++idx) {
        const bool inspected = swatches[idx].inspected != 0;
        auto *colour = m_swatches->item(idx, 0);
        if (!colour) {
            colour = new QTableWidgetItem;
            m_swatches->setItem(idx, 0, colour);
        }
        colour->setBackground(QBrush(QColor::fromRgb(swatches[idx].rgb & 0xFFFFFF)));
        setCell(m_swatches, idx, 1, QStringLiteral("$%1 %2").arg(hex(unsigned(swatches[idx].slot), 2), QString::fromUtf8(swatches[idx].name)), inspected);
        setCell(m_swatches, idx, 2, QStringLiteral("$%1").arg(hex(unsigned(swatches[idx].colour), 2)), inspected);
        setCell(m_swatches, idx, 3, QStringLiteral("#") + hex(swatches[idx].rgb & 0xFFFFFF, 6), inspected);
        setCell(m_swatches, idx, 4, QStringLiteral("%1 px").arg(swatches[idx].uses), inspected);
        setCell(m_swatches, idx, 5, QString::fromUtf8(swatches[idx].role), inspected);
    }
    m_swatches->resizeColumnsToContents();
    m_swatches->setColumnWidth(0, 28);
}

/* One row for each band of lines drawn from the same state: a program that never changes it in the frame has one. */
void VideoTab::fillLines()
{
    const nesvid_band *bands;
    const int count = nesvid_tab_lines(m_tab, &bands);
    QString sig = QString::number(count);
    for (int idx = 0; idx < count; ++idx) sig += QString::fromUtf8(bands[idx].text);
    if (sig == m_linesSig) return;
    m_linesSig = sig;
    const int vpos = m_lines->verticalScrollBar()->value();
    m_lines->setRowCount(count);
    for (int idx = 0; idx < count; ++idx) {
        const nesvid_band &band = bands[idx];
        const bool hot = band.writes > 0;
        setCell(m_lines, idx, 0, QStringLiteral("%1-%2").arg(band.first).arg(band.last), hot);
        setCell(m_lines, idx, 1, QString::number(band.scroll_x), hot);
        setCell(m_lines, idx, 2, QString::number(band.scroll_y), hot);
        setCell(m_lines, idx, 3, QStringLiteral("$") + hex(unsigned(band.ctrl), 2), hot);
        setCell(m_lines, idx, 4, QStringLiteral("$") + hex(unsigned(band.mask), 2), hot);
        setCell(m_lines, idx, 5, QString::number(band.fine_x), hot);
        setCell(m_lines, idx, 6, QStringLiteral("$") + hex(band.vaddr, 4), hot);
        setCell(m_lines, idx, 7, QString::number(band.writes), hot);
        setCell(m_lines, idx, 8, QString::number(band.mapper_writes), hot);
    }
    m_lines->resizeColumnsToContents();
    m_lines->verticalScrollBar()->setValue(vpos);
}

void VideoTab::fillTiming()
{
    const char *const *lines;
    const int count = nesvid_tab_timing(m_tab, &lines);
    QString text;
    for (int idx = 0; idx < count; ++idx) {
        const QString line = QString::fromUtf8(lines[idx]);
        const int tabPos = line.indexOf(QLatin1Char('\t'));
        text += tabPos >= 0 ? line.left(tabPos).leftJustified(14) + QLatin1Char(' ') + line.mid(tabPos + 1) : line;
        text += QLatin1Char('\n');
    }
    m_timingText->setText(text.isEmpty() ? QStringLiteral("No timing yet") : text);
    m_beam->setValue(nesvid_tab_beam_permille(m_tab));
}

/* The 64 OAM entries as the controller words them. */
void VideoTab::fillSprites()
{
    const nesvid_spriterow *rows;
    const int count = nesvid_tab_sprites(m_tab, &rows);
    QString text;
    for (int idx = 0; idx < count; ++idx) {
        if (idx) text += QLatin1Char('\n');
        text += QString::fromUtf8(rows[idx].text);
    }
    if (count == 0) text = QStringLiteral("No sprite data yet");
    if (text == m_spriteSig) return;
    m_spriteSig = text;
    const int vpos = m_spriteText->verticalScrollBar()->value();
    m_spriteText->setPlainText(text);
    m_spriteText->verticalScrollBar()->setValue(vpos);
}

void VideoTab::refreshView(unsigned what)
{
    if (what & NESVID_TAB_PICTURE) m_pic->sync();
    if (what & NESVID_TAB_TABLES) {
        m_status->setText(QString::fromUtf8(nesvid_tab_status(m_tab)));
        switch (m_sections->currentIndex()) {
        case NESVID_S_REGISTERS: fillRegs(); break;
        case NESVID_S_COLOURS: fillColours(); break;
        case NESVID_S_LINES: fillLines(); break;
        case NESVID_S_TIMING: fillTiming(); break;
        case NESVID_S_SPRITES: fillSprites(); break;
        default: break;
        }
        updatePixelSection();
    }
}

void VideoTab::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    /* Fit follows the room the picture has */
    if (nesvid_tab_zoom(m_tab) == 0) refresh(false);
}

/* The viewport has no size until the tab shows: Fit is worked out once it has one. */
void VideoTab::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    QTimer::singleShot(0, this, [this] {
        if (isVisible()) refresh(true);
    });
}

void VideoTab::refresh(bool force)
{
    const QSize viewport = m_scroll->viewport()->size();
    if (viewport.width() > 4 && viewport.height() > 4)
        nesvid_tab_set_viewport(m_tab, viewport.width() - 2, viewport.height() - 2);
    unsigned what = nesvid_tab_refresh(m_tab, force);
    if (force) what |= NESVID_TAB_PICTURE | NESVID_TAB_TABLES;
    if (what & NESVID_TAB_OFF) what |= NESVID_TAB_PICTURE | NESVID_TAB_TABLES;
    refreshView(what);
}
