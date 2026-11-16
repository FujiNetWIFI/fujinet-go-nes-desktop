/*
 * vidtab_live_test -- the Video tab's capture on a running machine: a tiny
 * NROM program draws nametable 0 with background and a sprite, and its NMI
 * handler moves the X scroll in the middle of every frame (a status-bar
 * style split). The capture must see the frame, the PPU's memory, the split
 * line by line (the replayed model must agree with the PPU's own registers
 * every frame), and a parked machine must be read live.
 *
 * Needs no ROM file: the program is assembled below.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nessession.h"
#include "nesdebug.h"
#include "nesvidcap.h"
#include "nesvidtab.h"
#include "ppumodel.h"
#include "test_tmpdir.h"

static int failures;
static void check(int passed, const char *what)
{
    printf("%s: %s\n", passed ? "ok" : "FAIL", what);
    if (!passed) failures++;
}

static void sleep_ms(int millis)
{
    struct timespec pause = { millis / 1000, (long)(millis % 1000) * 1000000L };
    nanosleep(&pause, NULL);
}

static int wait_stopped(nesdebug *dbg, int want, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        if (!!nesdebug_is_stopped(dbg) == want) return 1;
        sleep_ms(5); waited += 5;
    }
    return 0;
}

/* The program, at $C000 of a 16K NROM:
 *   reset: PPU off, two vblank waits, palette ($3F00, 16 bytes), nametable 0
 *          ($2000-$23FF, byte i = i & 255, so the tile numbers count up and the
 *          attribute bytes are 192-255), OAM cleared, sprite 0 at x=100 y=49
 *          tile 1, scroll 0,0, PPUCTRL $90 (NMI on, background from $1000),
 *          PPUMASK $1E, then JMP *.
 *   NMI:   scroll 0,0 for the next frame, a delay of about 12.9K cycles (to
 *          line ~92 of that frame), then PPUSCROLL X = 64 (a split: the rest
 *          of the frame is drawn from the X = 64 column).
 * The patterns: every tile's second plane is $FF, so every pixel is opaque
 * (colour 2 or 3) and the sprite 0 hit lands where the sprite starts. */
