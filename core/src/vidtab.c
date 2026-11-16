/*
 * vidtab.c -- the Video tab's controller (core/include/nesvidtab.h).
 *
 * Everything here works from a nesvid_capture: the finished frame, the PPU's
 * memory and registers at its end, and the per-line log of the state each
 * line started from. The Decoded views rebuild their picture from the PPU's
 * memory through the ppuview renderers; the log's bands say where a frame
 * changed scroll, pattern table or mask, and the inspector re-does the PPU's
 * own fetch for the pixel under the pointer, line by line.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nesvidtab.h"

#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "ppumodel.h"
#include "ppuview.h"

#define MAXPIX 6000000L         /* the largest picture we will build */
#define MAX_TIMING 24
#define MAX_BANDS 64
#define HEAT_FRAMES 24          /* changes are drawn this many captures */
#define NT_CELLS (4 * 960)
#define SPRITES 64
#define N_REGS 7
#define REG_CTRL 0
#define REG_MASK 1
#define REG_STATUS 2
#define REG_OAMADDR 3
#define REG_SCROLL 4
#define REG_VADDR 5
#define REG_TADDR 6

static const int k_zooms[] = { 0, 1, 2, 3, 4, 6, 8, 12, 16 };

enum { BK_FRAME = 0, BK_NT, BK_VALUES, BK_PATTERNS, BK_SPRITES, BK_PALETTE };   /* what the base picture is */
enum { CH_CTRL = 1, CH_MASK = 2, CH_SCROLL = 4 };                               /* what changed at a band's edge */

#define COL_BLACK 0xff000000u

typedef struct {
    int first, last;                /* picture lines, inclusive */
    nesvid_line start;              /* the state of its first line */
    int scrollx, scrolly;           /* the scroll position there, in the 512 x 480 plane */
    int writes;                     /* fields that changed on its first line */
    int changed;                    /* CH_* */
    int mapper_writes;
    int mid;                        /* some line of it saw a mid-line register write */
} band_rec;

struct nesvid_tab {
    nessession *session;
    nesdebug *dbg;
    int view, source, zoom, vieww, viewh, palette;
    unsigned ovl;
    int env_section, capture_on;

    /* the machine's state as presented */
    nesvid_capture *cap;            /* working capture */
    nesvid_capture *last;           /* last captured by the emulator thread */
    uint64_t last_serial, cap_serial;
    unsigned live_gen;              /* the debugger generation cap's live part was read at */
    int refill;                     /* read the parked machine again whatever the generation */
    int have, has_fb, has_live, stopped, fed;
    uint64_t sig, tsig;
    int live_valid;                 /* cap holds a live read of the parked machine */

    /* the lines the picture was drawn from, and their bands */
    nesvid_line eff[NESVID_H];
    int eff_logged;                 /* the lines come from a log (else from the registers as they stand) */
    band_rec bands[MAX_BANDS];
    int nbands;
    int main_band;                  /* the band with the most lines */

    /* what changed lately */
    uint32_t prev_reg[N_REGS];
    int reg_age[N_REGS];
    int tracked;
    uint8_t prev_nt[NESVID_NT];
    uint8_t prev_chr[NESVID_CHR];
    uint8_t prev_pal[32];
    uint8_t prev_oam[256];
    int heat_nt[NT_CELLS];          /* nametable * 960 + row * 32 + col */
    int heat_tile[512];             /* pattern tiles */
    int heat_pal[32];
    int heat_spr[SPRITES];

    /* the picture */
    uint32_t *base, *pic;
    size_t base_cap, pic_cap;
    int basew, baseh, picw, pich, effzoom, kind;
    int dirty;
    char why[200];

    /* tables */
    nesvid_reg regs[N_REGS];
    int nregs;
    nesvid_swatch swat[32];
    char swat_names[32][24];
    nesvid_band lines[MAX_BANDS];
    int nlines;
    nesvid_spriterow srows[SPRITES];
    char status[400];
    char timing_buf[MAX_TIMING][224];
    const char *timing_ptr[MAX_TIMING];
    int ntiming;

    /* pixels */
    int hovx, hovy, pinx, piny;     /* -1: none */
    nesvid_pixel hover, pin;
};

/* ---- tables of names ----------------------------------------------------------------- */

static const nesvid_overlay_info k_overlays[] = {
    { NESVID_OV_GRID,    "grid",    "Tile grid" },
    { NESVID_OV_SPRITES, "sprites", "Sprite boxes" },
    { NESVID_OV_SCROLL,  "scroll",  "Scroll windows" },
    { NESVID_OV_SPRITE0, "sprite0", "Sprite 0" },
    { NESVID_OV_BEAM,    "beam",    "Beam" },
    { NESVID_OV_HEAT,    "heat",    "Recent PPU writes" },
};
#define N_OVERLAYS ((int)(sizeof k_overlays / sizeof *k_overlays))

const nesvid_overlay_info *nesvid_overlays(int *count)
{
    if (count) *count = N_OVERLAYS;
    return k_overlays;
}

static const char *const k_view_names[] = { "Display", "Decoded", "Beam", "Values" };
static const char *const k_view_keys[] = { "display", "decoded", "beam", "values" };
static const char *const k_src_names[] = { "Nametables", "Patterns", "Sprites", "Palette" };
static const char *const k_src_keys[] = { "nametables", "patterns", "sprites", "palette" };
static const char *const k_sec_names[] = { "Pixel", "Registers", "Colours", "Lines", "Timing", "Sprites" };
static const char *const k_sec_keys[] = { "pixel", "registers", "colours", "lines", "timing", "sprites" };

const char *nesvid_view_name(int val) { return val >= 0 && val < NESVID_V_COUNT ? k_view_names[val] : ""; }
const char *nesvid_view_key(int val) { return val >= 0 && val < NESVID_V_COUNT ? k_view_keys[val] : ""; }
const char *nesvid_source_name(int val) { return val >= 0 && val < NESVID_SRC_COUNT ? k_src_names[val] : ""; }
const char *nesvid_source_key(int val) { return val >= 0 && val < NESVID_SRC_COUNT ? k_src_keys[val] : ""; }
const char *nesvid_section_name(int sec) { return sec >= 0 && sec < NESVID_S_COUNT ? k_sec_names[sec] : ""; }
const char *nesvid_section_key(int sec) { return sec >= 0 && sec < NESVID_S_COUNT ? k_sec_keys[sec] : ""; }

int nesvid_section_from_name(const char *name)
{
    if (!name || !*name) return -1;
    if (!strcasecmp(name, "regs")) return NESVID_S_REGISTERS;
    if (!strcasecmp(name, "oam")) return NESVID_S_SPRITES;
    for (int sec = 0; sec < NESVID_S_COUNT; sec++)
        if (!strncasecmp(name, k_sec_keys[sec], strlen(name))) return sec;
    return -1;
}

const int *nesvid_zooms(int *count)
{
    if (count) *count = (int)(sizeof k_zooms / sizeof *k_zooms);
    return k_zooms;
}

/* ---- small helpers ---------------------------------------------------------------------- */

static uint64_t fnv(uint64_t high, const void *datap, size_t num)
{
    const uint8_t *byte = datap;
    while (num--) { high ^= *byte++; high *= 1099511628211ull; }
    return high;
}

static uint32_t blend(uint32_t dst, uint32_t rgb, int a256)
{
    const uint32_t dstr = (dst >> 16) & 255, dstg = (dst >> 8) & 255, dstb = dst & 255;
    const uint32_t srcr = (rgb >> 16) & 255, srcg = (rgb >> 8) & 255, srcb = rgb & 255;
    const uint32_t ret = (dstr * (uint32_t)(256 - a256) + srcr * (uint32_t)a256) >> 8;
    const uint32_t grn = (dstg * (uint32_t)(256 - a256) + srcg * (uint32_t)a256) >> 8;
    const uint32_t blu = (dstb * (uint32_t)(256 - a256) + srcb * (uint32_t)a256) >> 8;
    return 0xff000000u | ret << 16 | grn << 8 | blu;
}

static int fits_zoom(int wide, int high, int scale)
{
    return (long)wide * scale * (long)high * scale <= MAXPIX;
}

static void why_set(nesvid_tab *tab, const char *str)
{
    snprintf(tab->why, sizeof tab->why, "%s", str);
}

static int ensure(uint32_t **buf, size_t *cap, size_t need)
{
    if (need > *cap) {
        uint32_t *grown = realloc(*buf, need * sizeof **buf);
        if (!grown) return 0;
        *buf = grown;
        *cap = need;
    }
    return 1;
}

/* the false colour of a byte: red climbs with the value, green is a triangle
 * of it, blue falls with the high nibble */
static uint32_t value_colour(unsigned byte)
{
    const int low = (int)(byte & 255), high = (int)(byte >> 4);
    const int tri = low < 128 ? low * 2 : (255 - low) * 2;
    return 0xff000000u | (uint32_t)(40 + low * 215 / 255) << 16 |
           (uint32_t)(40 + tri * 160 / 255) << 8 | (uint32_t)(255 - high * 200 / 15);
}

static int text_add(char *dst, size_t size, int num, const char *fmt, ...)
{
    va_list args;
    int add;
    if (num >= (int)size - 1) return num;
    va_start(args, fmt);
    add = vsnprintf(dst + num, size - (size_t)num, fmt, args);
    va_end(args);
    if (add < 0) return num;
    return num + add < (int)size ? num + add : (int)size - 1;
}

static int bg_table_of(int ctrl) { return (ctrl & 0x10) ? 0x1000 : 0; }
static int spr_table_of(int ctrl) { return (ctrl & 0x08) ? 0x1000 : 0; }
static int tall_of(int ctrl) { return (ctrl & 0x20) != 0; }
static int rendering_of(int mask) { return (mask & 0x18) != 0; }

static const char *region_name(int region)
{
    return region == NESVID_PAL ? "PAL" : region == NESVID_DENDY ? "Dendy" : "NTSC";
}

/* ---- the lines the frame was drawn from, and their bands ------------------------------------ */

/* The state each picture line started from: while parked, the lines the beam
 * has started come from the frame in progress and the rest from the last
 * finished frame; with no log at all, the registers as they stand. */
static void merge_lines(nesvid_tab *tab)
{
    const nesvid_capture *cap = tab->cap;
    const int live_log = tab->has_live && cap->cur_valid;

    tab->eff_logged = 0;
    for (int line = 0; line < NESVID_H; line++) {
        nesvid_line *dst = &tab->eff[line];
        if (live_log && line <= cap->clog.last_line && cap->clog.line[line].valid) {
            *dst = cap->clog.line[line];
            tab->eff_logged = 1;
        } else if (cap->log_valid && cap->flog.line[line].valid) {
            *dst = cap->flog.line[line];
            tab->eff_logged = 1;
        } else {
            memset(dst, 0, sizeof *dst);
            dst->ctrl = cap->ctrl;
            dst->mask = cap->mask;
            dst->fine_x = cap->fine_x;
            dst->vaddr = cap->taddr;
            dst->taddr = cap->taddr;
        }
    }
}

