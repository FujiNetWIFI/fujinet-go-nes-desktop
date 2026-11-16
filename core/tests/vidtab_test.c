/*
 * vidtab_test -- the PPU register model (core/src/ppumodel.c), the Decoded
 * renderers (ppuview.c) and the Video tab controller (vidtab.c) on made-up
 * captures, plus a fuzz. No machine, no debugger (vidtab_stubs.c records what
 * vidtab.c asks of it), so it always runs, and is MinGW-safe: no setenv; the
 * NES_DEBUGGER_VIDEO_* variables are set by ctest for `vidtab_test env`.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nesdebug.h"
#include "nesvidtab.h"
#include "ppumodel.h"
#include "ppuview.h"

extern int stub_stopped;
extern unsigned stub_generation;
extern int stub_capture_enabled;
extern nesdebug_breakpoint stub_bps[];
extern int stub_nbps;
extern const char *(*stub_video_text)(const char *section);
nessession *stub_session(void);
nesdebug *stub_debugger(void);

static int failures;

static void check(const char *what, int passed)
{
    if (!passed) {
        fprintf(stderr, "FAIL %s\n", what);
        failures++;
    }
}

static int contains(const char *text, const char *word)
{
    return text && strstr(text, word) != NULL;
}

/* ---- the model ------------------------------------------------------------------------------ */

static nesvid_ev evw(int line, int dot, int reg, int value)
{
    nesvid_ev event;
    memset(&event, 0, sizeof event);
    event.line = (int16_t)line; event.dot = (uint16_t)dot; event.kind = NESVID_EVK_WRITE;
    event.addr = (uint16_t)(0x2000 + reg); event.value = (uint8_t)value;
    return event;
}

static nesvid_ev evr(int line, int dot, int reg)
{
    nesvid_ev event = evw(line, dot, reg, 0);
    event.kind = NESVID_EVK_READ;
    return event;
}

static int line_x(const nesvid_frame_log *log, int line)
{
    int posx, posy;
    nesvid_line_scroll(&log->line[line], &posx, &posy);
    return posx;
}

static int line_y(const nesvid_frame_log *log, int line)
{
    int posx, posy;
    nesvid_line_scroll(&log->line[line], &posx, &posy);
    return posy;
}

