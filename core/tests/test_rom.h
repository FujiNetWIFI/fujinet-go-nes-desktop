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

/* An NROM-128 image that scans an expansion-port keyboard forever, the way
 * Family BASIC does: each pass selects row 0 (bit0, Family BASIC only),
 * then for every row writes column 0 and column 1 to $4016 (bit2 enables
 * the keyboard; column 1 -> 0 advances the row) and stores what $4017 reads
 * at $0300+row (column 0) and $0310+row (column 1). `rows` is 9 for the
 * Family BASIC keyboard, 13 for the Subor.
 *     $C000  SEI / CLD / LDX #$FF / TXS
 *     $C005  LDA #sel / STA $4016 / LDX #0
 *     $C00C  LDA #4 / STA $4016 / LDA $4017 / STA $0300,X
 *            LDA #6 / STA $4016 / LDA $4017 / STA $0310,X
 *            INX / CPX #rows / BNE $C00C / JMP $C005
 *     $C02A  RTI */
static inline size_t test_kbd_rom_write(const char *path, int rows, int reset)
{
    static uint8_t img[16 + 16384 + 8192];
    const uint8_t code[] = {
        0x78, 0xD8, 0xA2, 0xFF, 0x9A,
        0xA9, (uint8_t)(reset ? 0x05 : 0x04), 0x8D, 0x16, 0x40, 0xA2, 0x00,
        0xA9, 0x04, 0x8D, 0x16, 0x40, 0xAD, 0x17, 0x40, 0x9D, 0x00, 0x03,
        0xA9, 0x06, 0x8D, 0x16, 0x40, 0xAD, 0x17, 0x40, 0x9D, 0x10, 0x03,
        0xE8, 0xE0, (uint8_t)rows, 0xD0, 0xE5, 0x4C, 0x05, 0xC0, 0x40
    };
    FILE *f;
    memset(img, 0, sizeof img);
    memcpy(img, "NES\x1a", 4);
    img[4] = 1; img[5] = 1; img[6] = 0x01;
    memcpy(img + 16, code, sizeof code);
    img[16 + 0x3FFA] = 0x2A; img[16 + 0x3FFB] = 0xC0;
    img[16 + 0x3FFC] = 0x00; img[16 + 0x3FFD] = 0xC0;
    img[16 + 0x3FFE] = 0x2A; img[16 + 0x3FFF] = 0xC0;
    f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(img, 1, sizeof img, f);
    fclose(f);
    return sizeof img;
}

#endif
