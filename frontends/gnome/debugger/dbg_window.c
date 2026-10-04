/*
 * Debugger window (GTK4/libadwaita) over MesenCE's own debugger engine, via
 * core/include/nesdebug.h.
 *
 * Tabs: a Prompt (the contract's command set, with tab completion, symbol
 * loading and file saves), CPU + console RAM, the disassembly with a
 * breakpoint gutter, the PPU (registers, the nametable / pattern / sprite /
 * palette views and the OAM), the APU and controllers, breakpoints, and the
 * FujiNet cartridge. A toolbar carries the stepping controls; the family's
 * keys apply (F5 run/stop, F7 step, F8 step over, Shift+F8 step out, F12
 * close).
 *
 * The engine exists only while this window is showing: showing it attaches
 * (which stops the machine, as on every sibling), hiding it detaches and
 * lets the machine run on.
 *
 * The window polls the engine's generation counter on a short timer and
 * refreshes only when something changed, so a stopped machine costs
 * nothing and a running one shows live values twice a second.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <stdlib.h>
#include <string.h>

#include "nesdebug.h"
#include "../window.h"

#define DISASM_WINDOW 48

typedef struct {
    GtkWindow *win;
    nessession *session;
    nesdebug *dbg;
    unsigned seen_generation;
    gboolean was_stopped;
    int running_ticks;
    guint timer;

    GtkLabel *status;
    GtkButton *run_btn;

    /* prompt */
    GtkTextView *prompt_out;
    GtkEntry *prompt_in;
    GtkScrolledWindow *prompt_scroll;

    /* cpu + ram */
    GtkEntry *reg[6];         /* PC SP A X Y P */
    GtkCheckButton *flag[6];  /* N V D I Z C */
    GtkLabel *cycles;
    GtkTextView *ram_view;
    GtkEntry *ram_addr, *ram_val;
    gboolean updating_flags;

    /* disassembly */
    GtkTextView *disasm;
    GtkCheckButton *follow_pc;
    GtkEntry *jump;
    int disasm_top;           /* address of the first line */
    uint16_t line_addr[DISASM_WINDOW];
    int line_count;

    /* ppu */
    GtkTextView *ppu_text;
    GtkPicture *ppu_pic;
    GtkDropDown *ppu_view;
    GtkSpinButton *ppu_palette;
    GtkTextView *oam_view;
    guint32 *view_px, *view_px2;

    /* apu */
    GtkTextView *apu_text;

    /* breakpoints */
    GtkListBox *bp_list;
    GtkDropDown *bp_type;
    GtkEntry *bp_start, *bp_end, *bp_cond;
    GtkLabel *bp_msg;

    /* cart */
    GtkTextView *cart_text;
} DbgWin;

static DbgWin *g_win;

/* ---- helpers -------------------------------------------------------------- */

static void set_text(GtkTextView *view, const char *text)
{
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(view), text, -1);
}

static void append_text(GtkTextView *view, GtkScrolledWindow *scroll, const char *text)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(view);
    GtkTextIter end;
    GtkAdjustment *adj;
    gtk_text_buffer_get_end_iter(b, &end);
    gtk_text_buffer_insert(b, &end, text, -1);
    adj = gtk_scrolled_window_get_vadjustment(scroll);
    if (adj) gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj));
}

static int parse_num(const char *text, long *out)
{
    char *end;
    long v;
    while (*text == ' ') text++;
    if (*text == '$') v = strtol(text + 1, &end, 16);
    else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) v = strtol(text + 2, &end, 16);
    else if (*text == '#') v = strtol(text + 1, &end, 10);
    else v = strtol(text, &end, 16);
    if (end == text || *end) return 0;
    *out = v;
    return 1;
}

/* A label or a number, as typed into an address box. */
static int parse_addr(DbgWin *w, const char *text, long *out)
{
    int a = nesdebug_label_address(w->dbg, text);
    if (a >= 0) { *out = a; return 1; }
    return parse_num(text, out);
}

static GtkWidget *mono_view(GtkTextView **out, gboolean editable)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), editable);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), editable);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    *out = GTK_TEXT_VIEW(view);
    return scroll;
}

static GtkWidget *labeled(const char *text, GtkWidget *child)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "dim-label");
    gtk_box_append(GTK_BOX(box), l);
    gtk_box_append(GTK_BOX(box), child);
    return box;
}

static GtkWidget *padded(GtkWidget *w)
{
    gtk_widget_set_margin_top(w, 8);
    gtk_widget_set_margin_bottom(w, 8);
    gtk_widget_set_margin_start(w, 8);
    gtk_widget_set_margin_end(w, 8);
    return w;
}

/* ---- refresh -------------------------------------------------------------- */

static void refresh_cpu(DbgWin *w)
{
    nesdebug_cpu c;
    char buf[160];
    int i;
    nesdebug_cpu_get(w->dbg, &c);
    g_snprintf(buf, sizeof buf, "%04X", c.pc); gtk_editable_set_text(GTK_EDITABLE(w->reg[0]), buf);
    g_snprintf(buf, sizeof buf, "%02X", c.sp); gtk_editable_set_text(GTK_EDITABLE(w->reg[1]), buf);
    g_snprintf(buf, sizeof buf, "%02X", c.a); gtk_editable_set_text(GTK_EDITABLE(w->reg[2]), buf);
    g_snprintf(buf, sizeof buf, "%02X", c.x); gtk_editable_set_text(GTK_EDITABLE(w->reg[3]), buf);
    g_snprintf(buf, sizeof buf, "%02X", c.y); gtk_editable_set_text(GTK_EDITABLE(w->reg[4]), buf);
    g_snprintf(buf, sizeof buf, "%02X", c.ps); gtk_editable_set_text(GTK_EDITABLE(w->reg[5]), buf);
    {
        const int flags[6] = { c.n, c.v, c.d, c.i, c.z, c.c };
        w->updating_flags = TRUE;
        for (i = 0; i < 6; i++)
            gtk_check_button_set_active(w->flag[i], flags[i] != 0);
        w->updating_flags = FALSE;
    }
    g_snprintf(buf, sizeof buf, "cycle %llu   scanline %d   dot %d   frame %u%s%s",
               (unsigned long long)c.total_cycles, c.scanline, c.dot, c.frame,
               c.irq ? "   IRQ" : "", c.nmi ? "   NMI" : "");
    gtk_label_set_text(w->cycles, buf);
}

