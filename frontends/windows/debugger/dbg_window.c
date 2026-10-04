/*
 * Debugger window (Win32) over MesenCE's debugger engine, via
 * core/include/nesdebug.h. Mirrors the GTK and Qt debuggers tab for tab --
 * Prompt, CPU & RAM, Disassembly, PPU, APU & Input, Breakpoints, Cart --
 * built from plain common controls.
 *
 * The engine only exists while this window is showing: showing it attaches
 * (which stops the machine, as on every sibling) and hiding it detaches,
 * which lets the machine run on at full speed. The window refreshes on a
 * timer keyed to the engine's generation counter rather than on every tick.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nesdebug.h"

#define DISASM_WINDOW 48
#define MAX_BPS 128

#define WM_DBG_ACCEPT (WM_APP + 2)   /* wp: control id -- Enter in an edit */
#define WM_DBG_TAB    (WM_APP + 3)   /* Tab in the prompt: complete */

#define TIMER_REFRESH 1

enum {
    PAGE_PROMPT = 0, PAGE_CPU, PAGE_DISASM, PAGE_PPU, PAGE_APU, PAGE_BREAKS, PAGE_CART,
    PAGE_COUNT
};

enum {
    IDC_RUN = 1000, IDC_STEP, IDC_OVER, IDC_OUT, IDC_SCAN, IDC_FRAME,
    IDC_STATUS, IDC_TABS,
    IDC_PROMPT_OUT, IDC_PROMPT_IN, IDC_LOAD_SYMBOLS, IDC_SAVE,
    IDC_REG0, IDC_REG_LAST = IDC_REG0 + 5,
    IDC_FLAG0, IDC_FLAG_LAST = IDC_FLAG0 + 5,
    IDC_CYCLES, IDC_RAM_VIEW, IDC_RAM_ADDR, IDC_RAM_VAL,
    IDC_FOLLOW_PC, IDC_JUMP, IDC_DISASM,
    IDC_PPU_TEXT, IDC_PPU_VIEW, IDC_PPU_PALETTE, IDC_PPU_PIC, IDC_OAM,
    IDC_APU,
    IDC_BP_LIST, IDC_BP_TYPE, IDC_BP_START, IDC_BP_END, IDC_BP_COND, IDC_BP_ADD,
    IDC_BP_REMOVE, IDC_BP_ENABLE, IDC_BP_CLEAR,
    IDC_CART,
    IDC_SAVE_KIND0, IDC_SAVE_KIND_LAST = IDC_SAVE_KIND0 + 4,
    IDC_LABEL_FIRST
};

typedef struct {
    HWND hwnd;
    HWND tabs;
    HWND run_btn, step_btn, over_btn, out_btn, scan_btn, frame_btn, status;

    HWND prompt_out, prompt_in, load_symbols, save;

    HWND reg_label[6], reg_edit[6], flag[6], cycles, ram_label, ram_view;
    HWND ram_addr_label, ram_addr, ram_val_label, ram_val;

    HWND follow_pc, jump_label, jump, disasm_hint, disasm;

    HWND ppu_text, ppu_view_label, ppu_view, ppu_pal_label, ppu_palette, ppu_pic, oam;

    HWND apu;

    HWND bp_list, bp_type, bp_start_label, bp_start, bp_end_label, bp_end, bp_cond_label, bp_cond;
    HWND bp_add, bp_remove, bp_enable, bp_clear;

    HWND cart;

    HFONT mono, ui;
    HACCEL accel;
    HBRUSH accent;

    nessession *session;
    nesdebug *dbg;

    int page;
    unsigned seen_gen;
    int was_stopped;
    int running_ticks;

    int disasm_top;            /* address of the first disassembly line */
    uint16_t line_addr[DISASM_WINDOW];
    int line_count;
    int pc_line;

    nesdebug_breakpoint bps[MAX_BPS];
    int nbps;

    uint32_t *pic_px;
    int pic_w, pic_h;
} debugger;

static debugger *g_dbg;

static const char *const kSaveKinds[5] = { "dis", "prg", "chr", "ram", "wram" };
static const char *const kSaveTitles[5] = { "Disassembly ($8000-$FFFF)...", "PRG SRAM...", "CHR SRAM...",
                                            "Console RAM...", "Cartridge WRAM..." };

/* ---- helpers ---------------------------------------------------------------- */

static void set_text(HWND h, const char *text) { SetWindowTextA(h, text); }

/* EDIT controls want CRLF and render control characters as boxes. */
static char *to_crlf(const char *text)
{
    size_t n = strlen(text), i, o = 0;
    char *buf = malloc(n * 2 + 1);
    if (!buf) return NULL;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') { buf[o++] = '\r'; buf[o++] = '\n'; }
        else if (c >= 0x20 || c == '\t') buf[o++] = (char)c;
    }
    buf[o] = '\0';
    return buf;
}

static void set_text_lf(HWND h, const char *text)
{
    char *buf = to_crlf(text);
    set_text(h, buf ? buf : text);
    free(buf);
}

static void append_text_lf(HWND h, const char *text)
{
    char *buf = to_crlf(text);
    int len;
    if (!buf) return;
    len = GetWindowTextLengthA(h);
    SendMessageA(h, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageA(h, EM_REPLACESEL, FALSE, (LPARAM)buf);
    SendMessageA(h, EM_SCROLLCARET, 0, 0);
    free(buf);
}

static void edit_text(HWND h, char *out, int outsz) { GetWindowTextA(h, out, outsz); }

/* $hex, 0xhex, #dec, or bare hex, as the prompt reads numbers. */
static int parse_num(const char *text, long *out)
{
    char buf[64];
    const char *p;
    char *end;
    long v;
    int base = 16;

    snprintf(buf, sizeof buf, "%s", text);
    p = buf;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '$') p++;
    else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    else if (*p == '#') { p++; base = 10; }
    if (!*p) return 0;
    v = strtol(p, &end, base);
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (*end) return 0;
    *out = v;
    return 1;
}

static int resolve_addr(debugger *d, const char *text)
{
    long v;
    int addr = -1;
    const char *p = text;
    while (*p == ' ') p++;
    if (!*p) return -1;
    if (*p != '$' && *p != '#' && !(p[0] == '0' && (p[1] == 'x' || p[1] == 'X')))
        addr = nesdebug_label_address(d->dbg, p);
    if (addr < 0 && parse_num(p, &v) && v >= 0 && v <= 0xffff) addr = (int)v;
    return addr;
}

/* ---- refreshers -------------------------------------------------------------- */

static void refresh_status(debugger *d)
{
    char reason[160], buf[220];
    int addr;
    const int stopped = nesdebug_is_stopped(d->dbg);
    nesdebug_stop_reason(d->dbg, reason, sizeof reason, &addr);
    if (stopped) snprintf(buf, sizeof buf, "Stopped%s%s", reason[0] ? ": " : "", reason);
    else snprintf(buf, sizeof buf, "Running");
    set_text(d->status, buf);
    set_text(d->run_btn, stopped ? "Run (F5)" : "Stop (F5)");
    InvalidateRect(d->run_btn, NULL, FALSE);
}