static void test_model(void)
{
    static nesvid_frame_log log;
    nesvid_model model;
    nesvid_ev evs[16];
    int num;

    /* a quiet frame: Y climbs a line at a time, X stays */
    nesvid_model_seed(&model, 0x90, 0x1E, 0, 0, 0, 0);
    nesvid_model_run(&model, NULL, 0, -2, 0, &log);
    check("quiet: every line recorded", log.last_line == 239 && log.line[0].valid && log.line[239].valid);
    check("quiet: line 0 scroll 0,0", line_x(&log, 0) == 0 && line_y(&log, 0) == 0);
    check("quiet: Y steps a line a line", line_y(&log, 10) == 10 && line_y(&log, 100) == 100 && line_y(&log, 239) == 239);
    check("quiet: X does not move", line_x(&log, 200) == 0);
    check("quiet: no writes", log.nwrites == 0 && log.line[50].writes == 0);
    check("quiet: ctrl and mask carried", log.line[77].ctrl == 0x90 && log.line[77].mask == 0x1E);

    /* a scroll set in vblank (t) reaches the top of the next frame through the pre-render copies */
    nesvid_model_seed(&model, 0x90, 0x1E, 0, 0, 0, 0);
    num = 0;
    evs[num++] = evw(245, 50, 5, 0x28);         /* X = 40: coarse 5 */
    evs[num++] = evw(245, 60, 5, 0x15);         /* Y = 21: coarse 2, fine 5 */
    nesvid_model_run(&model, evs, num, -2, 0, &log);
    /* the pair is after line 239 of this frame; the lines above did not see it */
    check("vblank write: this frame's lines are unchanged", line_x(&log, 0) == 0 && line_y(&log, 0) == 0);
    check("vblank write: lands in t", (model.taddr & 0x1F) == 5 && ((model.taddr >> 5) & 0x1F) == 2 && ((model.taddr >> 12) & 7) == 5);
    {
        /* the next frame starts from it */
        nesvid_model next = model;
        nesvid_model_run(&next, NULL, 0, -2, 0, &log);
        check("vblank write: next frame starts at 40,21", line_x(&log, 0) == 40 && line_y(&log, 0) == 21);
        check("vblank write: and steps from there", line_y(&log, 9) == 30);
    }

    /* an X split before dot 257 reaches the next line; a fine X lands at once */
    nesvid_model_seed(&model, 0x90, 0x1E, 0, 0, 0, 0);
    num = 0;
    evs[num++] = evw(99, 250, 5, 83);           /* X = 83: coarse 10, fine 3 */
    evs[num++] = evw(99, 260, 5, 0);
    nesvid_model_run(&model, evs, num, -2, 0, &log);
    check("x split: line 99 is still 0", line_x(&log, 99) == 0);
    check("x split: line 100 starts at 83", line_x(&log, 100) == 83 && log.line[100].fine_x == 3);
    check("x split: and keeps going", line_x(&log, 239) == 83);
    check("x split: Y did not move", line_y(&log, 100) == 100 && line_y(&log, 101) == 101);
    check("x split: two writes logged on line 99", log.line[99].writes == 2 && log.line[99].mid);
    check("x split: the write list has them", log.nwrites == 2 && log.writes[0].line == 99 && log.writes[0].reg == 5 && log.writes[0].value == 83);
    check("x split: counted per register", log.reg_writes[5] == 2 && log.reg_vis[5] == 2 && log.reg_first[5] == 99);

    /* a write after dot 257 waits a line more */
    nesvid_model_seed(&model, 0x90, 0x1E, 0, 0, 0, 0);
    evs[0] = evw(99, 300, 5, 40);
    evs[1] = evw(99, 310, 5, 0);
    nesvid_model_run(&model, evs, 2, -2, 0, &log);
    check("late x split: line 100 still 0", line_x(&log, 100) == 0);
    check("late x split: line 101 is 40", line_x(&log, 101) == 40);

    /* the $2006 trick sets v at once, nametable and Y included */
    nesvid_model_seed(&model, 0x90, 0x1E, 0, 0, 0, 0);
    num = 0;
    evs[num++] = evw(99, 260, 6, 0x04);
    evs[num++] = evw(99, 270, 6, 0x00);
    nesvid_model_run(&model, evs, num, -2, 0, &log);
    check("2006 split: line 100 is nametable 1, Y 0", line_x(&log, 100) == 256 && line_y(&log, 100) == 0);
    check("2006 split: line 101 steps Y", line_x(&log, 101) == 256 && line_y(&log, 101) == 1);
    check("2006 split: line 99 untouched", line_x(&log, 99) == 0 && line_y(&log, 99) == 99);

    /* a control write mid-frame: the line it lands on keeps its start state */
    nesvid_model_seed(&model, 0x90, 0x1E, 0, 0, 0, 0);
    evs[0] = evw(49, 100, 0, 0x80);
    nesvid_model_run(&model, evs, 1, -2, 0, &log);
    check("ctrl split: line 49 keeps the old table", (log.line[49].ctrl & 0x10) != 0 && log.line[49].mid);
    check("ctrl split: line 50 has the new", (log.line[50].ctrl & 0x10) == 0);

    /* a write early on a line (dot 1) is the line's own start */
    nesvid_model_seed(&model, 0x90, 0x1E, 0, 0, 0, 0);
    evs[0] = evw(60, 1, 1, 0x00);
    nesvid_model_run(&model, evs, 1, -2, 0, &log);
    check("dot 1 write: the line starts with it", log.line[60].mask == 0 && log.line[59].mask == 0x1E && !log.line[60].mid);

    /* $2002 resets the latch */
    nesvid_model_seed(&model, 0x90, 0x1E, 0, 0, 0, 0);
    evs[0] = evw(250, 1, 5, 8);
    evs[1] = evr(250, 5, 2);
    evs[2] = evw(250, 9, 5, 16);
    nesvid_model_run(&model, evs, 3, -2, 0, &log);
    check("latch: the read made the third write a first write", model.wtoggle == 1 && (model.taddr & 0x1F) == 2 && model.fine_x == 0);

    /* $2007 steps v by 1 or 32 outside rendering */
    nesvid_model_seed(&model, 0x80, 0x00, 0, 0, 0, 0);
    evs[0] = evw(250, 1, 6, 0x20);
    evs[1] = evw(250, 5, 6, 0x00);
    evs[2] = evw(250, 9, 7, 0x41);
    evs[3] = evw(250, 13, 7, 0x42);
    nesvid_model_run(&model, evs, 4, -2, 0, &log);
    check("2007: v steps by one", model.vaddr == 0x2002);
    check("2007: the written addresses are marked", (log.wr_nt[0] & 1) && (log.wr_nt[0] & 2));
    nesvid_model_seed(&model, 0x84, 0x00, 0x2000, 0, 0, 0);
    evs[0] = evw(250, 9, 7, 0x41);
    nesvid_model_run(&model, evs, 1, -2, 0, &log);
    check("2007: v steps by 32 with PPUCTRL bit 2", model.vaddr == 0x2020);
    {
        nesvid_ev tgt = evw(250, 9, 7, 0x41);
        tgt.ttype = NESVID_TT_PPU; tgt.target = 0x3F05;
        nesvid_model_seed(&model, 0x80, 0x00, 0, 0, 0, 0);
        nesvid_model_run(&model, &tgt, 1, -2, 0, &log);
        check("2007: a palette write marks its entry", (log.wr_pal[0] >> 5) & 1);
        tgt.target = 0x0010;
        nesvid_model_run(&model, &tgt, 1, -2, 0, &log);
        check("2007: a pattern write marks its byte", (log.wr_chr[2] & 1) != 0);
        tgt = evw(250, 9, 4, 0x11);
        tgt.ttype = NESVID_TT_OAM; tgt.target = 0x42;
        nesvid_model_run(&model, &tgt, 1, -2, 0, &log);
        check("2004: an OAM write marks its byte", (log.wr_oam[8] >> 2) & 1);
    }

    /* rendering off: the PPU does not move v */
    nesvid_model_seed(&model, 0x90, 0x00, 0x0400, 0x0400, 0, 0);
    nesvid_model_run(&model, NULL, 0, -2, 0, &log);
    check("rendering off: v stays", log.line[0].vaddr == 0x0400 && log.line[200].vaddr == 0x0400);

    /* stopping with the beam mid-frame */
    nesvid_model_seed(&model, 0x90, 0x1E, 0, 0, 0, 0);
    evs[0] = evw(20, 100, 5, 8);
    nesvid_model_run(&model, evs, 1, 50, 10, &log);
    check("parked: the lines the beam started are recorded", log.last_line == 50 && log.line[50].valid && !log.line[51].valid);
    nesvid_model_run(&model, evs, 1, 50, 0, &log);
    check("parked at dot 0: the line has not started", log.last_line == 49);

    /* events and mapper writes */
    {
        nesvid_ev more[4];
        memset(more, 0, sizeof more);
        more[0].line = 241; more[0].dot = 1; more[0].kind = NESVID_EVK_NMI;
        more[1].line = 60; more[1].dot = 260; more[1].kind = NESVID_EVK_IRQ;
        more[2].line = 50; more[2].dot = 102; more[2].kind = NESVID_EVK_SPRITE0;
        more[3].line = 70; more[3].dot = 5; more[3].kind = NESVID_EVK_MAPPER; more[3].addr = 0x8000; more[3].value = 7;
        nesvid_model_seed(&model, 0x90, 0x1E, 0, 0, 0, 0);
        nesvid_model_run(&model, more, 4, -2, 0, &log);
        check("events: three kept in order of arrival", log.nevents == 3 && log.events[0].kind == NESVID_EV_NMI &&
              log.events[2].kind == NESVID_EV_SPRITE0 && log.events[2].line == 50);
        check("mapper: counted on its line", log.mapper_writes == 1 && log.line[70].mapper_writes == 1 &&
              log.line[70].mapper_addr == 0x8000 && log.line[70].mapper_value == 7);
    }

    /* garbage in: the model neither crashes nor leaves its arrays */
    {
        static nesvid_ev junk[500];
        unsigned seed = 12345;
        for (int item = 0; item < 500; item++) {
            seed = seed * 1103515245u + 12345u;
            junk[item].line = (int16_t)((int)(seed >> 8) % 400 - 50);
            junk[item].dot = (uint16_t)((seed >> 4) % 500);
            junk[item].kind = (uint8_t)((seed >> 20) % 9);
            junk[item].addr = (uint16_t)(seed >> 3);
            junk[item].value = (uint8_t)(seed >> 11);
            junk[item].ttype = (uint8_t)((seed >> 17) % 4);
            junk[item].target = (uint16_t)(seed >> 13);
        }
        nesvid_model_seed(&model, 0xFF, 0xFF, 0xFFFF, 0xFFFF, 0xFF, 1);
        nesvid_model_run(&model, junk, 500, -2, 0, &log);
        nesvid_model_run(&model, junk, 500, 120, 77, &log);
        check("junk events survive", log.nwrites <= NESVID_MAX_WRITES && log.nevents <= NESVID_MAX_EVENTS && model.vaddr < 0x8000);
    }
}

