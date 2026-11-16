/*
 * ppuview.c -- see ppuview.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ppuview.h"

#include <stdio.h>
#include <string.h>

uint32_t nesppu_rgb(const nesvid_capture *cap, int master)
{
    return 0xff000000u | (cap->rgb[master & 63] & 0x00ffffffu);
}

int nesppu_slot_colour(const nesvid_capture *cap, int slot)
{
    slot &= 31;
    return cap->pal[(slot & 3) == 0 ? 0 : slot] & 63;
}

static uint32_t slot_rgb(const nesvid_capture *cap, int slot)
{
    return nesppu_rgb(cap, nesppu_slot_colour(cap, slot));
}

int nesppu_tile_pixel(const nesvid_capture *cap, unsigned base, int tile, int pixx, int pixy)
{
    const unsigned addr = (base + (unsigned)tile * 16u + (unsigned)(pixy & 7)) & (NESVID_CHR - 1);
    const int low = (cap->chr[addr] >> (7 - (pixx & 7))) & 1;
    const int high = (cap->chr[(addr + 8) & (NESVID_CHR - 1)] >> (7 - (pixx & 7))) & 1;
    return low | (high << 1);
}

void nesppu_render_nametables(const nesvid_capture *cap, unsigned bg_base, uint32_t *dst)
{
    for (int ntab = 0; ntab < 4; ntab++) {
        const uint8_t *tab = &cap->ntab[ntab * 0x400];
        const int orgx = (ntab & 1) * 256, orgy = (ntab >> 1) * 240;
        for (int row = 0; row < 30; row++)
            for (int col = 0; col < 32; col++) {
                const int tile = tab[row * 32 + col];
                const uint8_t attr = tab[0x3C0 + (row / 4) * 8 + col / 4];
                const int shift = ((row & 2) << 1) | (col & 2);
                const int palette = (attr >> shift) & 3;
                for (int pixy = 0; pixy < 8; pixy++)
                    for (int pixx = 0; pixx < 8; pixx++) {
                        const int pix = nesppu_tile_pixel(cap, bg_base, tile, pixx, pixy);
                        dst[(size_t)(orgy + row * 8 + pixy) * NESPPU_NT_W + (size_t)(orgx + col * 8 + pixx)] =
                            slot_rgb(cap, palette * 4 + pix);
                    }
            }
    }
}

void nesppu_render_patterns(const nesvid_capture *cap, int palette, uint32_t *dst)
{
    if (palette < 0) palette = 0;
    if (palette > 7) palette = 7;
    for (int table = 0; table < 2; table++)
        for (int tile = 0; tile < 256; tile++) {
            const int orgx = table * 128 + (tile % 16) * 8, orgy = (tile / 16) * 8;
            for (int pixy = 0; pixy < 8; pixy++)
                for (int pixx = 0; pixx < 8; pixx++) {
                    const int pix = nesppu_tile_pixel(cap, (unsigned)table * 0x1000u, tile, pixx, pixy);
                    dst[(size_t)(orgy + pixy) * NESPPU_PAT_W + (size_t)(orgx + pixx)] = slot_rgb(cap, palette * 4 + pix);
                }
        }
}

void nesppu_sprite_info(const nesvid_capture *cap, int index, int tall, nesppu_sprite *out)
{
    const uint8_t *entry = &cap->oam[(index & 63) * 4];
    memset(out, 0, sizeof *out);
    out->index = index & 63;
    out->top = entry[0] + 1;
    out->tile = entry[1];
    out->attr = entry[2];
    out->left = entry[3];
    out->high = tall ? 16 : 8;
    out->palette = 4 + (entry[2] & 3);
    out->behind = (entry[2] & 0x20) != 0;
    out->hflip = (entry[2] & 0x40) != 0;
    out->vflip = (entry[2] & 0x80) != 0;
    out->on_screen = out->top < NESVID_H && out->left < NESVID_W;
}

int nesppu_sprite_pixel(const nesvid_capture *cap, const nesppu_sprite *spr, unsigned spr_base, int tall,
                        int offx, int offy, unsigned *addr)
{
    const int flipy = spr->vflip ? spr->high - 1 - offy : offy;
    const int flipx = spr->hflip ? 7 - offx : offx;
    unsigned base;
    int tile;
    if (tall) {
        base = (spr->tile & 1) ? 0x1000u : 0u;
        tile = (spr->tile & 0xFE) + (flipy >= 8 ? 1 : 0);
    } else {
        base = spr_base;
        tile = spr->tile;
    }
    if (addr) *addr = (base + (unsigned)tile * 16u + (unsigned)(flipy & 7)) & (NESVID_CHR - 1);
    return nesppu_tile_pixel(cap, base, tile, flipx, flipy & 7);
}

void nesppu_render_sprites(const nesvid_capture *cap, unsigned spr_base, int tall, uint32_t *dst)
{
    for (int item = 0; item < NESPPU_SPR_W * NESPPU_SPR_H; item++) dst[item] = NESPPU_PANEL;
    /* back to front, so sprite 0 ends up on top as on the console */
    for (int index = 63; index >= 0; index--) {
        nesppu_sprite spr;
        nesppu_sprite_info(cap, index, tall, &spr);
        for (int offy = 0; offy < spr.high; offy++)
            for (int offx = 0; offx < 8; offx++) {
                const int posx = spr.left + offx, posy = spr.top + offy;
                const int pix = nesppu_sprite_pixel(cap, &spr, spr_base, tall, offx, offy, NULL);
                if (pix && posx < NESPPU_SPR_W && posy < NESPPU_SPR_H)
                    dst[(size_t)posy * NESPPU_SPR_W + (size_t)posx] = slot_rgb(cap, spr.palette * 4 + pix);
            }
    }
}