static void refresh_cpu(debugger *d)
{
    nesdebug_cpu c;
    char buf[160];
    int i;
    nesdebug_cpu_get(d->dbg, &c);
    {
        const int v[6] = { c.pc, c.sp, c.a, c.x, c.y, c.ps };
        for (i = 0; i < 6; i++) {
            if (GetFocus() == d->reg_edit[i]) continue;   /* do not fight the user's typing */
            snprintf(buf, sizeof buf, i == 0 ? "%04X" : "%02X", v[i]);
            set_text(d->reg_edit[i], buf);
        }
    }
    {
        const int flags[6] = { c.n, c.v, c.d, c.i, c.z, c.c };
        for (i = 0; i < 6; i++)
            SendMessageA(d->flag[i], BM_SETCHECK, flags[i] ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    snprintf(buf, sizeof buf, "cycle %llu   scanline %d   dot %d   frame %u%s%s",
             (unsigned long long)c.total_cycles, c.scanline, c.dot, c.frame,
             c.irq ? "   IRQ" : "", c.nmi ? "   NMI" : "");
    set_text(d->cycles, buf);
}

static void refresh_ram(debugger *d)
{
    static uint8_t ram[2048];
    static char text[2048 / 16 * 80 + 128];
    size_t len = 0;
    int row, col;
    nesdebug_ram_get(d->dbg, ram);
    len += (size_t)snprintf(text + len, sizeof text - len,
                            "        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\r\n");
    for (row = 0; row < 2048 / 16; row++) {
        len += (size_t)snprintf(text + len, sizeof text - len, "$%04X: ", row * 16);
        for (col = 0; col < 16; col++)
            len += (size_t)snprintf(text + len, sizeof text - len, "%02X ", ram[row * 16 + col]);
        len += (size_t)snprintf(text + len, sizeof text - len, "\r\n");
    }
    {
        /* keep the scroll position across a refresh */
        int first = (int)SendMessageA(d->ram_view, EM_GETFIRSTVISIBLELINE, 0, 0);
        set_text(d->ram_view, text);
        SendMessageA(d->ram_view, EM_LINESCROLL, 0, first);
    }
}

static void refresh_disasm(debugger *d)
{
    static nesdebug_line lines[DISASM_WINDOW];
    nesdebug_cpu c;
    int pc_line = -1, n, i;
    char *text;
    size_t cap = DISASM_WINDOW * 200, len = 0;
    const int follow = SendMessageA(d->follow_pc, BM_GETCHECK, 0, 0) == BST_CHECKED;

    if (follow) {
        nesdebug_cpu_get(d->dbg, &c);
        d->disasm_top = nesdebug_row_address(d->dbg, (uint16_t)c.pc, -(DISASM_WINDOW / 3));
    }
    n = nesdebug_disassemble(d->dbg, (uint16_t)d->disasm_top, lines, DISASM_WINDOW, &pc_line);

    text = malloc(cap);
    if (!text) return;
    text[0] = '\0';
    d->line_count = n;
    d->pc_line = pc_line;
    for (i = 0; i < n; i++) {
        d->line_addr[i] = lines[i].address;
        len += (size_t)snprintf(text + len, cap - len, "%c%c %04X  %-9s %-12s %s%s%s\r\n",
                                lines[i].has_breakpoint ? '*' : ' ', lines[i].is_pc ? '>' : ' ',
                                lines[i].address, lines[i].bytes, lines[i].label, lines[i].disasm,
                                lines[i].comment[0] ? "  " : "", lines[i].comment);
        if (len + 220 >= cap) break;
    }
    if (n == 0) snprintf(text, cap, "(no disassembly)\r\n");
    set_text(d->disasm, text);
    /* The PC line, highlighted by selecting it (EDIT controls have no per-line
     * colours; the selection shows even without focus thanks to
     * ES_NOHIDESEL). */
    if (pc_line >= 0) {
        int start = (int)SendMessageA(d->disasm, EM_LINEINDEX, (WPARAM)pc_line, 0);
        int llen = (int)SendMessageA(d->disasm, EM_LINELENGTH, (WPARAM)start, 0);
        SendMessageA(d->disasm, EM_SETSEL, (WPARAM)start, (LPARAM)(start + llen));
    } else {
        SendMessageA(d->disasm, EM_SETSEL, (WPARAM)-1, 0);
    }
    SendMessageA(d->disasm, EM_LINESCROLL, 0, -DISASM_WINDOW);
    free(text);
}

static void refresh_ppu(debugger *d)
{
    nesdebug_ppu p;
    nesdebug_sprite oam[64];
    static char s[4096];
    size_t len = 0;
    int i, view, pal;

    nesdebug_ppu_get(d->dbg, &p);
    len += (size_t)snprintf(s + len, sizeof s - len,
        "Frame %u   scanline %d   dot %d\n\n"
        "PPUCTRL   $2000  %02X   NMI %s, sprites 8x%d, BG $%04X, SPR $%04X, +%d\n"
        "PPUMASK   $2001  %02X   BG %s%s, SPR %s%s%s\n"
        "PPUSTATUS $2002  %02X   VBlank %d  Sprite 0 hit %d  Overflow %d\n"
        "OAMADDR   $2003  %02X\n\n"
        "v $%04X   t $%04X   fine X %d   write toggle %d\n"
        "Mirroring: %s\n",
        p.frame, p.scanline, p.dot,
        p.ctrl, p.nmi_on_vblank ? "on" : "off", p.sprite_size_16 ? 16 : 8,
        p.bg_table_1000 ? 0x1000 : 0, p.spr_table_1000 ? 0x1000 : 0, p.increment_32 ? 32 : 1,
        p.mask, p.show_bg ? "on" : "off", p.show_bg_left ? "" : " (not left 8)",
        p.show_spr ? "on" : "off", p.show_spr_left ? "" : " (not left 8)", p.grayscale ? ", grayscale" : "",
        p.status, p.vblank, p.sprite0_hit, p.sprite_overflow,
        p.oam_addr, p.vram_addr, p.tmp_addr, p.fine_x, p.write_toggle, p.mirroring);
    set_text_lf(d->ppu_text, s);

    nesdebug_oam_get(d->dbg, oam);
    len = 0;
    len += (size_t)snprintf(s + len, sizeof s - len, " #   Y   tile attr  X\n");
    for (i = 0; i < 64; i++)
        len += (size_t)snprintf(s + len, sizeof s - len, "%2d  %3d  $%02X  $%02X %3d%s\n", i, oam[i].y,
                                oam[i].tile, oam[i].attr, oam[i].x, oam[i].y >= 0xEF ? "  (hidden)" : "");
    {
        int first = (int)SendMessageA(d->oam, EM_GETFIRSTVISIBLELINE, 0, 0);
        set_text_lf(d->oam, s);
        SendMessageA(d->oam, EM_LINESCROLL, 0, first);
    }

    view = (int)SendMessageA(d->ppu_view, CB_GETCURSEL, 0, 0);
    pal = (int)SendMessageA(d->ppu_palette, CB_GETCURSEL, 0, 0);
    if (view < 0) view = 0;
    if (pal < 0) pal = 0;
    if (!nesdebug_ppu_view(d->dbg, view, pal, d->pic_px, &d->pic_w, &d->pic_h))
        d->pic_w = d->pic_h = 0;
    InvalidateRect(d->ppu_pic, NULL, FALSE);
}

static void refresh_apu(debugger *d)
{
    nesdebug_apu a;
    static const char *const names[8] = { "A", "B", "Select", "Start", "Up", "Down", "Left", "Right" };
    char s[1600];
    size_t len = 0;
    int port, b;

    nesdebug_apu_get(d->dbg, &a);
    len += (size_t)snprintf(s + len, sizeof s - len,
        "Pulse 1    %s   period %4d   volume %2d   duty %d\n"
        "Pulse 2    %s   period %4d   volume %2d   duty %d\n"
        "Triangle   %s   period %4d\n"
        "Noise      %s   period %4d   volume %2d\n"
        "DMC        %s   %d bytes left\n\n"
        "Frame IRQ %s   DMC IRQ %s\n\n",
        a.pulse1_enabled ? "on " : "off", a.pulse1_period, a.pulse1_volume, a.pulse1_duty,
        a.pulse2_enabled ? "on " : "off", a.pulse2_period, a.pulse2_volume, a.pulse2_duty,
        a.triangle_enabled ? "on " : "off", a.triangle_period,
        a.noise_enabled ? "on " : "off", a.noise_period, a.noise_volume,
        a.dmc_enabled ? "on " : "off", a.dmc_bytes_left,
        a.frame_irq ? "enabled" : "inhibited", a.dmc_irq ? "enabled" : "off");
    for (port = 0; port < 2; port++) {
        len += (size_t)snprintf(s + len, sizeof s - len, "Controller %d  $%02X  ", port + 1, a.pad[port]);
        for (b = 0; b < 8; b++)
            if (a.pad[port] & (1 << b))
                len += (size_t)snprintf(s + len, sizeof s - len, " %s", names[b]);
        len += (size_t)snprintf(s + len, sizeof s - len, "\n");
    }
    set_text_lf(d->apu, s);
}

static void refresh_bps(debugger *d)
{
    int i, sel = (int)SendMessageA(d->bp_list, LB_GETCURSEL, 0, 0);
    d->nbps = nesdebug_breakpoint_list(d->dbg, d->bps, MAX_BPS);
    SendMessageA(d->bp_list, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < d->nbps; i++) {
        const nesdebug_breakpoint *b = &d->bps[i];
        char line[256], kind[8] = "";
        if (b->type & NESDEBUG_BP_EXEC) strcat(kind, "x");
        if (b->type & NESDEBUG_BP_READ) strcat(kind, "r");
        if (b->type & NESDEBUG_BP_WRITE) strcat(kind, "w");
        if (b->start == b->end)
            snprintf(line, sizeof line, "%c %3d  %-3s  $%04X        %s%s", b->enabled ? '+' : '-',
                     b->id, kind, b->start, b->condition[0] ? "if " : "", b->condition);
        else
            snprintf(line, sizeof line, "%c %3d  %-3s  $%04X-$%04X  %s%s", b->enabled ? '+' : '-',
                     b->id, kind, b->start, b->end, b->condition[0] ? "if " : "", b->condition);
        SendMessageA(d->bp_list, LB_ADDSTRING, 0, (LPARAM)line);
    }
    if (d->nbps == 0) SendMessageA(d->bp_list, LB_ADDSTRING, 0, (LPARAM)"(no breakpoints -- click a disassembly line, or add one below)");
    if (sel >= 0 && sel < d->nbps) SendMessageA(d->bp_list, LB_SETCURSEL, (WPARAM)sel, 0);
}

static void refresh_cart(debugger *d)
{
    nesdebug_cart c;
    char info[256];
    static char s[2048];
    nesdebug_cart_get(d->dbg, &c);
    nesdebug_cart_info(d->dbg, info, sizeof info);
    if (!c.present) { set_text_lf(d->cart, "No FujiNet cartridge is running."); return; }
    snprintf(s, sizeof s,
        "%s\n\n"
        "Link            %s%s%s\n"
        "Mailbox         %s\n"
        "SRAM            %s%s\n"
        "Image           %s, mapper %d %s\n"
        "PRG slots       $8000:%02X  $A000:%02X  $C000:%02X  $E000:%02X\n"
        "CHR slots       %03X %03X %03X %03X  %03X %03X %03X %03X\n"
        "Mirroring       %s\n"
        "WRAM            %s%s     CHR %s\n"
        "IRQ             %s  line %d  latch %d  counter %d\n\n"
        "ACKSEQ $%02X   status $%02X   last error %u\n"
        "Boot state $%02X  %u%%  error %u\n"
        "RMW dummies dropped %u   worker queue %u\n%s%s",
        info,
        c.link_up ? "up" : "down", c.busy ? " (transaction in flight)" : "",
        (!c.link_up && c.link_error[0]) ? "" : "",
        c.mailbox_live ? "live" : "closed (the running image does not claim it)",
        c.sram_enabled ? "enabled" : "off (power-on vectors)", c.loading ? "  -- loading" : "",
        c.booted_image ? "game" : "CONFIG", c.mapper, c.mapper_name,
        c.prg_slot[0], c.prg_slot[1], c.prg_slot[2], c.prg_slot[3],
        c.chr_slot[0], c.chr_slot[1], c.chr_slot[2], c.chr_slot[3],
        c.chr_slot[4], c.chr_slot[5], c.chr_slot[6], c.chr_slot[7],
        c.mirroring, c.wram_enabled ? "on" : "off", c.wram_protected ? " (protected)" : "",
        c.chr_writable ? "RAM" : "ROM",
        c.irq_enabled ? "enabled" : "off", c.irq_line, c.irq_latch, c.irq_counter,
        c.ackseq, c.status, c.last_error, c.boot_state, c.boot_pct, c.boot_err,
        c.diag_rmw, c.queue_depth,
        c.link_error[0] ? "\nLink: " : "", c.link_error);
    set_text_lf(d->cart, s);
}

static void refresh_all(debugger *d)
{
    refresh_status(d);
    refresh_cpu(d);
    refresh_ram(d);
    refresh_disasm(d);
    refresh_ppu(d);
    refresh_apu(d);
    refresh_bps(d);
    refresh_cart(d);
}

/* ---- actions ------------------------------------------------------------------ */

static void toggle_run(debugger *d)
{
    if (nesdebug_is_stopped(d->dbg)) nesdebug_resume(d->dbg);
    else nesdebug_stop(d->dbg);
    refresh_all(d);
}

static void run_prompt(debugger *d)
{
    static char out[65536];
    char cmd[512], echo[540];
    edit_text(d->prompt_in, cmd, sizeof cmd);
    if (!cmd[0]) return;
    snprintf(echo, sizeof echo, "> %s\n", cmd);
    append_text_lf(d->prompt_out, echo);
    nesdebug_command(d->dbg, cmd, out, sizeof out);
    append_text_lf(d->prompt_out, out);
    append_text_lf(d->prompt_out, "\n");
    set_text(d->prompt_in, "");
    refresh_all(d);
}

static void complete_prompt(debugger *d)
{
    char text[512], comps[4096];
    const char *word;
    char *sp;
    int n;
    edit_text(d->prompt_in, text, sizeof text);
    sp = strrchr(text, ' ');
    word = sp ? sp + 1 : text;
    n = nesdebug_completions(d->dbg, word, comps, sizeof comps);
    if (n == 1) {
        char line[256], joined[800];
        char *nl;
        snprintf(line, sizeof line, "%.255s", comps);
        nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (sp) sp[1] = '\0'; else text[0] = '\0';
        snprintf(joined, sizeof joined, "%s%s ", text, line);
        set_text(d->prompt_in, joined);
        SendMessageA(d->prompt_in, EM_SETSEL, (WPARAM)strlen(joined), (LPARAM)strlen(joined));
    } else if (n > 1) {
        append_text_lf(d->prompt_out, comps);
    }
}

static void jump_to(debugger *d, const char *text)
{
    const int addr = resolve_addr(d, text);
    if (addr < 0) {
        append_text_lf(d->prompt_out, "No such label or address.\n");
        return;
    }
    SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
    d->disasm_top = addr;
    refresh_disasm(d);
}

static int pick_file(debugger *d, int save, const char *title, const char *filter, char *path, DWORD pathsz)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    path[0] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = d->hwnd;
    ofn.lpstrFile = path;
    ofn.nMaxFile = pathsz;
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    if (save) {
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        return GetSaveFileNameA(&ofn) ? 1 : 0;
    }
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

static void save_file(debugger *d, int which)
{
    char path[MAX_PATH], msg[512], title[64];
    snprintf(title, sizeof title, "Save %s", kSaveTitles[which]);
    if (!pick_file(d, 1, title, "All files\0*.*\0\0", path, sizeof path)) return;
    nesdebug_save(d->dbg, kSaveKinds[which], path, msg, sizeof msg);
    append_text_lf(d->prompt_out, msg);
    append_text_lf(d->prompt_out, "\n");
}

static void save_menu(debugger *d)
{
    HMENU m = CreatePopupMenu();
    RECT r;
    int i;
    for (i = 0; i < 5; i++) AppendMenuA(m, MF_STRING, (UINT_PTR)(IDC_SAVE_KIND0 + i), kSaveTitles[i]);
    GetWindowRect(d->save, &r);
    TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, d->hwnd, NULL);
    DestroyMenu(m);
}

static void load_symbols(debugger *d)
{
    char path[MAX_PATH], msg[512];
    if (!pick_file(d, 0, "Load symbols",
                   "Symbol files (*.dbg;*.lbl;*.mlb;*.sym;*.labels)\0*.dbg;*.lbl;*.mlb;*.sym;*.labels\0All files\0*.*\0\0",
                   path, sizeof path))
        return;
    nesdebug_load_symbols(d->dbg, path, msg, sizeof msg);
    append_text_lf(d->prompt_out, msg);
    append_text_lf(d->prompt_out, "\n");
    refresh_all(d);
}

static void add_breakpoint(debugger *d)
{
    char a[64], b[64], cond[256];
    static const int types[4] = { NESDEBUG_BP_EXEC, NESDEBUG_BP_READ, NESDEBUG_BP_WRITE,
                                  NESDEBUG_BP_READ | NESDEBUG_BP_WRITE };
    int start, end, sel, id;
    edit_text(d->bp_start, a, sizeof a);
    edit_text(d->bp_end, b, sizeof b);
    edit_text(d->bp_cond, cond, sizeof cond);
    start = resolve_addr(d, a);
    if (start < 0) { MessageBoxA(d->hwnd, "The start is not an address or label.", "Breakpoint", MB_ICONWARNING); return; }
    end = b[0] ? resolve_addr(d, b) : start;
    if (end < start) end = start;
    sel = (int)SendMessageA(d->bp_type, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel > 3) sel = 0;
    id = nesdebug_breakpoint_add(d->dbg, types[sel], (uint16_t)start, (uint16_t)end, cond);
    if (id < 0) { MessageBoxA(d->hwnd, "The condition does not parse.", "Breakpoint", MB_ICONWARNING); return; }
    set_text(d->bp_start, "");
    set_text(d->bp_end, "");
    set_text(d->bp_cond, "");
    refresh_bps(d);
    refresh_disasm(d);
}

static void on_accept(debugger *d, int id)
{
    char buf[512];
    long v;

    if (id >= IDC_REG0 && id <= IDC_REG_LAST) {
        static const int reg_ids[6] = { NES_REG_PC, NES_REG_SP, NES_REG_A, NES_REG_X, NES_REG_Y, NES_REG_PS };
        edit_text(d->reg_edit[id - IDC_REG0], buf, sizeof buf);
        if (parse_num(buf, &v)) nesdebug_cpu_set(d->dbg, reg_ids[id - IDC_REG0], (int)v);
        SetFocus(d->hwnd);
        refresh_all(d);
        return;
    }
    switch (id) {
    case IDC_PROMPT_IN: run_prompt(d); break;
    case IDC_RAM_ADDR:
    case IDC_RAM_VAL: {
        char abuf[64];
        int a;
        edit_text(d->ram_addr, abuf, sizeof abuf);
        edit_text(d->ram_val, buf, sizeof buf);
        a = resolve_addr(d, abuf);
        if (a >= 0 && parse_num(buf, &v)) nesdebug_write(d->dbg, (uint16_t)a, (uint8_t)v);
        refresh_all(d);
        break;
    }
    case IDC_JUMP:
        edit_text(d->jump, buf, sizeof buf);
        jump_to(d, buf);
        break;
    case IDC_BP_START:
    case IDC_BP_END:
    case IDC_BP_COND:
        add_breakpoint(d);
        break;
    default: break;
    }
}

/* ---- the PPU picture --------------------------------------------------------------- */

static LRESULT CALLBACK pic_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT c;
        GetClientRect(hwnd, &c);
        FillRect(dc, &c, (HBRUSH)GetStockObject(DKGRAY_BRUSH));
        if (d && d->pic_px && d->pic_w > 0 && d->pic_h > 0) {
            BITMAPINFO bmi;
            int scale = 2, w, h;
            /* 2x nearest when it fits, else 1x: a tile's pixels stay whole */
            if (d->pic_w * 2 > c.right || d->pic_h * 2 > c.bottom) scale = 1;
            w = d->pic_w * scale;
            h = d->pic_h * scale;
            memset(&bmi, 0, sizeof bmi);
            bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
            bmi.bmiHeader.biWidth = d->pic_w;
            bmi.bmiHeader.biHeight = -d->pic_h;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchDIBits(dc, (c.right - w) / 2 > 0 ? (c.right - w) / 2 : 0, 0, w, h,
                          0, 0, d->pic_w, d->pic_h, d->pic_px, &bmi, DIB_RGB_COLORS, SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- edit subclassing ------------------------------------------------------------- */

static WNDPROC g_edit_proc;
static WNDPROC g_disasm_proc;

/* Enter in a single-line field means "apply this value"; Tab in the prompt
 * completes. Neither may beep its way through the default handler. */
static LRESULT CALLBACK edit_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    const int id = (int)GetWindowLongPtrA(hwnd, GWLP_ID);
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        PostMessageA(GetAncestor(hwnd, GA_ROOT), WM_DBG_ACCEPT, (WPARAM)id, 0);
        return 0;
    }
    if (msg == WM_CHAR && wp == VK_RETURN) return 0;
    if (id == IDC_PROMPT_IN && msg == WM_KEYDOWN && wp == VK_TAB) {
        PostMessageA(GetAncestor(hwnd, GA_ROOT), WM_DBG_TAB, 0, 0);
        return 0;
    }
    if (id == IDC_PROMPT_IN && msg == WM_CHAR && wp == VK_TAB) return 0;
    if (id == IDC_PROMPT_IN && msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    return CallWindowProcA(g_edit_proc, hwnd, msg, wp, lp);
}

/* A click in the disassembly toggles the breakpoint on that line; the
 * wheel browses (and turns Follow PC off, as in the other frontends). */
static LRESULT CALLBACK disasm_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (!d) return CallWindowProcA(g_disasm_proc, hwnd, msg, wp, lp);
    if (msg == WM_LBUTTONDOWN) {
        LRESULT pos = SendMessageA(hwnd, EM_CHARFROMPOS, 0, lp);
        int line = HIWORD(pos);
        if (line >= 0 && line < d->line_count) {
            nesdebug_breakpoint_toggle(d->dbg, d->line_addr[line]);
            refresh_disasm(d);
            refresh_bps(d);
        }
        return 0;
    }
    if (msg == WM_LBUTTONDBLCLK || msg == WM_MOUSEMOVE || msg == WM_LBUTTONUP) return 0;
    if (msg == WM_MOUSEWHEEL) {
        int rows = -GET_WHEEL_DELTA_WPARAM(wp) / 40;
        SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
        if (rows) d->disasm_top = nesdebug_row_address(d->dbg, (uint16_t)d->disasm_top, rows);
        refresh_disasm(d);
        return 0;
    }
    if (msg == WM_KEYDOWN && (wp == VK_PRIOR || wp == VK_NEXT || wp == VK_UP || wp == VK_DOWN)) {
        int rows = wp == VK_PRIOR ? -DISASM_WINDOW / 2 : wp == VK_NEXT ? DISASM_WINDOW / 2 : wp == VK_UP ? -1 : 1;
        SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
        d->disasm_top = nesdebug_row_address(d->dbg, (uint16_t)d->disasm_top, rows);
        refresh_disasm(d);
        return 0;
    }
    return CallWindowProcA(g_disasm_proc, hwnd, msg, wp, lp);
}

/* ---- construction ------------------------------------------------------------------ */

static HWND child(debugger *d, const char *cls, const char *text, DWORD style, int id, HFONT font)
{
    HWND h = CreateWindowExA(0, cls, text, WS_CHILD | style, 0, 0, 10, 10, d->hwnd,
                             (HMENU)(INT_PTR)id, (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE), NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);
    return h;
}

static HWND mono_view(debugger *d, int id, int wrap)
{
    return child(d, "EDIT", "",
                 WS_BORDER | WS_VSCROLL | (wrap ? 0 : WS_HSCROLL) | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                 id, d->mono);
}

static HWND field(debugger *d, int id, HFONT font)
{
    HWND h = child(d, "EDIT", "", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, id, font);
    g_edit_proc = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)edit_subclass);
    return h;
}

