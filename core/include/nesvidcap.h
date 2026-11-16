/*
 * nesvidcap -- the PPU's state as a frame left it, for the Video tab
 * (nesvidtab.h). Taken once per frame on MesenCE's own threads while a tab
 * exists (core/src/vidcap.cpp), so a reader sees one consistent frame while
 * the machine runs; a machine parked by the debugger is read live
 * (nesvid_host_capture_fill).
 *
 * What a capture holds:
 *   - pixels: the last finished frame, 0xAARRGGBB, 256 x 240, the very
 *     picture nessession_copy_frame hands out;
 *   - the PPU at the end of that frame (scanline 240): the pattern space
 *     ($0000-$1FFF through the cartridge's CHR banks), the four logical
 *     nametables ($2000-$2FFF through the mirroring), palette RAM, OAM, the
 *     registers and the 64 colours as the video output draws them;
 *   - the frame's log (flog): MesenCE's event viewer records every PPU
 *     register access with the scanline and dot it landed on. The model in
 *     ppumodel.h replays them -- $2000/$2001/$2005/$2006 write by write,
 *     with the PPU's own v/t/x/w rules and its dot 256/257/280-304 copies --
 *     into the state each visible line starts rendering from, so a status bar
 *     or a split screen shows as bands. The model is event-granular: a write
 *     in the middle of a line lands on that line (flagged `mid`), it does not
 *     split the line;
 *   - parked only: the PPU, memory and frame as they stand, the log of the
 *     lines the frame in progress has started (clog), and where the beam is.
 *
 * Not captured: CHR bank switches during the frame (a mapper IRQ splitting the
 * pattern tables). The pattern space is the one at the end of the frame; the
 * per-line log counts the mapper register writes, so a split shows in the
 * Lines table.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NESVIDCAP_H
#define NESVIDCAP_H

#include <stdint.h>

#include "nessession.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NESVID_W            256
#define NESVID_H            240
#define NESVID_MAX_LINES    320         /* scanlines in the longest frame (PAL 312) */
#define NESVID_CHR          0x2000
#define NESVID_NT           0x1000
#define NESVID_MAX_WRITES   256
#define NESVID_MAX_EVENTS   32

enum { NESVID_MIRROR_HORIZONTAL = 0, NESVID_MIRROR_VERTICAL, NESVID_MIRROR_SCREEN_A,
       NESVID_MIRROR_SCREEN_B, NESVID_MIRROR_FOUR };
enum { NESVID_NTSC = 0, NESVID_PAL, NESVID_DENDY };

/* The state a visible line starts rendering from. */
typedef struct nesvid_line {
    uint8_t valid;              /* the log reaches this line */
    uint8_t ctrl;               /* $2000 as written (bits 0-1 are the nametable in v) */
    uint8_t mask;               /* $2001 */
    uint8_t fine_x;             /* the fine X scroll */
    uint16_t vaddr;                 /* the VRAM address the line fetches from: coarse X, nametable, Y */
    uint16_t taddr;                 /* the temporary address (where the next frame starts) */
    uint8_t writes;             /* PPU register writes that landed on this line (saturates) */
    uint8_t mid;                /* some landed after dot 1: the line may be drawn from two states */
    uint8_t mapper_writes;      /* mapper register writes on this line (saturates) */
    uint8_t mapper_value;       /* the last one: value ... */
    uint16_t mapper_addr;       /* ... and address */
} nesvid_line;

typedef struct nesvid_write {   /* a write to $2000, $2001, $2003, $2005 or $2006 */
    int16_t line;               /* -1 is the pre-render line, 241 and up the blanking */
    uint16_t dot;
    uint8_t reg;                /* 0-7: $2000 + reg */
    uint8_t value;
} nesvid_write;

enum { NESVID_EV_NMI = 1, NESVID_EV_IRQ, NESVID_EV_SPRITE0 };
typedef struct nesvid_event {
    uint8_t kind;               /* NESVID_EV_* */
    int16_t line;
    uint16_t dot;
} nesvid_event;