static void refresh_ram(DbgWin *w)
{
    uint8_t ram[2048];
    static char text[2048 * 4 + 1024];
    int len = 0, row, col;
    nesdebug_ram_get(w->dbg, ram);
    len += g_snprintf(text + len, sizeof text - len,
                      "       0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");
    for (row = 0; row < 128; row++) {
        len += g_snprintf(text + len, sizeof text - len, "$%04X: ", row * 16);
        for (col = 0; col < 16; col++)
            len += g_snprintf(text + len, sizeof text - len, "%02X ", ram[row * 16 + col]);
        len += g_snprintf(text + len, sizeof text - len, "\n");
    }
    set_text(w->ram_view, text);
}

static void refresh_disasm(DbgWin *w)
{
    static nesdebug_line lines[DISASM_WINDOW];
    static char text[DISASM_WINDOW * 160];
    int n, pc_line, i, len = 0;
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->disasm);
    GtkTextIter s, e;

    /* Following, the PC sits a third of the way down the window. */
    if (gtk_check_button_get_active(w->follow_pc)) {
        nesdebug_cpu c;
        nesdebug_cpu_get(w->dbg, &c);
        w->disasm_top = nesdebug_row_address(w->dbg, (uint16_t)c.pc, -(DISASM_WINDOW / 3));
    }

    n = nesdebug_disassemble(w->dbg, (uint16_t)w->disasm_top, lines, DISASM_WINDOW, &pc_line);
    w->line_count = n;
    for (i = 0; i < n; i++) {
        w->line_addr[i] = lines[i].address;
        len += g_snprintf(text + len, sizeof text - len, "%c%c %04X  %-9s %-12s %s%s%s\n",
                          lines[i].has_breakpoint ? '*' : ' ',
                          lines[i].is_pc ? '>' : ' ',
                          lines[i].address, lines[i].bytes, lines[i].label,
                          lines[i].disasm, lines[i].comment[0] ? "  " : "",
                          lines[i].comment);
    }
    if (n == 0)
        len += g_snprintf(text + len, sizeof text - len, "(no disassembly)\n");
    gtk_text_buffer_set_text(b, text, -1);

    /* the PC line in the accent colour */
    if (pc_line >= 0 && pc_line < n) {
        gtk_text_buffer_get_iter_at_line(b, &s, pc_line);
        gtk_text_buffer_get_iter_at_line(b, &e, pc_line + 1);
        gtk_text_buffer_apply_tag_by_name(b, "pc", &s, &e);
    }
}

static void show_view(DbgWin *w)
{
    int view = (int)gtk_drop_down_get_selected(w->ppu_view);
    int pal = gtk_spin_button_get_value_as_int(w->ppu_palette);
    int vw = 0, vh = 0, x, y, scale, ow, oh;
    GBytes *bytes;
    GdkTexture *tex;

    if (!nesdebug_ppu_view(w->dbg, view, pal, w->view_px, &vw, &vh) || vw <= 0 || vh <= 0)
        return;
    /* Scaled here, nearest-neighbour, so the tiles stay crisp: the small
     * views twice, the 512 x 480 nametables as they are. */
    scale = view == NESDEBUG_VIEW_NAMETABLES ? 1 : 2;
    ow = vw * scale;
    oh = vh * scale;
    for (y = 0; y < oh; y++)
        for (x = 0; x < ow; x++)
            w->view_px2[y * ow + x] = w->view_px[(y / scale) * vw + x / scale] | 0xFF000000u;
    bytes = g_bytes_new(w->view_px2, (gsize)ow * oh * 4);
    tex = gdk_memory_texture_new(ow, oh, GDK_MEMORY_B8G8R8A8, bytes, (gsize)ow * 4);
    gtk_picture_set_paintable(w->ppu_pic, GDK_PAINTABLE(tex));
    gtk_widget_set_size_request(GTK_WIDGET(w->ppu_pic), ow, oh);
    g_object_unref(tex);
    g_bytes_unref(bytes);
}

static void refresh_ppu(DbgWin *w)
{
    nesdebug_ppu p;
    nesdebug_sprite oam[64];
    static char text[2048], otext[64 * 48 + 128];
    int len = 0, i;

    nesdebug_ppu_get(w->dbg, &p);
    g_snprintf(text, sizeof text,
        "Scanline %d   dot %d   frame %u\n\n"
        "PPUCTRL   $%02X   NMI on vblank %s   sprites %s   BG table $%04X   "
        "sprite table $%04X   increment %d\n"
        "PPUMASK   $%02X   BG %s%s   sprites %s%s%s\n"
        "PPUSTATUS $%02X   vblank %d   sprite 0 hit %d   overflow %d\n"
        "OAMADDR   $%02X\n\n"
        "v $%04X   t $%04X   fine X %d   write toggle %d\n"
        "Mirroring %s\n",
        p.scanline, p.dot, p.frame,
        p.ctrl, p.nmi_on_vblank ? "on" : "off", p.sprite_size_16 ? "8x16" : "8x8",
        p.bg_table_1000 ? 0x1000 : 0, p.spr_table_1000 ? 0x1000 : 0, p.increment_32 ? 32 : 1,
        p.mask, p.show_bg ? "on" : "off", p.show_bg_left ? "" : " (not left 8)",
        p.show_spr ? "on" : "off", p.show_spr_left ? "" : " (not left 8)",
        p.grayscale ? "   grayscale" : "",
        p.status, p.vblank, p.sprite0_hit, p.sprite_overflow, p.oam_addr,
        p.vram_addr, p.tmp_addr, p.fine_x, p.write_toggle, p.mirroring);
    set_text(w->ppu_text, text);

    nesdebug_oam_get(w->dbg, oam);
    len += g_snprintf(otext + len, sizeof otext - len, " #   X    Y   tile attr\n");
    for (i = 0; i < 64; i++)
        len += g_snprintf(otext + len, sizeof otext - len, "%2d  %3u  %3u   $%02X  $%02X%s%s\n",
                          i, oam[i].x, oam[i].y, oam[i].tile, oam[i].attr,
                          (oam[i].attr & 0x40) ? " H" : "", (oam[i].attr & 0x80) ? " V" : "");
    set_text(w->oam_view, otext);
    show_view(w);
}

