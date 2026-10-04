/*
 * Tiny iNES images built in memory, so no test depends on a ROM file.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NES_TEST_ROM_H
#define NES_TEST_ROM_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* An NROM-128 image (mapper `mapper`, 16K PRG, 8K CHR) whose reset handler
 * counts in zero page $10 forever:
 *     $C000  SEI / CLD / LDX #$FF / TXS
 *     $C005  INC $10 / JMP $C005
 *     $C00A  RTI                      (NMI and IRQ)
 * Returns the file size written. */
static inline size_t test_rom_write(const char *path, int mapper)
{
    static uint8_t img[16 + 16384 + 8192];
    static const uint8_t code[] = {
        0x78, 0xD8, 0xA2, 0xFF, 0x9A, 0xE6, 0x10, 0x4C, 0x05, 0xC0, 0x40
    };
    FILE *f;
    memset(img, 0, sizeof img);
    memcpy(img, "NES\x1a", 4);
    img[4] = 1;                                   /* 16K PRG */
    img[5] = 1;                                   /* 8K CHR */
    img[6] = (uint8_t)(((mapper & 0x0F) << 4) | 0x01);   /* vertical */
    img[7] = (uint8_t)(mapper & 0xF0);
    memcpy(img + 16, code, sizeof code);
    /* vectors at the top of the 16K bank: NMI, RESET, IRQ */
    img[16 + 0x3FFA] = 0x0A; img[16 + 0x3FFB] = 0xC0;
    img[16 + 0x3FFC] = 0x00; img[16 + 0x3FFD] = 0xC0;
    img[16 + 0x3FFE] = 0x0A; img[16 + 0x3FFF] = 0xC0;
    f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(img, 1, sizeof img, f);
    fclose(f);
    return sizeof img;
}

#endif
