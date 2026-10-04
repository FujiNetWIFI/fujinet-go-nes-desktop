/*
 * FujiNet Go NES -- the Windows (Win32 + GDI) frontend.
 *
 * No toolkit: a plain window, a menu bar, a status bar and StretchDIBits.
 * That is enough for a 256x240 framebuffer, and it keeps the artifact a
 * folder you copy rather than a runtime hunt.
 *
 * Two Windows-specific things are load bearing:
 *
 *   DwmFlush() on a present thread is this platform's frame clock. There is
 *   no GdkFrameClock here, and a plain timer would beat against the panel.
 *
 *   WM_ACTIVATE releases every held key. Alt-tabbing away mid-jump and coming
 *   back to a character walking into a wall is the classic symptom of not
 *   doing this, and Windows is where it happens most, because the WM eats the
 *   key-up.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nessession.h"
#include "controllers/controller_window.h"
#include "debugger/dbg_window.h"
#include "key_forward.h"
#include "resource.h"

#define WIN_CLASS "FujiNetGoNES"
#define APP_TITLE "FujiNet Go NES"

/* How long a gamepad connect/disconnect notice stays in the status bar. */
#define PAD_NOTICE_MS 4000

static nessession *g_session;
static HWND g_hwnd;
static HWND g_statusbar;
static uint32_t *g_fb;
static int g_fb_height;
static uint64_t g_serial;
static BITMAPINFO g_bmi;
static CRITICAL_SECTION g_fb_lock;
static volatile LONG g_running = 1;
static HANDLE g_present_thread;
static int g_aspect = 0, g_smooth = 0, g_fullscreen = 0;   /* aspect: 0 TV (8:7 pixels), 1 square */
static WINDOWPLACEMENT g_placement;
static int g_sysact_down[NES_SYSACT_COUNT];
static unsigned g_pad_generation;
static char g_pad_notice[160];
static DWORD g_pad_notice_until;

/* ---- the present thread: this platform's frame clock ---------------------- */

static DWORD WINAPI present_thread(LPVOID arg)
{
    (void)arg;
    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        LARGE_INTEGER t, f;
        int h = 0;
        /* Blocks until the compositor's next vblank; falls through at once
         * if the DWM is off, and Mesen's wall-clock pacing takes over --
         * which is the whole reason notify_vsync is advisory. */
        if (FAILED(DwmFlush())) Sleep(8);
        QueryPerformanceCounter(&t);
        QueryPerformanceFrequency(&f);
        nessession_notify_vsync(g_session,
                                (int64_t)(t.QuadPart * 1000000000LL / f.QuadPart));
        EnterCriticalSection(&g_fb_lock);
        if (nessession_copy_frame(g_session, g_fb, &h, &g_serial)) {
            g_fb_height = h;
            LeaveCriticalSection(&g_fb_lock);
            InvalidateRect(g_hwnd, NULL, FALSE);
        } else {
            LeaveCriticalSection(&g_fb_lock);
        }
    }
    return 0;
}

/* ---- painting ------------------------------------------------------------- */

static int statusbar_height(void)
{
    RECT r;
    if (!g_statusbar || !IsWindowVisible(g_statusbar)) return 0;
    GetWindowRect(g_statusbar, &r);
    return r.bottom - r.top;
}

static void paint(HDC dc)
{
    RECT rc;
    double want, w, h, sw, sh;
    int fbh;

    GetClientRect(g_hwnd, &rc);
    rc.bottom -= statusbar_height();
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    if (w <= 0 || h <= 0) return;

    EnterCriticalSection(&g_fb_lock);
    fbh = g_fb_height;
    if (fbh <= 0) { LeaveCriticalSection(&g_fb_lock); return; }

    /* A television drew each NES pixel 8/7 as wide as it is tall; "square"
     * shows the framebuffer exactly. */
    want = (double)NESSESSION_FB_WIDTH / (double)fbh;
    if (g_aspect == 0) want *= 8.0 / 7.0;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }

    /* The session's pixels are 0x00RRGGBB: exactly a 32-bit BI_RGB DIB. */
    g_bmi.bmiHeader.biHeight = -fbh;   /* top-down */
    SetStretchBltMode(dc, g_smooth ? HALFTONE : COLORONCOLOR);
    StretchDIBits(dc, (int)((w - sw) / 2), (int)((h - sh) / 2), (int)sw, (int)sh,
                  0, 0, NESSESSION_FB_WIDTH, fbh, g_fb, &g_bmi, DIB_RGB_COLORS, SRCCOPY);
    LeaveCriticalSection(&g_fb_lock);
}

/* ---- helpers -------------------------------------------------------------- */

static void restart_session(void)
{
    nessession_start_opts o;
    nessession_settings_flush(g_session);
    nessession_default_opts(g_session, &o);
    nessession_stop(g_session);
    if (nessession_start(g_session, &o) != 0)
        MessageBoxA(g_hwnd, nessession_last_error(g_session), "Could not start",
                    MB_ICONWARNING | MB_OK);
}

static void run_sysaction(int sa)
{
    switch (sa) {
    case NES_SYSACT_RESET_CONFIG: nessession_sysaction(g_session, sa); break;
    case NES_SYSACT_PAUSE:
        /* Showing the debugger attaches it, and attaching stops the machine. */
        nes_debugger_show(g_hwnd, g_session);
        break;
    default: break;
    }
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '\\');
    const char *t = strrchr(p, '/');
    if (t && (!s || t > s)) s = t;
    return s ? s + 1 : p;
}