static void refresh_apu(DbgWin *w)
{
    nesdebug_apu a;
    char text[1600], pad[2][64];
    static const char *const names[8] = { "A", "B", "Select", "Start", "Up", "Down", "Left", "Right" };
    int port, b;

    nesdebug_apu_get(w->dbg, &a);
    for (port = 0; port < 2; port++) {
        int len = 0;
        pad[port][0] = '\0';
        for (b = 0; b < 8; b++)
            if (a.pad[port] & (1u << b))
                len += g_snprintf(pad[port] + len, sizeof pad[port] - len, "%s ", names[b]);
        if (!len) g_snprintf(pad[port], sizeof pad[port], "(nothing held)");
    }
    g_snprintf(text, sizeof text,
        "Pulse 1    %s   period %4d   volume %2d   duty %d\n"
        "Pulse 2    %s   period %4d   volume %2d   duty %d\n"
        "Triangle   %s   period %4d\n"
        "Noise      %s   period %4d   volume %2d\n"
        "DMC        %s   %d bytes left\n\n"
        "Frame IRQ %s   DMC IRQ %s\n\n"
        "Controller 1  $%02X  %s\n"
        "Controller 2  $%02X  %s\n",
        a.pulse1_enabled ? "on " : "off", a.pulse1_period, a.pulse1_volume, a.pulse1_duty,
        a.pulse2_enabled ? "on " : "off", a.pulse2_period, a.pulse2_volume, a.pulse2_duty,
        a.triangle_enabled ? "on " : "off", a.triangle_period,
        a.noise_enabled ? "on " : "off", a.noise_period, a.noise_volume,
        a.dmc_enabled ? "on " : "off", a.dmc_bytes_left,
        a.frame_irq ? "enabled" : "off", a.dmc_irq ? "enabled" : "off",
        a.pad[0], pad[0], a.pad[1], pad[1]);
    set_text(w->apu_text, text);
}

static void refresh_cart(DbgWin *w)
{
    nesdebug_cart c;
    char info[256], text[2048];
    nesdebug_cart_get(w->dbg, &c);
    nesdebug_cart_info(w->dbg, info, sizeof info);
    if (!c.present) {
        set_text(w->cart_text, info);
        return;
    }
    g_snprintf(text, sizeof text,
        "%s\n\n"
        "Link        %s%s%s%s\n"
        "Mailbox     %s   ACKSEQ $%02X   status $%02X   last error %u\n"
        "Boot        state $%02X   %u%%   error %u\n"
        "Loader      %s%s\n"
        "Image       %s   mapper %d %s\n"
        "PRG slots   %02X %02X %02X %02X   (8K banks at $8000 $A000 $C000 $E000)\n"
        "CHR slots   %03X %03X %03X %03X %03X %03X %03X %03X   (1K banks)\n"
        "Mirroring   %s\n"
        "WRAM        %s%s   CHR %s\n"
        "IRQ         %s   line %d   latch %d   counter %d\n"
        "Diagnostics RMW dummy writes dropped %u   worker queue %u\n",
        info,
        c.link_up ? "up" : "down", c.busy ? " (transaction in flight)" : "",
        (!c.link_up && c.link_error[0]) ? ": " : "", c.link_up ? "" : c.link_error,
        c.mailbox_live ? "live" : "closed", c.ackseq, c.status, c.last_error,
        c.boot_state, c.boot_pct, c.boot_err,
        c.loading ? "loading " : (c.sram_enabled ? "SRAM in place" : "serving vectors"),
        c.loading ? "" : "",
        c.booted_image ? "booted" : "CONFIG", c.mapper, c.mapper_name,
        c.prg_slot[0], c.prg_slot[1], c.prg_slot[2], c.prg_slot[3],
        c.chr_slot[0], c.chr_slot[1], c.chr_slot[2], c.chr_slot[3],
        c.chr_slot[4], c.chr_slot[5], c.chr_slot[6], c.chr_slot[7],
        c.mirroring,
        c.wram_enabled ? "enabled" : "off", c.wram_protected ? " (write-protected)" : "",
        c.chr_writable ? "RAM (writable)" : "ROM (protected)",
        c.irq_enabled ? "enabled" : "off", c.irq_line, c.irq_latch, c.irq_counter,
        c.diag_rmw, c.queue_depth);
    if (c.loading) {
        char pct[32];
        g_snprintf(pct, sizeof pct, "  (%d%%)", c.load_pct);
        g_strlcat(text, pct, sizeof text);
    }
    set_text(w->cart_text, text);
}

static void bp_enable_toggled(GtkCheckButton *b, gpointer ud);
static void bp_remove_clicked(GtkButton *b, gpointer ud);