/* ---- made-up machines ---------------------------------------------------------------------------- */

static nesvid_capture *new_cap(void)
{
    nesvid_capture *cap = calloc(1, sizeof *cap);
    static const uint8_t pal[32] = { 0x0F, 0x16, 0x27, 0x18, 0x0F, 0x21, 0x31, 0x11, 0x0F, 0x2A, 0x1A, 0x0A, 0x0F, 0x06, 0x16, 0x26,
                                     0x0F, 0x12, 0x22, 0x32, 0x0F, 0x14, 0x24, 0x34, 0x0F, 0x1C, 0x2C, 0x3C, 0x0F, 0x08, 0x18, 0x28 };
    if (!cap) { fprintf(stderr, "out of memory\n"); exit(1); }
    for (int idx = 0; idx < 64; idx++)
        cap->rgb[idx] = 0xff000000u | (uint32_t)((idx * 4) << 16) | (uint32_t)(((63 - idx) * 4) << 8) | (uint32_t)(idx * 3);
    memcpy(cap->pal, pal, sizeof pal);
    for (int tile = 0; tile < 512; tile++)
        for (int row = 0; row < 8; row++) {
            cap->chr[tile * 16 + row] = (uint8_t)((tile & 1) ? 0xF0 : 0x0F);
            cap->chr[tile * 16 + 8 + row] = (uint8_t)((tile & 2) ? 0xAA : 0x00);
        }
    for (int ntab = 0; ntab < 4; ntab++) {
        for (int cell = 0; cell < 960; cell++) cap->ntab[ntab * 0x400 + cell] = (uint8_t)(cell * 7 + ntab);
        for (int attrib = 0; attrib < 64; attrib++) cap->ntab[ntab * 0x400 + 0x3C0 + attrib] = 0x1B;
    }
    for (int spr = 0; spr < 64; spr++) cap->oam[spr * 4] = 0xFF;
    cap->oam[0] = 49; cap->oam[1] = 1; cap->oam[2] = 0; cap->oam[3] = 100;
    cap->oam[4] = 80; cap->oam[5] = 3; cap->oam[6] = 0x21; cap->oam[7] = 100;      /* behind the background */
    cap->ctrl = 0x90; cap->mask = 0x1E; cap->status = 0x80;
    cap->mirroring = NESVID_MIRROR_VERTICAL;
    cap->region = NESVID_NTSC; cap->nlines = 262; cap->nmi_line = 241;
    cap->mem_valid = 1;
    cap->beam_line = -2;
    cap->frame = 5;
    cap->serial = 1;
    return cap;
}

/* a frame whose log has an X split at line 100 (X = 80) */
static nesvid_capture *split_cap(void)
{
    nesvid_capture *cap = new_cap();
    nesvid_model model;
    nesvid_ev evs[4];
    memset(evs, 0, sizeof evs);
    evs[0] = evw(99, 250, 5, 80);
    evs[1] = evw(99, 260, 5, 0);
    evs[2].line = 50; evs[2].dot = 102; evs[2].kind = NESVID_EVK_SPRITE0;
    nesvid_model_seed(&model, 0x90, 0x1E, 0, 0, 0, 0);
    nesvid_model_run(&model, evs, 3, -2, 0, &cap->flog);
    cap->log_valid = 1;
    cap->log_frame = 4;
    for (int row = 0; row < NESVID_H; row++)
        for (int col = 0; col < NESVID_W; col++)
            cap->pixels[row * NESVID_W + col] = 0xff000000u | (uint32_t)((row * 3) << 16) | (uint32_t)((col & 255) << 8) | (uint32_t)(row ^ col);
    cap->fb_valid = 1;
    return cap;
}

static unsigned refresh(nesvid_tab *tab) { return nesvid_tab_refresh(tab, 1); }

static const char *section_text(nesvid_tab *tab, int section)
{
    static char buf[16384];
    nesvid_tab_text(tab, section, buf, (int)sizeof buf);
    return buf;
}

/* ---- the checks ---------------------------------------------------------------------------------- */

static void test_renderers(void)
{
    nesvid_capture *cap = new_cap();
    static uint32_t img[NESPPU_NT_W * NESPPU_NT_H];
    int aliases[4];

    nesppu_render_palette(cap, img);
    check("palette: entry 0 is the backdrop colour", img[0] == nesppu_rgb(cap, 0x0F));
    check("palette: entry 17 sits in the second row", img[(NESPPU_PAL_CELL + 3) * NESPPU_PAL_W + 3 * NESPPU_PAL_CELL + 4] == nesppu_rgb(cap, 0x22) ||
          img[(NESPPU_PAL_CELL + 3) * NESPPU_PAL_W + 1 * NESPPU_PAL_CELL + 4] == nesppu_rgb(cap, 0x12));
    check("slot colour: slot 4 shows the universal backdrop", nesppu_slot_colour(cap, 4) == 0x0F && nesppu_slot_colour(cap, 5) == 0x21);

    /* tile 1: plane 0 = 11110000 on every row */
    check("tile pixel: left half set", nesppu_tile_pixel(cap, 0, 1, 0, 0) == 1 && nesppu_tile_pixel(cap, 0, 1, 7, 3) == 0);
    check("tile pixel: tile 3 has both planes", nesppu_tile_pixel(cap, 0, 3, 0, 0) == 3 && nesppu_tile_pixel(cap, 0, 3, 1, 0) == 1 && nesppu_tile_pixel(cap, 0, 3, 2, 0) == 3);

    nesppu_render_patterns(cap, 1, img);
    check("patterns: tile 1 of table 0 at 8,0", img[0 * NESPPU_PAT_W + 8] == nesppu_rgb(cap, cap->pal[4 + 1]));
    check("patterns: the second table sits at 128", img[0 * NESPPU_PAT_W + 128 + 8] == nesppu_rgb(cap, cap->pal[4 + 1]));
    check("patterns: transparent shows the backdrop", img[0 * NESPPU_PAT_W + 15] == nesppu_rgb(cap, 0x0F));

    nesppu_render_nametables(cap, 0, img);
    {
        /* nametable 0 cell 0 = tile 0, attribute 0x1B -> palette 3 for the top-left quadrant */
        const int tile = cap->ntab[0];
        const int pix = nesppu_tile_pixel(cap, 0, tile, 2, 1);
        const int attr = cap->ntab[0x3C0];
        check("nametables: first cell", img[1 * NESPPU_NT_W + 2] == nesppu_rgb(cap, nesppu_slot_colour(cap, (attr & 3) * 4 + pix)));
        /* nametable 1 starts at x = 256 */
        check("nametables: the second table", img[256] == nesppu_rgb(cap, nesppu_slot_colour(cap,
              ((cap->ntab[0x400 + 0x3C0] & 3) * 4) + nesppu_tile_pixel(cap, 0, cap->ntab[0x400], 0, 0))));
    }

    nesppu_render_sprites(cap, 0, 0, img);
    check("sprites: the panel behind", img[0] == NESPPU_PANEL);
    check("sprites: sprite 0 at 100,50", img[50 * NESPPU_SPR_W + 100] == nesppu_rgb(cap, nesppu_slot_colour(cap, 16 + 1)));
    check("sprites: its transparent half", img[50 * NESPPU_SPR_W + 105] == NESPPU_PANEL);

    check("aliases: vertical mirroring pairs 0/2", nesppu_nt_aliases(NESVID_MIRROR_VERTICAL, 0, aliases) == 2 && aliases[0] == 0 && aliases[1] == 2);
    check("aliases: horizontal pairs 0/1", nesppu_nt_aliases(NESVID_MIRROR_HORIZONTAL, 3, aliases) == 2 && aliases[0] == 2 && aliases[1] == 3);
    check("aliases: four-screen has none", nesppu_nt_aliases(NESVID_MIRROR_FOUR, 1, aliases) == 1);
    check("aliases: one-screen has all", nesppu_nt_aliases(NESVID_MIRROR_SCREEN_A, 2, aliases) == 4);
    free(cap);
}