static void compute_bands(nesvid_tab *tab)
{
    int best = 0, best_lines = -1;
    tab->nbands = 0;
    for (int line = 0; line < NESVID_H; line++) {
        const nesvid_line *cur = &tab->eff[line];
        int posx, posy, changed = 0, breaks = 0;
        nesvid_line_scroll(cur, &posx, &posy);
        if (tab->nbands) {
            band_rec *prev = &tab->bands[tab->nbands - 1];
            const nesvid_line *before = &tab->eff[line - 1];
            int prevx, prevy;
            nesvid_line_scroll(before, &prevx, &prevy);
            if ((cur->ctrl ^ before->ctrl) & 0xFC) changed |= CH_CTRL;
            if (cur->mask != before->mask) changed |= CH_MASK;
            if (posx != prevx || posy != (prevy + 1) % 480) changed |= CH_SCROLL;
            breaks = changed != 0;
            if (breaks && tab->nbands < MAX_BANDS) {
                band_rec *band = &tab->bands[tab->nbands++];
                memset(band, 0, sizeof *band);
                band->first = band->last = line;
                band->start = *cur;
                band->scrollx = posx; band->scrolly = posy;
                band->changed = changed;
                band->writes = ((changed & CH_CTRL) != 0) + ((changed & CH_MASK) != 0) + ((changed & CH_SCROLL) != 0);
                band->mapper_writes = cur->mapper_writes;
                band->mid = cur->mid;
                continue;
            }
            prev->last = line;
            prev->mapper_writes += cur->mapper_writes;
            if (cur->mid) prev->mid = 1;
            continue;
        }
        {
            band_rec *band = &tab->bands[tab->nbands++];
            memset(band, 0, sizeof *band);
            band->first = band->last = line;
            band->start = *cur;
            band->scrollx = posx; band->scrolly = posy;
            band->mapper_writes = cur->mapper_writes;
            band->mid = cur->mid;
        }
    }
    for (int bnd = 0; bnd < tab->nbands; bnd++) {
        const int span = tab->bands[bnd].last - tab->bands[bnd].first + 1;
        if (span > best_lines) { best_lines = span; best = bnd; }
    }
    tab->main_band = best;
}

static const band_rec *band_of_line(const nesvid_tab *tab, int line)
{
    for (int bnd = 0; bnd < tab->nbands; bnd++)
        if (line >= tab->bands[bnd].first && line <= tab->bands[bnd].last) return &tab->bands[bnd];
    return &tab->bands[tab->nbands ? tab->nbands - 1 : 0];
}

/* ---- new / free / settings -------------------------------------------------------------- */

static nessession *s_last_session;     /* the console's `video` reads through the newest tab's session */
static int s_tab_count;                /* tabs alive; the session is forgotten when the last goes */

nesvid_tab *nesvid_tab_new(nessession *session)
{
    nesvid_tab *tab = calloc(1, sizeof *tab);
    const char *envv;
    if (!tab) return NULL;
    tab->session = session;
    tab->dbg = session ? nessession_debugger(session) : NULL;
    tab->cap = calloc(1, sizeof *tab->cap);
    tab->last = calloc(1, sizeof *tab->last);
    if (!tab->cap || !tab->last) {
        nesvid_tab_free(tab);
        return NULL;
    }
    tab->view = NESVID_V_DISPLAY;
    tab->source = NESVID_SRC_NAMETABLES;
    tab->ovl = NESVID_OV_SPRITES;
    tab->zoom = 0;
    tab->vieww = NESVID_W;
    tab->viewh = NESVID_H;
    tab->hovx = tab->hovy = tab->pinx = tab->piny = -1;
    tab->dirty = 1;

    if ((envv = getenv("NES_DEBUGGER_VIDEO_VIEW")) && *envv) {
        for (int item = 0; item < NESVID_V_COUNT; item++)
            if (!strncasecmp(envv, k_view_keys[item], strlen(envv))) { tab->view = item; break; }
    }
    if ((envv = getenv("NES_DEBUGGER_VIDEO_SOURCE")) && *envv) {
        for (int item = 0; item < NESVID_SRC_COUNT; item++)
            if (!strncasecmp(envv, k_src_keys[item], strlen(envv))) { tab->source = item; break; }
    }
    if ((envv = getenv("NES_DEBUGGER_VIDEO_OVERLAYS"))) {
        char buf[200], *tok;
        unsigned ovl = 0;
        snprintf(buf, sizeof buf, "%s", envv);
        for (tok = strtok(buf, ", "); tok; tok = strtok(NULL, ", ")) {
            if (!strcasecmp(tok, "all")) ovl |= NESVID_OV_ALL;
            else if (!strcasecmp(tok, "none")) ovl = 0;
            else
                for (int item = 0; item < N_OVERLAYS; item++)
                    if (!strcasecmp(tok, k_overlays[item].name)) ovl |= k_overlays[item].bit;
        }
        tab->ovl = ovl;
    }
    if ((envv = getenv("NES_DEBUGGER_VIDEO_ZOOM")) && *envv) {
        if (!strcasecmp(envv, "fit")) tab->zoom = 0;
        else {
            const int scale = atoi(envv);
            for (size_t item = 1; item < sizeof k_zooms / sizeof *k_zooms; item++)
                if (k_zooms[item] == scale) tab->zoom = scale;
        }
    }
    tab->env_section = NESVID_S_PIXEL;
    if ((envv = getenv("NES_DEBUGGER_VIDEO_SECTION")) && *envv) {
        const int sec = nesvid_section_from_name(envv);
        if (sec >= 0) tab->env_section = sec;
    }
    if (session) {
        nesvid_host_capture_enable(session, 1);
        tab->capture_on = 1;
        s_last_session = session;
        s_tab_count++;
    }
    return tab;
}

void nesvid_tab_free(nesvid_tab *tab)
{
    if (!tab) return;
    if (tab->capture_on) {
        nesvid_host_capture_enable(tab->session, 0);
        if (s_tab_count > 0 && --s_tab_count == 0) s_last_session = NULL;
    }
    free(tab->cap);
    free(tab->last);
    free(tab->base);
    free(tab->pic);
    free(tab);
}

void nesvid_tab_set_view(nesvid_tab *tab, int val)
{
    if (tab && val >= 0 && val < NESVID_V_COUNT && val != tab->view) { tab->view = val; tab->dirty = 1; }
}
int nesvid_tab_view(const nesvid_tab *tab) { return tab ? tab->view : 0; }
void nesvid_tab_set_source(nesvid_tab *tab, int val)
{
    if (tab && val >= 0 && val < NESVID_SRC_COUNT && val != tab->source) { tab->source = val; tab->dirty = 1; }
}
int nesvid_tab_source(const nesvid_tab *tab) { return tab ? tab->source : 0; }
void nesvid_tab_set_overlays(nesvid_tab *tab, unsigned ovl)
{
    if (tab && (ovl & NESVID_OV_ALL) != tab->ovl) { tab->ovl = ovl & NESVID_OV_ALL; tab->dirty = 1; }
}
unsigned nesvid_tab_overlays(const nesvid_tab *tab) { return tab ? tab->ovl : 0; }
void nesvid_tab_set_zoom(nesvid_tab *tab, int scale)
{
    if (tab && scale >= 0 && scale <= 16 && scale != tab->zoom) { tab->zoom = scale; tab->dirty = 1; }
}
int nesvid_tab_zoom(const nesvid_tab *tab) { return tab ? tab->zoom : 0; }
void nesvid_tab_set_palette(nesvid_tab *tab, int palette)
{
    if (tab && palette >= 0 && palette < 8 && palette != tab->palette) { tab->palette = palette; tab->dirty = 1; }
}
int nesvid_tab_palette(const nesvid_tab *tab) { return tab ? tab->palette : 0; }
void nesvid_tab_set_viewport(nesvid_tab *tab, int wide, int high)
{
    if (!tab) return;
    if (wide < 1) wide = 1;
    if (high < 1) high = 1;
    if (wide != tab->vieww || high != tab->viewh) {
        tab->vieww = wide; tab->viewh = high;
        if (tab->zoom == 0) tab->dirty = 1;
    }
}
int nesvid_tab_env_section(const nesvid_tab *tab) { return tab ? tab->env_section : 0; }

/* ---- feeding ----------------------------------------------------------------------------- */

static void present(nesvid_tab *tab)
{
    merge_lines(tab);
    compute_bands(tab);
}

void nesvid_tab_feed(nesvid_tab *tab, const nesvid_capture *cap, int stopped)
{
    if (!tab || !cap) return;
    memcpy(tab->cap, cap, sizeof *tab->cap);
    tab->fed = 1;
    tab->have = 1;
    tab->has_fb = cap->fb_valid || cap->live_pixels_valid;
    tab->stopped = stopped != 0;
    tab->has_live = tab->stopped;
    present(tab);
}

static int dbg_stopped(const nesvid_tab *tab)
{
    return tab->dbg && nesdebug_is_stopped(tab->dbg);
}

/* Take what the machine has. 0 if there is no machine or no frame yet. */
static int acquire(nesvid_tab *tab)
{
    int copied = 0;

    if (tab->fed)
        return tab->have;
    tab->have = 0;
    if (!tab->session)
        return 0;
    (void)nesvid_host_capture_get(tab->session, tab->last, &tab->last_serial);
    tab->stopped = dbg_stopped(tab);
    tab->has_live = tab->stopped;

    if (tab->last->serial != 0 && (tab->cap_serial != tab->last->serial || (tab->live_valid && !tab->stopped))) {
        memcpy(tab->cap, tab->last, sizeof *tab->cap);
        tab->cap_serial = tab->last->serial;
        tab->live_valid = 0;
        copied = 1;
    }
    if (tab->stopped) {
        /* A parked machine does not change on its own: read it once per stop
         * (the generation moves when anything ran or was edited). The last
         * finished frame stays what the Display view shows. */
        const unsigned gen = tab->dbg ? nesdebug_generation(tab->dbg) : 0;
        if (copied || !tab->live_valid || tab->refill || gen != tab->live_gen) {
            if (!nesvid_host_capture_fill(tab->session, tab->cap))
                return 0;
            tab->live_valid = 1;
            tab->live_gen = gen;
            tab->refill = 0;
            copied = 1;
        }
    } else if (tab->cap_serial == 0) {
        return 0;
    }
    tab->has_fb = tab->cap->fb_valid || tab->cap->live_pixels_valid;
    tab->have = 1;
    if (copied || tab->dirty)
        present(tab);
    return 1;
}

/* The picture and what it is made from. */
static uint64_t picture_signature(const nesvid_tab *tab)
{
    const nesvid_capture *cap = tab->cap;
    uint64_t high = 1469598103934665603ull;
    high = fnv(high, &cap->mem_valid, sizeof cap->mem_valid);
    if (cap->mem_valid) {
        high = fnv(high, cap->chr, sizeof cap->chr);
        high = fnv(high, cap->ntab, sizeof cap->ntab);
        high = fnv(high, cap->pal, sizeof cap->pal);
        high = fnv(high, cap->oam, sizeof cap->oam);
        high = fnv(high, cap->rgb, sizeof cap->rgb);
        high = fnv(high, &cap->ctrl, sizeof cap->ctrl);
        high = fnv(high, &cap->mask, sizeof cap->mask);
        high = fnv(high, &cap->status, sizeof cap->status);
        high = fnv(high, &cap->vaddr, sizeof cap->vaddr);
        high = fnv(high, &cap->taddr, sizeof cap->taddr);
        high = fnv(high, &cap->fine_x, sizeof cap->fine_x);
    }
    high = fnv(high, &cap->fb_valid, sizeof cap->fb_valid);
    high = fnv(high, &cap->live_pixels_valid, sizeof cap->live_pixels_valid);
    if (cap->fb_valid) high = fnv(high, cap->pixels, sizeof cap->pixels);
    if (cap->live_pixels_valid) high = fnv(high, cap->live_pixels, sizeof cap->live_pixels);
    high = fnv(high, &cap->log_valid, sizeof cap->log_valid);
    if (cap->log_valid) high = fnv(high, cap->flog.line, sizeof cap->flog.line);
    high = fnv(high, &cap->cur_valid, sizeof cap->cur_valid);
    if (cap->cur_valid) high = fnv(high, cap->clog.line, sizeof cap->clog.line);
    high = fnv(high, &cap->beam_line, sizeof cap->beam_line);
    high = fnv(high, &cap->beam_dot, sizeof cap->beam_dot);
    high = fnv(high, &cap->live, sizeof cap->live);
    high = fnv(high, &tab->has_live, sizeof tab->has_live);
    high = fnv(high, &tab->has_fb, sizeof tab->has_fb);
    return high;
}

/* Everything the tables show. */
static uint64_t table_signature(const nesvid_tab *tab, uint64_t picture)
{
    uint64_t high = picture;
    high = fnv(high, &tab->cap->frame, sizeof tab->cap->frame);
    high = fnv(high, &tab->cap->cycles, sizeof tab->cap->cycles);
    return high;
}