static void refresh_bps(DbgWin *w)
{
    nesdebug_breakpoint bps[128];
    GtkWidget *child;
    int n = nesdebug_breakpoint_list(w->dbg, bps, 128), i;

    while ((child = gtk_widget_get_first_child(GTK_WIDGET(w->bp_list))) != NULL)
        gtk_list_box_remove(w->bp_list, child);
    if (n == 0) {
        GtkWidget *l = gtk_label_new("No breakpoints. Click a disassembly line, or add one below.");
        gtk_widget_add_css_class(l, "dim-label");
        gtk_list_box_append(w->bp_list, l);
        return;
    }
    for (i = 0; i < n; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *en = gtk_check_button_new();
        GtkWidget *rm = gtk_button_new_from_icon_name("user-trash-symbolic");
        GtkWidget *l;
        char text[256], range[32];
        const char *type = (bps[i].type & NESDEBUG_BP_EXEC) ? "Execute"
                         : (bps[i].type == (NESDEBUG_BP_READ | NESDEBUG_BP_WRITE)) ? "Read/Write"
                         : (bps[i].type & NESDEBUG_BP_READ) ? "Read" : "Write";
        if (bps[i].end != bps[i].start)
            g_snprintf(range, sizeof range, "$%04X-$%04X", bps[i].start, bps[i].end);
        else
            g_snprintf(range, sizeof range, "$%04X", bps[i].start);
        g_snprintf(text, sizeof text, "%-10s %-12s %s%s", type, range,
                   bps[i].condition[0] ? "if " : "", bps[i].condition);
        l = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_set_hexpand(l, TRUE);
        gtk_widget_add_css_class(l, "monospace");
        gtk_check_button_set_active(GTK_CHECK_BUTTON(en), bps[i].enabled != 0);
        g_object_set_data(G_OBJECT(en), "bp", GINT_TO_POINTER(bps[i].id));
        g_object_set_data(G_OBJECT(rm), "bp", GINT_TO_POINTER(bps[i].id));
        g_signal_connect(en, "toggled", G_CALLBACK(bp_enable_toggled), w);
        g_signal_connect(rm, "clicked", G_CALLBACK(bp_remove_clicked), w);
        gtk_widget_add_css_class(rm, "flat");
        gtk_box_append(GTK_BOX(row), en);
        gtk_box_append(GTK_BOX(row), l);
        gtk_box_append(GTK_BOX(row), rm);
        gtk_list_box_append(w->bp_list, row);
    }
}

static void refresh_status(DbgWin *w)
{
    char reason[160], text[256];
    int addr;
    gboolean stopped = nesdebug_is_stopped(w->dbg) != 0;
    nesdebug_stop_reason(w->dbg, reason, sizeof reason, &addr);
    if (stopped)
        g_snprintf(text, sizeof text, "Stopped%s%s", reason[0] ? ": " : "", reason);
    else
        g_snprintf(text, sizeof text, "Running");
    gtk_label_set_text(w->status, text);
    gtk_button_set_label(w->run_btn, stopped ? "Run (F5)" : "Stop (F5)");
    if (stopped) gtk_widget_add_css_class(GTK_WIDGET(w->run_btn), "nes-accent");
    else gtk_widget_remove_css_class(GTK_WIDGET(w->run_btn), "nes-accent");
}

static void refresh_all(DbgWin *w)
{
    refresh_status(w);
    refresh_cpu(w);
    refresh_ram(w);
    refresh_disasm(w);
    refresh_ppu(w);
    refresh_apu(w);
    refresh_bps(w);
    refresh_cart(w);
}

static void attach_and_refresh(DbgWin *w);

static gboolean tick_refresh(gpointer ud)
{
    DbgWin *w = ud;
    unsigned gen;
    gboolean stopped;
    if (!gtk_widget_get_visible(GTK_WIDGET(w->win))) return G_SOURCE_CONTINUE;
    /* Opened before the session started (NES_OPEN_DEBUGGER), or showing
     * across a session restart: the engine comes up with the machine. */
    if (!nesdebug_is_attached(w->dbg) && nessession_is_running(w->session)) {
        attach_and_refresh(w);
        return G_SOURCE_CONTINUE;
    }
    gen = nesdebug_generation(w->dbg);
    stopped = nesdebug_is_stopped(w->dbg) != 0;
    if (gen != w->seen_generation || stopped != w->was_stopped) {
        w->seen_generation = gen;
        w->was_stopped = stopped;
        refresh_all(w);
    } else if (!stopped && ++w->running_ticks >= 5) {
        /* live values twice a second while the machine runs */
        w->running_ticks = 0;
        refresh_status(w);
        refresh_cpu(w);
        refresh_ppu(w);
        refresh_apu(w);
        refresh_cart(w);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- handlers ------------------------------------------------------------- */

static void on_run(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    if (nesdebug_is_stopped(w->dbg)) nesdebug_resume(w->dbg);
    else nesdebug_stop(w->dbg);
    refresh_all(w);
}

static void on_step(GtkButton *b, gpointer ud) { (void)b; nesdebug_step(((DbgWin *)ud)->dbg); refresh_all(ud); }
static void on_step_over(GtkButton *b, gpointer ud) { (void)b; nesdebug_step_over(((DbgWin *)ud)->dbg); refresh_all(ud); }
static void on_step_out(GtkButton *b, gpointer ud) { (void)b; nesdebug_step_out(((DbgWin *)ud)->dbg); refresh_all(ud); }
static void on_scanline(GtkButton *b, gpointer ud) { (void)b; nesdebug_scanline(((DbgWin *)ud)->dbg, 1); refresh_all(ud); }
static void on_frame(GtkButton *b, gpointer ud) { (void)b; nesdebug_frame(((DbgWin *)ud)->dbg, 1); refresh_all(ud); }

static void on_prompt(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    static char out[65536];
    const char *cmd = gtk_editable_get_text(GTK_EDITABLE(entry));
    char line[512];
    if (!cmd || !*cmd) return;
    g_snprintf(line, sizeof line, "> %s\n", cmd);
    append_text(w->prompt_out, w->prompt_scroll, line);
    nesdebug_command(w->dbg, cmd, out, sizeof out);
    append_text(w->prompt_out, w->prompt_scroll, out);
    if (out[0] && out[strlen(out) - 1] != '\n')
        append_text(w->prompt_out, w->prompt_scroll, "\n");
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    refresh_all(w);
}

/* Tab completes the current word against the commands and the labels;
 * several matches are listed instead. */
static gboolean on_prompt_key(GtkEventControllerKey *c, guint keyval, guint code,
                              GdkModifierType st, gpointer ud)
{
    DbgWin *w = ud;
    char comps[4096];
    const char *text;
    const char *word;
    int n;
    (void)c; (void)code; (void)st;
    if (keyval != GDK_KEY_Tab) return FALSE;
    text = gtk_editable_get_text(GTK_EDITABLE(w->prompt_in));
    word = strrchr(text, ' ');
    word = word ? word + 1 : text;
    n = nesdebug_completions(w->dbg, word, comps, sizeof comps);
    if (n == 1) {
        char merged[600];
        char *nl = strchr(comps, '\n');
        if (nl) *nl = '\0';
        g_snprintf(merged, sizeof merged, "%.*s%s ", (int)(word - text), text, comps);
        gtk_editable_set_text(GTK_EDITABLE(w->prompt_in), merged);
        gtk_editable_set_position(GTK_EDITABLE(w->prompt_in), -1);
    } else if (n > 1) {
        append_text(w->prompt_out, w->prompt_scroll, comps);
    }
    return TRUE;
}

static void on_reg_activate(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "reg"));
    long v;
    static const int regs[6] = { NES_REG_PC, NES_REG_SP, NES_REG_A, NES_REG_X, NES_REG_Y, NES_REG_PS };
    if (parse_num(gtk_editable_get_text(GTK_EDITABLE(entry)), &v))
        nesdebug_cpu_set(w->dbg, regs[i], (int)v);
    refresh_all(w);
}