static void test_display_and_views(void)
{
    nesvid_capture *cap = split_cap();
    nesvid_tab *tab = nesvid_tab_new(stub_session());
    int wide = 0, high = 0;
    const uint32_t *pic;

    check("the tab asks for captures", stub_capture_enabled >= 1);
    nesvid_tab_feed(tab, cap, 0);
    nesvid_tab_set_zoom(tab, 1);
    nesvid_tab_set_overlays(tab, 0);
    check("refresh redraws", (refresh(tab) & NESVID_TAB_PICTURE) != 0);
    pic = nesvid_tab_picture(tab, &wide, &high);
    check("display: 256 x 240", pic && wide == 256 && high == 240);
    check("display: the published pixels", pic && memcmp(pic, cap->pixels, sizeof cap->pixels) == 0);

    nesvid_tab_set_zoom(tab, 2);
    refresh(tab);
    pic = nesvid_tab_picture(tab, &wide, &high);
    check("display: zoom 2", pic && wide == 512 && high == 480 && pic[1] == cap->pixels[0] && pic[512 + 1] == cap->pixels[0]);

    nesvid_tab_set_zoom(tab, 0);
    nesvid_tab_set_viewport(tab, 600, 500);
    refresh(tab);
    check("fit picks the largest zoom that fits", nesvid_tab_effective_zoom(tab) == 2);
    nesvid_tab_set_viewport(tab, 2000, 2000);
    refresh(tab);
    check("fit grows with the viewport", nesvid_tab_effective_zoom(tab) == 6);
    nesvid_tab_set_zoom(tab, 1);

    nesvid_tab_set_view(tab, NESVID_V_DECODED);
    refresh(tab);
    pic = nesvid_tab_picture(tab, &wide, &high);
    check("decoded nametables: 512 x 480", pic && wide == 512 && high == 480);
    {
        static uint32_t want[NESPPU_NT_W * NESPPU_NT_H];
        /* the band with the most lines is the second (100-239); both use bg table 0x1000 */
        nesppu_render_nametables(cap, 0x1000, want);
        check("decoded nametables: the renderer's pixels (no overlays)", pic && memcmp(pic, want, sizeof want) == 0);
    }
    nesvid_tab_set_overlays(tab, NESVID_OV_SCROLL);
    refresh(tab);
    pic = nesvid_tab_picture(tab, &wide, &high);
    {
        static uint32_t want[NESPPU_NT_W * NESPPU_NT_H];
        nesppu_render_nametables(cap, 0x1000, want);
        check("decoded nametables: the scroll windows paint over it", pic && memcmp(pic, want, sizeof want) != 0);
        /* band 2: X 80, Y 100 at line 100: its left edge column is x = 80, rows 100.. */
        check("decoded nametables: band edge pixel", pic && pic[150 * NESPPU_NT_W + 80] != want[150 * NESPPU_NT_W + 80]);
    }
    nesvid_tab_set_overlays(tab, 0);

    nesvid_tab_set_source(tab, NESVID_SRC_PATTERNS);
    nesvid_tab_set_palette(tab, 5);
    refresh(tab);
    pic = nesvid_tab_picture(tab, &wide, &high);
    check("decoded patterns: 256 x 128", pic && wide == 256 && high == 128);
    check("decoded patterns: the chosen palette", pic && pic[8] == nesppu_rgb(cap, cap->pal[5 * 4 + 1]));
    check("decoded patterns: palette getter", nesvid_tab_palette(tab) == 5);
    nesvid_tab_set_palette(tab, 9);
    check("decoded patterns: palette is clamped", nesvid_tab_palette(tab) == 5);

    nesvid_tab_set_source(tab, NESVID_SRC_SPRITES);
    refresh(tab);
    pic = nesvid_tab_picture(tab, &wide, &high);
    check("decoded sprites: 256 x 240", pic && wide == 256 && high == 240);

    nesvid_tab_set_source(tab, NESVID_SRC_PALETTE);
    refresh(tab);
    pic = nesvid_tab_picture(tab, &wide, &high);
    check("decoded palette: 256 x 32", pic && wide == 256 && high == 32);

    nesvid_tab_set_view(tab, NESVID_V_VALUES);
    refresh(tab);
    pic = nesvid_tab_picture(tab, &wide, &high);
    check("values: 512 x 480", pic && wide == 512 && high == 480);
    check("values: cells with different bytes differ", pic && pic[0] != pic[8]);
    check("values: the same byte is the same colour", pic && pic[0] == pic[3 * NESPPU_NT_W + 7]);

    nesvid_tab_set_view(tab, NESVID_V_BEAM);
    refresh(tab);
    check("beam: refused while running, with a reason", nesvid_tab_picture(tab, &wide, &high) == NULL && contains(nesvid_tab_why(tab), "stopped"));

    nesvid_tab_free(tab);
    free(cap);
}

