/*
 * ppuview -- the Video tab's Decoded sources, drawn from a capture
 * (nesvidcap.h): the four nametables, the pattern tables, the sprites where
 * they sit on the screen and the palette; and the PPU's own geometry (tile
 * addresses, attributes, sprite boxes) the inspector and the overlays share.
 * Pure C on the capture's arrays: no machine, no debugger.
 *
 * Pixels are 0xFFRRGGBB. The master colours come from cap->rgb, which the host
 * fills through the palette the video output uses.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NESPPUVIEW_H
#define NESPPUVIEW_H

#include <stdint.h>

#include "nesvidcap.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NESPPU_NT_W     512
#define NESPPU_NT_H     480
#define NESPPU_PAT_W    256
#define NESPPU_PAT_H    128
#define NESPPU_SPR_W    256
#define NESPPU_SPR_H    240
#define NESPPU_PAL_W    256
#define NESPPU_PAL_H    32
#define NESPPU_PAL_CELL 16
#define NESPPU_PAL_COLS 16
#define NESPPU_PANEL    0xFF202428u

/* The colour of a master colour index (0-63), opaque. */
uint32_t nesppu_rgb(const nesvid_capture *cap, int master);
/* The master colour in palette RAM slot 0-31: slots 4, 8, 12 and the sprite
 * backdrops 16, 20, 24, 28 draw the universal backdrop (slot 0). */
int nesppu_slot_colour(const nesvid_capture *cap, int slot);
/* Pixel x,y (0-7) of a tile at PPU address `base` + tile * 16: 0-3. */
int nesppu_tile_pixel(const nesvid_capture *cap, unsigned base, int tile, int pixx, int pixy);

/* 512 x 480: the four nametables, backgrounds drawn with the pattern table at bg_base. */
void nesppu_render_nametables(const nesvid_capture *cap, unsigned bg_base, uint32_t *dst);
/* 256 x 128: $0000 and $1000 side by side in palette 0-7 (4-7 are the sprite palettes). */
void nesppu_render_patterns(const nesvid_capture *cap, int palette, uint32_t *dst);
/* 256 x 240: the 64 sprites where they sit, on a dark panel. */
void nesppu_render_sprites(const nesvid_capture *cap, unsigned spr_base, int tall, uint32_t *dst);
/* 256 x 32: the 32 palette entries as 16 x 16 swatches (backgrounds above, sprites below). */
void nesppu_render_palette(const nesvid_capture *cap, uint32_t *dst);

/* One OAM entry, placed. */
typedef struct nesppu_sprite {
    int index;
    int left, top;              /* the screen pixel of its top left corner (top is the OAM Y + 1) */
    int high;                   /* 8 or 16 */
    int tile, attr, palette;    /* palette 4-7 */
    int behind, hflip, vflip;
    int on_screen;              /* some of it is inside 256 x 240 */
} nesppu_sprite;
void nesppu_sprite_info(const nesvid_capture *cap, int index, int tall, nesppu_sprite *out);
/* The pixel (0-3) of the sprite at its box offset, and the PPU address of the pattern byte. */
int  nesppu_sprite_pixel(const nesvid_capture *cap, const nesppu_sprite *spr, unsigned spr_base, int tall,
                         int offx, int offy, unsigned *addr);

/* The nametables that show the same bytes as nametable nt (0-3), itself included. */
int  nesppu_nt_aliases(int mirroring, int ntab, int out[4]);
const char *nesppu_mirroring_name(int mirroring);

/* One-line meanings of the registers' bits. */
void nesppu_describe_ctrl(uint8_t ctrl, char *dst, int dstsz);
void nesppu_describe_mask(uint8_t mask, char *dst, int dstsz);
void nesppu_describe_status(uint8_t status, char *dst, int dstsz);

#ifdef __cplusplus
}
#endif

#endif