static void on_flag_toggled(GtkCheckButton *b, gpointer ud)
{
    DbgWin *w = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "flag"));
    static const int flags[6] = { NES_FLAG_N, NES_FLAG_V, NES_FLAG_D, NES_FLAG_I, NES_FLAG_Z, NES_FLAG_C };
    if (w->updating_flags || !nesdebug_is_stopped(w->dbg)) return;
    nesdebug_cpu_set(w->dbg, flags[i], gtk_check_button_get_active(b));
}

static void on_ram_write(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    long a, v;
    (void)entry;
    if (parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(w->ram_addr)), &a)
        && parse_num(gtk_editable_get_text(GTK_EDITABLE(w->ram_val)), &v))
        nesdebug_write(w->dbg, (uint16_t)a, (uint8_t)v);
    refresh_all(w);
}

static void on_disasm_click(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    DbgWin *w = ud;
    GtkTextIter it;
    int bx, by, line;
    (void)g; (void)n;
    gtk_text_view_window_to_buffer_coords(w->disasm, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
    gtk_text_view_get_iter_at_location(w->disasm, &it, bx, by);
    line = gtk_text_iter_get_line(&it);
    if (line < 0 || line >= w->line_count) return;
    nesdebug_breakpoint_toggle(w->dbg, w->line_addr[line]);
    refresh_disasm(w);
    refresh_bps(w);
}

static void on_follow_toggled(GtkCheckButton *b, gpointer ud)
{
    (void)b;
    refresh_disasm(ud);
}

static void on_jump(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    long a;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(entry)), &a)) return;
    gtk_check_button_set_active(w->follow_pc, FALSE);
    w->disasm_top = (int)(a & 0xFFFF);
    refresh_disasm(w);
}

static gboolean on_disasm_scroll(GtkEventControllerScroll *c, double dx, double dy, gpointer ud)
{
    DbgWin *w = ud;
    int rows = (int)(dy * 3);
    (void)c; (void)dx;
    if (!rows) rows = dy > 0 ? 1 : -1;
    gtk_check_button_set_active(w->follow_pc, FALSE);
    w->disasm_top = nesdebug_row_address(w->dbg, (uint16_t)w->disasm_top, rows);
    refresh_disasm(w);
    return TRUE;
}

static void on_view_changed(GObject *o, GParamSpec *ps, gpointer ud)
{
    DbgWin *w = ud;
    (void)o; (void)ps;
    gtk_widget_set_sensitive(GTK_WIDGET(w->ppu_palette),
        gtk_drop_down_get_selected(w->ppu_view) == NESDEBUG_VIEW_PATTERNS);
    show_view(w);
}

static void on_palette_changed(GtkSpinButton *b, gpointer ud)
{
    (void)b;
    show_view(ud);
}

static void bp_enable_toggled(GtkCheckButton *b, gpointer ud)
{
    DbgWin *w = ud;
    nesdebug_breakpoint_enable(w->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "bp")),
                               gtk_check_button_get_active(b));
    refresh_disasm(w);
}

static void bp_remove_clicked(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    nesdebug_breakpoint_remove(w->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "bp")));
    refresh_all(w);
}

static void on_bp_add(GtkWidget *wgt, gpointer ud)
{
    DbgWin *w = ud;
    static const int types[4] = { NESDEBUG_BP_EXEC, NESDEBUG_BP_READ, NESDEBUG_BP_WRITE,
                                  NESDEBUG_BP_READ | NESDEBUG_BP_WRITE };
    long a, b;
    int id;
    const char *end = gtk_editable_get_text(GTK_EDITABLE(w->bp_end));
    (void)wgt;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(w->bp_start)), &a)) {
        gtk_label_set_text(w->bp_msg, "Not an address or a label");
        return;
    }
    b = a;
    if (end && *end && !parse_addr(w, end, &b)) {
        gtk_label_set_text(w->bp_msg, "The end is not an address or a label");
        return;
    }
    id = nesdebug_breakpoint_add(w->dbg, types[gtk_drop_down_get_selected(w->bp_type) & 3],
                                 (uint16_t)a, (uint16_t)b,
                                 gtk_editable_get_text(GTK_EDITABLE(w->bp_cond)));
    if (id < 0) {
        gtk_label_set_text(w->bp_msg, "The condition does not parse");
        return;
    }
    gtk_label_set_text(w->bp_msg, "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_start), "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_end), "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_cond), "");
    refresh_all(w);
}

