/*
 * The Win32 Controllers window: both NES controllers side by side -- the
 * cross, Select, Start, B, A and the two turbo buttons -- each with its
 * port's controller type and the gamepad driving it, then the console's
 * RESET and Reset to CONFIG, then the Map row.
 *
 * A button lights in the accent colour whenever the console sees it held,
 * from whatever source (keyboard, gamepad, or a click here), so the window
 * doubles as an input tester.
 *
 * Buttons are driven by WM_LBUTTONDOWN/WM_LBUTTONUP on the window rather
 * than by BN_CLICKED: a controller button is HELD, and a game polls it, so
 * a value present only for the instant of a click falls between frames.
 * The mouse is captured on press and released on button-up wherever that
 * happens, so dragging off a button cannot strand the machine with a button
 * held forever.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controller_window.h"

/* GET_X_LPARAM / GET_Y_LPARAM live here, not in windows.h. */
#include <windowsx.h>

#include <stdio.h>
#include <string.h>

#include "../key_forward.h"

#define PAD_CLASS "FujiNetGoNESControllers"

#define BTN      40         /* a D-pad arm */
#define FACE_W   48         /* B, A, turbos */
#define FACE_H   40
#define PILL_W   62         /* Select, Start */
#define PILL_H   24
#define GAP       6
#define PAD_W   (3 * BTN + 2 * GAP + 24 + 2 * PILL_W + GAP + 24 + 2 * FACE_W + GAP)
#define MARGIN   12
#define HEAD_H   22
#define TYPE_H   26
#define PADS_H   20
#define WIDE_W  150

#define IDT_CAPTURE 1
#define IDT_HELD    2

typedef struct {
    RECT rc;
    int target;
    char face[20];
} pad_button;

static HWND g_panel;
static nessession *g_session;
static pad_button g_btn[NES_TARGET_COUNT];
static int g_nbtn;
static int g_held = -1;          /* button index under the captured mouse */
static int g_map_state = -2;     /* -2 idle, -1 armed, >=0 awaiting a key/button */
static RECT g_map_rc, g_defaults_rc, g_hint_rc;
static RECT g_head_rc[2], g_type_rc[2], g_pads_rc[2], g_box_rc[2];
static unsigned g_shown_held[2];
static char g_hint[200];
static HBRUSH g_accent_brush;
static HFONT g_bold;
static int g_total_w, g_total_h;

static void add_button(const char *face, int target, int x, int y, int w, int h)
{
    pad_button *b;
    if (g_nbtn >= NES_TARGET_COUNT) return;
    b = &g_btn[g_nbtn++];
    SetRect(&b->rc, x, y, x + w, y + h);
    b->target = target;
    snprintf(b->face, sizeof b->face, "%s", face);
}

/* One controller, laid out like the real one: the cross left, Select and
 * Start in the middle, B and A right with the turbos above them. Returns the
 * y below it. */