static HWND label(debugger *d, const char *text)
{
    static int next_id;
    return child(d, "STATIC", text, SS_LEFT, IDC_LABEL_FIRST + next_id++, d->ui);
}

static HWND button(debugger *d, const char *text, int id)
{
    return child(d, "BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, id, d->ui);
}

static HWND checkbox(debugger *d, const char *text, int id)
{
    return child(d, "BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, id, d->ui);
}

static HWND combo(debugger *d, int id, const char *const *items, int n)
{
    HWND c = child(d, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id, d->ui);
    int i;
    for (i = 0; i < n; i++) SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)items[i]);
    SendMessageA(c, CB_SETCURSEL, 0, 0);
    return c;
}

static void build_controls(debugger *d)
{
    TCITEMA item;
    static const char *const tabs[PAGE_COUNT] = { "Prompt", "CPU && RAM", "Disassembly", "PPU",
                                                  "APU && Input", "Breakpoints", "Cart" };
    static const char *const reg_names[6] = { "PC", "SP", "A", "X", "Y", "P" };
    static const char *const flag_names[6] = { "N", "V", "D", "I", "Z", "C" };
    static const char *const views[4] = { "Nametables", "Patterns", "Sprites", "Palette" };
    static const char *const pals[8] = { "BG 0", "BG 1", "BG 2", "BG 3", "Sprite 0", "Sprite 1", "Sprite 2", "Sprite 3" };
    static const char *const bp_types[4] = { "Execute", "Read", "Write", "Read/Write" };
    int i;

    d->tabs = child(d, WC_TABCONTROLA, "", WS_VISIBLE | WS_CLIPSIBLINGS, IDC_TABS, d->ui);
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    for (i = 0; i < PAGE_COUNT; i++) {
        item.pszText = (char *)tabs[i];
        SendMessageA(d->tabs, TCM_INSERTITEMA, (WPARAM)i, (LPARAM)&item);
    }

    /* Toolbar. Run/Stop is owner-drawn so it can wear the accent colour
     * while the machine is stopped. */
    d->run_btn = child(d, "BUTTON", "Stop (F5)", BS_OWNERDRAW | WS_TABSTOP, IDC_RUN, d->ui);
    d->step_btn = button(d, "Step (F7)", IDC_STEP);
    d->over_btn = button(d, "Over (F8)", IDC_OVER);
    d->out_btn = button(d, "Out (Shift+F8)", IDC_OUT);
    d->scan_btn = button(d, "Scanline+1", IDC_SCAN);
    d->frame_btn = button(d, "Frame+1", IDC_FRAME);
    d->status = child(d, "STATIC", "Running", SS_RIGHT | SS_ENDELLIPSIS, IDC_STATUS, d->ui);
    {
        HWND bar[7] = { d->run_btn, d->step_btn, d->over_btn, d->out_btn, d->scan_btn, d->frame_btn, d->status };
        for (i = 0; i < 7; i++) ShowWindow(bar[i], SW_SHOW);
    }

    /* Prompt */
    d->prompt_out = mono_view(d, IDC_PROMPT_OUT, 1);
    set_text(d->prompt_out, "NES debugger prompt. Type 'help' for every command.\r\n");
    d->prompt_in = field(d, IDC_PROMPT_IN, d->mono);
    SendMessageA(d->prompt_in, EM_SETCUEBANNER, TRUE,
                 (LPARAM)L"command (help, step, break, bpw, print, mem, runto, ...) - Tab completes");
    d->load_symbols = button(d, "Load symbols...", IDC_LOAD_SYMBOLS);
    d->save = button(d, "Save...", IDC_SAVE);

    /* CPU & RAM */
    for (i = 0; i < 6; i++) {
        d->reg_label[i] = label(d, reg_names[i]);
        d->reg_edit[i] = field(d, IDC_REG0 + i, d->mono);
        SendMessageA(d->reg_edit[i], EM_SETLIMITTEXT, 6, 0);
    }
    for (i = 0; i < 6; i++) d->flag[i] = checkbox(d, flag_names[i], IDC_FLAG0 + i);
    d->cycles = label(d, "");
    d->ram_label = label(d, "Console RAM ($0000-$07FF)");
    d->ram_view = mono_view(d, IDC_RAM_VIEW, 0);
    d->ram_addr_label = label(d, "Write address");
    d->ram_addr = field(d, IDC_RAM_ADDR, d->mono);
    d->ram_val_label = label(d, "value");
    d->ram_val = field(d, IDC_RAM_VAL, d->mono);

    /* Disassembly */
    d->follow_pc = checkbox(d, "Follow PC", IDC_FOLLOW_PC);
    SendMessageA(d->follow_pc, BM_SETCHECK, BST_CHECKED, 0);
    d->jump_label = label(d, "Jump to");
    d->jump = field(d, IDC_JUMP, d->mono);
    d->disasm_hint = label(d, "Click a line to toggle its breakpoint; scroll to browse");
    d->disasm = child(d, "EDIT", "", WS_BORDER | ES_MULTILINE | ES_READONLY | ES_NOHIDESEL,
                      IDC_DISASM, d->mono);
    g_disasm_proc = (WNDPROC)SetWindowLongPtrA(d->disasm, GWLP_WNDPROC, (LONG_PTR)disasm_subclass);

    /* PPU */
    d->ppu_text = mono_view(d, IDC_PPU_TEXT, 1);
    d->ppu_view_label = label(d, "View");
    d->ppu_view = combo(d, IDC_PPU_VIEW, views, 4);
    d->ppu_pal_label = label(d, "Palette");
    d->ppu_palette = combo(d, IDC_PPU_PALETTE, pals, 8);
    d->ppu_pic = child(d, "NESDbgPixels", "", 0, IDC_PPU_PIC, d->ui);
    d->oam = mono_view(d, IDC_OAM, 0);

    /* APU & Input */
    d->apu = mono_view(d, IDC_APU, 1);

    /* Breakpoints */
    d->bp_list = child(d, "LISTBOX", "", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP, IDC_BP_LIST, d->mono);
    d->bp_type = combo(d, IDC_BP_TYPE, bp_types, 4);
    d->bp_start_label = label(d, "Address");
    d->bp_start = field(d, IDC_BP_START, d->mono);
    d->bp_end_label = label(d, "to");
    d->bp_end = field(d, IDC_BP_END, d->mono);
    d->bp_cond_label = label(d, "if");
    d->bp_cond = field(d, IDC_BP_COND, d->mono);
    SendMessageA(d->bp_cond, EM_SETCUEBANNER, TRUE, (LPARAM)L"optional condition, e.g. a == $FF");
    d->bp_add = button(d, "Add", IDC_BP_ADD);
    d->bp_remove = button(d, "Remove", IDC_BP_REMOVE);
    d->bp_enable = button(d, "Enable/Disable", IDC_BP_ENABLE);
    d->bp_clear = button(d, "Clear all", IDC_BP_CLEAR);

    /* Cart */
    d->cart = mono_view(d, IDC_CART, 1);
}

