/*
 * nesvidtab -- the Video tab's controller. All four frontends draw the same
 * tab from it, so what they own is widgets: a view and zoom chooser, overlay
 * toggles, a picture, the inspector and the sections. Modelled on the
 * Coleco, Adam and MS-DOS controllers (colecovidtab.h), so a frontend that has
 * one ports this by changing names:
 *
 *   - the machine is only COPIED: a finished frame (nesvidcap.h) is captured
 *     once per frame by MesenCE's own threads, and a parked machine is read
 *     live. Decoding, scaling and overlays run on the caller's (UI) thread;
 *   - the picture is rebuilt only when something it shows changed, into one
 *     reused buffer;
 *   - hover and pin are state here, so a mouse move never re-renders the
 *     picture: draw the marks from nesvid_tab_marks() over the cached image;
 *   - pixels are 0xFFRRGGBB (opaque), whatever the toolkit does with alpha;
 *   - one table of overlays and one env-var parser, so the frontends cannot
 *     disagree.
 *
 * The PPU draws every line from the registers as they stand when the line
 * starts. The per-line log of a captured frame (nesvid_line: $2000, $2001,
 * the fine X and the VRAM address the line fetches from, replayed from
 * MesenCE's register-access events) says what each line was made from. The
 * Lines table lists the BANDS of lines drawn from one state (a status bar or
 * a split screen is a second band), the Nametables source outlines every
 * band's scroll window, and the Registers section shows how often the
 * registers were written down the frame. The log is event-granular: a write
 * in the middle of a line lands on that line.
 *
 * Views: Display is the last finished frame, exactly the picture of the main
 * window (256 x 240); Decoded redraws one source from the PPU's memory: the
 * four nametables with the scroll windows, the pattern tables in a chosen
 * palette, the sprites where they sit, or the palette; Beam is the frame as
 * the PPU is drawing it, with the lines the beam has not reached dimmed
 * (stopped only); Values shows each nametable entry as a flat false colour of
 * its value.
 *
 * Not decoded: CHR bank switches during the frame (the pattern tables are the
 * ones at the end of it). The line log counts mapper register writes.
 *
 * UI thread only. Not thread-safe.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NESVIDTAB_H
#define NESVIDTAB_H

#include <stdint.h>

#include "nesdebug.h"
#include "nesvidcap.h"
#include "nessession.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nesvid_tab nesvid_tab;

enum nesvid_view {
    NESVID_V_DISPLAY = 0,       /* the last finished frame, 256 x 240 */
    NESVID_V_DECODED,           /* one source rebuilt from the PPU's memory (see nesvid_source) */
    NESVID_V_BEAM,              /* the frame as the PPU is drawing it, the lines below the beam dimmed (stopped) */
    NESVID_V_VALUES,            /* each nametable entry as a flat false colour of its value */
    NESVID_V_COUNT
};

/* What the Decoded view redraws. */
enum nesvid_source {
    NESVID_SRC_NAMETABLES = 0,  /* the four nametables, 512 x 480, with every band's scroll window */
    NESVID_SRC_PATTERNS,        /* $0000 and $1000, 256 x 128, in the chosen palette */
    NESVID_SRC_SPRITES,         /* the 64 sprites where they sit, 256 x 240 */
    NESVID_SRC_PALETTE,         /* the 32 palette RAM entries */
    NESVID_SRC_COUNT
};

/* Overlays: one table, bit per entry. */
enum {
    NESVID_OV_GRID    = 1u << 0,    /* the 8 x 8 tile grid (follows each band's scroll on Display and Beam) */
    NESVID_OV_SPRITES = 1u << 1,    /* each sprite's box (Display, Beam, Sprites) */
    NESVID_OV_SCROLL  = 1u << 2,    /* the scroll windows of the bands (Nametables), the band edges (Display, Beam) */
    NESVID_OV_SPRITE0 = 1u << 3,    /* sprite 0 and where its hit landed (Display, Beam, Sprites) */
    NESVID_OV_BEAM    = 1u << 4,    /* where the beam is (stopped) */
    NESVID_OV_HEAT    = 1u << 5,    /* what was written to the PPU lately (Nametables, Values, Patterns, Palette) */
    NESVID_OV_ALL     = 0x3Fu
};
typedef struct {
    unsigned bit;
    const char *name;           /* the env name: grid, sprites, ... */
    const char *label;          /* the checkbox text */
} nesvid_overlay_info;
const nesvid_overlay_info *nesvid_overlays(int *count);
const char *nesvid_view_name(int view);             /* "Display", ... */
const char *nesvid_view_key(int view);              /* "display", ... (env) */
const char *nesvid_source_name(int source);         /* "Nametables", ... */
const char *nesvid_source_key(int source);          /* "nametables", ... */