static int build_controller(int port, int x0, int y0)
{
    int y = y0, cy, cx;

    SetRect(&g_head_rc[port], x0, y, x0 + PAD_W, y + HEAD_H);
    y += HEAD_H;
    SetRect(&g_type_rc[port], x0, y, x0 + PAD_W, y + TYPE_H);
    y += TYPE_H;
    SetRect(&g_pads_rc[port], x0, y, x0 + PAD_W, y + PADS_H);
    y += PADS_H + GAP;

    SetRect(&g_box_rc[port], x0, y, x0 + PAD_W, y + 3 * BTN + 2 * GAP + 2 * MARGIN);
    y += MARGIN;
    cx = x0 + MARGIN;
    cy = y;
    add_button("Up", NES_TARGET_PORT(port, NES_ACT_UP), cx + BTN + GAP, cy, BTN, BTN);
    add_button("Left", NES_TARGET_PORT(port, NES_ACT_LEFT), cx, cy + BTN + GAP, BTN, BTN);
    add_button("Right", NES_TARGET_PORT(port, NES_ACT_RIGHT), cx + 2 * (BTN + GAP), cy + BTN + GAP, BTN, BTN);
    add_button("Down", NES_TARGET_PORT(port, NES_ACT_DOWN), cx + BTN + GAP, cy + 2 * (BTN + GAP), BTN, BTN);

    cx += 3 * BTN + 2 * GAP + 12;
    add_button("Select", NES_TARGET_PORT(port, NES_ACT_SELECT), cx, cy + BTN + GAP + (BTN - PILL_H) / 2, PILL_W, PILL_H);
    add_button("Start", NES_TARGET_PORT(port, NES_ACT_START), cx + PILL_W + GAP, cy + BTN + GAP + (BTN - PILL_H) / 2, PILL_W, PILL_H);

    cx += 2 * PILL_W + GAP + 12;
    add_button("Turbo B", NES_TARGET_PORT(port, NES_ACT_TURBO_B), cx, cy, FACE_W, FACE_H);
    add_button("Turbo A", NES_TARGET_PORT(port, NES_ACT_TURBO_A), cx + FACE_W + GAP, cy, FACE_W, FACE_H);
    add_button("B", NES_TARGET_PORT(port, NES_ACT_B), cx, cy + 2 * (BTN + GAP) - (FACE_H - BTN), FACE_W, FACE_H);
    add_button("A", NES_TARGET_PORT(port, NES_ACT_A), cx + FACE_W + GAP, cy + 2 * (BTN + GAP) - (FACE_H - BTN), FACE_W, FACE_H);

    return g_box_rc[port].bottom;
}

static void layout(void)
{
    const int x1 = MARGIN + PAD_W + 2 * MARGIN;
    int y, cx;

    g_nbtn = 0;
    y = build_controller(0, MARGIN, MARGIN);
    build_controller(1, x1, MARGIN);
    g_total_w = x1 + PAD_W + MARGIN;

    /* The console's buttons, centred. */
    y += MARGIN + 4;
    cx = (g_total_w - (2 * WIDE_W + GAP)) / 2;
    add_button("Reset Game", NES_TARGET_SWITCH(NES_SW_RESET), cx, y, WIDE_W, FACE_H);
    add_button("Reset to CONFIG", NES_TARGET_SYSACT(NES_SYSACT_RESET_CONFIG), cx + WIDE_W + GAP, y, WIDE_W, FACE_H);
    y += FACE_H + MARGIN;

    SetRect(&g_map_rc, MARGIN, y, MARGIN + 70, y + 30);
    SetRect(&g_defaults_rc, MARGIN + 76, y, MARGIN + 76 + 84, y + 30);
    SetRect(&g_hint_rc, MARGIN + 76 + 84 + 10, y, g_total_w - MARGIN, y + 30);
    g_total_h = y + 30 + MARGIN;
}

static void press_target(int target, int down)
{
    if (target >= NES_TARGET_SYSACT(0)) {
        /* System actions fire on release, like a real button: pressing and
         * dragging off must not power-cycle the machine. */
        if (!down) nessession_sysaction(g_session, target - NES_TARGET_SYSACT(0));
        return;
    }
    nessession_press(g_session, target, down);
}

static int hit(int x, int y)
{
    POINT p = { x, y };
    int i;
    for (i = 0; i < g_nbtn; i++)
        if (PtInRect(&g_btn[i].rc, p)) return i;
    return -1;
}

static void set_map_state(int state)
{
    g_map_state = state;
    if (state == -2) {
        KillTimer(g_panel, IDT_CAPTURE);
        nessession_gamepad_capture_cancel(g_session);
        g_hint[0] = '\0';
    } else if (state == -1) {
        KillTimer(g_panel, IDT_CAPTURE);
        nessession_gamepad_capture_cancel(g_session);
        snprintf(g_hint, sizeof g_hint, "Click a button to remap it");
    } else {
        snprintf(g_hint, sizeof g_hint, "Press a key or gamepad button for %s", nes_target_name(state));
        nessession_gamepad_capture_begin(g_session);
        SetTimer(g_panel, IDT_CAPTURE, 50, NULL);
    }
    InvalidateRect(g_panel, NULL, TRUE);
}