static void show_page(debugger *d, int page)
{
    HWND prompt[] = { d->prompt_out, d->prompt_in, d->load_symbols, d->save };
    HWND cpu[] = { d->cycles, d->ram_label, d->ram_view, d->ram_addr_label, d->ram_addr, d->ram_val_label, d->ram_val };
    HWND dis[] = { d->follow_pc, d->jump_label, d->jump, d->disasm_hint, d->disasm };
    HWND ppu[] = { d->ppu_text, d->ppu_view_label, d->ppu_view, d->ppu_pal_label, d->ppu_palette, d->ppu_pic, d->oam };
    HWND brk[] = { d->bp_list, d->bp_type, d->bp_start_label, d->bp_start, d->bp_end_label, d->bp_end,
                   d->bp_cond_label, d->bp_cond, d->bp_add, d->bp_remove, d->bp_enable, d->bp_clear };
    size_t i;
    d->page = page;
#define SHOW(arr, p) for (i = 0; i < sizeof(arr) / sizeof((arr)[0]); i++) ShowWindow((arr)[i], page == (p) ? SW_SHOW : SW_HIDE)
    SHOW(prompt, PAGE_PROMPT);
    SHOW(cpu, PAGE_CPU);
    for (i = 0; i < 6; i++) {
        ShowWindow(d->reg_label[i], page == PAGE_CPU ? SW_SHOW : SW_HIDE);
        ShowWindow(d->reg_edit[i], page == PAGE_CPU ? SW_SHOW : SW_HIDE);
        ShowWindow(d->flag[i], page == PAGE_CPU ? SW_SHOW : SW_HIDE);
    }
    SHOW(dis, PAGE_DISASM);
    SHOW(ppu, PAGE_PPU);
    ShowWindow(d->apu, page == PAGE_APU ? SW_SHOW : SW_HIDE);
    SHOW(brk, PAGE_BREAKS);
    ShowWindow(d->cart, page == PAGE_CART ? SW_SHOW : SW_HIDE);
#undef SHOW
}