/* The value each Registers row shows, for the change trackers. */
static uint32_t reg_value(const nesvid_capture *cap, int reg)
{
    switch (reg) {
    case REG_CTRL: return cap->ctrl;
    case REG_MASK: return cap->mask;
    case REG_STATUS: return cap->status;
    case REG_OAMADDR: return cap->oam_addr;
    case REG_SCROLL: return ((uint32_t)cap->taddr << 3) | cap->fine_x;
    case REG_VADDR: return cap->vaddr;
    default: return cap->taddr;
    }
}

/* What changed since the capture before (registers, nametable entries,
 * pattern tiles, palette entries), for the heat overlay and the Registers
 * section. Called once per new capture. */
static void track(nesvid_tab *tab)
{
    const nesvid_capture *cap = tab->cap;
    const nesvid_frame_log *wlog = &cap->flog;

    for (int reg = 0; reg < N_REGS; reg++) {
        const uint32_t value = reg_value(cap, reg);
        if (!tab->tracked) tab->reg_age[reg] = NESVID_REG_FADE;
        else if (value != tab->prev_reg[reg]) tab->reg_age[reg] = 0;
        else if (tab->reg_age[reg] < NESVID_REG_FADE) tab->reg_age[reg]++;
        tab->prev_reg[reg] = value;
    }
    if (!cap->mem_valid) return;
    for (int cell = 0; cell < NT_CELLS; cell++) {
        const int ntab = cell / 960, off = cell % 960;
        const int idx = ntab * 0x400 + off;
        const int attr_idx = ntab * 0x400 + 0x3C0 + ((off / 32) / 4) * 8 + (off % 32) / 4;
        int hit = (tab->tracked && (cap->ntab[idx] != tab->prev_nt[idx] || cap->ntab[attr_idx] != tab->prev_nt[attr_idx]));
        if (!hit && cap->log_valid) {
            const unsigned bit = (unsigned)idx;
            hit = (wlog->wr_nt[bit >> 3] >> (bit & 7)) & 1;
        }
        if (hit) tab->heat_nt[cell] = HEAT_FRAMES;
        else if (tab->heat_nt[cell] > 0) tab->heat_nt[cell]--;
    }
    for (int tile = 0; tile < 512; tile++) {
        int hit = 0;
        for (int byte = 0; byte < 16 && !hit; byte++)
            if (tab->tracked && cap->chr[tile * 16 + byte] != tab->prev_chr[tile * 16 + byte]) hit = 1;
        if (!hit && cap->log_valid)
            for (int byte = 0; byte < 16 && !hit; byte++) {
                const unsigned bit = (unsigned)(tile * 16 + byte);
                hit = (wlog->wr_chr[bit >> 3] >> (bit & 7)) & 1;
            }
        if (hit) tab->heat_tile[tile] = HEAT_FRAMES;
        else if (tab->heat_tile[tile] > 0) tab->heat_tile[tile]--;
    }
    for (int slot = 0; slot < 32; slot++) {
        int hit = tab->tracked && cap->pal[slot] != tab->prev_pal[slot];
        if (!hit && cap->log_valid) hit = (wlog->wr_pal[slot >> 3] >> (slot & 7)) & 1;
        if (hit) tab->heat_pal[slot] = HEAT_FRAMES;
        else if (tab->heat_pal[slot] > 0) tab->heat_pal[slot]--;
    }
    for (int spr = 0; spr < SPRITES; spr++) {
        int hit = 0;
        for (int byte = 0; byte < 4 && !hit; byte++)
            if (tab->tracked && cap->oam[spr * 4 + byte] != tab->prev_oam[spr * 4 + byte]) hit = 1;
        if (hit) tab->heat_spr[spr] = HEAT_FRAMES;
        else if (tab->heat_spr[spr] > 0) tab->heat_spr[spr]--;
    }
    memcpy(tab->prev_nt, cap->ntab, sizeof tab->prev_nt);
    memcpy(tab->prev_chr, cap->chr, sizeof tab->prev_chr);
    memcpy(tab->prev_pal, cap->pal, sizeof tab->prev_pal);
    memcpy(tab->prev_oam, cap->oam, sizeof tab->prev_oam);
    tab->tracked = 1;
}

/* ---- the base picture (1x) -------------------------------------------------------------------- */

/* the picture row below which the beam has not drawn yet */
static int beam_row(const nesvid_capture *cap)
{
    if (cap->beam_line < -1) return 1 << 30;
    if (cap->beam_line >= NESVID_H) return NESVID_H;
    return cap->beam_line + 1;
}

static int build_decoded(nesvid_tab *tab)
{
    const nesvid_capture *cap = tab->cap;
    const int ctrl = tab->nbands ? tab->bands[tab->main_band].start.ctrl : cap->ctrl;

    if (!cap->mem_valid) {
        why_set(tab, "The PPU has not been read yet. Open the debugger (it attaches to the machine) and let a frame run.");
        return 0;
    }
    switch (tab->source) {
    case NESVID_SRC_PATTERNS:
        if (!ensure(&tab->base, &tab->base_cap, (size_t)NESPPU_PAT_W * NESPPU_PAT_H)) return 0;
        nesppu_render_patterns(cap, tab->palette, tab->base);
        tab->basew = NESPPU_PAT_W; tab->baseh = NESPPU_PAT_H;
        tab->kind = BK_PATTERNS;
        return 1;
    case NESVID_SRC_SPRITES:
        if (!ensure(&tab->base, &tab->base_cap, (size_t)NESPPU_SPR_W * NESPPU_SPR_H)) return 0;
        nesppu_render_sprites(cap, (unsigned)spr_table_of(cap->ctrl), tall_of(cap->ctrl), tab->base);
        tab->basew = NESPPU_SPR_W; tab->baseh = NESPPU_SPR_H;
        tab->kind = BK_SPRITES;
        return 1;
    case NESVID_SRC_PALETTE:
        if (!ensure(&tab->base, &tab->base_cap, (size_t)NESPPU_PAL_W * NESPPU_PAL_H)) return 0;
        nesppu_render_palette(cap, tab->base);
        tab->basew = NESPPU_PAL_W; tab->baseh = NESPPU_PAL_H;
        tab->kind = BK_PALETTE;
        return 1;
    default:
        if (!ensure(&tab->base, &tab->base_cap, (size_t)NESPPU_NT_W * NESPPU_NT_H)) return 0;
        nesppu_render_nametables(cap, (unsigned)bg_table_of(ctrl), tab->base);
        tab->basew = NESPPU_NT_W; tab->baseh = NESPPU_NT_H;
        tab->kind = BK_NT;
        return 1;
    }
}

/* The false colour of each nametable entry (the tiles, not the attributes). */
static int build_values(nesvid_tab *tab)
{
    const nesvid_capture *cap = tab->cap;
    if (!cap->mem_valid) {
        why_set(tab, "The PPU has not been read yet. Open the debugger (it attaches to the machine) and let a frame run.");
        return 0;
    }
    if (!ensure(&tab->base, &tab->base_cap, (size_t)NESPPU_NT_W * NESPPU_NT_H)) return 0;
    for (int ntab = 0; ntab < 4; ntab++)
        for (int cell = 0; cell < 960; cell++) {
            const uint32_t col = value_colour(cap->ntab[ntab * 0x400 + cell]);
            const int orgx = (ntab & 1) * 256 + (cell % 32) * 8, orgy = (ntab >> 1) * 240 + (cell / 32) * 8;
            for (int pixy = 0; pixy < 8; pixy++)
                for (int pixx = 0; pixx < 8; pixx++)
                    tab->base[(size_t)(orgy + pixy) * NESPPU_NT_W + (size_t)(orgx + pixx)] = col;
        }
    tab->basew = NESPPU_NT_W; tab->baseh = NESPPU_NT_H;
    tab->kind = BK_VALUES;
    return 1;
}

static int build_base(nesvid_tab *tab)
{
    const nesvid_capture *cap = tab->cap;

    if (tab->view == NESVID_V_DECODED) return build_decoded(tab);
    if (tab->view == NESVID_V_VALUES) return build_values(tab);
    if (!tab->has_fb) {
        why_set(tab, "No frame has been published yet. Let the machine run for a moment.");
        return 0;
    }
    {
        const uint32_t *src = cap->pixels;
        int below = 1 << 30;
        if (tab->view == NESVID_V_BEAM) {
            if (!tab->has_live || !cap->live_pixels_valid) {
                why_set(tab, "The Beam view needs the machine stopped: it shows the frame as the PPU is drawing it with the lines the beam has not reached yet dimmed.");
                return 0;
            }
            src = cap->live_pixels;
            below = beam_row(cap);
        } else if (!cap->fb_valid) {
            src = cap->live_pixels;     /* nothing finished yet: the frame as it stands */
        }
        if (!ensure(&tab->base, &tab->base_cap, (size_t)NESVID_W * NESVID_H)) return 0;
        for (int posy = 0; posy < NESVID_H; posy++)
            for (int posx = 0; posx < NESVID_W; posx++) {
                const uint32_t col = 0xff000000u | src[(size_t)posy * NESVID_W + (size_t)posx];
                tab->base[(size_t)posy * NESVID_W + (size_t)posx] = posy < below ? col : blend(col, 0x000000, 150);
            }
        tab->basew = NESVID_W; tab->baseh = NESVID_H;
        tab->kind = BK_FRAME;
    }
    return 1;
}

/* ---- overlays ---------------------------------------------------------------------------- */

typedef struct { uint32_t *pixp; int wide, high; } canvas;

static void px_blend(canvas *cnv, int posx, int posy, uint32_t rgb, int alpha)
{
    if (posx >= 0 && posy >= 0 && posx < cnv->wide && posy < cnv->high)
        cnv->pixp[(size_t)posy * (size_t)cnv->wide + (size_t)posx] = blend(cnv->pixp[(size_t)posy * (size_t)cnv->wide + (size_t)posx], rgb, alpha);
}
static void hline(canvas *cnv, int posy, int minx, int maxx, uint32_t rgb, int alpha)
{
    for (int posx = minx; posx < maxx; posx++) px_blend(cnv, posx, posy, rgb, alpha);
}
static void vline(canvas *cnv, int posx, int starty, int endy, uint32_t rgb, int alpha)
{
    for (int posy = starty; posy < endy; posy++) px_blend(cnv, posx, posy, rgb, alpha);
}
static void fill_rect(canvas *cnv, int posx, int posy, int wide, int high, uint32_t rgb, int alpha)
{
    for (int offy = 0; offy < high; offy++)
        for (int offx = 0; offx < wide; offx++) px_blend(cnv, posx + offx, posy + offy, rgb, alpha);
}
static void frame_rect(canvas *cnv, int posx, int posy, int wide, int high, int thick, uint32_t rgb, int alpha)
{
    for (int edge = 0; edge < thick; edge++) {
        hline(cnv, posy + edge, posx, posx + wide, rgb, alpha);
        hline(cnv, posy + high - 1 - edge, posx, posx + wide, rgb, alpha);
        vline(cnv, posx + edge, posy, posy + high, rgb, alpha);
        vline(cnv, posx + wide - 1 - edge, posy, posy + high, rgb, alpha);
    }
}

static uint32_t sprite_colour(int item)
{
    return 0xff000000u | (uint32_t)(60 + (item * 37) % 196) << 16 | (uint32_t)(255 - (item * 53) % 180) << 8 | (uint32_t)(90 + (item * 71) % 166);
}

static uint32_t band_colour(int bnd)
{
    static const uint32_t cols[] = { 0xff40ff80, 0xffffd040, 0xff40c0ff, 0xffff6090, 0xffc080ff, 0xffa0ff40 };
    return cols[bnd % 6];
}

/* a log's event count, however the capture came by it */
static int log_events(const nesvid_frame_log *logp)
{
    return logp->nevents < 0 ? 0 : logp->nevents > NESVID_MAX_EVENTS ? NESVID_MAX_EVENTS : logp->nevents;
}

