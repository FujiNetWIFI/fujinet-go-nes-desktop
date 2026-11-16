/*
 * The debugger's Video tab (Win32): a thin view over core/include/nesvidtab.h. It is one
 * child window holding all its own controls, so the debugger shows, hides, places and pops it
 * out as a unit.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NES_WINDOWS_DBG_VIDEO_H
#define NES_WINDOWS_DBG_VIDEO_H

#include <windows.h>

#include "nessession.h"

/* Posted to the debugger window when the inspector's Show in Memory is pressed:
 * wParam the nesdebug_space, lParam the address in it. */
#define DBG_VIDEO_SHOW_MEMORY (WM_APP + 7)

/* The tab's page, a child of `parent`. ui and mono are the fonts it uses. */
HWND dbg_video_create(HWND parent, nessession *session, HFONT ui_font, HFONT mono, int dpi);
/* Move and size the page (it lays out its own controls). */
void dbg_video_place(HWND page, int pos_x, int pos_y, int width, int height);
/* Take a snapshot and redraw what changed (force: everything). Cheap to call every tick. */
void dbg_video_refresh(HWND page, int force);
/* The debugger's fonts or DPI changed. */
void dbg_video_fonts(HWND page, HFONT ui_font, HFONT mono, int dpi);

#endif