static void layout(debugger *d)
{
    RECT client, page;
    int y = 8, bx = 8, px, py, pw, ph, i;

    GetClientRect(d->hwnd, &client);

    MoveWindow(d->run_btn, bx, y, 96, 26, TRUE);     bx += 102;
    MoveWindow(d->step_btn, bx, y, 90, 26, TRUE);    bx += 96;
    MoveWindow(d->over_btn, bx, y, 90, 26, TRUE);    bx += 96;
    MoveWindow(d->out_btn, bx, y, 110, 26, TRUE);    bx += 116;
    MoveWindow(d->scan_btn, bx, y, 90, 26, TRUE);    bx += 96;
    MoveWindow(d->frame_btn, bx, y, 80, 26, TRUE);   bx += 86;
    MoveWindow(d->status, bx, y + 5, client.right - bx - 8 > 40 ? client.right - bx - 8 : 40, 20, TRUE);

    MoveWindow(d->tabs, 8, 40, client.right - 16, client.bottom - 48, TRUE);
    page = (RECT){ 8, 40, client.right - 8, client.bottom - 8 };
    SendMessageA(d->tabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&page);
    px = page.left + 4; py = page.top + 4;
    pw = page.right - page.left - 8; ph = page.bottom - page.top - 8;
    if (pw < 200) pw = 200;
    if (ph < 200) ph = 200;

    /* Prompt */
    MoveWindow(d->prompt_out, px, py, pw, ph - 32, TRUE);
    MoveWindow(d->prompt_in, px, py + ph - 26, pw - 236, 24, TRUE);
    MoveWindow(d->load_symbols, px + pw - 228, py + ph - 27, 120, 26, TRUE);
    MoveWindow(d->save, px + pw - 102, py + ph - 27, 102, 26, TRUE);

    /* CPU & RAM */
    {
        int x = px, ry = py;
        for (i = 0; i < 6; i++) {
            MoveWindow(d->reg_label[i], x, ry + 4, 24, 18, TRUE); x += 26;
            MoveWindow(d->reg_edit[i], x, ry, 62, 24, TRUE); x += 74;
        }
        ry += 32;
        x = px;
        for (i = 0; i < 6; i++) { MoveWindow(d->flag[i], x, ry, 40, 22, TRUE); x += 44; }
        MoveWindow(d->cycles, x + 8, ry + 3, pw - (x - px) - 8, 18, TRUE);
        ry += 30;
        MoveWindow(d->ram_label, px, ry, pw, 18, TRUE); ry += 20;
        MoveWindow(d->ram_view, px, ry, pw, ph - (ry - py) - 34, TRUE);
        ry = py + ph - 26;
        MoveWindow(d->ram_addr_label, px, ry + 4, 90, 18, TRUE);
        MoveWindow(d->ram_addr, px + 94, ry, 80, 24, TRUE);
        MoveWindow(d->ram_val_label, px + 184, ry + 4, 40, 18, TRUE);
        MoveWindow(d->ram_val, px + 226, ry, 60, 24, TRUE);
    }

    /* Disassembly */
    MoveWindow(d->follow_pc, px, py + 2, 90, 22, TRUE);
    MoveWindow(d->jump_label, px + 96, py + 4, 50, 18, TRUE);
    MoveWindow(d->jump, px + 148, py, 150, 24, TRUE);
    MoveWindow(d->disasm_hint, px + 308, py + 4, pw - 308 > 40 ? pw - 308 : 40, 18, TRUE);
    MoveWindow(d->disasm, px, py + 30, pw, ph - 30, TRUE);

    /* PPU: registers and OAM left, the picture right */
    {
        int lw = pw * 42 / 100, rx = px + lw + 10, rw = pw - lw - 10;
        int th = ph * 45 / 100;
        MoveWindow(d->ppu_text, px, py, lw, th, TRUE);
        MoveWindow(d->oam, px, py + th + 6, lw, ph - th - 6, TRUE);
        MoveWindow(d->ppu_view_label, rx, py + 4, 40, 18, TRUE);
        MoveWindow(d->ppu_view, rx + 42, py, 120, 200, TRUE);
        MoveWindow(d->ppu_pal_label, rx + 172, py + 4, 46, 18, TRUE);
        MoveWindow(d->ppu_palette, rx + 220, py, 100, 200, TRUE);
        MoveWindow(d->ppu_pic, rx, py + 30, rw, ph - 30, TRUE);
    }

    /* APU & Input */
    MoveWindow(d->apu, px, py, pw, ph, TRUE);

    /* Breakpoints */
    {
        int ry = py + ph - 62;
        MoveWindow(d->bp_list, px, py, pw, ry - py - 6, TRUE);
        MoveWindow(d->bp_type, px, ry, 100, 200, TRUE);
        MoveWindow(d->bp_start_label, px + 108, ry + 4, 50, 18, TRUE);
        MoveWindow(d->bp_start, px + 160, ry, 90, 24, TRUE);
        MoveWindow(d->bp_end_label, px + 256, ry + 4, 18, 18, TRUE);
        MoveWindow(d->bp_end, px + 276, ry, 90, 24, TRUE);
        MoveWindow(d->bp_cond_label, px + 374, ry + 4, 14, 18, TRUE);
        MoveWindow(d->bp_cond, px + 390, ry, pw - 390 - 70 > 80 ? pw - 390 - 70 : 80, 24, TRUE);
        MoveWindow(d->bp_add, px + pw - 64, ry - 1, 64, 26, TRUE);
        ry += 32;
        MoveWindow(d->bp_remove, px, ry, 90, 26, TRUE);
        MoveWindow(d->bp_enable, px + 96, ry, 120, 26, TRUE);
        MoveWindow(d->bp_clear, px + 222, ry, 90, 26, TRUE);
    }

    /* Cart */
    MoveWindow(d->cart, px, py, pw, ph, TRUE);
}