static void test_bands_and_tables(void)
{
    nesvid_capture *cap = split_cap();
    nesvid_tab *tab = nesvid_tab_new(stub_session());
    const nesvid_band *lines;
    const nesvid_reg *regs;
    const nesvid_swatch *swat;
    const nesvid_spriterow *rows;
    const char *const *timing;
    int num;

    nesvid_tab_feed(tab, cap, 0);
    refresh(tab);
    num = nesvid_tab_lines(tab, &lines);
    check("lines: two bands", num == 2);
    check("lines: the split is at 100", num == 2 && lines[0].first == 0 && lines[0].last == 99 && lines[1].first == 100 && lines[1].last == 239);
    check("lines: scroll of each", num == 2 && lines[0].scroll_x == 0 && lines[1].scroll_x == 80 && lines[1].scroll_y == 100);
    check("lines: the change is named", num == 2 && contains(lines[1].text, "scroll") && lines[1].writes == 1);
    check("lines: text", contains(section_text(tab, NESVID_S_LINES), "lines 100-239"));

    num = nesvid_tab_regs(tab, &regs);
    check("registers: seven rows", num == 7);
    check("registers: PPUCTRL", num == 7 && strcmp(regs[0].name, "PPUCTRL") == 0 && strcmp(regs[0].value, "90") == 0 && contains(regs[0].meaning, "NMI on"));
    check("registers: PPUMASK", num == 7 && strcmp(regs[1].value, "1E") == 0 && contains(regs[1].meaning, "background on"));
    check("registers: scroll writes from the log", num == 7 && regs[4].writes == 2 && regs[4].first_line == 99);
    check("registers: first refresh counts as old", num == 7 && regs[0].age >= NESVID_REG_FADE);
    cap->ctrl = 0x80;
    nesvid_tab_feed(tab, cap, 0);
    refresh(tab);
    nesvid_tab_regs(tab, &regs);
    check("registers: a change resets the age", regs[0].age == 0 && regs[1].age >= 1);
    check("registers: text", contains(section_text(tab, NESVID_S_REGISTERS), "PPUSCROLL") && contains(section_text(tab, NESVID_S_REGISTERS), "first on line 99"));

    num = nesvid_tab_swatches(tab, &swat);
    check("colours: 32 entries", num == 32);
    check("colours: names", num == 32 && strcmp(swat[0].name, "Backdrop") == 0 && strcmp(swat[1].name, "BG 0.1") == 0 && strcmp(swat[17].name, "Sprite 0.1") == 0);
    check("colours: master colour and rgb", num == 32 && swat[1].colour == 0x16 && swat[1].rgb == nesppu_rgb(cap, 0x16));
    {
        nesvid_capture *two = new_cap();
        nesvid_tab *other = nesvid_tab_new(NULL);
        for (int item = 0; item < NESVID_W * NESVID_H; item++)
            two->pixels[item] = nesppu_rgb(two, item < 1000 ? 0x16 : 0x21);
        two->fb_valid = 1;
        nesvid_tab_feed(other, two, 0);
        refresh(other);
        nesvid_tab_swatches(other, &swat);
        check("colours: usage counted from the frame", swat[1].uses == 1000 && swat[5].uses == NESVID_W * NESVID_H - 1000);
        nesvid_tab_free(other);
        free(two);
    }

    num = nesvid_tab_sprites(tab, &rows);
    check("sprites section: 64 rows", num == 64);
    check("sprites section: sprite 0", num == 64 && contains(rows[0].text, "x=100") && contains(rows[0].text, "tile=$01") && rows[0].visible);
    check("sprites section: a hidden one", num == 64 && !rows[9].visible && contains(rows[9].text, "off screen"));
    check("sprites section: flags", num == 64 && contains(rows[1].text, "behind") && contains(rows[1].text, "pal=1"));

    num = nesvid_tab_timing(tab, &timing);
    check("timing: lines", num > 5 && contains(timing[0], "NTSC") && contains(timing[0], "60.09"));
    check("timing: sprite 0 hit from the log", contains(section_text(tab, NESVID_S_TIMING), "Sprite 0 hit: line 50, dot 102"));
    check("timing: beam only when stopped", contains(section_text(tab, NESVID_S_TIMING), "(stopped only)"));
    check("status", contains(nesvid_tab_status(tab), "NTSC") && contains(nesvid_tab_status(tab), "2 bands"));

    cap->region = NESVID_PAL; cap->nlines = 312; cap->nmi_line = 241;
    nesvid_tab_feed(tab, cap, 0);
    refresh(tab);
    nesvid_tab_timing(tab, &timing);
    check("timing: PAL", contains(timing[0], "PAL") && contains(timing[0], "50.00") && contains(section_text(tab, NESVID_S_TIMING), "33247.5"));
    cap->region = NESVID_DENDY; cap->nlines = 312; cap->nmi_line = 291;
    nesvid_tab_feed(tab, cap, 0);
    refresh(tab);
    check("timing: Dendy", contains(section_text(tab, NESVID_S_TIMING), "Dendy") && contains(section_text(tab, NESVID_S_TIMING), "line 291"));

    check("section names", nesvid_section_from_name("reg") == NESVID_S_REGISTERS && nesvid_section_from_name("sprites") == NESVID_S_SPRITES &&
          nesvid_section_from_name("col") == NESVID_S_COLOURS && nesvid_section_from_name("zzz") < 0 && nesvid_section_from_name("oam") == NESVID_S_SPRITES);
    nesvid_tab_free(tab);
    free(cap);
}