/* the sprite 0 hit the log saw, as a screen position */
static int sprite0_hit(const nesvid_tab *tab, int *line, int *dot)
{
    const nesvid_capture *cap = tab->cap;
    const nesvid_frame_log *logs[2] = { tab->has_live && cap->cur_valid ? &cap->clog : NULL, cap->log_valid ? &cap->flog : NULL };
    for (int which = 0; which < 2; which++)
        if (logs[which])
            for (int item = 0; item < log_events(logs[which]); item++)
                if (logs[which]->events[item].kind == NESVID_EV_SPRITE0 && logs[which]->events[item].line >= 0) {
                    *line = logs[which]->events[item].line;
                    *dot = logs[which]->events[item].dot;
                    return 1;
                }
    return 0;
}

static void draw_tile_grid(canvas *cnv, int scale, int wide, int high, int step)
{
    for (int col = 0; col <= wide; col += step)
        vline(cnv, col * scale - (col == wide), 0, high * scale, 0xffffffff, col % 256 == 0 && wide > 256 ? 140 : 50);
    for (int row = 0; row <= high; row += step)
        hline(cnv, row * scale - (row == high), 0, wide * scale, 0xffffffff, row % 240 == 0 && high > 240 ? 140 : 50);
}

static void draw_overlays(nesvid_tab *tab, canvas *cnv, int scale)
{
    const nesvid_capture *cap = tab->cap;
    const int framed = tab->kind == BK_FRAME;
    const int planed = tab->kind == BK_NT || tab->kind == BK_VALUES;
    const int tall = tall_of(cap->ctrl);

    if ((tab->ovl & NESVID_OV_HEAT) && cap->mem_valid && tab->tracked) {
        if (planed) {
            for (int cell = 0; cell < NT_CELLS; cell++)
                if (tab->heat_nt[cell] > 0) {
                    const int ntab = cell / 960, off = cell % 960;
                    fill_rect(cnv, ((ntab & 1) * 256 + (off % 32) * 8) * scale, ((ntab >> 1) * 240 + (off / 32) * 8) * scale,
                              8 * scale, 8 * scale, 0xffff8020, 60 + tab->heat_nt[cell] * 6);
                }
        } else if (tab->kind == BK_PATTERNS) {
            for (int tile = 0; tile < 512; tile++)
                if (tab->heat_tile[tile] > 0) {
                    const int table = tile / 256, idx = tile % 256;
                    fill_rect(cnv, (table * 128 + (idx % 16) * 8) * scale, (idx / 16) * 8 * scale, 8 * scale, 8 * scale,
                              0xffff8020, 60 + tab->heat_tile[tile] * 6);
                }
        } else if (tab->kind == BK_PALETTE) {
            for (int slot = 0; slot < 32; slot++)
                if (tab->heat_pal[slot] > 0)
                    fill_rect(cnv, (slot % NESPPU_PAL_COLS) * NESPPU_PAL_CELL * scale, (slot / NESPPU_PAL_COLS) * NESPPU_PAL_CELL * scale,
                              NESPPU_PAL_CELL * scale, NESPPU_PAL_CELL * scale, 0xffff8020, 60 + tab->heat_pal[slot] * 6);
        } else if (tab->kind == BK_SPRITES) {
            for (int spr = 0; spr < SPRITES; spr++)
                if (tab->heat_spr[spr] > 0) {
                    nesppu_sprite placed;
                    nesppu_sprite_info(cap, spr, tall, &placed);
                    fill_rect(cnv, placed.left * scale, placed.top * scale, 8 * scale, placed.high * scale, 0xffff8020, 60 + tab->heat_spr[spr] * 6);
                }
        }
    }
    if (tab->ovl & NESVID_OV_GRID) {
        if (planed) {
            draw_tile_grid(cnv, scale, NESPPU_NT_W, NESPPU_NT_H, 8);
        } else if (tab->kind == BK_PATTERNS) {
            draw_tile_grid(cnv, scale, NESPPU_PAT_W, NESPPU_PAT_H, 8);
        } else if (framed) {
            /* each band's tiles sit where its own scroll puts them */
            for (int bnd = 0; bnd < tab->nbands; bnd++) {
                const band_rec *band = &tab->bands[bnd];
                for (int col = (8 - band->scrollx % 8) % 8; col < NESVID_W; col += 8)
                    vline(cnv, col * scale, band->first * scale, (band->last + 1) * scale, 0xffffffff, 50);
                for (int line = band->first; line <= band->last; line++)
                    if ((band->scrolly + line - band->first) % 8 == 0)
                        hline(cnv, line * scale, 0, NESVID_W * scale, 0xffffffff, 50);
            }
        }
    }
    if ((tab->ovl & NESVID_OV_SCROLL) && tab->kind == BK_NT) {
        /* every band's window in the 512 x 480 plane (it wraps) */
        for (int bnd = 0; bnd < tab->nbands; bnd++) {
            const band_rec *band = &tab->bands[bnd];
            const uint32_t col = band_colour(bnd);
            for (int line = band->first; line <= band->last; line++) {
                const int posy = (band->scrolly + line - band->first) % NESPPU_NT_H;
                const int edge = line == band->first || line == band->last;
                fill_rect(cnv, band->scrollx * scale, posy * scale, scale, scale, col, 240);
                fill_rect(cnv, ((band->scrollx + NESVID_W - 1) % NESPPU_NT_W) * scale, posy * scale, scale, scale, col, 240);
                if (edge)
                    for (int posx = 0; posx < NESVID_W; posx++)
                        fill_rect(cnv, ((band->scrollx + posx) % NESPPU_NT_W) * scale, posy * scale, scale, scale, col, 240);
            }
        }
    }
    if ((tab->ovl & NESVID_OV_SCROLL) && framed && tab->nbands > 1) {
        for (int bnd = 1; bnd < tab->nbands; bnd++)
            hline(cnv, tab->bands[bnd].first * scale, 0, NESVID_W * scale, band_colour(bnd), 220);
    }
    if ((tab->ovl & NESVID_OV_SPRITES) && (framed || tab->kind == BK_SPRITES) && cap->mem_valid) {
        for (int item = 0; item < SPRITES; item++) {
            nesppu_sprite placed;
            nesppu_sprite_info(cap, item, tall, &placed);
            if (!placed.on_screen) continue;
            frame_rect(cnv, placed.left * scale, placed.top * scale, 8 * scale, placed.high * scale, scale < 3 ? 1 : 2, sprite_colour(item), 220);
        }
    }
    if ((tab->ovl & NESVID_OV_SPRITE0) && (framed || tab->kind == BK_SPRITES) && cap->mem_valid) {
        nesppu_sprite placed;
        int line, dot;
        nesppu_sprite_info(cap, 0, tall, &placed);
        if (placed.on_screen) frame_rect(cnv, placed.left * scale - 1, placed.top * scale - 1, 8 * scale + 2, placed.high * scale + 2, 2, 0xffff40ff, 255);
        if (framed && sprite0_hit(tab, &line, &dot)) {
            const int posx = (dot > 0 ? dot - 1 : 0) * scale, posy = line * scale;
            hline(cnv, posy, posx - 4 * scale, posx + 5 * scale, 0xffff40ff, 255);
            vline(cnv, posx, posy - 4 * scale, posy + 5 * scale, 0xffff40ff, 255);
        }
    }
    if ((tab->ovl & NESVID_OV_BEAM) && tab->has_live && framed && cap->beam_line >= 0) {
        const int posy = cap->beam_line < NESVID_H ? cap->beam_line : NESVID_H - 1;
        hline(cnv, posy * scale, 0, tab->basew * scale, 0xffff4040, 230);
        if (cap->beam_line < NESVID_H && cap->beam_dot > 0 && cap->beam_dot < NESVID_W + 1)
            vline(cnv, (cap->beam_dot - 1) * scale, (posy > 4 ? posy - 4 : 0) * scale, (posy + 5) * scale, 0xffff4040, 230);
    }
}

static int pick_zoom(const nesvid_tab *tab)
{
    int scale = tab->zoom;
    if (scale == 0) {
        scale = 1;
        for (size_t item = 1; item < sizeof k_zooms / sizeof *k_zooms; item++)
            if (tab->basew * k_zooms[item] <= tab->vieww && tab->baseh * k_zooms[item] <= tab->viewh)
                scale = k_zooms[item];
    }
    while (scale > 1 && !fits_zoom(tab->basew, tab->baseh, scale)) {
        int lower = 1;
        for (size_t item = 1; item < sizeof k_zooms / sizeof *k_zooms; item++)
            if (k_zooms[item] < scale) lower = k_zooms[item];
        scale = lower;
    }
    return scale;
}

static void build_picture(nesvid_tab *tab)
{
    int scale;
    canvas cnv;

    tab->why[0] = '\0';
    tab->picw = tab->pich = 0;
    if (!build_base(tab))
        return;
    scale = pick_zoom(tab);
    tab->effzoom = scale;
    tab->picw = tab->basew * scale;
    tab->pich = tab->baseh * scale;
    if (!ensure(&tab->pic, &tab->pic_cap, (size_t)tab->picw * (size_t)tab->pich)) {
        tab->picw = tab->pich = 0;
        why_set(tab, "Out of memory.");
        return;
    }
    if (scale == 1)
        memcpy(tab->pic, tab->base, (size_t)tab->basew * (size_t)tab->baseh * sizeof *tab->pic);
    else
        for (int posy = 0; posy < tab->pich; posy++) {
            const uint32_t *src = &tab->base[(size_t)(posy / scale) * (size_t)tab->basew];
            uint32_t *dst = &tab->pic[(size_t)posy * (size_t)tab->picw];
            for (int posx = 0; posx < tab->picw; posx++)
                dst[posx] = src[posx / scale];
        }
    cnv.pixp = tab->pic; cnv.wide = tab->picw; cnv.high = tab->pich;
    draw_overlays(tab, &cnv, scale);
}

/* ---- the PPU's fetch, redone for one pixel ------------------------------------------------------ */

typedef struct {
    int ntab, col, row, tile, finex, finey, palette;
    int pixval;                         /* 0-3 */
    int slot, master;               /* the palette RAM entry and the master colour */
    unsigned name_addr, attr_addr, pat_addr;
    int attr_bad;                   /* coarse Y 30/31: the "tiles" are attribute bytes */
} bgpix;

/* The background pixel the PPU draws at screen x on a line that starts from
 * `ln`: the origin (v, fine X) plus x, as the tile fetches walk it. */
static void bg_pixel(const nesvid_capture *cap, const nesvid_line *linerec, int posx, bgpix *out)
{
    const unsigned vaddr = linerec->vaddr;
    const int abs_x = (int)(vaddr & 0x1F) * 8 + linerec->fine_x + posx;
    const int ntx = (int)((vaddr >> 10) & 1) ^ ((abs_x >> 8) & 1);
    const int xin = abs_x & 255;
    const int row = (int)((vaddr >> 5) & 0x1F), nty = (int)((vaddr >> 11) & 1);
    const int base = bg_table_of(linerec->ctrl);
    int attr, shift;

    memset(out, 0, sizeof *out);
    out->ntab = nty * 2 + ntx;
    out->col = xin >> 3;
    out->row = row;
    out->finex = xin & 7;
    out->finey = (int)((vaddr >> 12) & 7);
    out->attr_bad = row >= 30;
    out->tile = cap->ntab[out->ntab * 0x400 + row * 32 + out->col];
    attr = cap->ntab[out->ntab * 0x400 + 0x3C0 + (row / 4) * 8 + out->col / 4];
    shift = ((row & 2) << 1) | (out->col & 2);
    out->palette = (attr >> shift) & 3;
    out->pixval = nesppu_tile_pixel(cap, (unsigned)base, out->tile, out->finex, out->finey);
    out->slot = out->pixval ? out->palette * 4 + out->pixval : 0;
    out->master = nesppu_slot_colour(cap, out->slot);
    out->name_addr = 0x2000u + (unsigned)out->ntab * 0x400u + (unsigned)(row * 32 + out->col);
    out->attr_addr = 0x2000u + (unsigned)out->ntab * 0x400u + 0x3C0u + (unsigned)((row / 4) * 8 + out->col / 4);
    out->pat_addr = (unsigned)base + (unsigned)out->tile * 16u + (unsigned)out->finey;
}