static void on_bp_clear(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    nesdebug_breakpoint_clear(w->dbg);
    refresh_all(w);
}

static void prompt_say(DbgWin *w, const char *msg)
{
    char line[700];
    g_snprintf(line, sizeof line, "%s\n", msg);
    append_text(w->prompt_out, w->prompt_scroll, line);
}

static void on_symbols_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    DbgWin *w = ud;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    char msg[512];
    if (!file) return;
    path = g_file_get_path(file);
    if (!path) return;
    nesdebug_load_symbols(w->dbg, path, msg, sizeof msg);
    prompt_say(w, msg);
    refresh_all(w);
}

static void on_symbols(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    (void)b;
    gtk_file_dialog_set_title(dlg, "Load Symbols (ld65 -Ln, VICE, ca65 .dbg, Mesen .mlb)");
    gtk_file_dialog_open(dlg, w->win, NULL, on_symbols_chosen, w);
    g_object_unref(dlg);
}

static void on_save_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    DbgWin *w = ud;
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    const char *kind = g_object_get_data(src, "kind");
    char msg[512];
    if (!file) return;
    path = g_file_get_path(file);
    if (!path) return;
    nesdebug_save(w->dbg, kind, path, msg, sizeof msg);
    prompt_say(w, msg);
}

static void on_save(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    const char *kind = g_object_get_data(G_OBJECT(b), "kind");
    char title[64];
    g_snprintf(title, sizeof title, "Save %s", (const char *)g_object_get_data(G_OBJECT(b), "title"));
    gtk_file_dialog_set_title(dlg, title);
    gtk_file_dialog_set_initial_name(dlg, g_object_get_data(G_OBJECT(b), "file"));
    g_object_set_data(G_OBJECT(dlg), "kind", (gpointer)kind);
    gtk_file_dialog_save(dlg, w->win, NULL, on_save_chosen, w);
    g_object_unref(dlg);
}

static void hide_window(DbgWin *w)
{
    nesdebug_detach(w->dbg);
    gtk_widget_set_visible(GTK_WIDGET(w->win), FALSE);
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint code,
                       GdkModifierType st, gpointer ud)
{
    DbgWin *w = ud;
    (void)c; (void)code;
    switch (keyval) {
    case GDK_KEY_F5: on_run(NULL, w); return TRUE;
    case GDK_KEY_F7: on_step(NULL, w); return TRUE;
    case GDK_KEY_F8:
        if (st & GDK_SHIFT_MASK) on_step_out(NULL, w); else on_step_over(NULL, w);
        return TRUE;
    case GDK_KEY_F12: hide_window(w); return TRUE;
    default: return FALSE;
    }
}

static gboolean on_close(GtkWindow *win, gpointer ud)
{
    (void)win;
    hide_window(ud);
    return TRUE;
}

/* ---- construction --------------------------------------------------------- */

static GtkWidget *build_toolbar(DbgWin *w)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *b;
    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 8);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 6);
#define TB(label, cb) do { b = gtk_button_new_with_label(label); \
    g_signal_connect(b, "clicked", G_CALLBACK(cb), w); gtk_box_append(GTK_BOX(bar), b); } while (0)
    w->run_btn = GTK_BUTTON(gtk_button_new_with_label("Stop (F5)"));
    g_signal_connect(w->run_btn, "clicked", G_CALLBACK(on_run), w);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->run_btn));
    TB("Step (F7)", on_step);
    TB("Step Over (F8)", on_step_over);
    TB("Step Out (\xe2\x87\xa7""F8)", on_step_out);
    TB("Scanline+1", on_scanline);
    TB("Frame+1", on_frame);
#undef TB
    w->status = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->status), "dim-label");
    gtk_widget_set_hexpand(GTK_WIDGET(w->status), TRUE);
    gtk_label_set_xalign(w->status, 1.0);
    gtk_label_set_ellipsize(w->status, PANGO_ELLIPSIZE_START);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->status));
    return bar;
}

static GtkWidget *build_prompt(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = mono_view(&w->prompt_out, FALSE);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *b, *save, *pop, *popbox;
    GtkEventController *keys;
    static const struct { const char *kind, *title, *file; } saves[] = {
        { "dis", "Disassembly", "disassembly.asm" },
        { "prg", "PRG SRAM", "prg.bin" },
        { "chr", "CHR SRAM", "chr.bin" },
        { "ram", "Console RAM", "ram.bin" },
        { "wram", "WRAM", "wram.bin" } };
    unsigned i;

    w->prompt_scroll = GTK_SCROLLED_WINDOW(scroll);
    gtk_text_view_set_wrap_mode(w->prompt_out, GTK_WRAP_WORD_CHAR);
    w->prompt_in = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->prompt_in,
        "Command (help, step, break, bpw, print, mem, poke, disasm, cart, ...) \xe2\x80\x94 Tab completes");
    gtk_widget_set_hexpand(GTK_WIDGET(w->prompt_in), TRUE);
    g_signal_connect(w->prompt_in, "activate", G_CALLBACK(on_prompt), w);
    keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_prompt_key), w);
    gtk_widget_add_controller(GTK_WIDGET(w->prompt_in), keys);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->prompt_in));
    b = gtk_button_new_with_label("Load symbols\xe2\x80\xa6");
    g_signal_connect(b, "clicked", G_CALLBACK(on_symbols), w);
    gtk_box_append(GTK_BOX(row), b);

    save = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(save), "Save\xe2\x80\xa6");
    pop = gtk_popover_new();
    popbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    for (i = 0; i < G_N_ELEMENTS(saves); i++) {
        GtkWidget *sb = gtk_button_new_with_label(saves[i].title);
        gtk_widget_add_css_class(sb, "flat");
        g_object_set_data(G_OBJECT(sb), "kind", (gpointer)saves[i].kind);
        g_object_set_data(G_OBJECT(sb), "title", (gpointer)saves[i].title);
        g_object_set_data(G_OBJECT(sb), "file", (gpointer)saves[i].file);
        g_signal_connect(sb, "clicked", G_CALLBACK(on_save), w);
        gtk_box_append(GTK_BOX(popbox), sb);
    }
    gtk_popover_set_child(GTK_POPOVER(pop), popbox);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(save), pop);
    gtk_box_append(GTK_BOX(row), save);

    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), row);
    set_text(w->prompt_out, "NES debugger prompt (MesenCE engine). Type 'help' for every command.\n");
    return padded(box);
}