void nesppu_render_palette(const nesvid_capture *cap, uint32_t *dst)
{
    for (int slot = 0; slot < 32; slot++) {
        const int orgx = (slot % NESPPU_PAL_COLS) * NESPPU_PAL_CELL, orgy = (slot / NESPPU_PAL_COLS) * NESPPU_PAL_CELL;
        const uint32_t col = nesppu_rgb(cap, cap->pal[slot] & 63);
        for (int pixy = 0; pixy < NESPPU_PAL_CELL; pixy++)
            for (int pixx = 0; pixx < NESPPU_PAL_CELL; pixx++)
                dst[(size_t)(orgy + pixy) * NESPPU_PAL_W + (size_t)(orgx + pixx)] = col;
    }
}

int nesppu_nt_aliases(int mirroring, int ntab, int out[4])
{
    int num = 0;
    ntab &= 3;
    for (int other = 0; other < 4; other++) {
        int same;
        switch (mirroring) {
        case NESVID_MIRROR_HORIZONTAL: same = (other >> 1) == (ntab >> 1); break;
        case NESVID_MIRROR_VERTICAL: same = (other & 1) == (ntab & 1); break;
        case NESVID_MIRROR_SCREEN_A:
        case NESVID_MIRROR_SCREEN_B: same = 1; break;
        default: same = other == ntab; break;
        }
        if (same) out[num++] = other;
    }
    return num;
}

const char *nesppu_mirroring_name(int mirroring)
{
    switch (mirroring) {
    case NESVID_MIRROR_HORIZONTAL: return "Horizontal";
    case NESVID_MIRROR_VERTICAL: return "Vertical";
    case NESVID_MIRROR_SCREEN_A: return "Screen A";
    case NESVID_MIRROR_SCREEN_B: return "Screen B";
    default: return "Four-screen";
    }
}

void nesppu_describe_ctrl(uint8_t ctrl, char *dst, int dstsz)
{
    snprintf(dst, (size_t)dstsz, "NMI %s, %s sprites, background patterns $%04X, sprite patterns %s, VRAM step %d, nametable %d",
             (ctrl & 0x80) ? "on" : "off", (ctrl & 0x20) ? "8x16" : "8x8", (ctrl & 0x10) ? 0x1000 : 0,
             (ctrl & 0x20) ? "by tile bit 0" : ((ctrl & 0x08) ? "$1000" : "$0000"), (ctrl & 4) ? 32 : 1, ctrl & 3);
}

void nesppu_describe_mask(uint8_t mask, char *dst, int dstsz)
{
    snprintf(dst, (size_t)dstsz, "background %s, sprites %s, left 8 px: background %s, sprites %s%s%s%s%s",
             (mask & 0x08) ? "on" : "off", (mask & 0x10) ? "on" : "off",
             (mask & 0x02) ? "shown" : "hidden", (mask & 0x04) ? "shown" : "hidden",
             (mask & 0x01) ? ", greyscale" : "", (mask & 0x20) ? ", emphasize red" : "",
             (mask & 0x40) ? ", emphasize green" : "", (mask & 0x80) ? ", emphasize blue" : "");
}

void nesppu_describe_status(uint8_t status, char *dst, int dstsz)
{
    snprintf(dst, (size_t)dstsz, "vblank %s, sprite 0 hit %s, sprite overflow %s",
             (status & 0x80) ? "set" : "clear", (status & 0x40) ? "set" : "clear", (status & 0x20) ? "set" : "clear");
}