typedef struct {
    int front;                      /* the sprite drawn at the pixel, -1 */
    int front_px, front_slot;
    unsigned front_addr;
    uint64_t mask;                  /* every box that holds the pixel */
    uint64_t hidden;                /* sprites the line had no room for (more than eight) */
    int overflow_line;
} sprpix;

static void sprites_at(const nesvid_capture *cap, int ctrl, int posx, int line, sprpix *out)
{
    const int tall = tall_of(ctrl);
    const unsigned base = (unsigned)spr_table_of(ctrl);
    int on_line = 0;

    memset(out, 0, sizeof *out);
    out->front = -1;
    for (int item = 0; item < SPRITES; item++) {
        nesppu_sprite placed;
        nesppu_sprite_info(cap, item, tall, &placed);
        if (line < placed.top || line >= placed.top + placed.high || placed.top >= NESVID_H) continue;
        on_line++;
        if (on_line > 8) {
            if (posx >= placed.left && posx < placed.left + 8) out->hidden |= (uint64_t)1 << item;
            out->overflow_line = 1;
            continue;
        }
        if (posx < placed.left || posx >= placed.left + 8) continue;
        out->mask |= (uint64_t)1 << item;
        {
            unsigned addr;
            const int pixval = nesppu_sprite_pixel(cap, &placed, base, tall, posx - placed.left, line - placed.top, &addr);
            if (out->front < 0 && pixval) {
                out->front = item;
                out->front_px = pixval;
                out->front_slot = placed.palette * 4 + pixval;
                out->front_addr = addr;
            }
        }
    }
}

/* ---- the inspector ----------------------------------------------------------------------- */

static void add_break(nesvid_pixel *outp, int space, unsigned lowest, unsigned highest)
{
    for (int item = 0; item < outp->nbreak; item++)
        if (outp->break_space[item] == space && outp->break_lo[item] == lowest && outp->break_hi[item] == highest) return;
    if (outp->nbreak < NESVID_MAX_BREAK) {
        outp->break_space[outp->nbreak] = space;
        outp->break_lo[outp->nbreak] = lowest;
        outp->break_hi[outp->nbreak] = highest;
        outp->nbreak++;
    }
}

/* the nametable entry, its attribute byte and the pattern bytes of a tile, in every alias of the nametable */
static void add_tile_breaks(const nesvid_tab *tab, nesvid_pixel *outp, int ntab, int off, unsigned pat_lo, unsigned pat_hi)
{
    int aliases[4];
    const int num = nesppu_nt_aliases(tab->cap->mirroring, ntab, aliases);
    for (int item = 0; item < num && outp->nbreak < NESVID_MAX_BREAK - 2; item++) {
        add_break(outp, NESVID_BK_PPU, 0x2000u + (unsigned)aliases[item] * 0x400u + (unsigned)off,
                  0x2000u + (unsigned)aliases[item] * 0x400u + (unsigned)off);
    }
    add_break(outp, NESVID_BK_PPU, pat_lo, pat_hi);
}

static void add_band_breaks(nesvid_pixel *outp, const band_rec *band)
{
    if (band->changed & CH_CTRL) add_break(outp, NESVID_BK_CPU, 0x2000, 0x2000);
    if (band->changed & CH_MASK) add_break(outp, NESVID_BK_CPU, 0x2001, 0x2001);
    if (band->changed & CH_SCROLL) add_break(outp, NESVID_BK_CPU, 0x2005, 0x2006);
}

/* The screen pixel (posx, line): what the PPU drew there, appended to the
 * pixel's text, and the break targets. */
static int describe_screen_pixel(nesvid_tab *tab, int posx, int line, nesvid_pixel *outp, int num)
{
    const nesvid_capture *cap = tab->cap;
    char *dst = outp->text;
    const size_t size = sizeof outp->text;
    const nesvid_line *linerec = &tab->eff[line];
    const band_rec *band = band_of_line(tab, line);
    const int bg_on = (linerec->mask & 0x08) && (posx >= 8 || (linerec->mask & 0x02));
    const int spr_on = (linerec->mask & 0x10) && (posx >= 8 || (linerec->mask & 0x04));
    bgpix back;
    sprpix spr;
    int scrollx, scrolly;

    outp->screenx = posx; outp->screeny = line;
    outp->line = line;
    if (!cap->mem_valid) {
        outp->kind = NESVID_PK_BACKDROP;
        num = text_add(dst, size, num, "Screen pixel %d,%d (the PPU has not been read yet)\n", posx, line);
        return num;
    }
    nesvid_line_scroll(linerec, &scrollx, &scrolly);
    num = text_add(dst, size, num, "Screen pixel %d,%d on line %d: scroll %d,%d in the nametable plane%s\n", posx, line, line, scrollx, scrolly,
                   band->first == line && tab->nbands > 1 && band != &tab->bands[0] ? " (a new band starts here)" : "");
    if (!rendering_of(linerec->mask)) {
        outp->kind = NESVID_PK_BACKDROP;
        outp->slot = 0;
        outp->colour = nesppu_slot_colour(cap, 0);
        num = text_add(dst, size, num, "Rendering is off here ($2001 = $%02X): the backdrop, master colour $%02X from palette entry 0\n", linerec->mask, outp->colour);
        add_break(outp, NESVID_BK_PALETTE, 0, 0);
        add_break(outp, NESVID_BK_CPU, 0x2001, 0x2001);
        outp->mem_space = NESDEBUG_SPACE_PALETTE; outp->mem_addr = 0;
        add_band_breaks(outp, band);
        return num;
    }
    bg_pixel(cap, linerec, posx, &back);
    sprites_at(cap, linerec->ctrl, posx, line, &spr);
    outp->sprite = spr.front;
    outp->sprite_mask = spr.mask;
    if (spr.hidden) {
        num = text_add(dst, size, num, "Not drawn, the line has more than eight sprites:");
        for (int item = 0; item < SPRITES; item++)
            if (spr.hidden & ((uint64_t)1 << item)) num = text_add(dst, size, num, " %d", item);
        num = text_add(dst, size, num, "\n");
    }
    {
        const int sprite_wins = spr_on && spr.front >= 0 &&
                                !(((cap->oam[spr.front * 4 + 2] & 0x20) != 0) && bg_on && back.pixval != 0);
        if (sprite_wins) {
            nesppu_sprite placed;
            const int sprmaster = nesppu_slot_colour(cap, spr.front_slot);
            nesppu_sprite_info(cap, spr.front, tall_of(linerec->ctrl), &placed);
            outp->kind = NESVID_PK_SPRITE;
            outp->colour = sprmaster; outp->slot = spr.front_slot;
            outp->markx = placed.left; outp->marky = placed.top; outp->markw = 8; outp->markh = placed.high;
            outp->ppu_addr = spr.front_addr;
            outp->mem_space = NESDEBUG_SPACE_OAM; outp->mem_addr = (unsigned)spr.front * 4;
            num = text_add(dst, size, num, "Sprite %d in front: OAM y=%d tile $%02X attr $%02X x=%d (palette %d%s%s%s), pattern byte at PPU $%04X\n",
                           spr.front, cap->oam[spr.front * 4], placed.tile, placed.attr, placed.left, placed.palette - 4,
                           placed.behind ? ", behind the background" : "", placed.hflip ? ", flipped H" : "", placed.vflip ? ", flipped V" : "", spr.front_addr);
            num = text_add(dst, size, num, "Colour: palette entry %d = master colour $%02X\n", spr.front_slot, sprmaster);
            if (spr.mask & ~((uint64_t)1 << spr.front)) {
                num = text_add(dst, size, num, "Also under the pointer:");
                for (int item = 0; item < SPRITES; item++)
                    if ((spr.mask & ((uint64_t)1 << item)) && item != spr.front) num = text_add(dst, size, num, " %d", item);
                num = text_add(dst, size, num, "\n");
            }
            if (spr.front == 0 && bg_on && back.pixval != 0 && posx != 255)
                num = text_add(dst, size, num, "Sprite 0 overlaps an opaque background pixel here: this is where the sprite 0 hit can land\n");
            add_break(outp, NESVID_BK_OAM, (unsigned)spr.front * 4, (unsigned)spr.front * 4 + 3);
            add_break(outp, NESVID_BK_PPU, spr.front_addr & ~15u, (spr.front_addr & ~15u) + 15);
            add_break(outp, NESVID_BK_PALETTE, (unsigned)spr.front_slot, (unsigned)spr.front_slot);
            return num;
        }
    }
    if (!bg_on) {
        outp->kind = NESVID_PK_BACKDROP;
        outp->slot = 0;
        outp->colour = nesppu_slot_colour(cap, 0);
        num = text_add(dst, size, num, "Background %s here: the backdrop, master colour $%02X from palette entry 0\n",
                       (linerec->mask & 0x08) ? "hidden in the left 8 pixels" : "is off ($2001 bit 3)", outp->colour);
        add_break(outp, NESVID_BK_PALETTE, 0, 0);
        outp->mem_space = NESDEBUG_SPACE_PALETTE; outp->mem_addr = 0;
        add_band_breaks(outp, band);
        return num;
    }
    outp->kind = back.pixval ? NESVID_PK_TILE : NESVID_PK_BACKDROP;
    outp->colour = back.master; outp->slot = back.slot;
    outp->ntab = back.ntab; outp->cell = back.ntab * 960 + back.row * 32 + back.col; outp->tile = back.tile;
    outp->ppu_addr = back.name_addr;
    outp->mem_space = NESDEBUG_SPACE_PPU; outp->mem_addr = back.name_addr;
    outp->markx = posx - back.finex; outp->marky = line - back.finey; outp->markw = 8; outp->markh = 8;
    num = text_add(dst, size, num, "Nametable %d (at $%04X), column %d, row %d: entry $%04X holds tile $%02X\n",
                   back.ntab, 0x2000 + back.ntab * 0x400, back.col, back.row, back.name_addr, back.tile);
    if (back.attr_bad)
        num = text_add(dst, size, num, "Coarse Y %d is past the 30 tile rows: the PPU is reading attribute bytes as tiles here\n", back.row);
    num = text_add(dst, size, num, "Attribute byte $%04X: palette %d. Pattern row at PPU $%04X (pixel %d,%d of the tile = %d)\n",
                   back.attr_addr, back.palette, back.pat_addr, back.finex, back.finey, back.pixval);
    num = text_add(dst, size, num, "Colour: palette entry %d = master colour $%02X%s\n", back.slot, back.master,
                   back.pixval ? "" : " (the universal backdrop: pixel value 0)");
    if (spr_on && spr.front >= 0)
        num = text_add(dst, size, num, "Sprite %d is behind the background here\n", spr.front);
    add_tile_breaks(tab, outp, back.ntab, back.row * 32 + back.col, back.pat_addr - (unsigned)back.finey, back.pat_addr - (unsigned)back.finey + 15);
    add_break(outp, NESVID_BK_PPU, back.attr_addr, back.attr_addr);
    add_break(outp, NESVID_BK_PALETTE, (unsigned)back.slot, (unsigned)back.slot);
    add_band_breaks(outp, band);
    return num;
}

