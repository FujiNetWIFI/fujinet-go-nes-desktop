/*
 * DisplayGlue.h -- what DisplayView.m needs to know about THIS machine: the
 * session's functions, the frame size and the shape of the picture.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "nessession.h"

#define FN_VIEW      NESDisplayView
#define FN_SESSION   nessession
#define FN_FB_W      NESSESSION_FB_WIDTH
#define FN_FB_H      NESSESSION_FB_MAX_HEIGHT
#define FN_INITIAL_W 960
#define FN_INITIAL_H 720

static inline int fn_machine_hz(FN_SESSION *s) { return nessession_refresh_rate(s); }

static inline void fn_notify_vsync(FN_SESSION *s, int64_t ns)
{
    nessession_notify_vsync(s, ns);
}

static inline int fn_pull_frame(FN_SESSION *s, uint32_t *dst, int *lines, uint64_t *serial)
{
    return nessession_copy_frame(s, dst, lines, serial);
}

/* On a television the NES pixel is 8:7 wide; square pixels show the 256
 * columns as they are. */
static inline double fn_picture_aspect(FN_SESSION *s, int lines, BOOL tv)
{
    (void)s;
    return (double)FN_FB_W * (tv ? (8.0 / 7.0) : 1.0) / (double)lines;
}