static GtkWidget *build_cpu(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *regs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *flags = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *ram = mono_view(&w->ram_view, FALSE);
    GtkWidget *edit = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    static const char *const names[6] = { "PC", "SP", "A", "X", "Y", "P" };
    static const char *const fnames[6] = { "N", "V", "D", "I", "Z", "C" };
    int i;
    for (i = 0; i < 6; i++) {
        w->reg[i] = GTK_ENTRY(gtk_entry_new());
        gtk_entry_set_max_length(w->reg[i], 6);
        gtk_editable_set_width_chars(GTK_EDITABLE(w->reg[i]), 5);
        g_object_set_data(G_OBJECT(w->reg[i]), "reg", GINT_TO_POINTER(i));
        g_signal_connect(w->reg[i], "activate", G_CALLBACK(on_reg_activate), w);
        gtk_box_append(GTK_BOX(regs), labeled(names[i], GTK_WIDGET(w->reg[i])));
    }
    for (i = 0; i < 6; i++) {
        w->flag[i] = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(fnames[i]));
        g_object_set_data(G_OBJECT(w->flag[i]), "flag", GINT_TO_POINTER(i));
        g_signal_connect(w->flag[i], "toggled", G_CALLBACK(on_flag_toggled), w);
        gtk_box_append(GTK_BOX(flags), GTK_WIDGET(w->flag[i]));
    }
    w->cycles = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->cycles), "dim-label");
    gtk_box_append(GTK_BOX(flags), GTK_WIDGET(w->cycles));

    w->ram_addr = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(w->ram_addr), 8);
    gtk_entry_set_placeholder_text(w->ram_addr, "$0300");
    w->ram_val = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(w->ram_val), 4);
    gtk_entry_set_placeholder_text(w->ram_val, "$00");
    g_signal_connect(w->ram_val, "activate", G_CALLBACK(on_ram_write), w);
    gtk_box_append(GTK_BOX(edit), labeled("Write address", GTK_WIDGET(w->ram_addr)));
    gtk_box_append(GTK_BOX(edit), labeled("value", GTK_WIDGET(w->ram_val)));
    gtk_box_append(GTK_BOX(edit), gtk_label_new("(RAM, WRAM, the cartridge SRAMs and labels all take it)"));

    gtk_box_append(GTK_BOX(box), regs);
    gtk_box_append(GTK_BOX(box), flags);
    gtk_box_append(GTK_BOX(box), gtk_label_new("Console RAM ($0000-$07FF)"));
    gtk_box_append(GTK_BOX(box), ram);
    gtk_box_append(GTK_BOX(box), edit);
    return padded(box);
}

static GtkWidget *build_disasm(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *scroll = mono_view(&w->disasm, FALSE);
    GtkGesture *click = gtk_gesture_click_new();
    GtkEventController *scrollc = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->disasm);
    GtkWidget *hint;
    char rgb[16];

    g_snprintf(rgb, sizeof rgb, "#%06x", NESSESSION_ACCENT_RGB);
    gtk_text_buffer_create_tag(b, "pc", "background", rgb, "foreground", "#ffffff", NULL);

    w->follow_pc = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Follow PC"));
    gtk_check_button_set_active(w->follow_pc, TRUE);
    g_signal_connect(w->follow_pc, "toggled", G_CALLBACK(on_follow_toggled), w);
    w->jump = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->jump, "$C000 or label");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->jump), 14);
    g_signal_connect(w->jump, "activate", G_CALLBACK(on_jump), w);
    hint = gtk_label_new("Click a line to toggle its breakpoint; scroll to browse");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->follow_pc));
    gtk_box_append(GTK_BOX(row), labeled("Jump to", GTK_WIDGET(w->jump)));
    gtk_box_append(GTK_BOX(row), hint);

    g_signal_connect(click, "pressed", G_CALLBACK(on_disasm_click), w);
    gtk_widget_add_controller(GTK_WIDGET(w->disasm), GTK_EVENT_CONTROLLER(click));
    g_signal_connect(scrollc, "scroll", G_CALLBACK(on_disasm_scroll), w);
    gtk_widget_add_controller(GTK_WIDGET(w->disasm), scrollc);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);

    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), scroll);
    w->disasm_top = 0x8000;
    return padded(box);
}