/* ---- attach / detach -------------------------------------------------------------- */

static void attach(debugger *d)
{
    nesdebug_attach(d->dbg);
    d->seen_gen = nesdebug_generation(d->dbg);
    d->was_stopped = 1;
    refresh_all(d);
    SetTimer(d->hwnd, TIMER_REFRESH, 100, NULL);
}

static void detach_and_hide(debugger *d)
{
    KillTimer(d->hwnd, TIMER_REFRESH);
    nesdebug_detach(d->dbg);
    ShowWindow(d->hwnd, SW_HIDE);
}

/* ---- window proc -------------------------------------------------------------------- */

static void draw_run_button(debugger *d, const DRAWITEMSTRUCT *di)
{
    char text[32];
    const int stopped = nesdebug_is_stopped(d->dbg);
    RECT r = di->rcItem;
    if (stopped) {
        FillRect(di->hDC, &r, d->accent);
        FrameRect(di->hDC, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(di->hDC, RGB(255, 255, 255));
    } else {
        DrawFrameControl(di->hDC, &r, DFC_BUTTON, DFCS_BUTTONPUSH | ((di->itemState & ODS_SELECTED) ? DFCS_PUSHED : 0));
        SetTextColor(di->hDC, GetSysColor(COLOR_BTNTEXT));
    }
    SetBkMode(di->hDC, TRANSPARENT);
    GetWindowTextA(di->hwndItem, text, sizeof text);
    DrawTextA(di->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (di->itemState & ODS_FOCUS) {
        InflateRect(&r, -3, -3);
        DrawFocusRect(di->hDC, &r);
    }
}

static LRESULT CALLBACK dbg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (!d || d->hwnd != hwnd) return DefWindowProcA(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_SIZE:
        layout(d);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_REFRESH && IsWindowVisible(hwnd)) {
            const unsigned gen = nesdebug_generation(d->dbg);
            const int stopped = nesdebug_is_stopped(d->dbg);
            if (gen != d->seen_gen || stopped != d->was_stopped) {
                d->seen_gen = gen;
                d->was_stopped = stopped;
                refresh_all(d);
            } else if (!stopped && ++d->running_ticks >= 5) {
                d->running_ticks = 0;
                refresh_status(d);
                if (d->page == PAGE_CPU) { refresh_cpu(d); refresh_ram(d); }
                else if (d->page == PAGE_DISASM) refresh_disasm(d);
                else if (d->page == PAGE_PPU) refresh_ppu(d);
                else if (d->page == PAGE_APU) refresh_apu(d);
                else if (d->page == PAGE_CART) refresh_cart(d);
            }
        }
        return 0;
    case WM_DBG_ACCEPT:
        on_accept(d, (int)wp);
        return 0;
    case WM_DBG_TAB:
        complete_prompt(d);
        return 0;
    case WM_NOTIFY:
        if (((LPNMHDR)lp)->code == (UINT)TCN_SELCHANGE) {
            show_page(d, (int)SendMessageA(d->tabs, TCM_GETCURSEL, 0, 0));
            layout(d);
            return 0;
        }
        break;
    case WM_DRAWITEM:
        if (wp == IDC_RUN) { draw_run_button(d, (const DRAWITEMSTRUCT *)lp); return TRUE; }
        break;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == d->status || (HWND)lp == d->cycles || (HWND)lp == d->disasm_hint) {
            SetTextColor((HDC)wp, GetSysColor(COLOR_GRAYTEXT));
            SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id >= IDC_FLAG0 && id <= IDC_FLAG_LAST && HIWORD(wp) == BN_CLICKED) {
            static const int flag_ids[6] = { NES_FLAG_N, NES_FLAG_V, NES_FLAG_D, NES_FLAG_I, NES_FLAG_Z, NES_FLAG_C };
            if (nesdebug_is_stopped(d->dbg))
                nesdebug_cpu_set(d->dbg, flag_ids[id - IDC_FLAG0],
                                 SendMessageA(d->flag[id - IDC_FLAG0], BM_GETCHECK, 0, 0) == BST_CHECKED);
            refresh_cpu(d);
            return 0;
        }
        if (id >= IDC_SAVE_KIND0 && id <= IDC_SAVE_KIND_LAST) { save_file(d, id - IDC_SAVE_KIND0); return 0; }
        switch (id) {
        case IDC_RUN: toggle_run(d); return 0;
        case IDC_STEP: nesdebug_step(d->dbg); refresh_all(d); return 0;
        case IDC_OVER: nesdebug_step_over(d->dbg); refresh_all(d); return 0;
        case IDC_OUT: nesdebug_step_out(d->dbg); refresh_all(d); return 0;
        case IDC_SCAN: nesdebug_scanline(d->dbg, 1); refresh_all(d); return 0;
        case IDC_FRAME: nesdebug_frame(d->dbg, 1); refresh_all(d); return 0;
        case IDC_LOAD_SYMBOLS: load_symbols(d); return 0;
        case IDC_SAVE: save_menu(d); return 0;
        case IDC_FOLLOW_PC: refresh_disasm(d); return 0;
        case IDC_PPU_VIEW:
        case IDC_PPU_PALETTE:
            if (HIWORD(wp) == CBN_SELCHANGE) refresh_ppu(d);
            return 0;
        case IDC_BP_ADD: add_breakpoint(d); return 0;
        case IDC_BP_REMOVE:
        case IDC_BP_ENABLE: {
            int sel = (int)SendMessageA(d->bp_list, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < d->nbps) {
                if (id == IDC_BP_REMOVE) nesdebug_breakpoint_remove(d->dbg, d->bps[sel].id);
                else nesdebug_breakpoint_enable(d->dbg, d->bps[sel].id, !d->bps[sel].enabled);
                refresh_bps(d);
                refresh_disasm(d);
            }
            return 0;
        }
        case IDC_BP_LIST:
            if (HIWORD(wp) == LBN_DBLCLK) {
                int sel = (int)SendMessageA(d->bp_list, LB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < d->nbps) {
                    char a[16];
                    snprintf(a, sizeof a, "$%04X", d->bps[sel].start);
                    TabCtrl_SetCurSel(d->tabs, PAGE_DISASM);
                    show_page(d, PAGE_DISASM);
                    layout(d);
                    jump_to(d, a);
                }
            }
            return 0;
        case IDC_BP_CLEAR:
            nesdebug_breakpoint_clear(d->dbg);
            refresh_bps(d);
            refresh_disasm(d);
            return 0;
        case IDCANCEL:
            detach_and_hide(d);
            return 0;
        default: break;
        }
        break;
    }
    case WM_CLOSE:
        /* Hide and let the machine run; F12 brings it back, stopped. */
        detach_and_hide(d);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_REFRESH);
        DeleteObject(d->mono);
        DeleteObject(d->accent);
        DestroyAcceleratorTable(d->accel);
        free(d->pic_px);
        free(d);
        g_dbg = NULL;
        return 0;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- entry points ---------------------------------------------------------------------- */