static size_t rom_write(const char *path)
{
    static uint8_t img[16 + 16384 + 8192];
    static const uint8_t code[] = {
        0x78, 0xD8, 0xA2, 0xFF, 0x9A,                       /* C000 SEI CLD LDX #$FF TXS */
        0xA9, 0x00, 0x8D, 0x00, 0x20, 0x8D, 0x01, 0x20,     /* C005 LDA #0 STA $2000 STA $2001 */
        0x2C, 0x02, 0x20, 0x10, 0xFB,                       /* C00D BIT $2002 BPL */
        0x2C, 0x02, 0x20, 0x10, 0xFB,                       /* C012 BIT $2002 BPL */
        0xA9, 0x3F, 0x8D, 0x06, 0x20, 0xA9, 0x00, 0x8D, 0x06, 0x20,    /* C017 palette address */
        0xA2, 0x00, 0xBD, 0xC0, 0xC0, 0x8D, 0x07, 0x20, 0xE8, 0xE0, 0x10, 0xD0, 0xF5,  /* C021 copy 16 */
        0xA9, 0x20, 0x8D, 0x06, 0x20, 0xA9, 0x00, 0x8D, 0x06, 0x20,    /* C02E nametable address */
        0xA0, 0x04, 0xA2, 0x00, 0x8E, 0x07, 0x20, 0xE8, 0xD0, 0xFA, 0x88, 0xD0, 0xF5,  /* C038 4 x 256 */
        0xA9, 0x00, 0x8D, 0x03, 0x20, 0xA9, 0xFF, 0xA2, 0x00, 0x8D, 0x04, 0x20, 0xE8, 0xD0, 0xFA,  /* C045 clear OAM */
        0xA9, 0x00, 0x8D, 0x03, 0x20,                       /* C054 OAMADDR = 0 */
        0xA9, 0x31, 0x8D, 0x04, 0x20, 0xA9, 0x01, 0x8D, 0x04, 0x20,    /* C059 y = 49, tile 1 */
        0xA9, 0x00, 0x8D, 0x04, 0x20, 0xA9, 0x64, 0x8D, 0x04, 0x20,    /* C063 attr 0, x = 100 */
        0x2C, 0x02, 0x20, 0xA9, 0x00, 0x8D, 0x05, 0x20, 0x8D, 0x05, 0x20,   /* C06D scroll 0,0 */
        0xA9, 0x90, 0x8D, 0x00, 0x20, 0xA9, 0x1E, 0x8D, 0x01, 0x20,    /* C078 PPUCTRL PPUMASK */
        0x4C, 0x82, 0xC0                                    /* C082 JMP * */
    };
    static const uint8_t nmi[] = {
        0x48,                                               /* C090 PHA */
        0x2C, 0x02, 0x20, 0xA9, 0x00, 0x8D, 0x05, 0x20, 0x8D, 0x05, 0x20,   /* C091 scroll 0,0 */
        0xA9, 0x90, 0x8D, 0x00, 0x20,                       /* C09C PPUCTRL */
        0xA0, 0x0A, 0xA2, 0x00, 0xCA, 0xD0, 0xFD, 0x88, 0xD0, 0xF8,    /* C0A1 delay */
        0xA9, 0x40, 0x8D, 0x05, 0x20, 0xA9, 0x00, 0x8D, 0x05, 0x20,    /* C0AB the split: X = 64 */
        0x68, 0x40                                          /* C0B5 PLA RTI */
    };
    static const uint8_t pal[16] = { 0x0F, 0x16, 0x27, 0x18, 0x0F, 0x21, 0x31, 0x11, 0x0F, 0x2A, 0x1A, 0x0A, 0x0F, 0x06, 0x16, 0x26 };
    FILE *file;
    memset(img, 0, sizeof img);
    memcpy(img, "NES\x1a", 4);
    img[4] = 1; img[5] = 1; img[6] = 0x01;
    memcpy(img + 16, code, sizeof code);
    memcpy(img + 16 + 0x90, nmi, sizeof nmi);
    memcpy(img + 16 + 0xC0, pal, sizeof pal);
    img[16 + 0xB6] = 0x40;                                  /* the IRQ vector's RTI */
    img[16 + 0x3FFA] = 0x90; img[16 + 0x3FFB] = 0xC0;
    img[16 + 0x3FFC] = 0x00; img[16 + 0x3FFD] = 0xC0;
    img[16 + 0x3FFE] = 0xB6; img[16 + 0x3FFF] = 0xC0;
    for (int tile = 0; tile < 512; tile++)
        for (int row = 0; row < 8; row++) {
            img[16 + 16384 + tile * 16 + row] = (uint8_t)(tile * 37 + row * 11);
            img[16 + 16384 + tile * 16 + 8 + row] = 0xFF;
        }
    file = fopen(path, "wb");
    if (!file) return 0;
    fwrite(img, 1, sizeof img, file);
    fclose(file);
    return sizeof img;
}

static int get_capture(nessession *session, nesvid_capture *cap, int want_log, int timeout_ms)
{
    uint64_t serial = 0;
    for (int waited = 0; waited < timeout_ms; waited += 20) {
        if (nesvid_host_capture_get(session, cap, &serial) && cap->fb_valid && cap->mem_valid && (!want_log || cap->log_valid))
            return 1;
        sleep_ms(20);
        serial = 0;
    }
    return 0;
}