static void test_inspector(void)
{
    nesvid_capture *cap = split_cap();
    nesvid_tab *tab = nesvid_tab_new(stub_session());
    const nesvid_pixel *pixp;
    char want[96];
    int hover[4], pin[4];

    nesvid_tab_feed(tab, cap, 0);
    nesvid_tab_set_zoom(tab, 1);
    refresh(tab);

    /* a background pixel in band 2: scroll X 80, so x=10 is at 90 in the plane: column 11 */
    check("hover reports a change", nesvid_tab_hover(tab, 10, 150) == 1);
    pixp = nesvid_tab_pixel(tab);
    check("inspector: a pixel", pixp && pixp->valid && pixp->screenx == 10 && pixp->screeny == 150 && pixp->line == 150);
    {
        const int row = 150 / 8, col = (80 + 10) / 8, finex = (80 + 10) % 8, finey = 150 % 8;
        const int tile = cap->ntab[row * 32 + col];
        const unsigned name = 0x2000u + (unsigned)(row * 32 + col);
        const int pix = nesppu_tile_pixel(cap, 0x1000, tile, finex, finey);
        snprintf(want, sizeof want, "entry $%04X holds tile $%02X", name, tile);
        check("inspector: the nametable entry the PPU fetched", pixp && contains(pixp->text, want));
        check("inspector: the tile", pixp && pixp->kind == (pix ? NESVID_PK_TILE : NESVID_PK_BACKDROP) && pixp->tile == tile && pixp->ntab == 0 && pixp->cell == row * 32 + col);
        snprintf(want, sizeof want, "pattern row at PPU $%04X", 0x1000 + tile * 16 + finey);
        check("inspector: the pattern byte", pixp && (contains(pixp->text, want) || contains(pixp->text, "Pattern row at PPU $")));
        check("inspector: the palette entry", pixp && pixp->slot >= 0 && contains(pixp->text, "master colour"));
        check("inspector: memory link", pixp && pixp->mem_space == NESDEBUG_SPACE_PPU && pixp->mem_addr == name);
    }
    check("inspector: band edge named", (nesvid_tab_hover(tab, 10, 100), contains(nesvid_tab_pixel(tab)->text, "a new band starts here")));
    check("inspector: the same pixel is not a change", nesvid_tab_hover(tab, 10, 100) == 0);

    /* before the split the scroll is 0 */
    nesvid_tab_hover(tab, 10, 20);
    pixp = nesvid_tab_pixel(tab);
    {
        const int tile = cap->ntab[(20 / 8) * 32 + 1];
        snprintf(want, sizeof want, "holds tile $%02X", tile);
        check("inspector: band 1 reads column 1", pixp && contains(pixp->text, want) && contains(pixp->text, "scroll 0,20"));
    }

    /* sprite 0 is in front at its first pixel */
    nesvid_tab_hover(tab, 100, 50);
    pixp = nesvid_tab_pixel(tab);
    check("inspector: the front sprite", pixp && pixp->kind == NESVID_PK_SPRITE && pixp->sprite == 0 && contains(pixp->text, "Sprite 0 in front"));
    check("inspector: sprite memory link", pixp && pixp->mem_space == NESDEBUG_SPACE_OAM && pixp->mem_addr == 0);
    check("inspector: sprite 0 hit note", pixp && contains(pixp->text, "sprite 0 hit"));
    check("inspector: marks", nesvid_tab_marks(tab, hover, pin) == 1 && hover[0] == 100 && hover[1] == 50 && hover[2] == 8 && hover[3] == 8);

    /* the behind-background sprite loses to an opaque background pixel */
    nesvid_tab_hover(tab, 100, 81);
    pixp = nesvid_tab_pixel(tab);
    check("inspector: a sprite behind an opaque background is not in front", pixp && pixp->kind != NESVID_PK_SPRITE);

    /* nowhere */
    check("hover leaves", nesvid_tab_hover(tab, -1, -1) == 1);
    check("inspector: nothing under no pointer", nesvid_tab_pixel(tab) == NULL);

    /* break on writes */
    stub_nbps = 0;
    check("break: nothing to break on without a pixel", !nesvid_tab_can_break(tab) && nesvid_tab_break_on_pixel(tab) == -1);
    nesvid_tab_pin(tab, 10, 150);
    check("break: a pinned tile offers it", nesvid_tab_can_break(tab));
    {
        const int first = nesvid_tab_break_on_pixel(tab);
        int saw_name = 0, saw_pattern = 0, saw_pal = 0, saw_attr = 0;
        check("break: an id came back", first >= 1);
        for (int item = 0; item < stub_nbps; item++) {
            const nesdebug_breakpoint *entry = &stub_bps[item];
            if (entry->type != NESDEBUG_BP_WRITE) continue;
            if (entry->space == NESDEBUG_SPACE_PPU && entry->start == entry->end && entry->start >= 0x2000 && entry->start < 0x23C0) saw_name = 1;
            if (entry->space == NESDEBUG_SPACE_PPU && entry->start == entry->end && entry->start >= 0x23C0 && entry->start < 0x2400) saw_attr = 1;
            if (entry->space == NESDEBUG_SPACE_PPU && entry->end - entry->start == 15 && entry->start >= 0x1000) saw_pattern = 1;
            if (entry->space == NESDEBUG_SPACE_PALETTE) saw_pal = 1;
        }
        check("break: the name table entry, its attribute, its pattern and its colour", saw_name && saw_attr && saw_pattern && saw_pal);
        {
            const int count = stub_nbps;
            nesvid_tab_break_on_pixel(tab);
            check("break: not added twice", stub_nbps == count);
        }
        {
            /* vertical mirroring: nametable 0 and 2 show the same bytes, so both are watched */
            int low = 0, high = 0;
            for (int item = 0; item < stub_nbps; item++) {
                if (stub_bps[item].space == NESDEBUG_SPACE_PPU && stub_bps[item].start == 0x2000 + (150 / 8) * 32 + 11) low = 1;
                if (stub_bps[item].space == NESDEBUG_SPACE_PPU && stub_bps[item].start == 0x2800 + (150 / 8) * 32 + 11) high = 1;
            }
            check("break: the mirrored nametable too", low && high);
        }
    }
    nesvid_tab_unpin(tab);

    /* a pixel at the split offers the registers that changed there */
    stub_nbps = 0;
    nesvid_tab_pin(tab, 10, 120);
    nesvid_tab_break_on_pixel(tab);
    {
        int scroll = 0;
        for (int item = 0; item < stub_nbps; item++)
            if (stub_bps[item].space == NESDEBUG_SPACE_CPU && stub_bps[item].start == 0x2005 && stub_bps[item].end == 0x2006) scroll = 1;
        check("break: a band at a scroll split offers $2005-$2006", scroll);
    }
    nesvid_tab_unpin(tab);

    /* a sprite */
    stub_nbps = 0;
    nesvid_tab_pin(tab, 100, 50);
    nesvid_tab_break_on_pixel(tab);
    {
        int oam = 0;
        for (int item = 0; item < stub_nbps; item++)
            if (stub_bps[item].space == NESDEBUG_SPACE_OAM && stub_bps[item].start == 0 && stub_bps[item].end == 3) oam = 1;
        check("break: a sprite's four OAM bytes", oam);
    }
    check("pin again unpins", (nesvid_tab_pin(tab, 100, 50), nesvid_tab_pixel(tab) == NULL || !nesvid_tab_pixel(tab)->valid));

    /* registers */
    stub_nbps = 0;
    check("break on a register row", nesvid_tab_break_on_register(tab, 4) >= 1 && stub_nbps == 1 && stub_bps[0].space == NESDEBUG_SPACE_CPU &&
          stub_bps[0].start == 0x2005 && stub_bps[0].type == NESDEBUG_BP_WRITE);
    check("break on PPUSTATUS has no target", nesvid_tab_break_on_register(tab, 2) == -1 && nesvid_tab_break_on_register(tab, 99) == -1);

    /* the Nametables source */
    nesvid_tab_set_view(tab, NESVID_V_DECODED);
    nesvid_tab_set_source(tab, NESVID_SRC_NAMETABLES);
    nesvid_tab_set_zoom(tab, 2);
    refresh(tab);
    nesvid_tab_hover(tab, 2 * (256 + 8 * 3 + 1), 2 * (8 * 4 + 1));
    pixp = nesvid_tab_pixel(tab);
    check("nametable pixel: the cell", pixp && pixp->kind == NESVID_PK_TILE && pixp->ntab == 1 && pixp->cell == 960 + 4 * 32 + 3 &&
          pixp->tile == cap->ntab[0x400 + 4 * 32 + 3] && contains(pixp->text, "Nametable 1 (at $2400), column 3, row 4"));
    check("nametable pixel: memory link", pixp && pixp->mem_space == NESDEBUG_SPACE_PPU && pixp->mem_addr == 0x2400u + 4 * 32 + 3);
    nesvid_tab_hover(tab, 2 * (80 + 20), 2 * (100 + 5));
    pixp = nesvid_tab_pixel(tab);
    check("nametable pixel: inside the second band's window", pixp && contains(pixp->text, "scroll window of lines 100-239"));
    check("nametable pixel: marks scale with the zoom", (nesvid_tab_marks(tab, hover, pin) & 1) && hover[2] == 16 && hover[3] == 16);

    nesvid_tab_set_source(tab, NESVID_SRC_PATTERNS);
    nesvid_tab_set_zoom(tab, 1);
    refresh(tab);
    nesvid_tab_hover(tab, 128 + 8 * 2 + 1, 8 * 3 + 1);
    pixp = nesvid_tab_pixel(tab);
    check("pattern pixel: table 1 tile $32", pixp && pixp->kind == NESVID_PK_PATTERN && pixp->tile == 3 * 16 + 2 && pixp->ppu_addr == 0x1000u + (3 * 16 + 2) * 16 &&
          contains(pixp->text, "Pattern table 1"));

    nesvid_tab_set_source(tab, NESVID_SRC_PALETTE);
    refresh(tab);
    nesvid_tab_hover(tab, 16 * 2 + 3, 16 + 3);
    pixp = nesvid_tab_pixel(tab);
    check("palette pixel: entry 18", pixp && pixp->kind == NESVID_PK_PALETTE && pixp->slot == 18 && pixp->colour == cap->pal[18] &&
          pixp->mem_space == NESDEBUG_SPACE_PALETTE && pixp->mem_addr == 18 && pixp->rgb == nesppu_rgb(cap, cap->pal[18]));

    nesvid_tab_set_source(tab, NESVID_SRC_SPRITES);
    refresh(tab);
    nesvid_tab_hover(tab, 101, 52);
    pixp = nesvid_tab_pixel(tab);
    check("sprites pixel: sprite 0", pixp && pixp->kind == NESVID_PK_SPRITE && pixp->sprite == 0 && pixp->mem_space == NESDEBUG_SPACE_OAM);

    nesvid_tab_free(tab);
    free(cap);
}