static GtkWidget *build_ppu(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *picscroll = gtk_scrolled_window_new();
    GtkWidget *regs = mono_view(&w->ppu_text, FALSE);
    GtkWidget *oam = mono_view(&w->oam_view, FALSE);
    static const char *const views[] = { "Nametables", "Patterns", "Sprites", "Palette", NULL };

    gtk_widget_set_size_request(left, 380, -1);
    gtk_widget_set_hexpand(left, FALSE);
    gtk_box_append(GTK_BOX(left), regs);
    gtk_box_append(GTK_BOX(left), gtk_label_new("OAM"));
    gtk_box_append(GTK_BOX(left), oam);

    w->ppu_view = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(views));
    g_signal_connect(w->ppu_view, "notify::selected", G_CALLBACK(on_view_changed), w);
    w->ppu_palette = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 7, 1));
    g_signal_connect(w->ppu_palette, "value-changed", G_CALLBACK(on_palette_changed), w);
    gtk_widget_set_sensitive(GTK_WIDGET(w->ppu_palette), FALSE);
    gtk_box_append(GTK_BOX(row), labeled("View", GTK_WIDGET(w->ppu_view)));
    gtk_box_append(GTK_BOX(row), labeled("Palette", GTK_WIDGET(w->ppu_palette)));

    w->ppu_pic = GTK_PICTURE(gtk_picture_new());
    gtk_picture_set_can_shrink(w->ppu_pic, FALSE);
    gtk_widget_set_halign(GTK_WIDGET(w->ppu_pic), GTK_ALIGN_START);
    gtk_widget_set_valign(GTK_WIDGET(w->ppu_pic), GTK_ALIGN_START);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(picscroll), GTK_WIDGET(w->ppu_pic));
    gtk_widget_set_vexpand(picscroll, TRUE);
    gtk_widget_set_hexpand(picscroll, TRUE);
    gtk_box_append(GTK_BOX(right), row);
    gtk_box_append(GTK_BOX(right), picscroll);

    gtk_box_append(GTK_BOX(box), left);
    gtk_box_append(GTK_BOX(box), right);
    w->view_px = g_new0(guint32, NESDEBUG_VIEW_MAX_PIXELS);
    w->view_px2 = g_new0(guint32, NESDEBUG_VIEW_MAX_PIXELS * 4);
    return padded(box);
}

static GtkWidget *build_apu(DbgWin *w)
{
    return padded(mono_view(&w->apu_text, FALSE));
}

static GtkWidget *build_breaks(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *add, *clear;
    static const char *const types[] = { "Execute", "Read", "Write", "Read/Write", NULL };

    w->bp_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(w->bp_list, GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(w->bp_list));
    gtk_widget_set_vexpand(scroll, TRUE);

    w->bp_type = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(types));
    w->bp_start = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_start, "$C000 / label");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->bp_start), 12);
    w->bp_end = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_end, "(end)");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->bp_end), 8);
    w->bp_cond = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_cond, "condition (a == $FF && [$10] > 3) \xe2\x80\x94 optional");
    gtk_widget_set_hexpand(GTK_WIDGET(w->bp_cond), TRUE);
    g_signal_connect(w->bp_start, "activate", G_CALLBACK(on_bp_add), w);
    g_signal_connect(w->bp_end, "activate", G_CALLBACK(on_bp_add), w);
    g_signal_connect(w->bp_cond, "activate", G_CALLBACK(on_bp_add), w);
    add = gtk_button_new_with_label("Add");
    g_signal_connect(add, "clicked", G_CALLBACK(on_bp_add), w);
    clear = gtk_button_new_with_label("Clear all");
    g_signal_connect(clear, "clicked", G_CALLBACK(on_bp_clear), w);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_type));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_start));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_end));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_cond));
    gtk_box_append(GTK_BOX(row), add);
    gtk_box_append(GTK_BOX(row), clear);
    w->bp_msg = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->bp_msg), "nes-accent-text");
    gtk_label_set_xalign(w->bp_msg, 0);

    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(w->bp_msg));
    return padded(box);
}

static GtkWidget *build_cart(DbgWin *w)
{
    return padded(mono_view(&w->cart_text, FALSE));
}

static void on_destroy(GtkWidget *widget, gpointer ud)
{
    DbgWin *w = ud;
    (void)widget;
    if (w->timer) g_source_remove(w->timer);
    nesdebug_detach(w->dbg);
    g_free(w->view_px);
    g_free(w->view_px2);
    if (g_win == w) g_win = NULL;
    g_free(w);
}

static void attach_and_refresh(DbgWin *w)
{
    nesdebug_attach(w->dbg);     /* stops the machine */
    w->seen_generation = nesdebug_generation(w->dbg);
    w->was_stopped = nesdebug_is_stopped(w->dbg) != 0;
    refresh_all(w);
}

void nes_debugger_show(GtkWindow *parent, nessession *session)
{
    DbgWin *w;
    GtkWidget *toolbar, *header, *root, *notebook;
    GtkEventController *keys;

    if (g_win) {
        gtk_window_present(g_win->win);
        attach_and_refresh(g_win);
        return;
    }
    w = g_new0(DbgWin, 1);
    w->session = session;
    w->dbg = nessession_debugger(session);
    g_win = w;

    w->win = GTK_WINDOW(adw_window_new());
    gtk_window_set_title(w->win, "Debugger");
    gtk_window_set_default_size(w->win, 1100, 800);
    gtk_window_set_transient_for(w->win, parent);
    gtk_window_set_application(w->win, gtk_window_get_application(parent));
    gtk_window_set_destroy_with_parent(w->win, TRUE);
    g_signal_connect(w->win, "close-request", G_CALLBACK(on_close), w);
    g_signal_connect(w->win, "destroy", G_CALLBACK(on_destroy), w);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(root), build_toolbar(w));
    notebook = gtk_notebook_new();
    gtk_widget_set_vexpand(notebook, TRUE);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_prompt(w), gtk_label_new("Prompt"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_cpu(w), gtk_label_new("CPU & RAM"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_disasm(w), gtk_label_new("Disassembly"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_ppu(w), gtk_label_new("PPU"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_apu(w), gtk_label_new("APU & Input"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_breaks(w), gtk_label_new("Breakpoints"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_cart(w), gtk_label_new("Cart"));
    gtk_box_append(GTK_BOX(root), notebook);
    {
        const char *tab = g_getenv("NES_DEBUGGER_TAB");
        if (tab && *tab) gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), atoi(tab));
    }

    header = adw_header_bar_new();
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root);
    adw_window_set_content(ADW_WINDOW(w->win), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), w);
    gtk_widget_add_controller(GTK_WIDGET(w->win), keys);

    attach_and_refresh(w);
    w->timer = g_timeout_add(100, tick_refresh, w);
    gtk_window_present(w->win);
}

void nes_debugger_toggle(GtkWindow *parent, nessession *session)
{
    if (g_win && gtk_widget_get_visible(GTK_WIDGET(g_win->win)))
        hide_window(g_win);
    else
        nes_debugger_show(parent, session);
}