/* Right-hand sections. */
enum { NESVID_S_PIXEL = 0, NESVID_S_REGISTERS, NESVID_S_COLOURS, NESVID_S_LINES,
       NESVID_S_TIMING, NESVID_S_SPRITES, NESVID_S_COUNT };
const char *nesvid_section_name(int section);
const char *nesvid_section_key(int section);

/* The tab asks the host to capture frames until it is freed. session is the
 * running session (its debugger is asked whether the machine is parked and
 * is given the write breakpoints); NULL (tests) makes every Break on... return
 * -1 and the machine count as running. */
nesvid_tab *nesvid_tab_new(nessession *session);
void        nesvid_tab_free(nesvid_tab *tab);

/* ---- what to show (they mark the picture stale) ------------------------------------ */
void     nesvid_tab_set_view(nesvid_tab *tab, int view);
int      nesvid_tab_view(const nesvid_tab *tab);
void     nesvid_tab_set_source(nesvid_tab *tab, int source);
int      nesvid_tab_source(const nesvid_tab *tab);
void     nesvid_tab_set_overlays(nesvid_tab *tab, unsigned ovl);
unsigned nesvid_tab_overlays(const nesvid_tab *tab);
/* 1, 2, 3, 4, 6, 8, 12, 16, or 0 for "fit the viewport" (the largest of
 * those that fits both ways) */
void     nesvid_tab_set_zoom(nesvid_tab *tab, int zoom);
int      nesvid_tab_zoom(const nesvid_tab *tab);
/* the room the picture has, for Fit; call when the widget resizes */
void     nesvid_tab_set_viewport(nesvid_tab *tab, int wide, int high);
/* the zoom steps the chooser offers (0 = Fit first) */
const int *nesvid_zooms(int *count);
/* The Patterns source draws in one of eight palettes: 0-3 background, 4-7 sprite. */
void     nesvid_tab_set_palette(nesvid_tab *tab, int palette);
int      nesvid_tab_palette(const nesvid_tab *tab);

/*
 * The scripted-capture variables, read once by nesvid_tab_new:
 *   NES_DEBUGGER_VIDEO_VIEW      display decoded beam values
 *   NES_DEBUGGER_VIDEO_SOURCE    nametables patterns sprites palette   (the Decoded view's source)
 *   NES_DEBUGGER_VIDEO_OVERLAYS  grid,sprites,scroll,sprite0,beam,heat | all | none
 *   NES_DEBUGGER_VIDEO_ZOOM      1..16 or fit
 *   NES_DEBUGGER_VIDEO_SECTION   pixel registers colours lines timing sprites   (names, never indexes)
 * Returns the starting section, NESVID_S_PIXEL when unset.
 */
int nesvid_tab_env_section(const nesvid_tab *tab);

/* ---- refresh ------------------------------------------------------------------------ */
#define NESVID_TAB_PICTURE 1u   /* repaint the picture (nesvid_tab_picture) */
#define NESVID_TAB_TABLES  2u   /* refill the status line and the sections */
#define NESVID_TAB_OFF     4u   /* no machine: show nesvid_tab_why() instead of a picture */

/* Take the machine's state, update the trackers, and say what to redraw.
 * force: redraw everything. */
unsigned nesvid_tab_refresh(nesvid_tab *tab, int force);

/* For tests: use this capture instead of the machine. stopped: the machine
 * counts as parked, so the Beam view works (cap->live says the PPU part is a
 * parked machine's, with cap->beam_line the beam). */
void nesvid_tab_feed(nesvid_tab *tab, const nesvid_capture *cap, int stopped);