static void locate(nesvid_tab *tab, int posx, int posy, nesvid_pixel *outp)
{
    const nesvid_capture *cap = tab->cap;
    const int scale = tab->effzoom ? tab->effzoom : 1;
    int picx, picy, num;

    memset(outp, 0, sizeof *outp);
    outp->screenx = outp->screeny = outp->line = outp->colour = outp->slot = outp->cell = outp->ntab = outp->tile = outp->sprite = -1;
    outp->mem_space = -1;
    outp->ppu_addr = NESVID_NO_ADDR;
    if (!tab->picw || posx < 0 || posy < 0 || posx >= tab->picw || posy >= tab->pich)
        return;
    picx = posx / scale; picy = posy / scale;
    outp->valid = 1;
    outp->picx = picx; outp->picy = picy;
    outp->markx = picx; outp->marky = picy; outp->markw = 1; outp->markh = 1;
    outp->rgb = tab->base ? tab->base[(size_t)picy * (size_t)tab->basew + (size_t)picx] : COL_BLACK;

    num = snprintf(outp->text, sizeof outp->text, "Picture pixel %d,%d\n", picx, picy);
    switch (tab->kind) {
    case BK_FRAME:
        /* the picture is the screen, so the marks (set in screen pixels) are already the picture's */
        if (picy < NESVID_H) {
            num = describe_screen_pixel(tab, picx, picy, outp, num);
            if (outp->kind != NESVID_PK_SPRITE && outp->kind != NESVID_PK_TILE) {
                outp->markx = picx; outp->marky = picy; outp->markw = 1; outp->markh = 1;
            }
        }
        break;
    case BK_NT:
    case BK_VALUES: {
        const int ntab = (picx / 256) + 2 * (picy / 240);
        const int col = (picx % 256) / 8, row = (picy % 240) / 8, off = row * 32 + col;
        const unsigned name_addr = 0x2000u + (unsigned)ntab * 0x400u + (unsigned)off;
        const int tile = cap->ntab[ntab * 0x400 + off];
        const int bgbase = bg_table_of(tab->nbands ? tab->bands[tab->main_band].start.ctrl : cap->ctrl);
        const unsigned attr_addr = 0x2000u + (unsigned)ntab * 0x400u + 0x3C0u + (unsigned)((row / 4) * 8 + col / 4);
        const int attr = cap->ntab[ntab * 0x400 + 0x3C0 + (row / 4) * 8 + col / 4];
        const int palette = (attr >> (((row & 2) << 1) | (col & 2))) & 3;
        const unsigned pat = (unsigned)bgbase + (unsigned)tile * 16u;
        outp->kind = tab->kind == BK_VALUES ? NESVID_PK_VALUE : NESVID_PK_TILE;
        outp->ntab = ntab; outp->cell = ntab * 960 + off; outp->tile = tile;
        outp->ppu_addr = name_addr;
        outp->mem_space = NESDEBUG_SPACE_PPU; outp->mem_addr = name_addr;
        outp->markx = (picx / 8) * 8; outp->marky = (picy / 8) * 8; outp->markw = 8; outp->markh = 8;
        num = text_add(outp->text, sizeof outp->text, num, "Nametable %d (at $%04X), column %d, row %d: entry $%04X = $%02X (%d)\n",
                       ntab, 0x2000 + ntab * 0x400, col, row, name_addr, tile, tile);
        num = text_add(outp->text, sizeof outp->text, num, "Attribute byte $%04X = $%02X: palette %d. Tile patterns at PPU $%04X-$%04X\n",
                       attr_addr, attr, palette, pat, pat + 15);
        for (int bnd = 0; bnd < tab->nbands; bnd++) {
            const band_rec *band = &tab->bands[bnd];
            const int xrel = (picx - band->scrollx + NESPPU_NT_W) % NESPPU_NT_W;
            const int yrel = ((picy - band->scrolly) % NESPPU_NT_H + NESPPU_NT_H) % NESPPU_NT_H;
            if (xrel < NESVID_W && yrel < band->last - band->first + 1) {
                num = text_add(outp->text, sizeof outp->text, num, "On screen: line %d, column %d, in the scroll window of lines %d-%d\n",
                               band->first + yrel, xrel, band->first, band->last);
                break;
            }
        }
        add_tile_breaks(tab, outp, ntab, off, pat, pat + 15);
        add_break(outp, NESVID_BK_PPU, attr_addr, attr_addr);
        outp->colour = nesppu_slot_colour(cap, palette * 4 + 1);
        break;
    }
    case BK_PATTERNS: {
        const int table = picx / 128, tile = (picy / 8) * 16 + (picx % 128) / 8;
        const unsigned addr = (unsigned)table * 0x1000u + (unsigned)tile * 16u;
        const int pixval = nesppu_tile_pixel(cap, (unsigned)table * 0x1000u, tile, picx % 8, picy % 8);
        outp->kind = NESVID_PK_PATTERN;
        outp->tile = tile;
        outp->ppu_addr = addr;
        outp->mem_space = NESDEBUG_SPACE_PPU; outp->mem_addr = addr;
        outp->slot = tab->palette * 4 + pixval;
        outp->colour = nesppu_slot_colour(cap, outp->slot);
        outp->markx = (picx / 8) * 8; outp->marky = (picy / 8) * 8; outp->markw = 8; outp->markh = 8;
        num = text_add(outp->text, sizeof outp->text, num, "Pattern table %d ($%04X), tile $%02X: bytes at PPU $%04X-$%04X; pixel %d,%d = %d\n",
                       table, table * 0x1000, tile, addr, addr + 15, picx % 8, picy % 8, pixval);
        add_break(outp, NESVID_BK_PPU, addr, addr + 15);
        break;
    }
    case BK_SPRITES: {
        sprpix spr;
        int idx = -1;
        /* the front sprite under the pointer; failing that the sprite whose box holds it */
        sprites_at(cap, cap->ctrl, picx, picy, &spr);
        idx = spr.front >= 0 ? spr.front : -1;
        if (idx < 0)
            for (int item = 0; item < SPRITES; item++)
                if (spr.mask & ((uint64_t)1 << item)) { idx = item; break; }
        outp->kind = NESVID_PK_SPRITE;
        outp->screenx = picx; outp->screeny = picy; outp->line = picy;
        outp->sprite = idx;
        outp->sprite_mask = spr.mask;
        if (idx >= 0) {
            nesppu_sprite placed;
            nesppu_sprite_info(cap, idx, tall_of(cap->ctrl), &placed);
            outp->markx = placed.left; outp->marky = placed.top; outp->markw = 8; outp->markh = placed.high;
            outp->mem_space = NESDEBUG_SPACE_OAM; outp->mem_addr = (unsigned)idx * 4;
            outp->slot = placed.palette * 4 + 1;
            num = text_add(outp->text, sizeof outp->text, num, "Sprite %d: OAM y=%d tile $%02X attr $%02X x=%d, palette %d%s%s%s\n", idx,
                           cap->oam[idx * 4], placed.tile, placed.attr, placed.left, placed.palette - 4,
                           placed.behind ? ", behind the background" : "", placed.hflip ? ", flipped H" : "", placed.vflip ? ", flipped V" : "");
            add_break(outp, NESVID_BK_OAM, (unsigned)idx * 4, (unsigned)idx * 4 + 3);
        } else {
            num = text_add(outp->text, sizeof outp->text, num, "No sprite here\n");
        }
        break;
    }
    case BK_PALETTE: {
        const int slot = (picy / NESPPU_PAL_CELL) * NESPPU_PAL_COLS + picx / NESPPU_PAL_CELL;
        const int master = cap->pal[slot & 31] & 63;
        const uint32_t rgb = nesppu_rgb(cap, master);
        outp->kind = NESVID_PK_PALETTE;
        outp->slot = slot & 31;
        outp->colour = master;
        outp->mem_space = NESDEBUG_SPACE_PALETTE; outp->mem_addr = (unsigned)(slot & 31);
        outp->markx = (picx / NESPPU_PAL_CELL) * NESPPU_PAL_CELL; outp->marky = (picy / NESPPU_PAL_CELL) * NESPPU_PAL_CELL;
        outp->markw = NESPPU_PAL_CELL; outp->markh = NESPPU_PAL_CELL;
        num = text_add(outp->text, sizeof outp->text, num, "Palette entry %d ($3F%02X) = master colour $%02X: R %d, G %d, B %d\n", slot & 31, 0x00 + (slot & 31), master,
                       (int)((rgb >> 16) & 255), (int)((rgb >> 8) & 255), (int)(rgb & 255));
        add_break(outp, NESVID_BK_PALETTE, (unsigned)(slot & 31), (unsigned)(slot & 31));
        break;
    }
    default:
        break;
    }
    (void)num;
    if (outp->colour >= 0 && (tab->kind == BK_PATTERNS || tab->kind == BK_PALETTE || tab->kind == BK_NT || tab->kind == BK_SPRITES) &&
        outp->kind != NESVID_PK_VALUE && cap->mem_valid)
        outp->rgb = nesppu_rgb(cap, outp->colour);
}

int nesvid_tab_hover(nesvid_tab *tab, int posx, int posy)
{
    nesvid_pixel before;
    if (!tab) return 0;
    before = tab->hover;
    tab->hovx = posx; tab->hovy = posy;
    locate(tab, posx, posy, &tab->hover);
    return tab->hover.valid != before.valid || tab->hover.picx != before.picx || tab->hover.picy != before.picy ||
           strcmp(tab->hover.text, before.text) != 0;
}

void nesvid_tab_pin(nesvid_tab *tab, int posx, int posy)
{
    nesvid_pixel probe;
    if (!tab) return;
    locate(tab, posx, posy, &probe);
    if (tab->pin.valid && probe.valid && tab->pin.markx == probe.markx && tab->pin.marky == probe.marky &&
        tab->pin.markw == probe.markw && tab->pin.markh == probe.markh) {
        nesvid_tab_unpin(tab);
        return;
    }
    tab->pinx = posx; tab->piny = posy;
    tab->pin = probe;
}

void nesvid_tab_unpin(nesvid_tab *tab)
{
    if (!tab) return;
    tab->pinx = tab->piny = -1;
    memset(&tab->pin, 0, sizeof tab->pin);
}

const nesvid_pixel *nesvid_tab_pixel(const nesvid_tab *tab)
{
    if (!tab) return NULL;
    if (tab->pin.valid) return &tab->pin;
    if (tab->hover.valid) return &tab->hover;
    return NULL;
}

int nesvid_tab_marks(const nesvid_tab *tab, int hover[4], int pin[4])
{
    const int scale = tab && tab->effzoom ? tab->effzoom : 1;
    int ret = 0;
    if (!tab) return 0;
    if (tab->hover.valid && hover) {
        hover[0] = tab->hover.markx * scale; hover[1] = tab->hover.marky * scale;
        hover[2] = tab->hover.markw * scale; hover[3] = tab->hover.markh * scale;
        ret |= 1;
    }
    if (tab->pin.valid && pin) {
        pin[0] = tab->pin.markx * scale; pin[1] = tab->pin.marky * scale;
        pin[2] = tab->pin.markw * scale; pin[3] = tab->pin.markh * scale;
        ret |= 2;
    }
    return ret;
}

/* ---- Break on writes ------------------------------------------------------------------------- */

int nesvid_tab_can_break(const nesvid_tab *tab)
{
    const nesvid_pixel *pixp = nesvid_tab_pixel(tab);
    return tab && tab->dbg && pixp && pixp->nbreak > 0;
}

/* The same range, space and type already in the engine's table is found and reused. */
static int break_write(nesvid_tab *tab, int space, unsigned lowest, unsigned highest)
{
    nesdebug_breakpoint list[NESDEBUG_MAX_BREAKPOINTS];
    char err[128];
    const int count = nesdebug_breakpoint_list(tab->dbg, list, NESDEBUG_MAX_BREAKPOINTS);

    for (int item = 0; item < count && item < NESDEBUG_MAX_BREAKPOINTS; item++)
        if (list[item].space == space && list[item].type == NESDEBUG_BP_WRITE && list[item].start == lowest &&
            list[item].end == highest && list[item].condition[0] == '\0')
            return list[item].id;
    return nesdebug_breakpoint_add_in(tab->dbg, space, NESDEBUG_BP_WRITE, lowest, highest, "", err, (int)sizeof err);
}

int nesvid_tab_break_on_pixel(nesvid_tab *tab)
{
    const nesvid_pixel *pixp = nesvid_tab_pixel(tab);
    int first = -1;
    if (!tab || !tab->dbg || !pixp || pixp->nbreak == 0) return -1;
    for (int item = 0; item < pixp->nbreak; item++) {
        const int bpid = break_write(tab, pixp->break_space[item], pixp->break_lo[item], pixp->break_hi[item]);
        if (first < 0) first = bpid;
    }
    return first;
}