static void test_parked(void)
{
    nesvid_capture *cap = split_cap();
    nesvid_tab *tab = nesvid_tab_new(stub_session());
    int wide, high;
    const uint32_t *pic;
    const nesvid_band *lines;

    /* the machine is parked at line 120: lines up to there are the frame in progress's, with another scroll */
    for (int item = 0; item < NESVID_W * NESVID_H; item++) cap->live_pixels[item] = 0xff112233u;
    cap->live_pixels_valid = 1;
    cap->live = 1;
    cap->beam_line = 120; cap->beam_dot = 77;
    cap->cur_valid = 1;
    {
        nesvid_model model;
        nesvid_ev evs[2];
        memset(evs, 0, sizeof evs);
        evs[0] = evw(60, 250, 5, 160);
        evs[1] = evw(60, 260, 5, 0);
        nesvid_model_seed(&model, 0x90, 0x1E, 0, 0, 0, 0);
        nesvid_model_run(&model, evs, 2, 120, 77, &cap->clog);
    }
    nesvid_tab_feed(tab, cap, 1);
    nesvid_tab_set_zoom(tab, 1);
    nesvid_tab_set_view(tab, NESVID_V_BEAM);
    refresh(tab);
    pic = nesvid_tab_picture(tab, &wide, &high);
    check("beam: 256 x 240", pic && wide == 256 && high == 240);
    check("beam: the live rows above the beam", pic && pic[10 * 256 + 10] == 0xff112233u);
    check("beam: dimmed below", pic && pic[200 * 256 + 10] != 0xff112233u && (pic[200 * 256 + 10] & 0xff) < 0x33);
    check("beam: the beam's own line is not dimmed", pic && pic[120 * 256 + 10] == 0xff112233u && pic[121 * 256 + 10] != 0xff112233u);
    nesvid_tab_set_view(tab, NESVID_V_DISPLAY);
    nesvid_tab_set_overlays(tab, 0);
    refresh(tab);
    pic = nesvid_tab_picture(tab, &wide, &high);
    check("display while parked: still the finished frame", pic && memcmp(pic, cap->pixels, sizeof cap->pixels) == 0);
    nesvid_tab_lines(tab, &lines);
    {
        /* lines 0-120 from the frame in progress (split at 61), the rest from the finished frame (split at 100) */
        const nesvid_band *bands;
        const int count = nesvid_tab_lines(tab, &bands);
        check("parked bands: merged from both logs", count == 3 && bands[1].first == 61 && bands[1].scroll_x == 160 &&
              bands[2].first == 121);
    }
    check("parked status", contains(nesvid_tab_status(tab), "Stopped") && contains(nesvid_tab_status(tab), "beam at line 120"));
    check("parked timing", contains(section_text(tab, NESVID_S_TIMING), "Beam: line 120 (picture), dot 77"));
    check("beam permille", nesvid_tab_beam_permille(tab) > 400 && nesvid_tab_beam_permille(tab) < 500);
    nesvid_tab_set_overlays(tab, NESVID_OV_ALL);
    refresh(tab);
    check("every overlay draws on a parked machine", nesvid_tab_picture(tab, &wide, &high) != NULL);
    nesvid_tab_free(tab);
    free(cap);
}

static void test_no_machine(void)
{
    nesvid_tab *tab = nesvid_tab_new(stub_session());
    unsigned flags;
    int wide = 0, high = 0;
    stub_stopped = 0;
    flags = nesvid_tab_refresh(tab, 1);
    check("no capture yet: says so", (flags & NESVID_TAB_OFF) && nesvid_tab_picture(tab, &wide, &high) == NULL && contains(nesvid_tab_why(tab), "No machine"));
    nesvid_tab_free(tab);

    tab = nesvid_tab_new(NULL);
    flags = nesvid_tab_refresh(tab, 1);
    check("no session: says so", (flags & NESVID_TAB_OFF) != 0);
    check("no session: every call is safe", nesvid_tab_break_on_pixel(tab) == -1 && !nesvid_tab_can_break(tab) &&
          nesvid_tab_break_on_register(tab, 0) == -1 && nesvid_tab_pixel(tab) == NULL);
    nesvid_tab_hover(tab, 3, 3);
    nesvid_tab_pin(tab, 3, 3);
    nesvid_tab_free(tab);
    nesvid_tab_free(NULL);

    /* the capture comes back to zero after every tab is freed */
    check("tabs balance the capture count", stub_capture_enabled == 0);
}