/* ---- the picture ------------------------------------------------------------------- */
/*
 * The rendered picture, 0xFFRRGGBB, valid until the next refresh or set_*. NULL
 * when this view cannot be drawn: nesvid_tab_why() says why, and the tab
 * should show that text.
 */
const uint32_t *nesvid_tab_picture(nesvid_tab *tab, int *wide, int *high);
const char     *nesvid_tab_why(const nesvid_tab *tab);
int             nesvid_tab_effective_zoom(const nesvid_tab *tab);

/* ---- the tables -------------------------------------------------------------------- */
const char *nesvid_tab_status(const nesvid_tab *tab);       /* two lines, '\n' between */

/* The PPU's registers. */
typedef struct {
    const char *name;           /* "PPUCTRL", ... */
    char value[16];             /* "90" */
    char meaning[240];
    int age;                    /* refreshes since it last changed (NESVID_REG_FADE+: not recent) */
    int writes;                 /* writes (reads, for PPUSTATUS) in the logged frame */
    int first_line;             /* the first on a picture line, -1 when none */
} nesvid_reg;
#define NESVID_REG_FADE 12
int nesvid_tab_regs(const nesvid_tab *tab, const nesvid_reg **out);

/* The 32 palette RAM entries. */
typedef struct {
    int slot;                   /* 0..31 */
    const char *name;           /* "BG 2.3", "Sprite 1.2", "Backdrop" */
    int colour;                 /* the master colour 0-63 */
    uint32_t rgb;               /* 0xFFRRGGBB */
    long uses;                  /* pixels of the finished frame with exactly this colour (shared by slots with the same colour) */
    char role[96];
    int inspected;              /* the inspected pixel uses it */
} nesvid_swatch;
int nesvid_tab_swatches(const nesvid_tab *tab, const nesvid_swatch **out);

/* The Lines table: one entry for each band of lines drawn from the same
 * state (a program that never changes it in the frame has one). */
typedef struct {
    int first, last;            /* picture lines, inclusive */
    int scroll_x, scroll_y;     /* the scroll position at the band's first line, in the 512 x 480 plane */
    int ctrl, mask, fine_x;
    unsigned vaddr;             /* the VRAM address its first line fetches from */
    int writes;                 /* fields that changed on its first line: $2000, $2001, X scroll, Y scroll */
    int mapper_writes;          /* mapper register writes on its lines */
    char text[320];             /* ready-made line */
} nesvid_band;
int nesvid_tab_lines(const nesvid_tab *tab, const nesvid_band **out);

/* The sprites section: the 64 OAM entries as ready-made lines. */
typedef struct {
    int index;
    int visible;                /* inside the picture */
    char text[120];
} nesvid_spriterow;
int nesvid_tab_sprites(const nesvid_tab *tab, const nesvid_spriterow **out);

/* the timing section, as ready-made "label\tvalue" lines */
int  nesvid_tab_timing(const nesvid_tab *tab, const char *const **lines);
/* where the beam is in the whole frame, 0..1000 (0 when unknown) */
int  nesvid_tab_beam_permille(const nesvid_tab *tab);

/* A section as plain text (the console's `video [section]` shares it). */
int  nesvid_tab_text(nesvid_tab *tab, int section, char *dst, int dstsz);
/* A section by name or prefix ("pixel", "reg", ...); -1 if none. */
int  nesvid_section_from_name(const char *name);
/* The console's `video`: brings a tab up for the call, no window needed. The
 * session is the one the newest nesvid_tab_new was given. section NULL or ""
 * is the status and the registers. Returns the length. */
int  nesvid_tab_text_for(nesdebug *dbg, const char *section, char *dst, int dstsz);

/* Installs nesvid_tab_text_for as the console's `video [section]` text
 * (nesdebug_set_video_text) for dbg. Call once the engine exists. */
void nesvid_tab_register_console(nesdebug *dbg);

/* ---- hover, pin ---------------------------------------------------------------------- */
enum { NESVID_PK_NONE = 0, NESVID_PK_BACKDROP, NESVID_PK_TILE, NESVID_PK_SPRITE,
       NESVID_PK_PATTERN, NESVID_PK_PALETTE, NESVID_PK_VALUE };