static void set_text_utf8(HWND h, const char *text, int statusbar)
{
    wchar_t w[512];
    MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 512);
    if (statusbar) SendMessageW(h, SB_SETTEXTW, 0, (LPARAM)w);
    else SetWindowTextW(h, w);
}

static void update_status(void)
{
    static const char *const tape_state[] = { "", "    Tape: PLAY", "    Tape: REC" };
    char title[512], st[160], line[400];
    int ts;
    const char *cart = nessession_cart_path(g_session);

    if (!nessession_is_running(g_session)) {
        snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 stopped");
        snprintf(st, sizeof st, "stopped");
    } else {
        nessession_cart_status(g_session, st, sizeof st);
        if (cart && cart[0])
            snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 %s", base_name(cart));
        else if (nessession_cart_booted_game(g_session))
            snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 booted from FujiNet");
        else
            snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 CONFIG");
    }
    set_text_utf8(g_hwnd, title, 0);

    if (!g_statusbar) return;
    if (g_pad_notice[0] && GetTickCount() < g_pad_notice_until) {
        snprintf(line, sizeof line, "%s", g_pad_notice);
    } else {
        g_pad_notice[0] = '\0';
        snprintf(line, sizeof line, "FujiNet: %s%s", st,
                 nessession_fujinet_running(g_session) ? "" : " (runtime not running)");
    }
    /* The expansion port: KBD while typing goes to the emulated keyboard,
     * and what the Data Recorder is doing. */
    ts = nessession_tape_state(g_session);
    snprintf(line + strlen(line), sizeof line - strlen(line), "%s%s",
             nessession_keyboard_captures(g_session) ? "    KBD" : "",
             ts > 0 && ts < 3 ? tape_state[ts] : "");
    set_text_utf8(g_statusbar, line, 1);
}

/* Gamepad hot-plug: the session bumps a generation on every add/remove and
 * keeps the last event as text; show it for a few seconds. */
static void poll_gamepads(void)
{
    const unsigned gen = nessession_gamepad_generation(g_session);
    if (gen == g_pad_generation) return;
    g_pad_generation = gen;
    if (nessession_gamepad_last_event(g_session, g_pad_notice, sizeof g_pad_notice) > 0) {
        g_pad_notice_until = GetTickCount() + PAD_NOTICE_MS;
        update_status();
    }
    nes_controller_window_gamepads_changed();
}

/* ---- menu ----------------------------------------------------------------- */

/* The keyboard / Data Recorder state the menu, the status bar and the
 * settings window last showed; -1 forces the next sync. */
static int g_kbd_ui_shown = -1;

static void build_menu(HWND hwnd)
{
    HMENU bar = CreateMenu();
    HMENU machine = CreatePopupMenu();
    HMENU tape = CreatePopupMenu();
    HMENU view = CreatePopupMenu();
    HMENU fuji = CreatePopupMenu();
    HMENU help = CreatePopupMenu();

    AppendMenuA(machine, MF_STRING, IDM_OPEN, "&Open Cartridge...\tCtrl+O");
    AppendMenuA(machine, MF_STRING, IDM_EJECT, "&Eject Cartridge");
    AppendMenuA(machine, MF_STRING, IDM_IMPORT_SD, "&Import Cartridge to SD...");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_RESET_GAME, "&Reset Game\tBackspace");
    AppendMenuA(machine, MF_STRING, IDM_RESET_CONFIG, "Reset to &CONFIG\tCtrl+R");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_KBD_MODE, "&Keyboard Mode\tScroll Lock");
    AppendMenuA(tape, MF_STRING, IDM_TAPE_PLAY, "&Play Tape...");
    AppendMenuA(tape, MF_STRING, IDM_TAPE_RECORD, "&Record Tape...");
    AppendMenuA(tape, MF_STRING, IDM_TAPE_STOP, "&Stop");
    AppendMenuA(machine, MF_POPUP, (UINT_PTR)tape, "&Data Recorder");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_SETTINGS, "&Settings...");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_EXIT, "E&xit");

    AppendMenuA(view, MF_STRING, IDM_CONTROLLERS, "&Controllers\tF9");
    AppendMenuA(view, MF_STRING, IDM_DEBUGGER, "&Debugger\tF12");
    AppendMenuA(view, MF_SEPARATOR, 0, NULL);
    AppendMenuA(view, MF_STRING | (g_aspect == 0 ? MF_CHECKED : 0), IDM_TV_ASPECT, "&TV Aspect (8:7 pixels)");
    AppendMenuA(view, MF_STRING | (g_smooth ? MF_CHECKED : 0), IDM_SMOOTH, "&Smooth Scaling");
    AppendMenuA(view, MF_STRING, IDM_FULLSCREEN, "&Fullscreen\tF11");

    AppendMenuA(fuji, MF_STRING, IDM_FUJINET_CONFIG, "&Web UI");
    AppendMenuA(fuji, MF_STRING, IDM_FUJINET_LOG, "Console &Log");

    AppendMenuA(help, MF_STRING, IDM_ABOUT, "&About " APP_TITLE);

    AppendMenuA(bar, MF_POPUP, (UINT_PTR)machine, "&Machine");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)view, "&View");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)fuji, "&FujiNet");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)help, "&Help");
    SetMenu(hwnd, bar);
    g_kbd_ui_shown = -1;
}

static void open_cart(const char *path)
{
    if (nessession_load_cart(g_session, path) != 0)
        MessageBoxA(g_hwnd, nessession_last_error(g_session), "Could not open",
                    MB_ICONWARNING | MB_OK);
    update_status();
}