static void test_console_text(void)
{
    nesvid_tab *tab;
    const char *text;
    nesvid_capture *cap = split_cap();
    (void)cap;
    stub_stopped = 1;
    tab = nesvid_tab_new(stub_session());
    nesvid_tab_register_console(stub_debugger());
    check("console: the hook is installed", stub_video_text != NULL);
    text = stub_video_text ? stub_video_text("") : NULL;
    check("console: video prints the registers", text && contains(text, "PPUCTRL"));
    text = stub_video_text ? stub_video_text("lines") : NULL;
    check("console: video lines", text != NULL);
    text = stub_video_text ? stub_video_text("nonsense") : NULL;
    check("console: an unknown section says so", text && contains(text, "unknown section"));
    nesvid_tab_register_console(NULL);
    check("console: the hook is removed", stub_video_text == NULL);
    nesvid_tab_free(tab);
    stub_stopped = 0;
    free(cap);
}

/* ---- fuzz --------------------------------------------------------------------------------------- */

static unsigned fz_seed = 1;
static unsigned rnd(void)
{
    fz_seed = fz_seed * 1664525u + 1013904223u;
    return fz_seed >> 8;
}

static void test_fuzz(void)
{
    nesvid_capture *cap = calloc(1, sizeof *cap);
    nesvid_tab *tab = nesvid_tab_new(stub_session());
    int survived = 0;
    if (!cap || !tab) { check("fuzz allocs", 0); return; }
    for (int round = 0; round < 60; round++) {
        uint8_t *raw = (uint8_t *)cap;
        /* random bytes over the PPU part, the log and the flags */
        for (size_t item = offsetof(nesvid_capture, chr); item < sizeof *cap; item++)
            raw[item] = (uint8_t)rnd();
        for (int item = 0; item < NESVID_W * NESVID_H; item++) {
            cap->pixels[item] = rnd();
            cap->live_pixels[item] = rnd();
        }
        cap->fb_valid = (int)(rnd() % 2);
        cap->live_pixels_valid = (int)(rnd() % 2);
        cap->mem_valid = (int)(rnd() % 4 != 0);
        cap->log_valid = (int)(rnd() % 2);
        cap->cur_valid = (int)(rnd() % 2);
        cap->mirroring = (int)(rnd() % 7);
        cap->region = (int)(rnd() % 4);
        cap->nlines = (int)(rnd() % 400);
        cap->nmi_line = (int)(rnd() % 400);
        cap->beam_line = (int)(rnd() % 400) - 20;
        cap->beam_dot = (int)(rnd() % 500);
        cap->flog.nwrites = (int)(rnd() % 400);
        cap->flog.nevents = (int)(rnd() % 50);
        cap->clog.nwrites = (int)(rnd() % 400);
        cap->clog.nevents = (int)(rnd() % 50);
        cap->clog.last_line = (int)(rnd() % 300) - 20;
        for (int line = 0; line < NESVID_H; line++) {
            cap->flog.line[line].valid = (uint8_t)(rnd() % 2);
            cap->clog.line[line].valid = (uint8_t)(rnd() % 2);
        }
        for (int reg = 0; reg < 8; reg++) cap->flog.reg_first[reg] = (int16_t)(rnd() % 500);
        cap->serial = (uint64_t)round + 1;
        nesvid_tab_feed(tab, cap, (int)(rnd() % 2));
        for (int step = 0; step < 24; step++) {
            switch (rnd() % 9) {
            case 0: nesvid_tab_set_view(tab, (int)(rnd() % 5) - 1); break;
            case 1: nesvid_tab_set_source(tab, (int)(rnd() % 5) - 1); break;
            case 2: nesvid_tab_set_overlays(tab, rnd()); break;
            case 3: nesvid_tab_set_zoom(tab, (int)(rnd() % 18) - 1); break;
            case 4: nesvid_tab_set_palette(tab, (int)(rnd() % 10) - 1); break;
            case 5: nesvid_tab_set_viewport(tab, (int)(rnd() % 3000) - 5, (int)(rnd() % 3000) - 5); break;
            case 6: nesvid_tab_hover(tab, (int)(rnd() % 8200) - 100, (int)(rnd() % 8200) - 100); break;
            case 7: nesvid_tab_pin(tab, (int)(rnd() % 1200) - 20, (int)(rnd() % 1000) - 20); break;
            default: stub_nbps = 0; nesvid_tab_break_on_pixel(tab); break;
            }
            refresh(tab);
            for (int sec = 0; sec < NESVID_S_COUNT; sec++) (void)section_text(tab, sec);
            survived++;
        }
    }
    check("fuzz survived", survived == 60 * 24);
    nesvid_tab_free(tab);
    free(cap);
}

/* ---- the environment variables ---------------------------------------------------------------- */

static void test_env(void)
{
    nesvid_tab *tab = nesvid_tab_new(NULL);
    check("env: view", nesvid_tab_view(tab) == NESVID_V_BEAM);
    check("env: source", nesvid_tab_source(tab) == NESVID_SRC_PALETTE);
    check("env: overlays", nesvid_tab_overlays(tab) == (NESVID_OV_GRID | NESVID_OV_SCROLL));
    check("env: zoom", nesvid_tab_zoom(tab) == 4);
    check("env: section", nesvid_tab_env_section(tab) == NESVID_S_TIMING);
    nesvid_tab_free(tab);
}

int main(int argc, char **argv)
{
    int count, zoom_count;
    const nesvid_overlay_info *overlays = nesvid_overlays(&count);
    const int *zooms = nesvid_zooms(&zoom_count);

    if (argc > 1 && strcmp(argv[1], "env") == 0) {
        test_env();
        if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
        printf("vidtab_test env: ok\n");
        return 0;
    }
    check("tables: six overlays, nine zooms", overlays && count == 6 && zoom_count == 9 && zooms[0] == 0);
    check("names", strcmp(nesvid_view_name(NESVID_V_BEAM), "Beam") == 0 && strcmp(nesvid_source_key(NESVID_SRC_NAMETABLES), "nametables") == 0 &&
          strcmp(nesvid_section_name(NESVID_S_SPRITES), "Sprites") == 0 && nesvid_view_name(9)[0] == '\0');
    test_model();
    test_renderers();
    test_display_and_views();
    test_bands_and_tables();
    test_inspector();
    test_parked();
    test_no_machine();
    test_console_text();
    test_fuzz();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("vidtab_test: ok\n");
    return 0;
}