/* One frame's worth of log. */
typedef struct nesvid_frame_log {
    int last_line;              /* the highest visible line recorded (a finished frame: 239), -1 none */
    nesvid_line line[NESVID_H];
    int nwrites;
    nesvid_write writes[NESVID_MAX_WRITES];
    int nevents;
    nesvid_event events[NESVID_MAX_EVENTS];
    uint32_t reg_writes[8];     /* writes to $2000 + n in the frame, blanking included */
    uint32_t reg_vis[8];        /* ... of those, on the picture's lines */
    uint32_t reg_reads[8];
    int16_t reg_first[8];       /* the first visible line one landed on, -1 none */
    uint32_t mapper_writes;
    uint8_t wr_nt[NESVID_NT / 8];       /* PPU addresses written through $2007 ($2000-$2FFF) */
    uint8_t wr_chr[NESVID_CHR / 8];     /* ... $0000-$1FFF */
    uint8_t wr_pal[4];                  /* ... palette RAM */
    uint8_t wr_oam[32];                 /* OAM bytes written (through $2004 or DMA) */
} nesvid_frame_log;

typedef struct nesvid_capture {
    uint32_t pixels[NESVID_W * NESVID_H];       /* the last finished frame */
    int fb_valid;
    uint32_t live_pixels[NESVID_W * NESVID_H];  /* parked: the frame as the PPU is drawing it */
    int live_pixels_valid;

    int mem_valid;              /* the PPU part below was read */
    uint8_t chr[NESVID_CHR];    /* $0000-$1FFF */
    uint8_t ntab[NESVID_NT];      /* the four logical nametables, $2000-$2FFF */
    uint8_t pal[32];            /* palette RAM */
    uint8_t oam[256];
    uint32_t rgb[64];           /* 0xAARRGGBB of each master colour */
    uint8_t ctrl, mask, status, oam_addr, fine_x, wtoggle;
    uint16_t vaddr, taddr;
    int mirroring;              /* NESVID_MIRROR_* */
    int region;                 /* NESVID_NTSC ... */
    int nlines;                 /* scanlines in the frame: 262, 312 */
    int nmi_line;               /* the scanline vblank starts on */

    int log_valid;              /* flog holds one finished frame */
    uint64_t log_frame;         /* the PPU frame number it belongs to */
    nesvid_frame_log flog;
    int cur_valid;              /* parked: clog holds the frame in progress */
    nesvid_frame_log clog;

    int beam_line;              /* parked only: the scanline the beam is on (-1 pre-render), -2 unknown */
    int beam_dot;
    uint64_t cycles;            /* parked only: CPU cycles since power-on */
    uint64_t frame;             /* the PPU frame number this capture describes */
    uint64_t serial;            /* 0: nothing captured yet */
    int live;                   /* the PPU part was read from a parked machine */
} nesvid_capture;

/* Frames are captured only while at least one tab wants them (a count:
 * enable(1) from each tab, enable(0) when it is freed). While the count is
 * above zero the host also records the PPU register log. Thread-safe. The
 * log starts with the first frame the machine runs after the debugger is
 * attached. */
void nesvid_host_capture_enable(nessession *session, int enable);

/* Copies the latest captured frame into dst iff its serial differs from
 * *serial_inout, updates it and returns 1; 0 and dst untouched otherwise.
 * *serial_inout = 0 forces a copy once something has been captured. */
int nesvid_host_capture_get(nessession *session, nesvid_capture *dst, uint64_t *serial_inout);

/* How many frames' logs the model has replayed, and in how many the PPU's own
 * registers at the end disagreed with the replay (a missed event; the model
 * re-seeds from the PPU every frame, so it never drifts). Diagnostics. */
void nesvid_host_capture_stats(nessession *session, uint64_t *batches, uint64_t *resyncs);

/* Overwrites the PPU part of dst (registers, memory, palette, the live frame,
 * the frame-in-progress log and the beam) with the machine as it stands and
 * sets live. The finished frame (pixels, flog) is left as it was. Only for a
 * machine parked by the debugger; returns 0, leaving dst alone, when there is
 * none. */
int nesvid_host_capture_fill(nessession *session, nesvid_capture *dst);

#ifdef __cplusplus
}
#endif

#endif