int main(void)
{
    char cfg[512], data[512], rom[700], out[16384];
    nessession_paths paths;
    nessession *session;
    nessession_start_opts opts;
    nesdebug *dbg;
    nesvid_tab *tab;
    static nesvid_capture cap, parked;
    static uint32_t shown[NESSESSION_FB_WIDTH * NESSESSION_FB_MAX_HEIGHT];
    uint64_t batches = 0, resyncs = 0, fserial = 0;
    int height = 0, split_line = -1;

    test_tmpdir(cfg, sizeof cfg, "vcfg");
    test_tmpdir(data, sizeof data, "vdata");
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg; paths.data_dir = data; paths.fujinet_lib = "";
    snprintf(rom, sizeof rom, "%s/split.nes", cfg);
    check(rom_write(rom) > 0, "the program is written");

    session = nessession_new(&paths);
    if (!session) return 1;
    nessession_default_opts(session, &opts);
    opts.enable_fujinet = 0; opts.enable_audio = 0; opts.enable_gamepad = 0;
    opts.cart_path = rom;
    check(nessession_start(session, &opts) == 0, "session starts with the split program");

    dbg = nessession_debugger(session);
    tab = nesvid_tab_new(session);
    check(tab != NULL, "a Video tab");
    nesvid_tab_register_console(dbg);
    nesdebug_attach(dbg);
    check(wait_stopped(dbg, 1, 3000), "attaching stops the machine");
    /* parked at boot, rendering still off: the address stands still, which is no scroll split */
    for (int item = 0; item < 5; item++) {
        const nesvid_band *bands;
        int count;
        sleep_ms(20);
        nesvid_tab_refresh(tab, 1);
        count = nesvid_tab_lines(tab, &bands);
        check(count == 1 && bands[0].first == 0 && bands[0].last == 239, "one band for the whole picture while the PPU is still off");
    }
    nesdebug_resume(dbg);
    check(wait_stopped(dbg, 0, 1000), "and resume runs it");
    sleep_ms(600);          /* past the first frames: the split starts with the second NMI */

    /* ---- a running machine: the capture comes by itself ---- */
    check(get_capture(session, &cap, 1, 8000), "a captured frame with its memory and line log");
    check(cap.fb_valid && cap.mem_valid && cap.log_valid && cap.serial > 0, "pixels, PPU memory and the log are all there");
    check(cap.region == NESVID_NTSC && cap.nlines == 262 && cap.nmi_line == 241, "NTSC, 262 lines, vblank at 241");
    check(cap.ctrl == 0x90 && cap.mask == 0x1E, "PPUCTRL $90 and PPUMASK $1E");
    check(cap.pal[0] == 0x0F && cap.pal[1] == 0x16 && cap.pal[15] == 0x26, "the palette the program loaded");
    check(cap.ntab[0] == 0 && cap.ntab[1] == 1 && cap.ntab[255] == 255 && cap.ntab[256] == 0 && cap.ntab[0x3C0] == 0xC0, "nametable 0 as the program wrote it");
    check(cap.mirroring == NESVID_MIRROR_VERTICAL, "vertical mirroring: nametable 2 shows nametable 0");
    check(cap.ntab[0x800 + 1] == 1 && cap.ntab[0x400 + 1] == 0, "and the logical tables say so");
    check(cap.chr[8] == 0xFF && cap.chr[16 + 0] == (uint8_t)37, "the pattern tables");
    check(cap.oam[0] == 49 && cap.oam[1] == 1 && cap.oam[3] == 100 && cap.oam[4] == 0xFF, "OAM: sprite 0 and the rest cleared");
    check(cap.rgb[0x16] != 0 && cap.rgb[0x16] != cap.rgb[0x0F], "master colours from the video palette");

    /* the published frame is the captured one */
    nessession_copy_frame(session, shown, &height, &fserial);
    check(height == 240, "the main window's frame is 240 lines");
    {
        int same = 1;
        for (int item = 0; item < NESVID_W * NESVID_H; item++)
            if ((shown[item] & 0xFFFFFF) != (cap.pixels[item] & 0xFFFFFF)) { same = 0; break; }
        check(same, "the captured pixels are the published frame");
    }

    /* the model against the real PPU, frame after frame */
    sleep_ms(500);
    nesvid_host_capture_stats(session, &batches, &resyncs);
    check(batches >= 10, "many frames' logs were replayed");
    check(resyncs == 0, "and the replay agreed with the PPU's own registers at the end of every frame");
    printf("   (%llu frames replayed, %llu disagreements)\n", (unsigned long long)batches, (unsigned long long)resyncs);

    /* the split */
    check(get_capture(session, &cap, 1, 4000), "a fresh capture");
    {
        int found = 0;
        for (int item = 0; item < cap.flog.nwrites; item++)
            if (cap.flog.writes[item].reg == 5 && cap.flog.writes[item].value == 0x40 && cap.flog.writes[item].line >= 0) {
                split_line = cap.flog.writes[item].line;
                found = 1;
                break;
            }
        check(found, "the log has the handler's PPUSCROLL write on a picture line");
    }
    printf("   (the X = 64 write landed on line %d)\n", split_line);
    check(split_line > 60 && split_line < 130, "near the delay's design (line ~92)");
    if (split_line > 2 && split_line < 237) {
        int posx, posy;
        nesvid_line_scroll(&cap.flog.line[split_line - 1], &posx, &posy);
        check(posx == 0 && posy == split_line - 1, "above the split: scroll 0 and Y = the line");
        nesvid_line_scroll(&cap.flog.line[split_line + 2], &posx, &posy);
        check(posx == 64 && posy == split_line + 2, "below it: X 64, Y still the line");
        nesvid_line_scroll(&cap.flog.line[239], &posx, &posy);
        check(posx == 64 && posy == 239, "to the bottom of the frame");
        check(cap.flog.line[split_line].writes >= 1 || cap.flog.line[split_line - 1].writes >= 1, "the line it landed on counts the write");
    }
    check(cap.flog.reg_vis[5] == 2 && cap.flog.reg_writes[5] >= 4, "PPUSCROLL: two writes on the picture, two in the blanking");
    {
        /* the end-to-end proof: the frame the PPU drew is the background
         * re-rendered from the captured memory and the replayed line log
         * (nothing in it but the background and sprite 0, which is skipped) */
        int bad = 0, first_x = -1, first_y = -1;
        for (int row = 0; row < NESVID_H; row++) {
            int scrollx, scrolly;
            nesvid_line_scroll(&cap.flog.line[row], &scrollx, &scrolly);
            for (int col = 0; col < NESVID_W; col++) {
                const int plane_x = (scrollx + col) % 512;
                const int ntab = (plane_x / 256) + 2 * (scrolly / 240);
                const int tile_col = (plane_x % 256) / 8, tile_row = (scrolly % 240) / 8;
                const int tile = cap.ntab[ntab * 0x400 + tile_row * 32 + tile_col];
                const int attr = cap.ntab[ntab * 0x400 + 0x3C0 + (tile_row / 4) * 8 + tile_col / 4];
                const int pal = (attr >> (((tile_row & 2) << 1) | (tile_col & 2))) & 3;
                const int low = (cap.chr[0x1000 + tile * 16 + (scrolly % 240) % 8] >> (7 - plane_x % 8)) & 1;
                const int high = (cap.chr[0x1000 + tile * 16 + 8 + (scrolly % 240) % 8] >> (7 - plane_x % 8)) & 1;
                const int pix = low | (high << 1);
                const uint32_t want = cap.rgb[cap.pal[(pix ? pal * 4 + pix : 0)] & 63] & 0xFFFFFF;
                if (col >= 100 && col < 108 && row >= 50 && row < 58) continue;     /* sprite 0 */
                if ((cap.pixels[row * NESVID_W + col] & 0xFFFFFF) != want) {
                    if (!bad) { first_x = col; first_y = row; }
                    bad++;
                }
            }
        }
        if (bad) printf("   (%d pixels differ, the first at %d,%d)\n", bad, first_x, first_y);
        check(bad == 0, "the frame is the background re-rendered from the memory and the replayed line log, pixel for pixel");
    }
    {
        int nmi = 0, spr0 = 0;
        for (int item = 0; item < cap.flog.nevents; item++) {
            if (cap.flog.events[item].kind == NESVID_EV_NMI) nmi = cap.flog.events[item].line;
            if (cap.flog.events[item].kind == NESVID_EV_SPRITE0 && cap.flog.events[item].line >= 0) spr0 = cap.flog.events[item].line;
        }
        check(nmi >= 241 && nmi <= 242, "the log has the NMI at the start of vblank");
        check(spr0 >= 50 && spr0 <= 51, "and the sprite 0 hit on the line where the sprite starts");
    }

    /* the tab on a running machine */
    nesvid_tab_set_zoom(tab, 1);
    nesvid_tab_set_overlays(tab, 0);
    {
        int wide = 0, high = 0;
        const uint32_t *pic;
        unsigned flags;
        for (int item = 0; item < 100; item++) {
            flags = nesvid_tab_refresh(tab, 1);
            if (!(flags & NESVID_TAB_OFF)) break;
            sleep_ms(20);
        }
        pic = nesvid_tab_picture(tab, &wide, &high);
        check(pic != NULL && wide == 256 && high == 240, "the tab shows the Display");
        {
            int same = pic != NULL;
            for (int item = 0; item < NESVID_W * NESVID_H && same; item++)
                if ((pic[item] & 0xFFFFFF) != (shown[item] & 0xFFFFFF)) same = 0;
            check(same, "which is the published frame");
        }
        {
            const nesvid_band *bands;
            const int count = nesvid_tab_lines(tab, &bands);
            check(count == 2 && bands[1].first >= 60 && bands[1].scroll_x == 64, "the Lines table shows the split as a second band");
            if (count == 2) printf("   (%s)\n   (%s)\n", bands[0].text, bands[1].text);
        }
        nesvid_tab_set_view(tab, NESVID_V_DECODED);
        nesvid_tab_set_overlays(tab, NESVID_OV_ALL);
        nesvid_tab_refresh(tab, 1);
        pic = nesvid_tab_picture(tab, &wide, &high);
        check(pic && wide == 512 && high == 480, "Decoded: the four nametables");
        nesvid_tab_set_view(tab, NESVID_V_BEAM);
        nesvid_tab_refresh(tab, 1);
        check(nesvid_tab_picture(tab, &wide, &high) == NULL, "Beam: not while the machine runs");
        nesvid_tab_set_view(tab, NESVID_V_DISPLAY);
    }

    /* ---- parked ---- */
    nesdebug_stop(dbg);
    check(wait_stopped(dbg, 1, 3000), "stopped");
    check(nesvid_host_capture_get(session, &parked, &(uint64_t){0}) != 0, "the finished frame is still there");
    check(nesvid_host_capture_fill(session, &parked) == 1 && parked.live, "a parked machine is read live");
    {
        nesdebug_ppu ppu;
        nesdebug_ppu_get(dbg, &ppu);
        check(parked.beam_line == ppu.scanline && parked.beam_dot == ppu.dot, "the beam is where the engine says");
        check(parked.vaddr == ppu.vram_addr && parked.taddr == ppu.tmp_addr && parked.fine_x == ppu.fine_x, "v, t and fine X are the PPU's");
        check(parked.ctrl == 0x90 || parked.ctrl == 0x91 || (parked.ctrl & 0xFC) == 0x90, "PPUCTRL");
        printf("   (parked at line %d dot %d; the frame in progress has %d lines started)\n", parked.beam_line, parked.beam_dot,
               parked.clog.last_line + 1);
    }
    check(parked.live_pixels_valid, "the live frame is read");
    {
        int same = 1;
        for (int item = 0; item < NESVID_W * NESVID_H; item++)
            if ((parked.live_pixels[item] & 0xFFFFFF) != (parked.pixels[item] & 0xFFFFFF)) { same = 0; break; }
        check(same, "and, the program drawing one picture every frame, equals the finished frame whatever the beam");
    }
    check(parked.cur_valid, "the frame in progress has a log");

    /* stop with the beam below the split: the lines the beam has started show it */
    {
        /* `scanline n` steps n lines from where the machine is parked */
        int steps = (150 - parked.beam_line + 262) % 262;
        if (steps == 0) steps = 262;
        nesdebug_scanline(dbg, steps);
    }
    for (int waited = 0; waited < 3000; waited += 10) {
        sleep_ms(10);
        if (wait_stopped(dbg, 1, 1) && nesvid_host_capture_fill(session, &parked) == 1 && parked.beam_line == 150) break;
    }
    check(wait_stopped(dbg, 1, 1000), "ran to scanline 150");
    check(nesvid_host_capture_fill(session, &parked) == 1, "read again at 150");
    printf("   (parked at line %d dot %d)\n", parked.beam_line, parked.beam_dot);
    check(parked.beam_line == 150, "the beam is on line 150");
    if (parked.beam_line == 150) {
        int posx, posy;
        check(parked.cur_valid && parked.clog.last_line == 150, "lines 0-150 are started");
        nesvid_line_scroll(&parked.clog.line[10], &posx, &posy);
        check(posx == 0 && posy == 10, "the frame in progress: line 10 unsplit");
        nesvid_line_scroll(&parked.clog.line[149], &posx, &posy);
        check(posx == 64 && posy == 149, "and line 149 past the split");
        check(parked.clog.line[149].mask == 0x1E, "with the mask on");
    }

    /* the tab while parked */
    {
        int wide = 0, high = 0;
        const uint32_t *pic;
        nesvid_tab_set_overlays(tab, 0);
        nesvid_tab_set_view(tab, NESVID_V_DISPLAY);
        nesvid_tab_refresh(tab, 1);
        pic = nesvid_tab_picture(tab, &wide, &high);
        nessession_copy_frame(session, shown, &height, &(uint64_t){0});
        {
            int same = pic != NULL;
            for (int item = 0; item < NESVID_W * NESVID_H && same; item++)
                if ((pic[item] & 0xFFFFFF) != (shown[item] & 0xFFFFFF)) same = 0;
            check(same, "Display while stopped is the published frame");
        }
        nesvid_tab_set_view(tab, NESVID_V_BEAM);
        nesvid_tab_refresh(tab, 1);
        pic = nesvid_tab_picture(tab, &wide, &high);
        check(pic != NULL && wide == 256 && high == 240, "Beam works while stopped");
        check(strstr(nesvid_tab_status(tab), "Stopped") && strstr(nesvid_tab_status(tab), "beam at line 150"), "the status says where the beam is");
        {
            const nesvid_band *bands;
            const int count = nesvid_tab_lines(tab, &bands);
            check(count == 2, "two bands while parked too");
        }
        nesvid_tab_set_view(tab, NESVID_V_DECODED);
        nesvid_tab_set_source(tab, NESVID_SRC_SPRITES);
        nesvid_tab_refresh(tab, 1);
        nesvid_tab_hover(tab, 101, 51);
        check(nesvid_tab_pixel(tab) && nesvid_tab_pixel(tab)->sprite == 0, "the Sprites source: sprite 0 under 101,51");
        nesvid_tab_set_view(tab, NESVID_V_DISPLAY);
        nesvid_tab_refresh(tab, 1);
        nesvid_tab_hover(tab, 120, 130);
        check(nesvid_tab_pixel(tab) && strstr(nesvid_tab_pixel(tab)->text, "scroll 64,130") != NULL, "the inspector re-does the fetch with the band's scroll");
        nesvid_tab_pin(tab, 120, 130);
        {
            const int id = nesvid_tab_break_on_pixel(tab);
            nesdebug_breakpoint list[NESDEBUG_MAX_BREAKPOINTS];
            const int count = nesdebug_breakpoint_list(dbg, list, NESDEBUG_MAX_BREAKPOINTS);
            int ppu_space = 0;
            check(id >= 1, "Break on writes made breakpoints in the engine");
            for (int item = 0; item < count; item++)
                if (list[item].space == NESDEBUG_SPACE_PPU && list[item].type == NESDEBUG_BP_WRITE) ppu_space++;
            check(ppu_space >= 2, "in PPU space, write type");
        }
        nesdebug_breakpoint_clear(dbg);
        nesvid_tab_unpin(tab);
    }

    /* the Lines table at every kind of stop: the split is one band edge wherever the beam is */
    {
        static const int stops[] = { 5, 40, 91, 92, 100, 200, 239, 245, 261 };
        for (size_t item = 0; item < sizeof stops / sizeof stops[0]; item++) {
            const nesvid_band *bands;
            int steps, count;
            nesvid_capture now;
            nesvid_host_capture_fill(session, &now);
            steps = (stops[item] - now.beam_line + 262) % 262;
            if (steps == 0) steps = 262;
            nesdebug_scanline(dbg, steps);
            sleep_ms(30);
            wait_stopped(dbg, 1, 3000);
            nesvid_tab_refresh(tab, 1);
            count = nesvid_tab_lines(tab, &bands);
            printf("   (stop at line %d: %d bands)\n", stops[item], count);
            check(count >= 1 && count <= 2, "the Lines table never invents bands at a stop");
            if (count > 2) for (int row = 0; row < count && row < 4; row++) printf("   (%s)\n", bands[row].text);
        }
    }

    /* a write breakpoint on PPUSCROLL stops the machine in the NMI handler */
    {
        int id = nesvid_tab_break_on_register(tab, 4);
        nesdebug_cpu cpu;
        check(id >= 1, "a write breakpoint on $2005 through the tab");
        nesdebug_resume(dbg);
        sleep_ms(60);
        check(wait_stopped(dbg, 1, 3000), "it fires");
        nesdebug_cpu_get(dbg, &cpu);
        check(cpu.pc >= 0xC091 && cpu.pc <= 0xC0B8, "inside the NMI handler");
        {
            char why[160];
            int addr = -1;
            nesdebug_stop_reason(dbg, why, sizeof why, &addr);
            printf("   (stopped at $%04X: %s)\n", cpu.pc, why);
        }
        nesdebug_breakpoint_clear(dbg);
    }

    /* the console */
    nesdebug_command(dbg, "video", out, sizeof out);
    check(strstr(out, "PPUCTRL") && strstr(out, "PPUSCROLL"), "the console's `video` prints the registers");
    nesdebug_command(dbg, "video lines", out, sizeof out);
    check(strstr(out, "lines 0-") != NULL, "`video lines` lists the bands");
    printf("   [%.200s]\n", out);
    nesdebug_command(dbg, "video timing", out, sizeof out);
    check(strstr(out, "NTSC") && strstr(out, "Beam"), "`video timing`");
    printf("   [%.200s]\n", out);

    /* a power cycle with the debugger attached and the tab open: CONFIG, then the program again */
    {
        uint64_t before_batches, before_resyncs;
        nesvid_host_capture_stats(session, &before_batches, &before_resyncs);
        check(nessession_reset_to_config(session) == 0, "reset to CONFIG with the tab open");
        nesdebug_resume(dbg);
        sleep_ms(1500);
        check(get_capture(session, &cap, 1, 8000), "CONFIG is captured too: frame, memory and log");
        check(cap.mem_valid && cap.nlines == 262, "with its PPU memory");
        nesvid_host_capture_stats(session, &batches, &resyncs);
        printf("   (CONFIG: PPUCTRL $%02X PPUMASK $%02X, %u mapper register writes in the frame, %llu frames replayed, %llu disagreements)\n",
               cap.ctrl, cap.mask, (unsigned)cap.flog.mapper_writes, (unsigned long long)(batches - before_batches),
               (unsigned long long)(resyncs - before_resyncs));
        check(batches > before_batches, "the log keeps being replayed after a power cycle");
        check(resyncs - before_resyncs <= (batches - before_batches) / 4 + 2, "and mostly agrees with the PPU (CONFIG programs the PPU with its own timing)");
        check(nessession_load_cart(session, rom) == 0, "the program is loaded again");
        nesdebug_resume(dbg);
        sleep_ms(1200);
        check(get_capture(session, &cap, 1, 8000) && cap.ntab[1] == 1 && cap.oam[3] == 100, "the capture is the program's again");
        split_line = -1;
        for (int item = 0; item < cap.flog.nwrites; item++)
            if (cap.flog.writes[item].reg == 5 && cap.flog.writes[item].value == 0x40 && cap.flog.writes[item].line >= 0) split_line = cap.flog.writes[item].line;
        check(split_line > 60 && split_line < 130, "with the split in its log");
    }

    /* the tab can go and come back */
    nesvid_tab_free(tab);
    tab = nesvid_tab_new(session);
    {
        unsigned flags = NESVID_TAB_OFF;
        for (int item = 0; item < 200 && (flags & NESVID_TAB_OFF); item++) {
            flags = nesvid_tab_refresh(tab, 1);
            if (flags & NESVID_TAB_OFF) sleep_ms(20);
        }
        check(!(flags & NESVID_TAB_OFF), "a new tab gets a capture of the running machine");
        {
            const nesvid_band *bands;
            int count = 0;
            for (int item = 0; item < 200; item++) {
                count = nesvid_tab_lines(tab, &bands);
                if (count == 2) break;
                sleep_ms(20);
                nesvid_tab_refresh(tab, 1);
            }
            check(count == 2, "and its log, a frame later");
        }
    }

    nesvid_tab_register_console(NULL);
    nesvid_tab_free(tab);
    nesdebug_detach(dbg);
    nessession_stop(session);
    nessession_free(session);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