int nesvid_tab_break_on_register(nesvid_tab *tab, int row)
{
    static const int addr[N_REGS] = { 0x2000, 0x2001, -1, 0x2003, 0x2005, 0x2006, -1 };
    if (!tab || !tab->dbg || row < 0 || row >= N_REGS || addr[row] < 0) return -1;
    return break_write(tab, NESVID_BK_CPU, (unsigned)addr[row], (unsigned)addr[row]);
}

/* ---- the tables ---------------------------------------------------------------------------- */

static void fill_status(nesvid_tab *tab)
{
    const nesvid_capture *cap = tab->cap;
    int num;
    const int lines = cap->nlines ? cap->nlines : 262;

    num = snprintf(tab->status, sizeof tab->status, "%s, %d lines, background %s, sprites %s %s",
                   region_name(cap->region), lines, (cap->mask & 0x08) ? "on" : "off",
                   (cap->mask & 0x10) ? "on" : "off", tall_of(cap->ctrl) ? "8x16" : "8x8");
    if (cap->mem_valid)
        num += snprintf(tab->status + num, sizeof tab->status - (size_t)num, ", %s mirroring, %d band%s",
                        nesppu_mirroring_name(cap->mirroring), tab->nbands, tab->nbands == 1 ? "" : "s");
    num += snprintf(tab->status + num, sizeof tab->status - (size_t)num, "\n");
    if (tab->has_live) {
        num += snprintf(tab->status + num, sizeof tab->status - (size_t)num, "Stopped; frame %llu",
                        (unsigned long long)cap->frame);
        if (cap->beam_line >= -1)
            snprintf(tab->status + num, sizeof tab->status - (size_t)num, "; beam at line %d of %d, dot %d of 341",
                     cap->beam_line, lines, cap->beam_dot);
    } else {
        snprintf(tab->status + num, sizeof tab->status - (size_t)num, "Running; frame %llu, %s",
                 (unsigned long long)cap->frame, cap->log_valid ? "the line log is recorded" : "waiting for a frame's line log");
    }
}

const char *nesvid_tab_status(const nesvid_tab *tab) { return tab ? tab->status : ""; }

static void reg_entry(nesvid_tab *tab, int index, const char *name, const char *value, const char *meaning, int writes, int first)
{
    nesvid_reg *reg = &tab->regs[tab->nregs];
    reg->name = name;
    snprintf(reg->value, sizeof reg->value, "%s", value);
    reg->age = tab->reg_age[index];
    reg->writes = writes;
    reg->first_line = first;
    snprintf(reg->meaning, sizeof reg->meaning, "%s", meaning);
    tab->nregs++;
}

static void fill_regs(nesvid_tab *tab)
{
    const nesvid_capture *cap = tab->cap;
    const nesvid_frame_log *logp = &cap->flog;
    const int logged = cap->log_valid;
    char val[16], text[240], detail[200];
    int posx, posy;

    tab->nregs = 0;
    snprintf(val, sizeof val, "%02X", cap->ctrl);
    nesppu_describe_ctrl(cap->ctrl, detail, (int)sizeof detail);
    snprintf(text, sizeof text, "$2000: %s", detail);
    reg_entry(tab, REG_CTRL, "PPUCTRL", val, text, logged ? (int)logp->reg_writes[0] : 0, logged ? logp->reg_first[0] : -1);
    snprintf(val, sizeof val, "%02X", cap->mask);
    nesppu_describe_mask(cap->mask, detail, (int)sizeof detail);
    snprintf(text, sizeof text, "$2001: %s", detail);
    reg_entry(tab, REG_MASK, "PPUMASK", val, text, logged ? (int)logp->reg_writes[1] : 0, logged ? logp->reg_first[1] : -1);
    snprintf(val, sizeof val, "%02X", cap->status);
    nesppu_describe_status(cap->status, detail, (int)sizeof detail);
    snprintf(text, sizeof text, "$2002 (read-only): %s", detail);
    reg_entry(tab, REG_STATUS, "PPUSTATUS", val, text, logged ? (int)logp->reg_reads[2] : 0, -1);
    snprintf(val, sizeof val, "%02X", cap->oam_addr);
    snprintf(text, sizeof text, "$2003: the OAM address ($2004 and DMA write here)");
    reg_entry(tab, REG_OAMADDR, "OAMADDR", val, text, logged ? (int)logp->reg_writes[3] : 0, logged ? logp->reg_first[3] : -1);
    {
        nesvid_line fake;
        memset(&fake, 0, sizeof fake);
        fake.vaddr = cap->taddr; fake.fine_x = cap->fine_x;
        nesvid_line_scroll(&fake, &posx, &posy);
    }
    snprintf(val, sizeof val, "%d,%d", posx, posy);
    snprintf(text, sizeof text, "$2005: the scroll the next frame starts from, X=%d Y=%d in the 512 x 480 nametable plane (fine X %d)",
             posx, posy, cap->fine_x);
    reg_entry(tab, REG_SCROLL, "PPUSCROLL", val, text, logged ? (int)logp->reg_writes[5] : 0, logged ? logp->reg_first[5] : -1);
    snprintf(val, sizeof val, "%04X", cap->vaddr);
    snprintf(text, sizeof text, "$2006: v, the VRAM address: coarse X %d, coarse Y %d, nametable %d, fine Y %d; write latch %s",
             cap->vaddr & 0x1F, (cap->vaddr >> 5) & 0x1F, (cap->vaddr >> 10) & 3, (cap->vaddr >> 12) & 7, cap->wtoggle ? "second" : "first");
    reg_entry(tab, REG_VADDR, "v", val, text, logged ? (int)logp->reg_writes[6] : 0, logged ? logp->reg_first[6] : -1);
    snprintf(val, sizeof val, "%04X", cap->taddr);
    snprintf(text, sizeof text, "t, the temporary address ($2000, $2005 and $2006 load it): coarse X %d, coarse Y %d, nametable %d, fine Y %d",
             cap->taddr & 0x1F, (cap->taddr >> 5) & 0x1F, (cap->taddr >> 10) & 3, (cap->taddr >> 12) & 7);
    reg_entry(tab, REG_TADDR, "t", val, text, 0, -1);
}

int nesvid_tab_regs(const nesvid_tab *tab, const nesvid_reg **out)
{
    if (!tab) return 0;
    if (out) *out = tab->regs;
    return tab->nregs;
}

static void fill_swatches(nesvid_tab *tab)
{
    const nesvid_capture *cap = tab->cap;
    const nesvid_pixel *pixp = nesvid_tab_pixel(tab);
    const uint32_t *frame = cap->fb_valid ? cap->pixels : cap->live_pixels;
    const int have_frame = cap->fb_valid || cap->live_pixels_valid;
    long hist[64];
    uint32_t rgb[64];

    memset(hist, 0, sizeof hist);
    for (int idx = 0; idx < 64; idx++) rgb[idx] = nesppu_rgb(cap, idx);
    if (have_frame && cap->mem_valid)
        for (size_t item = 0; item < (size_t)NESVID_W * NESVID_H; item++) {
            const uint32_t pix = 0xff000000u | frame[item];
            for (int idx = 0; idx < 64; idx++)
                if (pix == rgb[idx]) { hist[idx]++; break; }
        }
    for (int slot = 0; slot < 32; slot++) {
        nesvid_swatch *swat = &tab->swat[slot];
        const int master = cap->pal[slot] & 63;
        const int colidx = slot & 3;
        if (slot == 0) snprintf(tab->swat_names[slot], sizeof tab->swat_names[0], "Backdrop");
        else if (slot < 16) snprintf(tab->swat_names[slot], sizeof tab->swat_names[0], "BG %d.%d", slot / 4, colidx);
        else snprintf(tab->swat_names[slot], sizeof tab->swat_names[0], "Sprite %d.%d", (slot - 16) / 4, colidx);
        swat->slot = slot;
        swat->name = tab->swat_names[slot];
        swat->colour = master;
        swat->rgb = rgb[master];
        swat->uses = hist[master];
        if (slot == 0)
            snprintf(swat->role, sizeof swat->role, "universal backdrop ($3F00): shows wherever nothing else is drawn");
        else if (colidx == 0 && slot < 16)
            snprintf(swat->role, sizeof swat->role, "background palette %d, colour 0: not used, the backdrop shows", slot / 4);
        else if (colidx == 0)
            snprintf(swat->role, sizeof swat->role, "sprite palette %d, colour 0: transparent", (slot - 16) / 4);
        else if (slot < 16)
            snprintf(swat->role, sizeof swat->role, "background palette %d, colour %d (attribute value %d)", slot / 4, colidx, slot / 4);
        else
            snprintf(swat->role, sizeof swat->role, "sprite palette %d, colour %d (OAM attribute bits 0-1 = %d)", (slot - 16) / 4, colidx, (slot - 16) / 4);
        swat->inspected = pixp && pixp->slot == slot;
    }
}

int nesvid_tab_swatches(const nesvid_tab *tab, const nesvid_swatch **out)
{
    if (!tab) return 0;
    if (out) *out = tab->swat;
    return 32;
}

static void fill_lines(nesvid_tab *tab)
{
    tab->nlines = 0;
    for (int bnd = 0; bnd < tab->nbands; bnd++) {
        const band_rec *band = &tab->bands[bnd];
        nesvid_band *line = &tab->lines[tab->nlines++];
        char changed[80] = "";
        memset(line, 0, sizeof *line);
        line->first = band->first;
        line->last = band->last;
        line->scroll_x = band->scrollx;
        line->scroll_y = band->scrolly;
        line->ctrl = band->start.ctrl;
        line->mask = band->start.mask;
        line->fine_x = band->start.fine_x;
        line->vaddr = band->start.vaddr;
        line->writes = band->writes;
        line->mapper_writes = band->mapper_writes;
        if (band->writes) {
            snprintf(changed, sizeof changed, " [changed here:%s%s%s]", (band->changed & CH_CTRL) ? " $2000" : "",
                     (band->changed & CH_MASK) ? " $2001" : "", (band->changed & CH_SCROLL) ? " scroll" : "");
        }
        {
            int num = snprintf(line->text, sizeof line->text, "lines %d-%d: scroll %d,%d, $2000=$%02X (bg patterns $%04X%s), $2001=$%02X%s%s",
                               band->first, band->last, band->scrollx, band->scrolly, band->start.ctrl, bg_table_of(band->start.ctrl),
                               tall_of(band->start.ctrl) ? ", 8x16 sprites" : "", band->start.mask,
                               rendering_of(band->start.mask) ? "" : " (rendering off)", changed);
            if (band->mapper_writes && num < (int)sizeof line->text)
                num += snprintf(line->text + num, sizeof line->text - (size_t)num, " [%d mapper write%s]", band->mapper_writes,
                                band->mapper_writes == 1 ? "" : "s");
            if (band->mid && num < (int)sizeof line->text)
                snprintf(line->text + num, sizeof line->text - (size_t)num, " [a register changed mid-line]");
        }
    }
}

int nesvid_tab_lines(const nesvid_tab *tab, const nesvid_band **out)
{
    if (!tab) return 0;
    if (out) *out = tab->lines;
    return tab->nlines;
}

static void fill_sprites(nesvid_tab *tab)
{
    const nesvid_capture *cap = tab->cap;
    const int tall = tall_of(cap->ctrl);
    for (int item = 0; item < SPRITES; item++) {
        nesppu_sprite placed;
        nesvid_spriterow *row = &tab->srows[item];
        nesppu_sprite_info(cap, item, tall, &placed);
        row->index = item;
        row->visible = placed.on_screen;
        snprintf(row->text, sizeof row->text, "%2d  x=%3d y=%3d tile=$%02X pal=%d%s%s%s%s", item, placed.left, cap->oam[item * 4], placed.tile,
                 placed.palette - 4, placed.behind ? " behind" : "", placed.hflip ? " flipH" : "", placed.vflip ? " flipV" : "",
                 placed.on_screen ? "" : "  (off screen)");
    }
}