void nes_debugger_show(HWND parent, nessession *session)
{
    HINSTANCE inst;
    WNDCLASSA wc;
    debugger *d;
    ACCEL accels[6];
    const char *tab;

    if (g_dbg) {
        if (!IsWindowVisible(g_dbg->hwnd)) {
            ShowWindow(g_dbg->hwnd, SW_SHOW);
            attach(g_dbg);
        } else {
            nesdebug_stop(g_dbg->dbg);
            refresh_all(g_dbg);
        }
        SetForegroundWindow(g_dbg->hwnd);
        return;
    }

    inst = (HINSTANCE)GetWindowLongPtrA(parent, GWLP_HINSTANCE);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = dbg_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "NESDebuggerWindow";
    RegisterClassA(&wc);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = pic_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "NESDbgPixels";
    RegisterClassA(&wc);

    d = calloc(1, sizeof *d);
    if (!d) return;
    d->session = session;
    d->dbg = nessession_debugger(session);
    d->pic_px = calloc(NESDEBUG_VIEW_MAX_PIXELS, sizeof *d->pic_px);
    g_dbg = d;

    d->hwnd = CreateWindowExA(0, "NESDebuggerWindow", "Debugger", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1180, 820, NULL, NULL, inst, NULL);
    if (!d->hwnd) { free(d->pic_px); free(d); g_dbg = NULL; return; }

    d->ui = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    d->mono = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    d->accent = CreateSolidBrush(RGB((NESSESSION_ACCENT_RGB >> 16) & 0xff, (NESSESSION_ACCENT_RGB >> 8) & 0xff,
                                     NESSESSION_ACCENT_RGB & 0xff));

    build_controls(d);

    /* F5/F7/F8/Shift+F8/F12 work wherever the focus is inside the window. */
    accels[0].fVirt = FVIRTKEY;          accels[0].key = VK_F5;  accels[0].cmd = IDC_RUN;
    accels[1].fVirt = FVIRTKEY;          accels[1].key = VK_F7;  accels[1].cmd = IDC_STEP;
    accels[2].fVirt = FVIRTKEY;          accels[2].key = VK_F8;  accels[2].cmd = IDC_OVER;
    accels[3].fVirt = FVIRTKEY | FSHIFT; accels[3].key = VK_F8;  accels[3].cmd = IDC_OUT;
    accels[4].fVirt = FVIRTKEY;          accels[4].key = VK_F12; accels[4].cmd = IDCANCEL;
    accels[5].fVirt = FVIRTKEY | FSHIFT; accels[5].key = VK_F7;  accels[5].cmd = IDC_FRAME;
    d->accel = CreateAcceleratorTableA(accels, 6);

    tab = getenv("NES_DEBUGGER_TAB");
    d->page = PAGE_PROMPT;
    if (tab && *tab) {
        int t = atoi(tab);
        if (t >= 0 && t < PAGE_COUNT) d->page = t;
    }
    SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)d->page, 0);
    show_page(d, d->page);
    layout(d);

    ShowWindow(d->hwnd, SW_SHOW);
    attach(d);
}

int nes_debugger_pretranslate(MSG *msg)
{
    if (!g_dbg || !g_dbg->accel) return 0;
    if (msg->hwnd != g_dbg->hwnd && !IsChild(g_dbg->hwnd, msg->hwnd)) return 0;
    if (msg->message == WM_KEYDOWN && msg->wParam == VK_F12) { detach_and_hide(g_dbg); return 1; }
    return TranslateAcceleratorA(g_dbg->hwnd, g_dbg->accel, msg) ? 1 : 0;
}

int nes_debugger_visible(void)
{
    return g_dbg && IsWindowVisible(g_dbg->hwnd);
}

void nes_debugger_toggle(HWND parent, nessession *session)
{
    if (nes_debugger_visible()) detach_and_hide(g_dbg);
    else nes_debugger_show(parent, session);
}