static void button_label(const pad_button *b, char *out, int outsz)
{
    if (g_map_state != -2) {
        const nes_binding bind = nessession_binding_get(g_session, b->target);
        char key[32];
        nessession_keysym_name(bind.keysym, key, sizeof key);
        if (bind.button != NES_PAD_BTN_NONE)
            snprintf(out, outsz, "%s\n%s", key[0] ? key : "-", nes_pad_button_name(bind.button));
        else
            snprintf(out, outsz, "%s", key[0] ? key : "-");
    } else {
        snprintf(out, outsz, "%s", b->face);
    }
}

static void draw_button(HDC dc, const RECT *rc, const char *label, int pushed, int accent)
{
    RECT r = *rc, text;
    int h;

    if (accent) {
        FillRect(dc, &r, g_accent_brush);
        FrameRect(dc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(dc, RGB(255, 255, 255));
    } else {
        DrawFrameControl(dc, &r, DFC_BUTTON, DFCS_BUTTONPUSH | (pushed ? DFCS_PUSHED : 0));
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    }
    /* Vertically centred, word-broken on the newline a Map-mode label
     * carries between its key and its pad button. */
    text = r;
    InflateRect(&text, -2, 0);
    h = DrawTextA(dc, label, -1, &text, DT_CENTER | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    text.left = r.left + 2;
    text.right = r.right - 2;
    text.top = r.top + ((r.bottom - r.top) - h) / 2 + (pushed ? 1 : 0);
    text.bottom = r.bottom;
    DrawTextA(dc, label, -1, &text, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
}

/* The gamepads driving a port, as one line. */
static void pads_line(int port, char *out, int outsz)
{
    int i, n = nessession_gamepad_count(g_session), len = 0;
    out[0] = '\0';
    for (i = 0; i < n; i++) {
        char name[96];
        if (nessession_gamepad_effective_port(g_session, i) != port) continue;
        nessession_gamepad_name(g_session, i, name, sizeof name);
        len += snprintf(out + len, (size_t)(outsz - len), "%s%s", len ? ", " : "Gamepad: ", name);
        if (len >= outsz) break;
    }
    if (!out[0]) snprintf(out, (size_t)outsz, "No gamepad (keyboard only)");
}

static void paint_panel(HDC dc)
{
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HGDIOBJ old = SelectObject(dc, font);
    RECT client;
    int i, port;

    GetClientRect(g_panel, &client);
    FillRect(dc, &client, (HBRUSH)(COLOR_BTNFACE + 1));
    SetBkMode(dc, TRANSPARENT);

    for (port = 0; port < 2; port++) {
        char line[200];
        RECT box = g_box_rc[port];
        const int type = nessession_port_type(g_session, port);
        SelectObject(dc, g_bold);
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        DrawTextA(dc, port ? "Player 2" : "Player 1", -1, &g_head_rc[port],
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, font);
        /* The type line is itself a button: click to cycle what is plugged in. */
        snprintf(line, sizeof line, "Plugged in: %s  (click to change)", nes_ctrl_type_name(type));
        {
            RECT tr = g_type_rc[port];
            InflateRect(&tr, -40, -2);
            DrawFrameControl(dc, &tr, DFC_BUTTON, DFCS_BUTTONPUSH);
            SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
            DrawTextA(dc, line, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        pads_line(port, line, sizeof line);
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        DrawTextA(dc, line, -1, &g_pads_rc[port], DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        /* the controller's body */
        FillRect(dc, &box, (HBRUSH)GetStockObject(type == NES_CTRL_NONE ? LTGRAY_BRUSH : GRAY_BRUSH));
        FrameRect(dc, &box, (HBRUSH)GetStockObject(DKGRAY_BRUSH));
    }

    for (i = 0; i < g_nbtn; i++) {
        char label[64];
        const int t = g_btn[i].target;
        int lit = 0;
        const int clicked = (i == g_held) && g_map_state == -2;
        if (t < 2 * NES_ACT_PER_PORT)
            lit = (g_shown_held[t / NES_ACT_PER_PORT] >> (t % NES_ACT_PER_PORT)) & 1;
        button_label(&g_btn[i], label, sizeof label);
        draw_button(dc, &g_btn[i].rc, label, clicked,
                    (g_map_state == -2 && (lit || clicked)) || (g_map_state >= 0 && t == g_map_state));
    }

    draw_button(dc, &g_map_rc, g_map_state == -2 ? "Map" : "Done", 0, g_map_state != -2);
    draw_button(dc, &g_defaults_rc, "Defaults", 0, 0);

    if (g_hint[0]) {
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        DrawTextA(dc, g_hint, -1, &g_hint_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(dc, old);
}

static void refresh_held(void)
{
    int port, changed = 0;
    for (port = 0; port < 2; port++) {
        unsigned now = nessession_buttons_held(g_session, port);
        if (now != g_shown_held[port]) { g_shown_held[port] = now; changed = 1; }
    }
    if (changed) InvalidateRect(g_panel, NULL, FALSE);
}

static LRESULT CALLBACK pad_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        /* Off-screen first: the held highlights repaint at 20 Hz. */
        RECT c;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ oldbmp;
        GetClientRect(hwnd, &c);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, c.right, c.bottom);
        oldbmp = SelectObject(mem, bmp);
        paint_panel(mem);
        BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        POINT p = { x, y };
        int i = hit(x, y), port;

        if (PtInRect(&g_map_rc, p)) { set_map_state(g_map_state == -2 ? -1 : -2); return 0; }
        if (PtInRect(&g_defaults_rc, p)) {
            nessession_bindings_reset(g_session);
            snprintf(g_hint, sizeof g_hint, "Every key and button is back to its default");
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        for (port = 0; port < 2; port++) {
            if (PtInRect(&g_type_rc[port], p)) {
                int t = (nessession_port_type(g_session, port) + 1) % NES_CTRL_COUNT;
                nessession_set_port_type(g_session, port, t);
                InvalidateRect(hwnd, NULL, TRUE);
                return 0;
            }
        }
        if (i < 0) return 0;

        if (g_map_state == -1) { set_map_state(g_btn[i].target); return 0; }
        if (g_map_state >= 0) return 0;

        g_held = i;
        SetCapture(hwnd);
        press_target(g_btn[i].target, 1);
        InvalidateRect(hwnd, &g_btn[i].rc, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
        /* Release wherever the mouse ended up: capture means this arrives
         * even if the pointer left the button. */
        if (g_held >= 0) {
            RECT r = g_btn[g_held].rc;
            int t = g_btn[g_held].target;
            g_held = -1;
            ReleaseCapture();
            press_target(t, 0);
            InvalidateRect(hwnd, &r, FALSE);
        }
        return 0;

    case WM_TIMER:
        if (wp == IDT_CAPTURE && g_map_state >= 0) {
            int button;
            if (nessession_gamepad_capture_poll(g_session, &button)) {
                char stolen[128];
                const int target = g_map_state;
                nessession_binding_set_button(g_session, target, button, stolen, sizeof stolen);
                set_map_state(-1);   /* stay armed: remapping several in a row is normal */
                if (stolen[0])
                    snprintf(g_hint, sizeof g_hint, "%s: %s (taken from %s)", nes_target_name(target),
                             nes_pad_button_name(button), stolen);
                InvalidateRect(hwnd, NULL, TRUE);
            }
        } else if (wp == IDT_HELD) {
            refresh_held();
        }
        return 0;

    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        const uint32_t ks = nes_keysym_from_msg(wp, lp);
        int sa;
        if (lp & (1 << 30)) return 0;   /* auto-repeat */
        if (g_map_state >= 0) {
            if (wp == VK_ESCAPE && !(GetKeyState(VK_SHIFT) & 0x8000)) { set_map_state(-1); return 0; }
            if (ks) {
                char stolen[128], name[32];
                const int target = g_map_state;
                nessession_binding_set_key(g_session, target, ks, stolen, sizeof stolen);
                nessession_keysym_name(ks, name, sizeof name);
                set_map_state(-1);   /* stay armed: remapping several in a row is normal */
                if (stolen[0])
                    snprintf(g_hint, sizeof g_hint, "%s: %s (taken from %s)", nes_target_name(target), name, stolen);
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;
        }
        if (g_map_state == -1) {
            if (wp == VK_ESCAPE) set_map_state(-2);
            return 0;
        }
        if (wp == VK_F9) { ShowWindow(hwnd, SW_HIDE); return 0; }
        if (!ks) break;
        sa = nessession_key_sysaction(g_session, ks);
        if (sa >= 0) { nessession_sysaction(g_session, sa); return 0; }
        if (nessession_key(g_session, ks, 1)) return 0;
        break;
    }
    case WM_KEYUP: case WM_SYSKEYUP: {
        const uint32_t ks = nes_keysym_from_msg(wp, lp);
        if (g_map_state != -2) return 0;
        if (ks && nessession_key(g_session, ks, 0)) return 0;
        break;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) nessession_release_all(g_session);
        return 0;
    case WM_SHOWWINDOW:
        if (wp) { refresh_held(); SetTimer(hwnd, IDT_HELD, 50, NULL); }
        else KillTimer(hwnd, IDT_HELD);
        break;
    case WM_CLOSE:
        /* Hide, do not destroy: the window's position survives closing it. */
        set_map_state(-2);
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void nes_controller_window_toggle(HWND parent, nessession *session)
{
    g_session = session;

    if (!g_panel) {
        WNDCLASSEXA wc;
        RECT want;
        LOGFONTA lf;

        layout();
        g_accent_brush = CreateSolidBrush(RGB((NESSESSION_ACCENT_RGB >> 16) & 0xff,
                                              (NESSESSION_ACCENT_RGB >> 8) & 0xff,
                                              NESSESSION_ACCENT_RGB & 0xff));
        GetObjectA(GetStockObject(DEFAULT_GUI_FONT), sizeof lf, &lf);
        lf.lfWeight = FW_BOLD;
        g_bold = CreateFontIndirectA(&lf);

        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = pad_proc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = PAD_CLASS;
        RegisterClassExA(&wc);

        SetRect(&want, 0, 0, g_total_w, g_total_h);
        /* A tool window with a caption and no thick frame or maximize box:
         * it is exactly the size of its controls. WS_EX_TOOLWINDOW also
         * keeps it off the taskbar. */
        AdjustWindowRectEx(&want, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOOLWINDOW);
        g_panel = CreateWindowExA(WS_EX_TOOLWINDOW, PAD_CLASS, "Controllers",
                                  WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                                  want.right - want.left, want.bottom - want.top,
                                  parent, NULL, wc.hInstance, NULL);
        if (!g_panel) return;
        g_shown_held[0] = g_shown_held[1] = 0;
    }

    if (IsWindowVisible(g_panel)) {
        set_map_state(-2);
        ShowWindow(g_panel, SW_HIDE);
    } else {
        ShowWindow(g_panel, SW_SHOW);
        SetForegroundWindow(g_panel);
    }
}

void nes_controller_window_gamepads_changed(void)
{
    if (g_panel && IsWindowVisible(g_panel)) InvalidateRect(g_panel, NULL, FALSE);
}

int nes_controller_pretranslate(MSG *msg)
{
    /* The window has no child controls, so its own window proc sees every
     * key; nothing to steal here. Kept for symmetry with the debugger. */
    (void)msg;
    return 0;
}
