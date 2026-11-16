/*
 * The debugger's Video tab (Win32).
 *
 * Everything that decides what to show lives in the core (nesvidtab.h): this file is
 * controls and a GDI picture. The picture is the controller's cached 0xFFRRGGBB buffer drawn
 * 1:1 with StretchDIBits (BI_RGB ignores the alpha byte), double buffered; a mouse move
 * redraws only the marks. Only the section showing is filled in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "dbg_video.h"

#include <commctrl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "accent.h"
#include "nessession.h"
#include "nesdebug.h"
#include "nesvidcap.h"
#include "nesvidtab.h"

#define PAGE_CLASS L"FujiNetGoNESVideoPage"
#define PIC_CLASS L"FujiNetGoNESVideoPic"
#define COL_CLASS L"FujiNetGoNESVideoColours"

#define ACCENT NES_ACCENT_COLORREF                  /* the icon blue: fills and outlines on light */
#define ACCENT_INK NES_ACCENT_INK_COLORREF          /* ink on the accent fill */
#define ACCENT_LIGHT NES_ACCENT_LIGHT_COLORREF      /* accent lines and text on a dark ground */
#define MAX_OVERLAYS 16
#define PAL_COLS 4          /* the 32 palette RAM entries: a row a palette, 8 palettes of 4 */
#define PAL_ROWS 8

enum {
    IDV_VIEW = 3000, IDV_SOURCE, IDV_LABEL_SRC, IDV_ZOOM, IDV_BREAK, IDV_STATUS, IDV_NOTE, IDV_SEC, IDV_PIXEL, IDV_UNPIN,
    IDV_REGS, IDV_LINES, IDV_BEAM, IDV_TIMING, IDV_PIC, IDV_COLS, IDV_LABEL_VIEW, IDV_LABEL_ZOOM,
    IDV_PAL, IDV_LABEL_PAL, IDV_SPRITES, IDV_MEMORY,
    IDV_OV0 = 3200
};

#define MAX_ZOOMS 16
#define SPRITE_ROWS 64

typedef struct {
    unsigned char *hot;
    int nhot;
    uint64_t sig;
    int have_sig;
} listfill;

typedef struct {
    HWND page, view, source, zoom, brk, status, note, pic, sec;
    HWND overlay_box[MAX_OVERLAYS];
    unsigned ov_bit[MAX_OVERLAYS];
    int nov;
    HWND label_view, label_zoom, label_src, label_pal, palette;
    HWND pixel, unpin, memory, regs, lines, cols, beam, timing, sprites;
    nessession *session;
    HFONT ui_font, mono;
    int dpi;
    nesvid_tab *tab;
    int updating;
    int scroll_x, scroll_y;
    int tracking;
    int pal_hover, col_tracking;
    int zoom_val[MAX_ZOOMS], nzoom;
    listfill reg_fill, line_fill;
    uint64_t sprite_sig;
    int have_sprite_sig;
    BITMAPINFO bmi;
} vidwin;

#define scaled(win, num) MulDiv((num), (win)->dpi, 96)

/* ---- text ------------------------------------------------------------------------ */

/* UTF-8 to UTF-16; '\n' becomes CR LF (multi-line controls want it) */
static wchar_t *wide(const char *utf8)
{
    int num = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0), idx;
    wchar_t *wtext = malloc(((size_t)num + 1) * sizeof *wtext), *outbuf, *dest;
    if (!wtext) return NULL;
    outbuf = malloc(((size_t)num * 2 + 2) * sizeof *wtext);
    if (!outbuf) {
        free(wtext);
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wtext, num);
    dest = outbuf;
    for (idx = 0; wtext[idx]; idx++) {
        if (wtext[idx] == L'\n') *dest++ = L'\r';
        *dest++ = wtext[idx];
    }
    *dest = 0;
    free(wtext);
    return outbuf;
}

/* a control's text, set only when it changed (no flicker on the live refresh) */
static void set_text(HWND hwnd, const char *utf8)
{
    wchar_t *wtext = wide(utf8), cur[2048];
    if (!wtext) return;
    GetWindowTextW(hwnd, cur, 2048);
    if (wcscmp(cur, wtext) != 0) SetWindowTextW(hwnd, wtext);
    free(wtext);
}

static uint64_t fnv(uint64_t hash, const void *data, size_t size)
{
    const unsigned char *bytes = data;
    size_t idx;
    for (idx = 0; idx < size; idx++) hash = (hash ^ bytes[idx]) * 1099511628211ull;
    return hash;
}

/* ---- the picture --------------------------------------------------------------------- */

static void pic_update_scroll(vidwin *win)
{
    RECT bounds;
    SCROLLINFO scroll_info;
    int pic_width = 0, pic_height = 0;
    GetClientRect(win->pic, &bounds);
    nesvid_tab_picture(win->tab, &pic_width, &pic_height);
    memset(&scroll_info, 0, sizeof scroll_info);
    scroll_info.cbSize = sizeof scroll_info;
    scroll_info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    scroll_info.nMin = 0;
    scroll_info.nMax = pic_width > 0 ? pic_width - 1 : 0;
    scroll_info.nPage = (UINT)bounds.right;
    if (win->scroll_x > pic_width - bounds.right) win->scroll_x = pic_width > bounds.right ? pic_width - bounds.right : 0;
    scroll_info.nPos = win->scroll_x;
    SetScrollInfo(win->pic, SB_HORZ, &scroll_info, TRUE);
    scroll_info.nMax = pic_height > 0 ? pic_height - 1 : 0;
    scroll_info.nPage = (UINT)bounds.bottom;
    if (win->scroll_y > pic_height - bounds.bottom) win->scroll_y = pic_height > bounds.bottom ? pic_height - bounds.bottom : 0;
    scroll_info.nPos = win->scroll_y;
    SetScrollInfo(win->pic, SB_VERT, &scroll_info, TRUE);
}

