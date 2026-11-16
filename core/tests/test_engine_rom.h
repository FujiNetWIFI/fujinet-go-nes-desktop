/*
 * An NROM-128 image that writes the places the debugger watches, in a loop:
 *
 *     $C000  SEI / CLD / LDX #$FF / TXS
 *     $C005  LDA #$10 / STA $2000          PPUCTRL boot value
 *     $C00A  LDA #$00 / STA $0300          counter = 0
 *     $C00F  LDA #$90                      <- loop
 *     $C011  STA $2000                     PPUCTRL = $90 (was $10 the first time)
 *     $C014  INC $0300                     counter++  (read, then write)
 *     $C017  LDA $0300
 *     $C01A  STA $0301                     copy: old = the previous counter
 *     $C01D  JSR $C030
 *     $C020  JMP $C00F
 *     $C030  LDA $2002                     (resets the $2005/$2006 write toggle)
 *     $C033  $3F01 <- $21                  palette entry 1 (via $2006/$2007)
 *     $C042  $2000 <- $41                  nametable 0, tile 0
 *     $C051  OAM[0] <- $55                 (via $2003/$2004)
 *     $C05B  RTS
 *     $C060  RTI                           NMI and IRQ
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NES_TEST_ENGINE_ROM_H
#define NES_TEST_ENGINE_ROM_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Every address an instruction of the image starts at (for "stops land on
 * an instruction boundary"). */
static const uint16_t engine_rom_starts[] = {
    0xC000, 0xC001, 0xC002, 0xC004, 0xC005, 0xC007, 0xC00A, 0xC00C, 0xC00F,
    0xC011, 0xC014, 0xC017, 0xC01A, 0xC01D, 0xC020,
    0xC030, 0xC033, 0xC035, 0xC038, 0xC03A, 0xC03D, 0xC03F,
    0xC042, 0xC044, 0xC047, 0xC049, 0xC04C, 0xC04E,
    0xC051, 0xC053, 0xC056, 0xC058, 0xC05B, 0xC060
};

static inline size_t engine_rom_write(const char *path)
{
    static uint8_t img[16 + 16384 + 8192];
    static const uint8_t main_code[] = {
        0x78, 0xD8, 0xA2, 0xFF, 0x9A,
        0xA9, 0x10, 0x8D, 0x00, 0x20,
        0xA9, 0x00, 0x8D, 0x00, 0x03,
        0xA9, 0x90, 0x8D, 0x00, 0x20,
        0xEE, 0x00, 0x03,
        0xAD, 0x00, 0x03,
        0x8D, 0x01, 0x03,
        0x20, 0x30, 0xC0,
        0x4C, 0x0F, 0xC0
    };
    static const uint8_t sub_code[] = {
        0xAD, 0x02, 0x20,
        0xA9, 0x3F, 0x8D, 0x06, 0x20, 0xA9, 0x01, 0x8D, 0x06, 0x20, 0xA9, 0x21, 0x8D, 0x07, 0x20,
        0xA9, 0x20, 0x8D, 0x06, 0x20, 0xA9, 0x00, 0x8D, 0x06, 0x20, 0xA9, 0x41, 0x8D, 0x07, 0x20,
        0xA9, 0x00, 0x8D, 0x03, 0x20, 0xA9, 0x55, 0x8D, 0x04, 0x20,
        0x60
    };
    FILE *f;
    memset(img, 0, sizeof img);
    memcpy(img, "NES\x1a", 4);
    img[4] = 1; img[5] = 1; img[6] = 0x01;
    memcpy(img + 16, main_code, sizeof main_code);
    memcpy(img + 16 + 0x30, sub_code, sizeof sub_code);
    img[16 + 0x60] = 0x40;
    img[16 + 0x3FFA] = 0x60; img[16 + 0x3FFB] = 0xC0;
    img[16 + 0x3FFC] = 0x00; img[16 + 0x3FFD] = 0xC0;
    img[16 + 0x3FFE] = 0x60; img[16 + 0x3FFF] = 0xC0;
    f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(img, 1, sizeof img, f);
    fclose(f);
    return sizeof img;
}

#endif