static void load_media(const char *path)
{
    char dest[1024];
    if (nessession_media_is_cartridge(path)) { open_cart(path); return; }
    if (nessession_import_media(g_session, path, dest, sizeof dest) != 0) {
        MessageBoxA(g_hwnd, nessession_last_error(g_session), "Import failed",
                    MB_ICONWARNING | MB_OK);
        return;
    }
    MessageBoxA(g_hwnd, "Copied to FujiNet's SD folder. Mount it from the CONFIG client.",
                "Imported", MB_ICONINFORMATION | MB_OK);
}

static int pick_cart(const char *title, char *path, DWORD pathsz)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    path[0] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = "NES cartridges (*.nes)\0*.nes\0All files\0*.*\0\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = pathsz;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

/* The Data Recorder's pickers, starting in the session's tapes folder. */
static int pick_tape(int record, char *path, DWORD pathsz)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    path[0] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = "Family BASIC tapes (*.fbt)\0*.fbt\0All files\0*.*\0\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = pathsz;
    ofn.lpstrInitialDir = nessession_tapes_path(g_session);
    ofn.lpstrDefExt = "fbt";
    if (record) {
        ofn.lpstrTitle = "Record Tape";
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
        return GetSaveFileNameA(&ofn) ? 1 : 0;
    }
    ofn.lpstrTitle = "Play Tape";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

/* ---- settings window -------------------------------------------------------
 *
 * Same keys and defaults as the other frontends' Preferences, so a machine
 * configured in one comes up the same in another. Controller types, the
 * region, the stick and the picture apply live; the host options restart
 * the session when the window closes.
 */

static HWND g_settings_window;
static int g_settings_dirty;
static HWND g_pad_list, g_pad_port, g_port_combo[2], g_kbd_combo;

static const char *aspect_name(int i)
{
    static const char *const names[] = { "4:3 TV (8:7 pixels)", "Square pixels", NULL };
    return (i >= 0 && i < 2) ? names[i] : NULL;
}