static void pic_paint(vidwin *win, HWND hwnd)
{
    PAINTSTRUCT paint;
    HDC device = BeginPaint(hwnd, &paint), mem;
    RECT bounds;
    HBITMAP bmp, old;
    int pic_width = 0, pic_height = 0, marks, hover[4] = { 0 }, pin[4] = { 0 };
    const uint32_t *pixels;
    HBRUSH back_brush = CreateSolidBrush(RGB(18, 18, 24));
    GetClientRect(hwnd, &bounds);
    mem = CreateCompatibleDC(device);
    bmp = CreateCompatibleBitmap(device, bounds.right > 0 ? bounds.right : 1, bounds.bottom > 0 ? bounds.bottom : 1);
    old = SelectObject(mem, bmp);
    FillRect(mem, &bounds, back_brush);
    pixels = nesvid_tab_picture(win->tab, &pic_width, &pic_height);
    if (!pixels) {
        const char *why = nesvid_tab_why(win->tab);
        wchar_t *wtext = wide(why && *why ? why : "Nothing to show");
        RECT text_rect = bounds;
        InflateRect(&text_rect, -scaled(win, 10), -scaled(win, 10));
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(140, 140, 148));
        SelectObject(mem, win->ui_font);
        if (wtext) DrawTextW(mem, wtext, -1, &text_rect, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        free(wtext);
    } else {
        HPEN pen_h, pen_p, pen_k, oldpen;
        HGDIOBJ oldbr;
        win->bmi.bmiHeader.biWidth = pic_width;      /* the buffer is pw words per row, whatever the view */
        win->bmi.bmiHeader.biHeight = -pic_height;    /* top-down */
        SetStretchBltMode(mem, COLORONCOLOR);
        StretchDIBits(mem, -win->scroll_x, -win->scroll_y, pic_width, pic_height, 0, 0, pic_width, pic_height, pixels, &win->bmi, DIB_RGB_COLORS, SRCCOPY);
        marks = nesvid_tab_marks(win->tab, hover, pin);
        oldbr = SelectObject(mem, GetStockObject(NULL_BRUSH));
        pen_h = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
        pen_p = CreatePen(PS_SOLID, 2, ACCENT_LIGHT);
        pen_k = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
        if (marks & 1) {            /* hover: a white box just outside the pixel, over a dark one */
            oldpen = SelectObject(mem, pen_k);
            Rectangle(mem, hover[0] - win->scroll_x - 2, hover[1] - win->scroll_y - 2, hover[0] - win->scroll_x + hover[2] + 2,
                      hover[1] - win->scroll_y + hover[3] + 2);
            SelectObject(mem, pen_h);
            Rectangle(mem, hover[0] - win->scroll_x - 1, hover[1] - win->scroll_y - 1, hover[0] - win->scroll_x + hover[2] + 1,
                      hover[1] - win->scroll_y + hover[3] + 1);
            SelectObject(mem, oldpen);
        }
        if (marks & 2) {            /* pin: a thick yellow box */
            oldpen = SelectObject(mem, pen_k);
            Rectangle(mem, pin[0] - win->scroll_x - 3, pin[1] - win->scroll_y - 3, pin[0] - win->scroll_x + pin[2] + 3,
                      pin[1] - win->scroll_y + pin[3] + 3);
            SelectObject(mem, pen_p);
            Rectangle(mem, pin[0] - win->scroll_x - 1, pin[1] - win->scroll_y - 1, pin[0] - win->scroll_x + pin[2] + 1,
                      pin[1] - win->scroll_y + pin[3] + 1);
            SelectObject(mem, oldpen);
        }
        SelectObject(mem, oldbr);
        DeleteObject(pen_h);
        DeleteObject(pen_p);
        DeleteObject(pen_k);
    }
    BitBlt(device, 0, 0, bounds.right, bounds.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    DeleteObject(back_brush);
    EndPaint(hwnd, &paint);
}

/* Source applies to the Decoded view, Palette to its Patterns source */
static void enable_choosers(vidwin *win)
{
    const int decoded = nesvid_tab_view(win->tab) == NESVID_V_DECODED;
    EnableWindow(win->source, decoded);
    EnableWindow(win->palette, decoded && nesvid_tab_source(win->tab) == NESVID_SRC_PATTERNS);
}

static void update_pixel(vidwin *win)
{
    const nesvid_pixel *pixel = nesvid_tab_pixel(win->tab);
    set_text(win->pixel, pixel && pixel->text[0] ? pixel->text
                                       : "Move over the picture to inspect a pixel.\n"
                                         "Click to pin it; click again to unpin.\n"
                                         "Double-click to break on writes to what drew it.");
    if (win->cols) InvalidateRect(win->cols, NULL, FALSE);
    if (win->brk) EnableWindow(win->brk, nesvid_tab_can_break(win->tab));
    if (win->memory) EnableWindow(win->memory, pixel && pixel->mem_space >= 0);
}

/* The Memory tab at what the inspected pixel is made from (the debugger window opens it). */
static void show_in_memory(vidwin *win)
{
    const nesvid_pixel *pixel = nesvid_tab_pixel(win->tab);
    HWND main = (HWND)GetWindowLongPtrW(GetParent(win->page), GWLP_USERDATA);
    if (!pixel || pixel->mem_space < 0 || !main) return;
    PostMessageW(main, DBG_VIDEO_SHOW_MEMORY, (WPARAM)pixel->mem_space, (LPARAM)pixel->mem_addr);
}

static void set_note(vidwin *win, const char *text)
{
    set_text(win->note, text);
}

/* Break on writes to what the inspected pixel is made from, and say what was done. */
static void break_on_pixel(vidwin *win)
{
    char msg[160];
    int ctl_id = nesvid_tab_break_on_pixel(win->tab);
    if (ctl_id >= 0) snprintf(msg, sizeof msg, "Break on writes added (see Breakpoints)");
    else snprintf(msg, sizeof msg, "Nothing to break on: pin or hover a tile, a sprite or a colour");
    set_note(win, msg);
}

static LRESULT CALLBACK pic_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    vidwin *win = (vidwin *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!win) return DefWindowProcW(hwnd, msg, wparam, lparam);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        pic_paint(win, hwnd);
        return 0;
    case WM_SIZE:
        pic_update_scroll(win);
        return 0;
    case WM_HSCROLL:
    case WM_VSCROLL: {
        SCROLLINFO scroll_info;
        int bar = msg == WM_HSCROLL ? SB_HORZ : SB_VERT, pos;
        memset(&scroll_info, 0, sizeof scroll_info);
        scroll_info.cbSize = sizeof scroll_info;
        scroll_info.fMask = SIF_ALL;
        GetScrollInfo(hwnd, bar, &scroll_info);
        pos = scroll_info.nPos;
        switch (LOWORD(wparam)) {
        case SB_LINELEFT: pos -= 16; break;
        case SB_LINERIGHT: pos += 16; break;
        case SB_PAGELEFT: pos -= (int)scroll_info.nPage; break;
        case SB_PAGERIGHT: pos += (int)scroll_info.nPage; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = scroll_info.nTrackPos; break;
        default: break;
        }
        if (pos < 0) pos = 0;
        if (bar == SB_HORZ) win->scroll_x = pos; else win->scroll_y = pos;
        pic_update_scroll(win);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL:
        win->scroll_y -= GET_WHEEL_DELTA_WPARAM(wparam) / 3;
        if (win->scroll_y < 0) win->scroll_y = 0;
        pic_update_scroll(win);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_MOUSEMOVE: {
        int pos_x = (short)LOWORD(lparam) + win->scroll_x, pos_y = (short)HIWORD(lparam) + win->scroll_y;
        if (!win->tracking) {
            TRACKMOUSEEVENT track;
            memset(&track, 0, sizeof track);
            track.cbSize = sizeof track;
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            TrackMouseEvent(&track);
            win->tracking = 1;
        }
        if (nesvid_tab_hover(win->tab, pos_x, pos_y)) {
            InvalidateRect(hwnd, NULL, FALSE);
            update_pixel(win);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        win->tracking = 0;
        if (nesvid_tab_hover(win->tab, -1, -1)) {
            InvalidateRect(hwnd, NULL, FALSE);
            update_pixel(win);
        }
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        nesvid_tab_hover(win->tab, (short)LOWORD(lparam) + win->scroll_x, (short)HIWORD(lparam) + win->scroll_y);
        nesvid_tab_pin(win->tab, (short)LOWORD(lparam) + win->scroll_x, (short)HIWORD(lparam) + win->scroll_y);
        InvalidateRect(hwnd, NULL, FALSE);
        update_pixel(win);
        return 0;
    case WM_LBUTTONDBLCLK:
        nesvid_tab_hover(win->tab, (short)LOWORD(lparam) + win->scroll_x, (short)HIWORD(lparam) + win->scroll_y);
        break_on_pixel(win);
        InvalidateRect(hwnd, NULL, FALSE);
        update_pixel(win);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/* ---- the Colours panel: the 32 palette entries as swatches and a description ---- */

#define PAL_MAX_CELL 64

/* the grid's cell size, from the panel's size: 4 x 8 swatches, squared */
static void col_metrics(vidwin *win, const RECT *bounds, int *cell, int *top)
{
    int cell_size = bounds->right / PAL_COLS - scaled(win, 6);
    int by_height = (bounds->bottom - scaled(win, 70)) / PAL_ROWS - scaled(win, 6);
    if (by_height < cell_size) cell_size = by_height;
    if (cell_size > scaled(win, PAL_MAX_CELL)) cell_size = scaled(win, PAL_MAX_CELL);
    if (cell_size < 10) cell_size = 10;
    *cell = cell_size;
    *top = 0;
}

/* "BG 2.3: #3EB849 (colour $1A), 120 pixels of the finished frame" */
static void swatch_describe(const nesvid_swatch *swatch, char *out, size_t cap)
{
    snprintf(out, cap, "%s: #%06X (colour $%02X)  %ld pixels of the finished frame%s%s%s", swatch->name, swatch->rgb & 0xFFFFFF, swatch->colour, swatch->uses,
             swatch->role[0] ? "\nUsed as: " : "", swatch->role,
             swatch->inspected ? "\nThe inspected pixel has it." : "");
}

static void col_paint(vidwin *win, HWND hwnd)
{
    PAINTSTRUCT paint;
    HDC device = BeginPaint(hwnd, &paint), mem;
    HBITMAP bmp, old;
    RECT bounds;
    const nesvid_swatch *swatches;
    int count = nesvid_tab_swatches(win->tab, &swatches);
    int cell, top, index;
    HFONT oldf;
    GetClientRect(hwnd, &bounds);
    mem = CreateCompatibleDC(device);
    bmp = CreateCompatibleBitmap(device, bounds.right > 0 ? bounds.right : 1, bounds.bottom > 0 ? bounds.bottom : 1);
    old = SelectObject(mem, bmp);
    FillRect(mem, &bounds, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(mem, TRANSPARENT);
    oldf = SelectObject(mem, win->mono);
    col_metrics(win, &bounds, &cell, &top);
    for (index = 0; index < count && index < PAL_COLS * PAL_ROWS; index++) {
        const nesvid_swatch *swatch = &swatches[index];
        RECT cell_rect;
        HBRUSH fill = CreateSolidBrush(RGB((swatch->rgb >> 16) & 255, (swatch->rgb >> 8) & 255, swatch->rgb & 255));
        cell_rect.left = scaled(win, 3) + (index % PAL_COLS) * (cell + scaled(win, 6));
        cell_rect.top = top + scaled(win, 3) + (index / PAL_COLS) * (cell + scaled(win, 6));
        cell_rect.right = cell_rect.left + cell;
        cell_rect.bottom = cell_rect.top + cell;
        FillRect(mem, &cell_rect, fill);
        DeleteObject(fill);
        if (swatch->inspected) {
            HBRUSH accent_brush = CreateSolidBrush(ACCENT), white_brush = CreateSolidBrush(RGB(255, 255, 255));
            RECT outline = cell_rect;
            InflateRect(&outline, 1, 1);
            FrameRect(mem, &outline, accent_brush);
            InflateRect(&outline, -1, -1);
            FrameRect(mem, &outline, white_brush);
            DeleteObject(accent_brush);
            DeleteObject(white_brush);
        }
        if (index == win->pal_hover) {
            HBRUSH edge = CreateSolidBrush(RGB(0, 0, 0));
            RECT outline = cell_rect;
            InflateRect(&outline, 2, 2);
            FrameRect(mem, &outline, edge);
            DeleteObject(edge);
        }
    }
    {
        char text[300];
        RECT text_rect = { 0, top + PAL_ROWS * (cell + scaled(win, 6)) + scaled(win, 12), bounds.right, bounds.bottom };
        wchar_t *wide_text;
        int shown = win->pal_hover;
        text[0] = 0;
        if (shown < 0)
            for (index = 0; index < count; index++)
                if (swatches[index].inspected) shown = index;
        if (shown >= 0 && shown < count) swatch_describe(&swatches[shown], text, sizeof text);
        else snprintf(text, sizeof text, "Palette RAM: four background palettes, then four sprite palettes. Hover a swatch for its use count and role.");
        wide_text = wide(text);
        SetTextColor(mem, GetSysColor(COLOR_WINDOWTEXT));
        if (wide_text) DrawTextW(mem, wide_text, -1, &text_rect, DT_WORDBREAK | DT_NOPREFIX);
        free(wide_text);
    }
    SelectObject(mem, oldf);
    BitBlt(device, 0, 0, bounds.right, bounds.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &paint);
}

/* the swatch under a point of the panel, or -1 */
static int col_hit(vidwin *win, HWND hwnd, int mouse_x, int mouse_y)
{
    RECT bounds;
    int cell, top, column, row, step;
    GetClientRect(hwnd, &bounds);
    col_metrics(win, &bounds, &cell, &top);
    step = cell + scaled(win, 6);
    if (mouse_y < top + scaled(win, 3) || mouse_x < scaled(win, 3)) return -1;
    column = (mouse_x - scaled(win, 3)) / step;
    row = (mouse_y - top - scaled(win, 3)) / step;
    if (column >= PAL_COLS || row >= PAL_ROWS) return -1;
    if ((mouse_x - scaled(win, 3)) % step >= cell || (mouse_y - top - scaled(win, 3)) % step >= cell) return -1;
    return row * PAL_COLS + column;
}

static LRESULT CALLBACK col_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    vidwin *win = (vidwin *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!win) return DefWindowProcW(hwnd, msg, wparam, lparam);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        col_paint(win, hwnd);
        return 0;
    case WM_MOUSEMOVE: {
        int index = col_hit(win, hwnd, (short)LOWORD(lparam), (short)HIWORD(lparam));
        if (!win->col_tracking) {
            TRACKMOUSEEVENT track;
            memset(&track, 0, sizeof track);
            track.cbSize = sizeof track;
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            TrackMouseEvent(&track);
            win->col_tracking = 1;
        }
        if (index != win->pal_hover) {
            win->pal_hover = index;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        win->col_tracking = 0;
        if (win->pal_hover != -1) {
            win->pal_hover = -1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/* ---- sections ----------------------------------------------------------------------- */

static void lv_cell(HWND list_view, int row, int col, const char *text)
{
    wchar_t *wtext = wide(text);
    LVITEMW item;
    memset(&item, 0, sizeof item);
    item.iSubItem = col;
    item.pszText = wtext ? wtext : L"";
    if (col == 0) {
        item.mask = LVIF_TEXT;
        item.iItem = row;
        SendMessageW(list_view, LVM_INSERTITEMW, 0, (LPARAM)&item);
    } else {
        SendMessageW(list_view, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&item);
    }
    free(wtext);
}

static void mark_hot(listfill *line_fill, int num, int idx, int hot)
{
    if (idx == 0) {
        free(line_fill->hot);
        line_fill->hot = calloc((size_t)(num > 0 ? num : 1), 1);
        line_fill->nhot = line_fill->hot ? num : 0;
    }
    if (line_fill->hot && idx < line_fill->nhot) line_fill->hot[idx] = (unsigned char)hot;
}

static void fill_regs(vidwin *win)
{
    const nesvid_reg *entry;
    int num = nesvid_tab_regs(win->tab, &entry), idx;
    uint64_t sig = 1469598103934665603ull;
    char cell[32];
    for (idx = 0; idx < num; idx++) {
        int hot = entry[idx].age < NESVID_REG_FADE;
        sig = fnv(sig, entry[idx].name, strlen(entry[idx].name));
        sig = fnv(sig, entry[idx].value, strlen(entry[idx].value));
        sig = fnv(sig, entry[idx].meaning, strlen(entry[idx].meaning));
        sig = fnv(sig, &entry[idx].writes, sizeof entry[idx].writes);
        sig = fnv(sig, &hot, sizeof hot);
    }
    if (win->reg_fill.have_sig && win->reg_fill.sig == sig) return;
    win->reg_fill.sig = sig;
    win->reg_fill.have_sig = 1;
    SendMessageW(win->regs, WM_SETREDRAW, FALSE, 0);
    SendMessageW(win->regs, LVM_DELETEALLITEMS, 0, 0);
    for (idx = 0; idx < num; idx++) {
        lv_cell(win->regs, idx, 0, entry[idx].name);
        lv_cell(win->regs, idx, 1, entry[idx].value);
        snprintf(cell, sizeof cell, "%d", entry[idx].writes);
        lv_cell(win->regs, idx, 2, cell);
        if (entry[idx].first_line >= 0) snprintf(cell, sizeof cell, "%d", entry[idx].first_line);
        else snprintf(cell, sizeof cell, "-");
        lv_cell(win->regs, idx, 3, cell);
        lv_cell(win->regs, idx, 4, entry[idx].meaning);
        mark_hot(&win->reg_fill, num, idx, entry[idx].age < NESVID_REG_FADE);
    }
    if (num == 0) mark_hot(&win->reg_fill, 0, 0, 0);
    SendMessageW(win->regs, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(win->regs, NULL, TRUE);
}

static void fill_lines(vidwin *win)
{
    const nesvid_band *entry;
    int num = nesvid_tab_lines(win->tab, &entry), idx;
    uint64_t sig = 1469598103934665603ull;
    for (idx = 0; idx < num; idx++) {
        sig = fnv(sig, &entry[idx], sizeof entry[idx]);
    }
    sig = fnv(sig, &num, sizeof num);
    if (win->line_fill.have_sig && win->line_fill.sig == sig) return;
    win->line_fill.sig = sig;
    win->line_fill.have_sig = 1;
    SendMessageW(win->lines, WM_SETREDRAW, FALSE, 0);
    SendMessageW(win->lines, LVM_DELETEALLITEMS, 0, 0);
    for (idx = 0; idx < num; idx++) {
        char text[200];
        snprintf(text, sizeof text, "%d-%d", entry[idx].first, entry[idx].last);
        lv_cell(win->lines, idx, 0, text);
        snprintf(text, sizeof text, "%d", entry[idx].scroll_x);
        lv_cell(win->lines, idx, 1, text);
        snprintf(text, sizeof text, "%d", entry[idx].scroll_y);
        lv_cell(win->lines, idx, 2, text);
        snprintf(text, sizeof text, "%02X", entry[idx].ctrl);
        lv_cell(win->lines, idx, 3, text);
        snprintf(text, sizeof text, "%02X", entry[idx].mask);
        lv_cell(win->lines, idx, 4, text);
        snprintf(text, sizeof text, "%d", entry[idx].fine_x);
        lv_cell(win->lines, idx, 5, text);
        snprintf(text, sizeof text, "%04X", entry[idx].vaddr);
        lv_cell(win->lines, idx, 6, text);
        snprintf(text, sizeof text, "%d", entry[idx].writes);
        lv_cell(win->lines, idx, 7, text);
        snprintf(text, sizeof text, "%d", entry[idx].mapper_writes);
        lv_cell(win->lines, idx, 8, text);
        mark_hot(&win->line_fill, num, idx, entry[idx].writes > 0);
    }
    if (num == 0) mark_hot(&win->line_fill, 0, 0, 0);
    SendMessageW(win->lines, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(win->lines, NULL, TRUE);
}

/* the Sprites section: the 64 OAM entries, one a line; rebuilt only when a line changed */
static void fill_sprites(vidwin *win)
{
    static char text[SPRITE_ROWS * 128];
    const nesvid_spriterow *rows;
    const int num = nesvid_tab_sprites(win->tab, &rows);
    uint64_t sig = 1469598103934665603ull;
    size_t len = 0;
    int idx;
    for (idx = 0; idx < num && idx < SPRITE_ROWS; idx++) {
        const int room = (int)(sizeof text - len);
        const int wrote = snprintf(text + len, (size_t)(room > 0 ? room : 0), "%s\n", rows[idx].text);
        if (wrote > 0 && wrote < room) len += (size_t)wrote;
    }
    text[len] = '\0';
    sig = fnv(sig, text, len);
    if (win->have_sprite_sig && win->sprite_sig == sig) return;
    win->sprite_sig = sig;
    win->have_sprite_sig = 1;
    if (num == 0) snprintf(text, sizeof text, "No machine");
    {
        const int first = (int)SendMessageW(win->sprites, EM_GETFIRSTVISIBLELINE, 0, 0);
        wchar_t *wtext = wide(text);
        if (!wtext) return;
        SendMessageW(win->sprites, WM_SETREDRAW, FALSE, 0);
        SetWindowTextW(win->sprites, wtext);
        SendMessageW(win->sprites, EM_LINESCROLL, 0, first);
        SendMessageW(win->sprites, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(win->sprites, NULL, TRUE);
        free(wtext);
    }
}


static void fill_timing(vidwin *win)
{
    const char *const *lines;
    int num = nesvid_tab_timing(win->tab, &lines), idx;
    char buf[2048];
    size_t len = 0;
    buf[0] = 0;
    for (idx = 0; idx < num && len < sizeof buf - 160; idx++) {
        const char *tab = strchr(lines[idx], '\t');
        if (tab) len += (size_t)snprintf(buf + len, sizeof buf - len, "%-14.*s %s\n", (int)(tab - lines[idx]), lines[idx], tab + 1);
        else len += (size_t)snprintf(buf + len, sizeof buf - len, "%s\n", lines[idx]);
    }
    if (!num) snprintf(buf, sizeof buf, "No machine");
    set_text(win->timing, buf);
    SendMessageW(win->beam, PBM_SETPOS, (WPARAM)nesvid_tab_beam_permille(win->tab), 0);
}

static int cur_section(vidwin *win)
{
    int sec_index = (int)SendMessageW(win->sec, TCM_GETCURSEL, 0, 0);
    return sec_index < 0 ? 0 : sec_index;
}

static void show_section(vidwin *win)
{
    int sec_index = cur_section(win);
    ShowWindow(win->pixel, sec_index == NESVID_S_PIXEL ? SW_SHOW : SW_HIDE);
    ShowWindow(win->unpin, sec_index == NESVID_S_PIXEL ? SW_SHOW : SW_HIDE);
    ShowWindow(win->memory, sec_index == NESVID_S_PIXEL ? SW_SHOW : SW_HIDE);
    ShowWindow(win->regs, sec_index == NESVID_S_REGISTERS ? SW_SHOW : SW_HIDE);
    ShowWindow(win->cols, sec_index == NESVID_S_COLOURS ? SW_SHOW : SW_HIDE);
    ShowWindow(win->lines, sec_index == NESVID_S_LINES ? SW_SHOW : SW_HIDE);
    ShowWindow(win->beam, sec_index == NESVID_S_TIMING ? SW_SHOW : SW_HIDE);
    ShowWindow(win->timing, sec_index == NESVID_S_TIMING ? SW_SHOW : SW_HIDE);
    ShowWindow(win->sprites, sec_index == NESVID_S_SPRITES ? SW_SHOW : SW_HIDE);
}

/* ---- refresh -------------------------------------------------------------------------- */

#define R_RESET 0x8000u         /* the view or zoom changed: scroll back to the corner */

static void refresh_view(vidwin *win, unsigned what)
{
    if (what & (NESVID_TAB_PICTURE | NESVID_TAB_OFF)) {
        RECT bounds;
        GetClientRect(win->pic, &bounds);
        if (bounds.right > 4 && bounds.bottom > 4) nesvid_tab_set_viewport(win->tab, bounds.right - 2, bounds.bottom - 2);
        if (what & R_RESET) win->scroll_x = win->scroll_y = 0;
        pic_update_scroll(win);
        InvalidateRect(win->pic, NULL, FALSE);
    }
    if (what & NESVID_TAB_TABLES) {
        set_text(win->status, nesvid_tab_status(win->tab));
        switch (cur_section(win)) {
        case NESVID_S_REGISTERS: fill_regs(win); break;
        case NESVID_S_COLOURS: InvalidateRect(win->cols, NULL, FALSE); break;
        case NESVID_S_LINES: fill_lines(win); break;
        case NESVID_S_TIMING: fill_timing(win); break;
        case NESVID_S_SPRITES: fill_sprites(win); break;
        default: break;
        }
        update_pixel(win);
    }
}

void dbg_video_refresh(HWND page, int force)
{
    vidwin *win = (vidwin *)GetWindowLongPtrW(page, GWLP_USERDATA);
    unsigned what;
    if (!win) return;
    what = nesvid_tab_refresh(win->tab, force);
    if (force) {
        what |= NESVID_TAB_PICTURE | NESVID_TAB_TABLES;
        win->reg_fill.have_sig = win->line_fill.have_sig = win->have_sprite_sig = 0;
    }
    refresh_view(win, what);
}

/* ---- layout --------------------------------------------------------------------------- */

static int text_w(vidwin *win, HFONT font, const wchar_t *sec_index)
{
    HDC device = GetDC(win->page);
    HGDIOBJ old = SelectObject(device, font);
    SIZE size;
    GetTextExtentPoint32W(device, sec_index, (int)wcslen(sec_index), &size);
    SelectObject(device, old);
    ReleaseDC(win->page, device);
    return size.cx;
}

static void layout(vidwin *win)
{
    RECT bounds, text_rect;
    int width, height, right_w, left_w, pos_x, pos_y, idx, row_h = scaled(win, 24), line_h = scaled(win, 18), gap = scaled(win, 6), mono_h;
    int right_x, tab_h, top;
    TEXTMETRICW metrics;
    HDC device;
    GetClientRect(win->page, &bounds);
    width = bounds.right;
    height = bounds.bottom;
    if (width < 100 || height < 100) return;
    device = GetDC(win->page);
    SelectObject(device, win->mono);
    mono_h = GetTextMetricsW(device, &metrics) ? metrics.tmHeight : scaled(win, 15);
    ReleaseDC(win->page, device);

    /* toolbar: View, Zoom, Break on writes; then the overlay checkboxes, flowing */
    pos_x = 0;
    pos_y = 0;
    MoveWindow(win->label_view, pos_x, pos_y + (row_h - line_h) / 2, scaled(win, 36), line_h, TRUE);
    pos_x += scaled(win, 36);
    MoveWindow(win->view, pos_x, pos_y, scaled(win, 110), scaled(win, 300), TRUE);
    pos_x += scaled(win, 110) + gap * 2;
    MoveWindow(win->label_src, pos_x, pos_y + (row_h - line_h) / 2, scaled(win, 50), line_h, TRUE);
    pos_x += scaled(win, 50);
    MoveWindow(win->source, pos_x, pos_y, scaled(win, 100), scaled(win, 300), TRUE);
    pos_x += scaled(win, 100) + gap * 2;
    MoveWindow(win->label_pal, pos_x, pos_y + (row_h - line_h) / 2, scaled(win, 50), line_h, TRUE);
    pos_x += scaled(win, 50);
    MoveWindow(win->palette, pos_x, pos_y, scaled(win, 100), scaled(win, 300), TRUE);
    pos_x += scaled(win, 100) + gap * 2;
    MoveWindow(win->label_zoom, pos_x, pos_y + (row_h - line_h) / 2, scaled(win, 40), line_h, TRUE);
    pos_x += scaled(win, 40);
    MoveWindow(win->zoom, pos_x, pos_y, scaled(win, 80), scaled(win, 300), TRUE);
    pos_x += scaled(win, 80) + gap * 3;
    {
        int button_w = text_w(win, win->ui_font, L"Break on writes") + scaled(win, 28);
        MoveWindow(win->brk, pos_x, pos_y - scaled(win, 1), button_w, row_h, TRUE);
    }
    pos_y += row_h + gap;
    pos_x = 0;
    for (idx = 0; idx < win->nov; idx++) {
        char dummy[1];
        wchar_t lbl[64];
        int button_w;
        (void)dummy;
        GetWindowTextW(win->overlay_box[idx], lbl, 64);
        button_w = text_w(win, win->ui_font, lbl) + scaled(win, 26);
        if (pos_x + button_w > width && pos_x > 0) {
            pos_x = 0;
            pos_y += row_h - scaled(win, 2);
        }
        MoveWindow(win->overlay_box[idx], pos_x, pos_y, button_w, row_h - scaled(win, 4), TRUE);
        pos_x += button_w + gap;
    }
    pos_y += row_h;
    MoveWindow(win->status, 0, pos_y, width, mono_h * 2 + scaled(win, 4), TRUE);
    pos_y += mono_h * 2 + scaled(win, 8);
    top = pos_y;

    /* the split: picture left, sections right */
    right_w = scaled(win, 480);
    if (right_w > width * 50 / 100) right_w = width * 48 / 100;
    left_w = width - right_w - gap;
    right_x = left_w + gap;
    MoveWindow(win->pic, 0, top, left_w, height - top - line_h - gap > 20 ? height - top - line_h - gap : 20, TRUE);
    MoveWindow(win->note, 0, height - line_h, width, line_h, TRUE);

    MoveWindow(win->sec, right_x, top, right_w, height - top - line_h - gap, TRUE);
    SetRect(&text_rect, 0, 0, right_w, height - top - line_h - gap);
    SendMessageW(win->sec, TCM_ADJUSTRECT, FALSE, (LPARAM)&text_rect);
    tab_h = text_rect.top;
    {
        int inner_x = right_x + scaled(win, 6), inner_y = top + tab_h + scaled(win, 6), inner_w = right_w - scaled(win, 12);
        int inner_h = (height - line_h - gap) - inner_y - scaled(win, 6);
        MoveWindow(win->pixel, inner_x, inner_y, inner_w, inner_h - scaled(win, 34) > 20 ? inner_h - scaled(win, 34) : 20, TRUE);
        MoveWindow(win->unpin, inner_x, inner_y + (inner_h > scaled(win, 60) ? inner_h - scaled(win, 28) : scaled(win, 32)), scaled(win, 80), scaled(win, 26), TRUE);
        MoveWindow(win->memory, inner_x + scaled(win, 88), inner_y + (inner_h > scaled(win, 60) ? inner_h - scaled(win, 28) : scaled(win, 32)), scaled(win, 130), scaled(win, 26), TRUE);
        MoveWindow(win->regs, inner_x, inner_y, inner_w, inner_h, TRUE);
        MoveWindow(win->cols, inner_x, inner_y, inner_w, inner_h, TRUE);
        MoveWindow(win->lines, inner_x, inner_y, inner_w, inner_h, TRUE);
        MoveWindow(win->beam, inner_x, inner_y, inner_w, scaled(win, 20), TRUE);
        MoveWindow(win->timing, inner_x, inner_y + scaled(win, 30), inner_w, inner_h - scaled(win, 30), TRUE);
        MoveWindow(win->sprites, inner_x, inner_y, inner_w, inner_h, TRUE);
        {
            static const int rw_[5] = { 100, 60, 56, 52, 520 };
            for (idx = 0; idx < 5; idx++) SendMessageW(win->regs, LVM_SETCOLUMNWIDTH, (WPARAM)idx, scaled(win, rw_[idx]));
        }
        {
            static const int lw_[9] = { 70, 70, 70, 74, 74, 56, 84, 64, 100 };
            for (idx = 0; idx < 9; idx++) SendMessageW(win->lines, LVM_SETCOLUMNWIDTH, (WPARAM)idx, scaled(win, lw_[idx]));
        }
    }
    pic_update_scroll(win);
}

/* ---- the page -------------------------------------------------------------------------- */

static HWND mk_in(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style, int ctl_id, HFONT font)
{
    HWND hwnd = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)ctl_id,
                             (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE), NULL);
    if (font) SendMessageW(hwnd, WM_SETFONT, (WPARAM)font, TRUE);
    return hwnd;
}

#define make_child(win, cls, text, style, ctl_id, font) mk_in((win)->page, cls, text, style, ctl_id, font)

static void on_overlay(vidwin *win)
{
    unsigned mask = 0;
    int idx;
    for (idx = 0; idx < win->nov; idx++)
        if (SendMessageW(win->overlay_box[idx], BM_GETCHECK, 0, 0) == BST_CHECKED) mask |= win->ov_bit[idx];
    nesvid_tab_set_overlays(win->tab, mask);
    refresh_view(win, NESVID_TAB_PICTURE | NESVID_TAB_TABLES);
}

static LRESULT CALLBACK page_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    vidwin *win = (vidwin *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!win) return DefWindowProcW(hwnd, msg, wparam, lparam);
    switch (msg) {
    case WM_SIZE:
        layout(win);
        refresh_view(win, NESVID_TAB_PICTURE);
        return 0;
    case WM_COMMAND: {
        int ctl_id = LOWORD(wparam), code = HIWORD(wparam);
        if (win->updating) return 0;
        if (ctl_id == IDV_VIEW && code == CBN_SELCHANGE) {
            nesvid_tab_set_view(win->tab, (int)SendMessageW(win->view, CB_GETCURSEL, 0, 0));
            dbg_video_refresh(hwnd, 1);
            win->scroll_x = win->scroll_y = 0;
            pic_update_scroll(win);
            enable_choosers(win);
        } else if (ctl_id == IDV_SOURCE && code == CBN_SELCHANGE) {
            nesvid_tab_set_source(win->tab, (int)SendMessageW(win->source, CB_GETCURSEL, 0, 0));
            dbg_video_refresh(hwnd, 1);
            win->scroll_x = win->scroll_y = 0;
            pic_update_scroll(win);
            enable_choosers(win);
        } else if (ctl_id == IDV_PAL && code == CBN_SELCHANGE) {
            nesvid_tab_set_palette(win->tab, (int)SendMessageW(win->palette, CB_GETCURSEL, 0, 0));
            dbg_video_refresh(hwnd, 1);
        } else if (ctl_id == IDV_ZOOM && code == CBN_SELCHANGE) {
            int idx = (int)SendMessageW(win->zoom, CB_GETCURSEL, 0, 0);
            if (idx >= 0 && idx < win->nzoom) nesvid_tab_set_zoom(win->tab, win->zoom_val[idx]);
            refresh_view(win, NESVID_TAB_PICTURE | R_RESET);
        } else if (ctl_id >= IDV_OV0 && ctl_id < IDV_OV0 + win->nov && code == BN_CLICKED) {
            on_overlay(win);
        } else if (ctl_id == IDV_BREAK && code == BN_CLICKED) {
            break_on_pixel(win);
        } else if (ctl_id == IDV_MEMORY && code == BN_CLICKED) {
            show_in_memory(win);
        } else if (ctl_id == IDV_UNPIN && code == BN_CLICKED) {
            nesvid_tab_unpin(win->tab);
            InvalidateRect(win->pic, NULL, FALSE);
            update_pixel(win);
        }
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR *nmhdr = (NMHDR *)lparam;
        if (nmhdr->idFrom == IDV_SEC && nmhdr->code == TCN_SELCHANGE) {
            show_section(win);
            refresh_view(win, NESVID_TAB_TABLES);
        } else if (nmhdr->idFrom == IDV_REGS && nmhdr->code == NM_DBLCLK) {
            /* a register's row: break on CPU writes to it */
            const int row = (int)SendMessageW(win->regs, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
            if (row >= 0) {
                const int made = nesvid_tab_break_on_register(win->tab, row);
                set_note(win, made >= 0 ? "Break on writes added (see Breakpoints)"
                                        : "That register cannot be broken on (only PPUCTRL, PPUMASK, OAMADDR, PPUSCROLL, PPUADDR)");
            }
        } else if ((nmhdr->idFrom == IDV_REGS || nmhdr->idFrom == IDV_LINES) && nmhdr->code == NM_CUSTOMDRAW) {
            NMLVCUSTOMDRAW *custom = (NMLVCUSTOMDRAW *)lparam;
            listfill *line_fill = nmhdr->idFrom == IDV_REGS ? &win->reg_fill : &win->line_fill;
            if (custom->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (custom->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                int idx = (int)custom->nmcd.dwItemSpec;
                if (idx >= 0 && idx < line_fill->nhot && line_fill->hot[idx]) {
                    if (nmhdr->idFrom == IDV_REGS) {       /* a register that just changed: the accent */
                        custom->clrTextBk = ACCENT;
                        custom->clrText = ACCENT_INK;
                    } else {                            /* a line where something changed */
                        custom->clrTextBk = RGB(0xDD, 0xEE, 0xD5);
                        custom->clrText = RGB(0, 0, 0);
                    }
                }
                return CDRF_DODEFAULT;
            }
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        /* the read-only labels sit on the page colour */
        HDC device = (HDC)wparam;
        if ((HWND)lparam == win->sprites) {         /* the sprite list reads on the window colour, like the other text views */
            SetBkColor(device, GetSysColor(COLOR_WINDOW));
            SetTextColor(device, GetSysColor(COLOR_WINDOWTEXT));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        SetBkColor(device, GetSysColor(COLOR_BTNFACE));
        SetTextColor(device, GetSysColor(COLOR_BTNTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    case WM_DESTROY:
        nesvid_tab_free(win->tab);
        free(win->reg_fill.hot);
        free(win->line_fill.hot);
        free(win);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static void register_classes(HINSTANCE inst)
{
    static int done;
    WNDCLASSW wndclass;
    if (done) return;
    done = 1;
    memset(&wndclass, 0, sizeof wndclass);
    wndclass.lpfnWndProc = page_proc;
    wndclass.hInstance = inst;
    wndclass.hCursor = LoadCursor(NULL, IDC_ARROW);
    wndclass.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wndclass.lpszClassName = PAGE_CLASS;
    RegisterClassW(&wndclass);
    wndclass.lpfnWndProc = pic_proc;
    wndclass.lpszClassName = PIC_CLASS;
    wndclass.hbrBackground = NULL;
    wndclass.hCursor = LoadCursor(NULL, IDC_CROSS);
    wndclass.style = CS_DBLCLKS;
    RegisterClassW(&wndclass);
    wndclass.lpfnWndProc = col_proc;
    wndclass.lpszClassName = COL_CLASS;
    wndclass.hCursor = LoadCursor(NULL, IDC_ARROW);
    wndclass.style = 0;
    RegisterClassW(&wndclass);
}

static HWND mk_list(vidwin *win, int ctl_id, const wchar_t *const *names, int num)
{
    HWND list_view = make_child(win, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP,
                 ctl_id, win->mono);
    LVCOLUMNW col;
    int idx;
    SendMessageW(list_view, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER,
                 LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    memset(&col, 0, sizeof col);
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    for (idx = 0; idx < num; idx++) {
        col.pszText = (LPWSTR)names[idx];
        col.cx = 80;
        SendMessageW(list_view, LVM_INSERTCOLUMNW, (WPARAM)idx, (LPARAM)&col);
    }
    return list_view;
}

HWND dbg_video_create(HWND parent, nessession *session, HFONT ui_font, HFONT mono, int dpi)
{
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    vidwin *win = calloc(1, sizeof *win);
    int num, idx;
    const nesvid_overlay_info *overlay_defs;
    const int *zoom_values;
    TCITEMW item;
    static const wchar_t *const regcols[5] = { L"Register", L"Value", L"Writes", L"Line", L"Meaning" };
    static const wchar_t *const linecols[9] = { L"Lines", L"Scroll X", L"Scroll Y", L"PPUCTRL", L"PPUMASK", L"Fine X",
                                                 L"VRAM addr", L"Changed", L"Mapper writes" };
    if (!win) return NULL;
    register_classes(inst);
    win->ui_font = ui_font;
    win->mono = mono;
    win->session = session;
    win->dpi = dpi > 0 ? dpi : 96;
    win->pal_hover = -1;
    win->tab = nesvid_tab_new(session);
    if (!win->tab) {
        free(win);
        return NULL;
    }
    memset(&win->bmi, 0, sizeof win->bmi);
    win->bmi.bmiHeader.biSize = sizeof win->bmi.bmiHeader;
    win->bmi.bmiHeader.biPlanes = 1;
    win->bmi.bmiHeader.biBitCount = 32;
    win->bmi.bmiHeader.biCompression = BI_RGB;
    win->page = CreateWindowExW(0, PAGE_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent,
                              NULL, inst, NULL);
    SetWindowLongPtrW(win->page, GWLP_USERDATA, (LONG_PTR)win);

    win->label_view = make_child(win, L"STATIC", L"View", SS_LEFT, IDV_LABEL_VIEW, ui_font);
    win->view = make_child(win, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDV_VIEW, ui_font);
    for (idx = 0; idx < NESVID_V_COUNT; idx++) {
        wchar_t *wtext = wide(nesvid_view_name(idx));
        SendMessageW(win->view, CB_ADDSTRING, 0, (LPARAM)wtext);
        free(wtext);
    }
    win->label_src = make_child(win, L"STATIC", L"Source", SS_LEFT, IDV_LABEL_SRC, ui_font);
    win->source = make_child(win, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDV_SOURCE, ui_font);
    for (idx = 0; idx < NESVID_SRC_COUNT; idx++) {
        wchar_t *wtext = wide(nesvid_source_name(idx));
        SendMessageW(win->source, CB_ADDSTRING, 0, (LPARAM)wtext);
        free(wtext);
    }
    win->label_pal = make_child(win, L"STATIC", L"Palette", SS_LEFT, IDV_LABEL_PAL, ui_font);
    win->palette = make_child(win, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDV_PAL, ui_font);
    {
        static const wchar_t *const palette_names[8] = { L"BG 0", L"BG 1", L"BG 2", L"BG 3",
                                                          L"Sprite 0", L"Sprite 1", L"Sprite 2", L"Sprite 3" };
        for (idx = 0; idx < 8; idx++) SendMessageW(win->palette, CB_ADDSTRING, 0, (LPARAM)palette_names[idx]);
    }
    win->label_zoom = make_child(win, L"STATIC", L"Zoom", SS_LEFT, IDV_LABEL_ZOOM, ui_font);
    win->zoom = make_child(win, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDV_ZOOM, ui_font);
    zoom_values = nesvid_zooms(&num);
    for (idx = 0; idx < num && idx < MAX_ZOOMS; idx++) {
        wchar_t lbl[16];
        win->zoom_val[win->nzoom++] = zoom_values[idx];
        if (zoom_values[idx] == 0) wcscpy(lbl, L"Fit");
        else swprintf(lbl, 16, L"%d\u00d7", zoom_values[idx]);
        SendMessageW(win->zoom, CB_ADDSTRING, 0, (LPARAM)lbl);
    }
    win->brk = make_child(win, L"BUTTON", L"Break on writes", BS_PUSHBUTTON | WS_TABSTOP, IDV_BREAK, ui_font);
    overlay_defs = nesvid_overlays(&num);
    for (idx = 0; idx < num && idx < MAX_OVERLAYS; idx++) {
        wchar_t *wtext = wide(overlay_defs[idx].label);
        win->overlay_box[idx] = make_child(win, L"BUTTON", wtext, BS_AUTOCHECKBOX | WS_TABSTOP, IDV_OV0 + idx, ui_font);
        win->ov_bit[idx] = overlay_defs[idx].bit;
        win->nov++;
        free(wtext);
    }
    win->status = make_child(win, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDV_STATUS, mono);
    win->pic = CreateWindowExW(WS_EX_CLIENTEDGE, PIC_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_HSCROLL | WS_VSCROLL, 0, 0, 10, 10,
                             win->page, (HMENU)(INT_PTR)IDV_PIC, inst, NULL);
    SetWindowLongPtrW(win->pic, GWLP_USERDATA, (LONG_PTR)win);
    win->note = make_child(win, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, IDV_NOTE, ui_font);

    win->sec = make_child(win, WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | WS_TABSTOP, IDV_SEC, ui_font);
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    for (idx = 0; idx < NESVID_S_COUNT; idx++) {
        wchar_t *wtext = wide(nesvid_section_name(idx));
        item.pszText = wtext;
        SendMessageW(win->sec, TCM_INSERTITEMW, (WPARAM)idx, (LPARAM)&item);
        free(wtext);
    }
    SetWindowPos(win->sec, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);   /* under the section controls */
    win->pixel = make_child(win, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDV_PIXEL, mono);
    win->unpin = make_child(win, L"BUTTON", L"Unpin", BS_PUSHBUTTON | WS_TABSTOP, IDV_UNPIN, ui_font);
    win->memory = make_child(win, L"BUTTON", L"Show in Memory", BS_PUSHBUTTON | WS_TABSTOP, IDV_MEMORY, ui_font);
    win->regs = mk_list(win, IDV_REGS, regcols, 5);
    win->lines = mk_list(win, IDV_LINES, linecols, 9);
    win->cols = CreateWindowExW(0, COL_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 10, 10, win->page,
                              (HMENU)(INT_PTR)IDV_COLS, inst, NULL);
    SetWindowLongPtrW(win->cols, GWLP_USERDATA, (LONG_PTR)win);
    win->beam = make_child(win, PROGRESS_CLASSW, L"", PBS_SMOOTH, IDV_BEAM, NULL);
    SendMessageW(win->beam, PBM_SETRANGE32, 0, 1000);
    win->timing = make_child(win, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDV_TIMING, mono);
    win->sprites = make_child(win, L"EDIT", L"", WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                      IDV_SPRITES, mono);

    /* start from the controller's state (the NES_DEBUGGER_VIDEO_* variables) */
    win->updating = 1;
    SendMessageW(win->view, CB_SETCURSEL, (WPARAM)nesvid_tab_view(win->tab), 0);
    SendMessageW(win->source, CB_SETCURSEL, (WPARAM)nesvid_tab_source(win->tab), 0);
    SendMessageW(win->palette, CB_SETCURSEL, (WPARAM)nesvid_tab_palette(win->tab), 0);
    enable_choosers(win);
    for (idx = 0; idx < win->nzoom; idx++)
        if (win->zoom_val[idx] == nesvid_tab_zoom(win->tab)) SendMessageW(win->zoom, CB_SETCURSEL, (WPARAM)idx, 0);
    for (idx = 0; idx < win->nov; idx++)
        SendMessageW(win->overlay_box[idx], BM_SETCHECK, (nesvid_tab_overlays(win->tab) & win->ov_bit[idx]) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(win->sec, TCM_SETCURSEL, (WPARAM)nesvid_tab_env_section(win->tab), 0);
    win->updating = 0;
    show_section(win);
    return win->page;
}

void dbg_video_place(HWND page, int pos_x, int pos_y, int width, int height)
{
    MoveWindow(page, pos_x, pos_y, width > 1 ? width : 1, height > 1 ? height : 1, TRUE);
}

void dbg_video_fonts(HWND page, HFONT ui_font, HFONT mono, int dpi)
{
    vidwin *win = (vidwin *)GetWindowLongPtrW(page, GWLP_USERDATA);
    if (!win) return;
    win->ui_font = ui_font;
    win->mono = mono;
    win->dpi = dpi > 0 ? dpi : 96;
    layout(win);
    InvalidateRect(win->page, NULL, TRUE);
}