/* What Break on writes watches for each target: the CPU bus (a PPU register
 * range), the PPU bus (nametables, attributes, pattern bytes), the palette,
 * or OAM. These are the nesdebug_space values the targets are added in. */
enum { NESVID_BK_CPU = 0, NESVID_BK_PPU = 1, NESVID_BK_PALETTE = 2, NESVID_BK_OAM = 3 };
#define NESVID_MAX_BREAK 8
#define NESVID_NO_ADDR 0xFFFFFFFFu

typedef struct {
    int valid;
    int picx, picy;             /* picture pixel (base picture, before the zoom) */
    int kind;                   /* NESVID_PK_* */
    int markx, marky, markw, markh;     /* the object under the pointer, in picture pixels */
    int screenx, screeny;         /* the screen pixel (Display, Beam, Sprites), -1 when the view has none */
    int line;                   /* the picture line, -1 when none */
    int colour;                 /* master colour 0-63 of the pixel, -1 if none */
    int slot;                   /* palette RAM entry it came from, -1 if none */
    int cell;                   /* nametable entry under it (0-3839), -1 */
    int ntab;                     /* the nametable, -1 */
    int tile;                   /* the tile it shows, -1 */
    int sprite;                 /* the front sprite covering it, -1 */
    uint64_t sprite_mask;       /* every sprite box that holds it, bit per sprite */
    unsigned ppu_addr;          /* the PPU address the pixel is made from (name entry, pattern byte, ...), NESVID_NO_ADDR if none */
    int mem_space;              /* the Memory tab's space for it: nesdebug_space (PPU, PALETTE, OAM), -1 */
    unsigned mem_addr;
    int nbreak;                 /* write breakpoint targets */
    int break_space[NESVID_MAX_BREAK];  /* NESVID_BK_* */
    unsigned break_lo[NESVID_MAX_BREAK], break_hi[NESVID_MAX_BREAK];
    uint32_t rgb;               /* 0xFFRRGGBB */
    char text[1024];            /* ready-made inspector text */
} nesvid_pixel;

/* x, y in picture pixels at the current zoom; -1,-1 when the pointer left.
 * Returns 1 when the inspector changed. */
int  nesvid_tab_hover(nesvid_tab *tab, int posx, int posy);
void nesvid_tab_pin(nesvid_tab *tab, int posx, int posy);       /* a click; the same cell unpins */
void nesvid_tab_unpin(nesvid_tab *tab);
/* the pinned pixel if any, else the hovered one, else NULL */
const nesvid_pixel *nesvid_tab_pixel(const nesvid_tab *tab);
/* rectangles x,y,w,h in picture pixels (zoom applied) for the marks drawn over
 * the picture; bit0 of the result: hover valid, bit1: pin valid */
int  nesvid_tab_marks(const nesvid_tab *tab, int hover[4], int pin[4]);
/*
 * "Break on writes": write breakpoints on what the inspected pixel is made
 * from, through nesdebug_breakpoint_add_in:
 *   - a name table entry, its attribute byte and its pattern bytes: PPU-space
 *     writes (the entry in every nametable the mirroring shows it in);
 *   - the palette entries it was coloured from: palette-space writes;
 *   - a sprite: its four OAM bytes (OAM space) and its pattern bytes;
 *   - on a Display or Beam pixel at a band edge, the CPU-space writes to the
 *     registers that changed there ($2000, $2001, $2005/$2006).
 * Not added twice. Returns the first breakpoint's id, or -1 (no target, no
 * debugger, or the engine refused). */
int  nesvid_tab_break_on_pixel(nesvid_tab *tab);
/* 1 when "Break on writes" has something to break on now */
int  nesvid_tab_can_break(const nesvid_tab *tab);
/* A CPU-space write breakpoint on one of the Registers section's rows
 * (PPUCTRL $2000, PPUMASK $2001, OAMADDR $2003, PPUSCROLL $2005, PPUADDR $2006);
 * -1 for the others or without a debugger. */
int  nesvid_tab_break_on_register(nesvid_tab *tab, int row);

#ifdef __cplusplus
}
#endif

#endif