static void settings_apply_checkbox(HWND hwnd, int id, const char *key, int def)
{
    int on = SendMessageA(GetDlgItem(hwnd, id), BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (nessession_get_int(g_session, key, def) != on) {
        nessession_set_int(g_session, key, on);
        g_settings_dirty = 1;
    }
}

static void refresh_pad_list(void)
{
    int i, n, sel;
    if (!g_pad_list) return;
    sel = (int)SendMessageA(g_pad_list, LB_GETCURSEL, 0, 0);
    SendMessageA(g_pad_list, LB_RESETCONTENT, 0, 0);
    n = nessession_gamepad_count(g_session);
    if (n == 0) {
        SendMessageA(g_pad_list, LB_ADDSTRING, 0, (LPARAM)"(no gamepads connected)");
    }
    for (i = 0; i < n; i++) {
        char name[128], line[200];
        int eff = nessession_gamepad_effective_port(g_session, i);
        nessession_gamepad_name(g_session, i, name, sizeof name);
        if (eff >= 0)
            snprintf(line, sizeof line, "%s  [player %d%s]", name, eff + 1,
                     nessession_gamepad_assignment(g_session, i) < 0 ? ", automatic" : "");
        else
            snprintf(line, sizeof line, "%s  [unused]", name);
        SendMessageA(g_pad_list, LB_ADDSTRING, 0, (LPARAM)line);
    }
    if (sel >= 0 && sel < n) SendMessageA(g_pad_list, LB_SETCURSEL, (WPARAM)sel, 0);
}

static LRESULT CALLBACK settings_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SET_REGION:
            if (HIWORD(wp) == CBN_SELCHANGE)
                nessession_set_region(g_session,
                    (int)SendMessageA(GetDlgItem(hwnd, IDC_SET_REGION), CB_GETCURSEL, 0, 0));
            return 0;
        case IDC_SET_ASPECT:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                HMENU m = GetMenu(g_hwnd);
                g_aspect = (int)SendMessageA(GetDlgItem(hwnd, IDC_SET_ASPECT), CB_GETCURSEL, 0, 0);
                nessession_set_int(g_session, "aspect", g_aspect);
                if (m) CheckMenuItem(m, IDM_TV_ASPECT, MF_BYCOMMAND | (g_aspect == 0 ? MF_CHECKED : MF_UNCHECKED));
                InvalidateRect(g_hwnd, NULL, TRUE);
            }
            return 0;
        case IDC_SET_PORT0:
        case IDC_SET_PORT1:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int port = LOWORD(wp) == IDC_SET_PORT1;
                int sel = (int)SendMessageA(GetDlgItem(hwnd, LOWORD(wp)), CB_GETCURSEL, 0, 0);
                nessession_set_port_type(g_session, port, sel);
            }
            return 0;
        case IDC_SET_KEYBOARD:
            /* Live, like the controller types: the keyboard is plugged in
             * between frames. */
            if (HIWORD(wp) == CBN_SELCHANGE)
                nessession_set_keyboard(g_session,
                    (int)SendMessageA(GetDlgItem(hwnd, IDC_SET_KEYBOARD), CB_GETCURSEL, 0, 0));
            return 0;
        case IDC_SET_AN_JOY:
            nessession_set_analog(g_session,
                SendMessageA(GetDlgItem(hwnd, IDC_SET_AN_JOY), BM_GETCHECK, 0, 0) == BST_CHECKED);
            return 0;
        case IDC_SET_PAD_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE) {
                int sel = (int)SendMessageA(g_pad_list, LB_GETCURSEL, 0, 0);
                SendMessageA(g_pad_port, CB_SETCURSEL,
                             (WPARAM)(nessession_gamepad_assignment(g_session, sel) + 1), 0);
            }
            return 0;
        case IDC_SET_PAD_PORT:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int sel = (int)SendMessageA(g_pad_list, LB_GETCURSEL, 0, 0);
                int choice = (int)SendMessageA(g_pad_port, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < nessession_gamepad_count(g_session))
                    nessession_gamepad_assign(g_session, sel, choice - 1);
                refresh_pad_list();
                nes_controller_window_gamepads_changed();
            }
            return 0;
        case IDC_SET_FUJINET: settings_apply_checkbox(hwnd, IDC_SET_FUJINET, "enable_fujinet", 1); return 0;
        case IDC_SET_AUDIO: settings_apply_checkbox(hwnd, IDC_SET_AUDIO, "enable_audio", 1); return 0;
        case IDC_SET_GAMEPAD: settings_apply_checkbox(hwnd, IDC_SET_GAMEPAD, "enable_gamepad", 1); return 0;
        default: break;
        }
        break;
    case WM_HSCROLL:
        if ((HWND)lp == GetDlgItem(hwnd, IDC_SET_VOLUME))
            nessession_set_volume(g_session, (int)SendMessageA((HWND)lp, TBM_GETPOS, 0, 0));
        return 0;
    case WM_TIMER:
        if (wp == IDT_SETTINGS_PADS) {
            static unsigned seen;
            unsigned gen = nessession_gamepad_generation(g_session);
            if (gen != seen) { seen = gen; refresh_pad_list(); }
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, IDT_SETTINGS_PADS);
        g_settings_window = NULL;
        g_pad_list = g_pad_port = NULL;
        g_port_combo[0] = g_port_combo[1] = NULL;
        g_kbd_combo = NULL;
        if (g_settings_dirty) {
            g_settings_dirty = 0;
            restart_session();
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static HWND settings_checkbox(HWND parent, HINSTANCE inst, const char *text, int id, int x, int y, int w, int checked)
{
    HWND h = CreateWindowExA(0, "BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                             x, y, w, 22, parent, (HMENU)(INT_PTR)id, inst, NULL);
    SendMessageA(h, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(h, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return h;
}

static HWND settings_label(HWND parent, HINSTANCE inst, const char *text, int x, int y, int w, int h)
{
    HWND l = CreateWindowExA(0, "STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, parent,
                             NULL, inst, NULL);
    SendMessageA(l, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return l;
}

static HWND settings_combo(HWND parent, HINSTANCE inst, int id, int x, int y, int w,
                           const char *(*names)(int), int sel)
{
    HWND c = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                             x, y, w, 200, parent, (HMENU)(INT_PTR)id, inst, NULL);
    int i;
    for (i = 0; names(i); i++) SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)names(i));
    SendMessageA(c, CB_SETCURSEL, (WPARAM)sel, 0);
    SendMessageA(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}

static void show_settings(HINSTANCE inst)
{
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND h;
    int y = 12;

    if (g_settings_window) { SetForegroundWindow(g_settings_window); return; }
    {
        static int registered;
        if (!registered) {
            WNDCLASSA wc;
            memset(&wc, 0, sizeof wc);
            wc.lpfnWndProc = settings_proc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            wc.lpszClassName = "NESSettingsWindow";
            RegisterClassA(&wc);
            registered = 1;
        }
    }
    g_settings_window = CreateWindowA("NESSettingsWindow", "Settings",
        WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME),
        CW_USEDEFAULT, CW_USEDEFAULT, 460, 630, NULL, NULL, inst, NULL);

    settings_label(g_settings_window, inst, "Machine", 16, y, 420, 18); y += 22;
    settings_label(g_settings_window, inst, "Region:", 16, y + 3, 90, 18);
    settings_combo(g_settings_window, inst, IDC_SET_REGION, 110, y, 200, nes_region_name,
                   nessession_region(g_session));
    settings_label(g_settings_window, inst, "(next Reset to CONFIG)", 318, y + 3, 130, 18);
    y += 30;
    settings_label(g_settings_window, inst, "Picture:", 16, y + 3, 90, 18);
    settings_combo(g_settings_window, inst, IDC_SET_ASPECT, 110, y, 200, aspect_name, g_aspect);
    y += 36;

    settings_label(g_settings_window, inst, "Controllers (applied immediately)", 16, y, 420, 18); y += 22;
    settings_label(g_settings_window, inst, "Player 1:", 16, y + 3, 90, 18);
    g_port_combo[0] = settings_combo(g_settings_window, inst, IDC_SET_PORT0, 110, y, 200, nes_ctrl_type_name,
                                     nessession_port_type(g_session, 0));
    y += 28;
    settings_label(g_settings_window, inst, "Player 2:", 16, y + 3, 90, 18);
    g_port_combo[1] = settings_combo(g_settings_window, inst, IDC_SET_PORT1, 110, y, 200, nes_ctrl_type_name,
                                     nessession_port_type(g_session, 1));
    y += 28;
    settings_label(g_settings_window, inst, "Expansion port:", 16, y + 3, 90, 18);
    g_kbd_combo = settings_combo(g_settings_window, inst, IDC_SET_KEYBOARD, 110, y, 200, nes_keyboard_name,
                                 nessession_keyboard(g_session));
    y += 32;
    settings_checkbox(g_settings_window, inst, "The left stick drives the D-pad too", IDC_SET_AN_JOY, 16, y, 300,
                      nessession_get_int(g_session, "analog_joystick", 1));
    y += 32;

    settings_label(g_settings_window, inst, "Gamepads (select one, then choose its player):", 16, y, 420, 18); y += 22;
    g_pad_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY, 16, y, 300, 70,
        g_settings_window, (HMENU)(INT_PTR)IDC_SET_PAD_LIST, inst, NULL);
    SendMessageA(g_pad_list, WM_SETFONT, (WPARAM)font, TRUE);
    g_pad_port = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        326, y, 118, 200, g_settings_window, (HMENU)(INT_PTR)IDC_SET_PAD_PORT, inst, NULL);
    SendMessageA(g_pad_port, CB_ADDSTRING, 0, (LPARAM)"Automatic");
    SendMessageA(g_pad_port, CB_ADDSTRING, 0, (LPARAM)"Player 1");
    SendMessageA(g_pad_port, CB_ADDSTRING, 0, (LPARAM)"Player 2");
    SendMessageA(g_pad_port, CB_SETCURSEL, 0, 0);
    SendMessageA(g_pad_port, WM_SETFONT, (WPARAM)font, TRUE);
    refresh_pad_list();
    y += 80;

    settings_label(g_settings_window, inst, "Volume:", 16, y + 3, 90, 18);
    h = CreateWindowExA(0, TRACKBAR_CLASSA, "", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
                        110, y, 300, 28, g_settings_window, (HMENU)(INT_PTR)IDC_SET_VOLUME, inst, NULL);
    SendMessageA(h, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageA(h, TBM_SETTICFREQ, 10, 0);
    SendMessageA(h, TBM_SETPOS, TRUE, (LPARAM)nessession_get_int(g_session, "volume", 100));
    y += 40;

    settings_label(g_settings_window, inst, "Host (applied by restarting the session)", 16, y, 420, 18); y += 22;
    settings_checkbox(g_settings_window, inst, "Enable FujiNet", IDC_SET_FUJINET, 16, y, 200,
                      nessession_get_int(g_session, "enable_fujinet", 1)); y += 26;
    settings_checkbox(g_settings_window, inst, "Audio", IDC_SET_AUDIO, 16, y, 200,
                      nessession_get_int(g_session, "enable_audio", 1)); y += 26;
    settings_checkbox(g_settings_window, inst, "Gamepads", IDC_SET_GAMEPAD, 16, y, 200,
                      nessession_get_int(g_session, "enable_gamepad", 1));

    SetTimer(g_settings_window, IDT_SETTINGS_PADS, 500, NULL);
    ShowWindow(g_settings_window, SW_SHOW);
}

/* Keep the Keyboard Mode check, the Data Recorder items, the expansion-port
 * combo and the status bar's KBD / tape fields in step with the session:
 * Scroll Lock flips the mode inside the core, the Controllers window can
 * plug a keyboard in, and a tape stops by itself. Cheap enough for the
 * 100 ms timer; only touches the UI when something changed. */
static void sync_keyboard_ui(int force)
{
    const int kbd = nessession_keyboard(g_session);
    const int mode = nessession_keyboard_mode(g_session);
    const int cap = nessession_keyboard_captures(g_session);
    const int tape = nessession_tape_state(g_session);
    const int now = kbd | (mode << 4) | (cap << 5) | (tape << 6);
    HMENU m;

    if (!force && now == g_kbd_ui_shown) return;
    if (cap && !(g_kbd_ui_shown >= 0 && ((g_kbd_ui_shown >> 5) & 1)))
        memset(g_sysact_down, 0, sizeof g_sysact_down);   /* their key-ups go to the keyboard now */
    g_kbd_ui_shown = now;

    m = GetMenu(g_hwnd);
    if (m) {
        const UINT fb = kbd == NES_KBD_FAMILY_BASIC ? MF_ENABLED : MF_GRAYED;
        CheckMenuItem(m, IDM_KBD_MODE, MF_BYCOMMAND | (kbd != NES_KBD_NONE && mode ? MF_CHECKED : MF_UNCHECKED));
        EnableMenuItem(m, IDM_KBD_MODE, MF_BYCOMMAND | (kbd != NES_KBD_NONE ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(m, IDM_TAPE_PLAY, MF_BYCOMMAND | fb);
        EnableMenuItem(m, IDM_TAPE_RECORD, MF_BYCOMMAND | fb);
        EnableMenuItem(m, IDM_TAPE_STOP, MF_BYCOMMAND | (kbd == NES_KBD_FAMILY_BASIC && tape != NES_TAPE_IDLE
                                                         ? MF_ENABLED : MF_GRAYED));
    }
    if (g_kbd_combo && (int)SendMessageA(g_kbd_combo, CB_GETCURSEL, 0, 0) != kbd)
        SendMessageA(g_kbd_combo, CB_SETCURSEL, (WPARAM)kbd, 0);
    update_status();
}

/* ---- FujiNet console log --------------------------------------------------- */

static HWND g_log_window;
static HWND g_log_edit;

static void log_refresh(void)
{
    static char buf[128 * 1024];
    int n;
    DWORD first, last, lines;
    if (!g_log_edit) return;

    first = (DWORD)SendMessageA(g_log_edit, EM_GETFIRSTVISIBLELINE, 0, 0);
    lines = (DWORD)SendMessageA(g_log_edit, EM_GETLINECOUNT, 0, 0);
    {
        RECT rc;
        HDC dc = GetDC(g_log_edit);
        TEXTMETRICA tm;
        int visible = 1;
        GetClientRect(g_log_edit, &rc);
        if (dc) {
            HFONT of = (HFONT)SelectObject(dc, (HGDIOBJ)SendMessageA(g_log_edit, WM_GETFONT, 0, 0));
            if (GetTextMetricsA(dc, &tm) && tm.tmHeight > 0)
                visible = (rc.bottom - rc.top) / tm.tmHeight;
            SelectObject(dc, of);
            ReleaseDC(g_log_edit, dc);
        }
        last = first + (DWORD)(visible > 0 ? visible : 1);
    }
    n = nessession_fujinet_copy_log(g_session, buf, sizeof buf);
    SetWindowTextA(g_log_edit, n > 0 ? buf : "(no FujiNet output yet)");
    if (last >= lines) {
        int len = GetWindowTextLengthA(g_log_edit);
        SendMessageA(g_log_edit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
        SendMessageA(g_log_edit, EM_SCROLLCARET, 0, 0);
    }
}

static LRESULT CALLBACK log_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (g_log_edit) MoveWindow(g_log_edit, 0, 0, rc.right - rc.left, rc.bottom - rc.top, TRUE);
        return 0;
    }
    case WM_TIMER:
        if (wp == IDT_LOG_REFRESH) log_refresh();
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, IDT_LOG_REFRESH);
        g_log_window = NULL;
        g_log_edit = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void show_fujinet_log(HINSTANCE inst)
{
    RECT rc;
    if (g_log_window) { SetForegroundWindow(g_log_window); return; }
    {
        static int registered;
        if (!registered) {
            WNDCLASSA wc;
            memset(&wc, 0, sizeof wc);
            wc.lpfnWndProc = log_proc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            wc.lpszClassName = "NESFujiNetLogWindow";
            RegisterClassA(&wc);
            registered = 1;
        }
    }
    g_log_window = CreateWindowA("NESFujiNetLogWindow", "FujiNet Console Log", WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 860, 600, NULL, NULL, inst, NULL);
    GetClientRect(g_log_window, &rc);
    g_log_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        0, 0, rc.right - rc.left, rc.bottom - rc.top, g_log_window, (HMENU)(INT_PTR)IDC_LOG_EDIT, inst, NULL);
    SendMessageA(g_log_edit, WM_SETFONT, (WPARAM)GetStockObject(ANSI_FIXED_FONT), TRUE);
    SetTimer(g_log_window, IDT_LOG_REFRESH, 1000, NULL);
    log_refresh();
    ShowWindow(g_log_window, SW_SHOW);
}

/* ---- window --------------------------------------------------------------- */

static void toggle_fullscreen(HWND hwnd)
{
    DWORD style = GetWindowLong(hwnd, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi;
        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        g_placement.length = sizeof g_placement;
        GetWindowPlacement(hwnd, &g_placement);
        if (GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLong(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetMenu(hwnd, NULL);
            ShowWindow(g_statusbar, SW_HIDE);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_fullscreen = 1;
        }
    } else {
        SetWindowLong(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        build_menu(hwnd);
        ShowWindow(g_statusbar, SW_SHOW);
        SetWindowPlacement(hwnd, &g_placement);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = 0;
    }
    InvalidateRect(hwnd, NULL, TRUE);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (g_statusbar) SendMessageA(g_statusbar, WM_SIZE, 0, 0);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        uint32_t ks;
        int sa;
        const int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (nessession_keyboard_captures(g_session)) {
            /* Keyboard mode: every key is the emulated keyboard's -- Escape,
             * Backspace, the F-keys, F10 and both Alts (GRPH and KANA) arrive
             * here, some as WM_SYSKEYDOWN -- except the Ctrl accelerators.
             * Returning 0 keeps DefWindowProc from turning Alt or F10 into
             * the menu bar. Scroll Lock goes the same way: nessession_key
             * toggles the mode. */
            if (ctrl && wp == 'O') { PostMessage(hwnd, WM_COMMAND, IDM_OPEN, 0); return 0; }
            if (ctrl && wp == 'R') { PostMessage(hwnd, WM_COMMAND, IDM_RESET_CONFIG, 0); return 0; }
            if (lp & (1 << 30)) return 0;
            ks = nes_keysym_from_msg(wp, lp);
            if (ks) nessession_key(g_session, ks, 1);
            /* Alt+F4 still closes the window: the one way out that needs
             * no mouse. */
            if (msg == WM_SYSKEYDOWN && wp == VK_F4) break;
            return 0;
        }
        if (wp == VK_F9) { nes_controller_window_toggle(hwnd, g_session); return 0; }
        if (wp == VK_F12) { nes_debugger_toggle(hwnd, g_session); return 0; }
        if (wp == VK_F11) { toggle_fullscreen(hwnd); return 0; }
        if (msg == WM_SYSKEYDOWN) break;
        if (ctrl) {
            if (wp == 'O') { PostMessage(hwnd, WM_COMMAND, IDM_OPEN, 0); return 0; }
            if (wp == 'R') { PostMessage(hwnd, WM_COMMAND, IDM_RESET_CONFIG, 0); return 0; }
            break;
        }
        if (lp & (1 << 30)) return 0;  /* auto-repeat: the key is already held */
        ks = nes_keysym_from_msg(wp, lp);
        if (!ks) break;
        sa = nessession_key_sysaction(g_session, ks);
        if (sa >= 0) {
            if (!g_sysact_down[sa]) { g_sysact_down[sa] = 1; run_sysaction(sa); }
            return 0;
        }
        /* Scroll Lock lands here too: with a keyboard attached it turns
         * keyboard mode back on. */
        if (nessession_key(g_session, ks, 1)) { sync_keyboard_ui(0); return 0; }
        break;
    }
    case WM_KEYUP: case WM_SYSKEYUP: {
        uint32_t ks = nes_keysym_from_msg(wp, lp);
        int sa;
        if (nessession_keyboard_captures(g_session)) {
            if (ks) nessession_key(g_session, ks, 0);
            sync_keyboard_ui(0);
            return 0;   /* an Alt or F10 release must not open the menu bar */
        }
        if (!ks) break;
        sa = nessession_key_sysaction(g_session, ks);
        if (sa >= 0) { g_sysact_down[sa] = 0; return 0; }
        if (nessession_key(g_session, ks, 0)) return 0;
        break;
    }
    case WM_SYSCHAR:
        /* Alt+letter would open a menu by its mnemonic. */
        if (nessession_keyboard_captures(g_session)) return 0;
        break;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU && nessession_keyboard_captures(g_session)) return 0;
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            nessession_release_all(g_session);
            memset(g_sysact_down, 0, sizeof g_sysact_down);
        }
        return 0;

    case WM_TIMER:
        if (wp == IDT_STATUS) update_status();
        else if (wp == IDT_SYSACT) {
            int sa;
            while (nessession_sysaction_take(g_session, &sa)) run_sysaction(sa);
            poll_gamepads();
            sync_keyboard_ui(0);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_OPEN: {
            char path[MAX_PATH];
            if (pick_cart("Open Cartridge", path, sizeof path)) open_cart(path);
            return 0;
        }
        case IDM_EJECT: nessession_eject(g_session); update_status(); return 0;
        case IDM_IMPORT_SD: {
            char path[MAX_PATH], dest[1024], msg[1200];
            if (!pick_cart("Import Cartridge to SD", path, sizeof path)) return 0;
            if (nessession_import_cart_to_sd(g_session, path, dest, sizeof dest) != 0) {
                MessageBoxA(hwnd, nessession_last_error(g_session), "Import failed", MB_ICONWARNING | MB_OK);
                return 0;
            }
            snprintf(msg, sizeof msg, "%s is on the SD host. Boot it from the CONFIG client.", base_name(dest));
            MessageBoxA(hwnd, msg, "Imported", MB_ICONINFORMATION | MB_OK);
            return 0;
        }
        case IDM_RESET_GAME: nessession_reset_game(g_session); return 0;
        case IDM_KBD_MODE:
            nessession_set_keyboard_mode(g_session, !nessession_keyboard_mode(g_session));
            sync_keyboard_ui(0);
            return 0;
        case IDM_TAPE_PLAY: case IDM_TAPE_RECORD: {
            char path[MAX_PATH];
            const int rec = LOWORD(wp) == IDM_TAPE_RECORD;
            if (!pick_tape(rec, path, sizeof path)) return 0;
            if ((rec ? nessession_tape_record(g_session, path) : nessession_tape_play(g_session, path)) != 0)
                MessageBoxA(hwnd, nessession_last_error(g_session), rec ? "Could not record" : "Could not play",
                            MB_ICONWARNING | MB_OK);
            sync_keyboard_ui(0);
            return 0;
        }
        case IDM_TAPE_STOP:
            if (nessession_tape_stop(g_session) != 0)
                MessageBoxA(hwnd, nessession_last_error(g_session), "Data Recorder", MB_ICONWARNING | MB_OK);
            sync_keyboard_ui(0);
            return 0;
        case IDM_RESET_CONFIG: run_sysaction(NES_SYSACT_RESET_CONFIG); update_status(); return 0;
        case IDM_CONTROLLERS: nes_controller_window_toggle(hwnd, g_session); return 0;
        case IDM_DEBUGGER: nes_debugger_toggle(hwnd, g_session); return 0;
        case IDM_FULLSCREEN: toggle_fullscreen(hwnd); return 0;
        case IDM_TV_ASPECT: {
            HMENU m = GetMenu(hwnd);
            g_aspect = g_aspect ? 0 : 1;
            if (m) CheckMenuItem(m, IDM_TV_ASPECT, MF_BYCOMMAND | (g_aspect == 0 ? MF_CHECKED : MF_UNCHECKED));
            nessession_set_int(g_session, "aspect", g_aspect);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        case IDM_SMOOTH: {
            HMENU m = GetMenu(hwnd);
            g_smooth = !g_smooth;
            if (m) CheckMenuItem(m, IDM_SMOOTH, MF_BYCOMMAND | (g_smooth ? MF_CHECKED : MF_UNCHECKED));
            nessession_set_int(g_session, "smooth", g_smooth);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        case IDM_SETTINGS: show_settings((HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE)); return 0;
        case IDM_FUJINET_LOG: show_fujinet_log((HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE)); return 0;
        case IDM_FUJINET_CONFIG:
            if (!nessession_fujinet_running(g_session)) {
                MessageBoxA(hwnd, "FujiNet is not running.", "FujiNet", MB_ICONINFORMATION | MB_OK);
                return 0;
            }
            ShellExecuteA(hwnd, "open", nessession_fujinet_webui_url(g_session), NULL, NULL, SW_SHOWNORMAL);
            return 0;
        case IDM_ABOUT:
            MessageBoxA(hwnd,
                APP_TITLE " " NES_VERSION_STRING "\n\n"
                "An NES with a built-in FujiNet.\n"
                "The emulator is MesenCE (GPL-3.0-or-later), Mesen by Sour and\n"
                "the Mesen Community Edition contributors, with the FujiNet cartridge.\n\n"
                "Copyright (C) 2026 Thomas Cherryhomes -- GPL-3.0-or-later\n"
                "https://fujinet.online/",
                "About " APP_TITLE, MB_ICONINFORMATION | MB_OK);
            return 0;
        case IDM_EXIT: PostMessage(hwnd, WM_CLOSE, 0, 0); return 0;
        default: break;
        }
        break;

    case WM_DROPFILES: {
        char path[MAX_PATH];
        HDROP drop = (HDROP)wp;
        if (DragQueryFileA(drop, 0, path, sizeof path)) load_media(path);
        DragFinish(drop);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, IDT_STATUS);
        KillTimer(hwnd, IDT_SYSACT);
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static int env_on(const char *name)
{
    const char *env = getenv(name);
    return env && *env && *env != '0';
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    WNDCLASSEX wc;
    MSG msg;
    nessession_start_opts opts;
    RECT want = { 0, 0, 256 * 3 * 8 / 7, 240 * 3 };
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES };
    char cart[MAX_PATH];
    (void)prev;

    InitCommonControlsEx(&icc);
    InitializeCriticalSection(&g_fb_lock);

    g_session = nessession_new(NULL);
    if (!g_session) {
        MessageBoxA(NULL, "Could not create the session (unusable config or data directories?)",
                    APP_TITLE, MB_ICONERROR | MB_OK);
        return 1;
    }
    g_fb = calloc((size_t)NESSESSION_FB_WIDTH * NESSESSION_FB_MAX_HEIGHT, sizeof *g_fb);
    if (!g_fb) return 1;

    memset(&g_bmi, 0, sizeof g_bmi);
    g_bmi.bmiHeader.biSize = sizeof g_bmi.bmiHeader;
    g_bmi.bmiHeader.biWidth = NESSESSION_FB_WIDTH;
    g_bmi.bmiHeader.biHeight = -NESSESSION_FB_MAX_HEIGHT;
    g_bmi.bmiHeader.biPlanes = 1;
    g_bmi.bmiHeader.biBitCount = 32;
    g_bmi.bmiHeader.biCompression = BI_RGB;

    g_aspect = nessession_get_int(g_session, "aspect", 0);
    g_smooth = nessession_get_int(g_session, "smooth", 0);

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WIN_CLASS;
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(IDI_APPICON));
    wc.hIconSm = wc.hIcon;
    RegisterClassEx(&wc);

    /* Three times the console's picture, at the TV's aspect, plus chrome. */
    AdjustWindowRectEx(&want, WS_OVERLAPPEDWINDOW, TRUE, WS_EX_ACCEPTFILES);
    g_hwnd = CreateWindowEx(WS_EX_ACCEPTFILES, WIN_CLASS, APP_TITLE, WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left,
                            want.bottom - want.top + 24, NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;
    build_menu(g_hwnd);
    g_statusbar = CreateWindowExA(0, STATUSCLASSNAMEA, "", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                  0, 0, 0, 0, g_hwnd, (HMENU)(INT_PTR)IDC_STATUSBAR, inst, NULL);
    ShowWindow(g_hwnd, show);

    nessession_default_opts(g_session, &opts);
    if (cmdline && *cmdline) {
        /* a cartridge path on the command line, quotes and all */
        const char *p = cmdline;
        size_t n;
        if (*p == '"') p++;
        snprintf(cart, sizeof cart, "%s", p);
        n = strlen(cart);
        while (n && (cart[n - 1] == '"' || cart[n - 1] == ' ')) cart[--n] = '\0';
        if (cart[0]) opts.cart_path = cart;
    }
    if (nessession_start(g_session, &opts) != 0)
        MessageBoxA(g_hwnd, nessession_last_error(g_session), APP_TITLE, MB_ICONWARNING | MB_OK);

    g_pad_generation = nessession_gamepad_generation(g_session);
    SetTimer(g_hwnd, IDT_STATUS, 1000, NULL);
    SetTimer(g_hwnd, IDT_SYSACT, 100, NULL);
    sync_keyboard_ui(1);   /* also the status line */

    if (env_on("NES_OPEN_CONTROLLERS")) nes_controller_window_toggle(g_hwnd, g_session);
    if (env_on("NES_OPEN_KEYBOARD")) nes_controller_window_show_keyboard(g_hwnd, g_session);
    if (env_on("NES_OPEN_DEBUGGER")) nes_debugger_show(g_hwnd, g_session);
    if (env_on("NES_OPEN_SETTINGS")) show_settings(inst);

    g_present_thread = CreateThread(NULL, 0, present_thread, NULL, 0, NULL);

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        /* Before TranslateMessage, so the sub-windows' keys reach them
         * regardless of which child control has the focus. */
        if (nes_debugger_pretranslate(&msg)) continue;
        if (nes_controller_pretranslate(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    InterlockedExchange(&g_running, 0);
    if (g_present_thread) {
        WaitForSingleObject(g_present_thread, 2000);
        CloseHandle(g_present_thread);
    }
    nessession_stop(g_session);
    nessession_free(g_session);
    free(g_fb);
    DeleteCriticalSection(&g_fb_lock);
    return (int)msg.wParam;
}
