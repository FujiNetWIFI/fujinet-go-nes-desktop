/*
 * ppumodel -- the PPU's scroll registers, replayed from a frame's register
 * accesses (nesvidcap.h says why). Pure: no emulator, no threads.
 *
 * The model follows the nesdev "PPU scrolling" rules: $2000 writes the
 * nametable bits of t, $2005 and $2006 are the two-write latch (w, reset by a
 * $2002 read) that load t, fine X and v, $2007 steps v by 1 or 32 outside
 * rendering; and while rendering is on, dot 256 steps v's Y, dot 257 copies
 * t's horizontal bits into v, and dots 280-304 of the pre-render line copy
 * its vertical bits. The coarse X steps of the tile fetches are left out: a
 * line's recorded v is the fetch ORIGIN, where the line's leftmost tile (less
 * fine X) comes from.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NESPPUMODEL_H
#define NESPPUMODEL_H

#include <stdint.h>

#include "nesvidcap.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nesvid_model {
    uint8_t ctrl, mask, fine_x, wtoggle;
    uint16_t vaddr, taddr;
} nesvid_model;

enum { NESVID_EVK_WRITE = 1, NESVID_EVK_READ, NESVID_EVK_MAPPER,
       NESVID_EVK_NMI, NESVID_EVK_IRQ, NESVID_EVK_SPRITE0 };
enum { NESVID_TT_NONE = 0, NESVID_TT_PPU, NESVID_TT_OAM };

/* One logged access, in the order the machine made them. */
typedef struct nesvid_ev {
    int16_t line;               /* -1 pre-render, 0-239 picture, 241.. blanking */
    uint16_t dot;
    uint8_t kind;               /* NESVID_EVK_* */
    uint16_t addr;              /* $2000-$3FFF for the PPU, the mapper register otherwise */
    uint8_t value;              /* the byte written, or read */
    uint8_t ttype;              /* NESVID_TT_*: what a $2007/$2004 write reached ... */
    uint16_t target;            /* ... the PPU bus address / OAM address (0 when ttype is none) */
} nesvid_ev;

/* The state the model keeps between frames' logs. */
void nesvid_model_seed(nesvid_model *model, uint8_t ctrl, uint8_t mask, uint16_t vaddr, uint16_t taddr,
                       uint8_t fine_x, uint8_t wtoggle);

/* Replays `count` accesses of ONE frame -- from the pre-render line on -- into
 * *out (cleared first), advancing *m. stop_line < -1 runs the whole frame;
 * otherwise the replay stops with the beam at (stop_line, stop_dot) and only
 * the lines it has started are recorded (out->last_line says how many). */
void nesvid_model_run(nesvid_model *model, const nesvid_ev *events, int count,
                      int stop_line, int stop_dot, nesvid_frame_log *out);

/* The scroll position a recorded line draws from, in the 512 x 480 plane of
 * the four nametables (x includes fine X, y the fine Y). */
void nesvid_line_scroll(const nesvid_line *line, int *outx, int *outy);

#ifdef __cplusplus
}
#endif

#endif