int nesvid_tab_sprites(const nesvid_tab *tab, const nesvid_spriterow **out)
{
    if (!tab) return 0;
    if (out) *out = tab->srows;
    return SPRITES;
}

static void timing_add(nesvid_tab *tab, const char *label, const char *fmt, ...)
{
    char value[192];
    va_list args;
    if (tab->ntiming >= MAX_TIMING) return;
    va_start(args, fmt);
    vsnprintf(value, sizeof value, fmt, args);
    va_end(args);
    snprintf(tab->timing_buf[tab->ntiming], sizeof tab->timing_buf[0], "%s\t%s", label, value);
    tab->timing_ptr[tab->ntiming] = tab->timing_buf[tab->ntiming];
    tab->ntiming++;
}

static void fill_timing(nesvid_tab *tab)
{
    const nesvid_capture *cap = tab->cap;
    const int region = cap->region;
    const int lines = cap->nlines ? cap->nlines : 262;
    const double cpu_hz = region == NESVID_PAL ? 1662607.0 : region == NESVID_DENDY ? 1773448.0 : 1789773.0;
    const double ratio = region == NESVID_PAL ? 3.2 : 3.0;
    const double frame_cycles = region == NESVID_NTSC ? 29780.5 : (double)lines * 341.0 / ratio;
    const int nmi_line = cap->nmi_line ? cap->nmi_line : 241;

    tab->ntiming = 0;
    timing_add(tab, "Standard", "%s, %.4f Hz, %d lines of 341 dots", region_name(region), cpu_hz / frame_cycles, lines);
    timing_add(tab, "Dot", "%.1f PPU dots to each CPU cycle (CPU %.6f MHz)", ratio, cpu_hz / 1e6);
    timing_add(tab, "Line", "341 dots = %.2f CPU cycles = %.1f us", 341.0 / ratio, 341.0 / ratio * 1e6 / cpu_hz);
    timing_add(tab, "Frame", "%d lines = %.1f CPU cycles = %.2f ms", lines, frame_cycles, frame_cycles * 1e3 / cpu_hz);
    timing_add(tab, "Picture", "lines 0-239 draw; line -1 is the pre-render line (copies t into v at dots 280-304)");
    timing_add(tab, "Vertical blank", "starts at line %d dot 1: status bit 7 is set and the NMI is taken (PPUCTRL bit 7 %s)", nmi_line,
               (cap->ctrl & 0x80) ? "on: the NMI will fire" : "off: no NMI");
    timing_add(tab, "Line log", "%s", cap->log_valid ? "recorded for the captured frame, event by event (a write lands on the line it was made on)" : "none yet");
    if (cap->log_valid) {
        const nesvid_frame_log *logp = &cap->flog;
        timing_add(tab, "Logged frame", "%llu: %d scroll/control writes, %u mapper register writes", (unsigned long long)cap->log_frame,
                   logp->nwrites, (unsigned)logp->mapper_writes);
        for (int item = 0; item < log_events(logp) && tab->ntiming < MAX_TIMING - 8; item++) {
            const nesvid_event *eventp = &logp->events[item];
            const char *name = eventp->kind == NESVID_EV_NMI ? "NMI" : eventp->kind == NESVID_EV_IRQ ? "IRQ" : "Sprite 0 hit";
            if (eventp->kind == NESVID_EV_SPRITE0 || item < 4)
                timing_add(tab, name, "line %d, dot %d", eventp->line, eventp->dot);
        }
    }
    if (tab->has_live) {
        int done = cap->cur_valid ? cap->clog.last_line + 1 : 0;
        timing_add(tab, "Frame in progress", "%d picture lines started so far", done < 0 ? 0 : done);
        timing_add(tab, "Cycles", "%llu CPU cycles since power-on", (unsigned long long)cap->cycles);
        if (cap->beam_line >= -1)
            timing_add(tab, "Beam", "line %d (%s), dot %d of 341", cap->beam_line,
                       cap->beam_line < 0 ? "pre-render" : cap->beam_line < NESVID_H ? "picture" : "blanking", cap->beam_dot);
        else
            timing_add(tab, "Beam", "unknown");
    } else {
        timing_add(tab, "Beam", "(stopped only)");
    }
}

int nesvid_tab_timing(const nesvid_tab *tab, const char *const **lines)
{
    if (!tab) return 0;
    if (lines) *lines = tab->timing_ptr;
    return tab->ntiming;
}

int nesvid_tab_beam_permille(const nesvid_tab *tab)
{
    int per, lines;
    if (!tab || !tab->has_live || tab->cap->beam_line < -1) return 0;
    lines = tab->cap->nlines ? tab->cap->nlines : 262;
    per = (int)((long)(tab->cap->beam_line + 1) * 1000 / lines);
    return per > 1000 ? 1000 : per < 0 ? 0 : per;
}

/* ---- refresh --------------------------------------------------------------------------------- */

unsigned nesvid_tab_refresh(nesvid_tab *tab, int force)
{
    unsigned flags = 0;
    uint64_t sig, tsig;
    if (!tab) return 0;
    if (!acquire(tab)) {
        tab->picw = tab->pich = 0;
        why_set(tab, "No machine is running, or no frame has been captured yet.");
        tab->status[0] = '\0';
        tab->sig = 0;
        return NESVID_TAB_OFF | NESVID_TAB_PICTURE | NESVID_TAB_TABLES;
    }
    sig = picture_signature(tab);
    tsig = table_signature(tab, sig);
    if (force || tab->dirty || sig != tab->sig) {
        present(tab);
        track(tab);
        tab->sig = sig;
        tab->tsig = tsig;
        tab->dirty = 0;
        build_picture(tab);
        if (tab->hovx >= 0) locate(tab, tab->hovx, tab->hovy, &tab->hover);
        if (tab->pinx >= 0) locate(tab, tab->pinx, tab->piny, &tab->pin);
        fill_status(tab);
        fill_regs(tab);
        fill_swatches(tab);
        fill_lines(tab);
        fill_sprites(tab);
        fill_timing(tab);
        flags |= NESVID_TAB_PICTURE | NESVID_TAB_TABLES;
        if (!tab->picw) flags |= NESVID_TAB_OFF;
    } else if (tsig != tab->tsig) {
        tab->tsig = tsig;
        fill_status(tab);
        fill_timing(tab);
        flags |= NESVID_TAB_TABLES;
    }
    return flags;
}

const uint32_t *nesvid_tab_picture(nesvid_tab *tab, int *wide, int *high)
{
    if (!tab || !tab->picw) return NULL;
    if (wide) *wide = tab->picw;
    if (high) *high = tab->pich;
    return tab->pic;
}

const char *nesvid_tab_why(const nesvid_tab *tab) { return tab ? tab->why : ""; }
int nesvid_tab_effective_zoom(const nesvid_tab *tab) { return tab ? tab->effzoom : 1; }

/* ---- text -------------------------------------------------------------------------------------- */

int nesvid_tab_text(nesvid_tab *tab, int section, char *dst, int dstsz)
{
    int num = 0;
    const size_t size = (size_t)dstsz;
    if (!tab || !dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    switch (section) {
    case NESVID_S_REGISTERS:
        num = text_add(dst, size, num, "%s\n", tab->status);
        for (int item = 0; item < tab->nregs; item++) {
            const nesvid_reg *reg = &tab->regs[item];
            num = text_add(dst, size, num, "%-9s %-8s %s", reg->name, reg->value, reg->meaning);
            if (reg->writes && reg->first_line >= 0)
                num = text_add(dst, size, num, "  [%s %d time%s in the logged frame, first on line %d]", item == REG_STATUS ? "read" : "written",
                               reg->writes, reg->writes == 1 ? "" : "s", reg->first_line);
            else if (reg->writes && item == REG_STATUS)
                num = text_add(dst, size, num, "  [read %d time%s in the logged frame]", reg->writes, reg->writes == 1 ? "" : "s");
            else if (reg->writes)
                num = text_add(dst, size, num, "  [written %d time%s in the logged frame, none on the picture]", reg->writes, reg->writes == 1 ? "" : "s");
            if (reg->age < NESVID_REG_FADE) num = text_add(dst, size, num, "  [changed %d refresh%s ago]", reg->age, reg->age == 1 ? "" : "es");
            num = text_add(dst, size, num, "\n");
        }
        break;
    case NESVID_S_COLOURS:
        for (int item = 0; item < 32; item++) {
            const nesvid_swatch *swat = &tab->swat[item];
            num = text_add(dst, size, num, "%2d %-9s $%02X %06X  %ld px  %s\n", item, swat->name, swat->colour, swat->rgb & 0xFFFFFFu, swat->uses, swat->role);
        }
        break;
    case NESVID_S_LINES:
        for (int item = 0; item < tab->nlines; item++) num = text_add(dst, size, num, "%s\n", tab->lines[item].text);
        break;
    case NESVID_S_TIMING:
        for (int item = 0; item < tab->ntiming; item++) {
            const char *label = tab->timing_ptr[item], *value = strchr(label, '\t');
            if (value) num = text_add(dst, size, num, "%.*s: %s\n", (int)(value - label), label, value + 1);
            else num = text_add(dst, size, num, "%s\n", label);
        }
        break;
    case NESVID_S_SPRITES:
        for (int item = 0; item < SPRITES; item++) num = text_add(dst, size, num, "%s\n", tab->srows[item].text);
        break;
    default: {
        const nesvid_pixel *pixp = nesvid_tab_pixel(tab);
        if (pixp) num = text_add(dst, size, num, "%s", pixp->text);
        else num = text_add(dst, size, num, "No pixel under the pointer.\n");
        break;
    }
    }
    return num;
}

static void nap_ms(long millis)
{
    struct timespec nap;
    nap.tv_sec = millis / 1000;
    nap.tv_nsec = (millis % 1000) * 1000000L;
    nanosleep(&nap, NULL);
}

int nesvid_tab_text_for(nesdebug *dbg, const char *section, char *dst, int dstsz)
{
    nesvid_tab *tab;
    int sec = -1, num = 0, waited;
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (section && *section) {
        sec = nesvid_section_from_name(section);
        if (sec < 0) return snprintf(dst, (size_t)dstsz, "unknown section \"%s\" (pixel, registers, colours, lines, timing, sprites)\n", section);
    }
    if (!s_last_session || nessession_debugger(s_last_session) != dbg)
        return snprintf(dst, (size_t)dstsz, "No video state: the Video tab has not been opened.\n");
    tab = nesvid_tab_new(s_last_session);
    if (!tab) return snprintf(dst, (size_t)dstsz, "out of memory\n");
    /* A running machine publishes a frame within a moment, and records the
     * line log from the frame after that; a parked one is read at once. */
    for (waited = 0; waited < 600; waited += 20) {
        const unsigned flags = nesvid_tab_refresh(tab, 1);
        if (!(flags & NESVID_TAB_OFF) && (tab->has_live || tab->cap->log_valid || waited >= 200))
            break;
        nap_ms(20);
    }
    if (!tab->have)
        num = snprintf(dst, (size_t)dstsz, "No machine is running, or no frame has been captured yet.\n");
    else
        num = nesvid_tab_text(tab, sec < 0 ? NESVID_S_REGISTERS : sec, dst, dstsz);
    nesvid_tab_free(tab);
    return num;
}

/* ---- the console's `video` ---------------------------------------------------------------------- */

static nesdebug *s_console_dbg;
static char s_console_text[16384];

static const char *console_text(const char *section)
{
    if (nesvid_tab_text_for(s_console_dbg, section, s_console_text, (int)sizeof s_console_text) <= 0)
        return NULL;
    return s_console_text;
}

void nesvid_tab_register_console(nesdebug *dbg)
{
    s_console_dbg = dbg;
    nesdebug_set_video_text(dbg ? console_text : NULL);
}
