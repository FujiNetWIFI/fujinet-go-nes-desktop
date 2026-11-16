/*
 * memtools_test -- the pure parts of the debugger's tools (nesmemtools.h):
 * range and search parsing, the watch list, breakpoint descriptions, region
 * names, and the trace row's text and filters. No machine runs: with no
 * engine only numbers resolve.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nesdebug.h"
#include "nesmemtools.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static int range(const char *text, unsigned *lo, unsigned *hi)
{
    uint16_t l = 0, h = 0;
    int r = nessym_parse_range(NULL, text, &l, &h);
    *lo = l; *hi = h;
    return r;
}

int main(void)
{
    unsigned lo, hi;
    uint8_t pat[NESSEARCH_MAX], mask = 0;
    int count = 0, flags = -1;
    char buf[300];
    nesdebug_breakpoint bp;
    nesdebug_region map[3];
    nesdebug_trace_entry e;
    int i;

    /* ---- ranges ---- */
    check(range("C000", &lo, &hi) && lo == 0xC000 && hi == 0xC000, "a bare 4-digit token is hex");
    check(range("$2000", &lo, &hi) && lo == 0x2000 && hi == 0x2000, "$ is hex");
    check(range("0x10", &lo, &hi) && lo == 0x10 && hi == 0x10, "0x is hex");
    check(range("1000", &lo, &hi) && lo == 0x1000, "four digits are hex, not decimal");
    check(range("C000..C0FF", &lo, &hi) && lo == 0xC000 && hi == 0xC0FF, "low..high");
    check(range("$0200-$02FF", &lo, &hi) && lo == 0x200 && hi == 0x2FF, "low-high");
    check(range("0x20-0x10", &lo, &hi) && lo == 0x10 && hi == 0x20, "a reversed range is swapped");
    check(range("  $10 .. $12 ", &lo, &hi) && lo == 0x10 && hi == 0x12, "spaces around");
    check(!range("", &lo, &hi) && !range("zz", &lo, &hi) && !range("$", &lo, &hi), "nonsense means nothing");
    check(!range("$10000", &lo, &hi), "past $FFFF means nothing");
    check(!range("$10-", &lo, &hi), "half a range means nothing");
    check(nessym_parse_range(NULL, NULL, NULL, NULL) == 0, "NULL text");
    check(nessym_parse_range(NULL, "$10", NULL, NULL) == 1, "NULL outputs are allowed");

    /* ---- search text ---- */
    check(nessearch_parse(NESSEARCH_HEX, "78 D8 A2 FF", pat, &count, &mask, &flags) && count == 4 &&
          pat[0] == 0x78 && pat[3] == 0xFF && mask == 0xFF && flags == 0, "hex bytes with spaces");
    check(nessearch_parse(NESSEARCH_HEX, "$78d8,0xA2 ff", pat, &count, &mask, &flags) && count == 4 && pat[1] == 0xD8,
          "hex with $, 0x and commas");
    check(nessearch_parse(NESSEARCH_HEX, "7", pat, &count, &mask, &flags) == 0, "an odd digit count is refused");
    check(nessearch_parse(NESSEARCH_HEX, "78 ZZ", pat, &count, &mask, &flags) == 0, "non-hex is refused");
    check(nessearch_parse(NESSEARCH_HEX, "", pat, &count, &mask, &flags) == 0, "empty is refused");
    check(nessearch_parse(NESSEARCH_TEXT, "Hello", pat, &count, &mask, &flags) && count == 5 && pat[0] == 'H' &&
          (flags & NESDEBUG_SEARCH_NOCASE), "text is case-insensitive");
    memset(buf, 'A', 40); buf[40] = 0;
    check(nessearch_parse(NESSEARCH_TEXT, buf, pat, &count, &mask, &flags) == 0, "text past 32 bytes is refused");
    check(nessearch_parse(NESSEARCH_HEX, "00", NULL, &count, &mask, &flags) == 0, "NULL pattern");

    /* ---- watches ---- */
    neswatch_clear();
    check(neswatch_count() == 0, "the watch list starts empty");
    {
        unsigned gen = neswatch_generation();
        check(neswatch_add("  $0300 ") == 0 && neswatch_add("$10..$13") == 1, "watches are added in order");
        check(neswatch_generation() != gen, "the generation moves");
        check(strcmp(neswatch_text(0), "$0300") == 0, "text is trimmed");
        check(neswatch_add("") == -1 && neswatch_add("   ") == -1, "empty is refused");
    }
    check(neswatch_format(NULL, 0, buf, sizeof buf) > 0 && strstr(buf, "$0300") && strstr(buf, "-"),
          "a watch with the machine off shows -");
    check(neswatch_format(NULL, 1, buf, sizeof buf) > 0 && strstr(buf, "$0010-$0013"), "a range shows its bounds");
    {
        uint16_t l, h;
        check(neswatch_range(NULL, 1, &l, &h) == 1 && l == 0x10 && h == 0x13, "the range of a watch");
        check(neswatch_range(NULL, 5, &l, &h) == 0, "no such watch");
    }
    neswatch_add("nosuchlabel");
    check(neswatch_format(NULL, 2, buf, sizeof buf) > 0 && strstr(buf, "?"), "an unresolved watch shows ?");
    neswatch_remove(1);
    check(neswatch_count() == 2 && strcmp(neswatch_text(1), "nosuchlabel") == 0, "remove");
    neswatch_remove(9);
    check(neswatch_count() == 2, "removing a missing watch is harmless");
    for (i = 0; i < NESWATCH_MAX; i++) neswatch_add("$20");
    check(neswatch_count() == NESWATCH_MAX && neswatch_add("$30") == -1, "the list is capped");
    neswatch_clear();
    check(neswatch_count() == 0, "clear");

    /* ---- breakpoint text ---- */
    memset(&bp, 0, sizeof bp);
    bp.type = NESDEBUG_BP_WRITE; bp.start = bp.end = 0x2000; bp.space = NESDEBUG_SPACE_CPU;
    snprintf(bp.condition, sizeof bp.condition, "value != old");
    check(nesbp_describe(NULL, &bp, buf, sizeof buf) > 0 && strstr(buf, "Write") && strstr(buf, "$2000") &&
          strstr(buf, "if value != old"), "a CPU write with its condition");
    bp.type = NESDEBUG_BP_ACCESS; bp.start = 0x300; bp.end = 0x30F; bp.condition[0] = 0;
    check(nesbp_describe(NULL, &bp, buf, sizeof buf) > 0 && strstr(buf, "Access") && strstr(buf, "$0300-$030F") &&
          !strstr(buf, "if"), "an access range");
    bp.type = NESDEBUG_BP_EXEC; bp.start = bp.end = 0xC000;
    check(nesbp_describe(NULL, &bp, buf, sizeof buf) > 0 && strstr(buf, "Execute"), "execute");
    bp.type = NESDEBUG_BP_WRITE; bp.space = NESDEBUG_SPACE_PALETTE; bp.start = bp.end = 3;
    check(nesbp_describe(NULL, &bp, buf, sizeof buf) > 0 && strstr(buf, "palette $03"), "a palette entry");
    bp.space = NESDEBUG_SPACE_OAM; bp.start = 0; bp.end = 3;
    check(nesbp_describe(NULL, &bp, buf, sizeof buf) > 0 && strstr(buf, "oam $00-$03"), "an OAM range");
    bp.space = NESDEBUG_SPACE_PPU; bp.type = NESDEBUG_BP_READ; bp.start = bp.end = 0x2005;
    check(nesbp_describe(NULL, &bp, buf, sizeof buf) > 0 && strstr(buf, "Read  ppu $2005"), "a PPU read");

    /* ---- region names ---- */
    memset(map, 0, sizeof map);
    map[0].low = 0; map[0].high = 0x1FFF; snprintf(map[0].name, sizeof map[0].name, "RAM");
    map[1].low = 0x2000; map[1].high = 0x3FFF; snprintf(map[1].name, sizeof map[1].name, "PPU");
    check(nesmem_region_name(map, 2, 0x2002, buf, sizeof buf) == 3 && strcmp(buf, "PPU") == 0, "a region by address");
    check(nesmem_region_name(map, 2, 0x4000, buf, sizeof buf) == 0 && buf[0] == 0, "none past the map");
    check(nesmem_region_name(NULL, 0, 0, buf, sizeof buf) == 0, "an empty map");

    /* ---- trace rows ---- */
    memset(&e, 0, sizeof e);
    e.pc = 0xC005; e.a = 0x12; e.x = 0x34; e.y = 0x56; e.sp = 0xFD; e.ps = 0xA5; e.len = 2;
    e.bytes[0] = 0xA9; e.bytes[1] = 0x10;
    snprintf(e.disasm, sizeof e.disasm, "LDA #$10");
    check(nesdebug_trace_format(&e, buf, sizeof buf) > 0 && strstr(buf, "C005  LDA #$10") &&
          strstr(buf, "A=12 X=34 Y=56 SP=FD") && strstr(buf, "P=Nv--dIzC"), "a row's text");
    check(nesdebug_trace_match(&e, NESDEBUG_TRACE_ALL, 0, 0) == 1, "All keeps everything");
    check(nesdebug_trace_match(&e, NESDEBUG_TRACE_BRANCHES, 0, 0) == 0, "LDA is not a branch");
    {
        static const uint8_t branchy[] = { 0x00, 0x40, 0x20, 0x60, 0x4C, 0x6C, 0x10, 0x30, 0x50, 0x70, 0x90, 0xB0, 0xD0, 0xF0 };
        static const uint8_t plain[] = { 0xEA, 0xA9, 0x8D, 0xEE, 0x4D, 0x1E, 0x11, 0x61, 0x86, 0xE6 };
        int ok = 1;
        for (i = 0; i < (int)sizeof branchy; i++) { e.bytes[0] = branchy[i]; if (!nesdebug_trace_match(&e, NESDEBUG_TRACE_BRANCHES, 0, 0)) ok = 0; }
        check(ok, "BRK RTI JSR RTS JMP and the eight branches are branches");
        ok = 1;
        for (i = 0; i < (int)sizeof plain; i++) { e.bytes[0] = plain[i]; if (nesdebug_trace_match(&e, NESDEBUG_TRACE_BRANCHES, 0, 0)) ok = 0; }
        check(ok, "other opcodes are not");
    }
    check(nesdebug_trace_match(&e, NESDEBUG_TRACE_RANGE, 0xC000, 0xC00F) == 1 &&
          nesdebug_trace_match(&e, NESDEBUG_TRACE_RANGE, 0xC006, 0xC00F) == 0, "Range filters on the PC");
    check(nesdebug_trace_match(NULL, NESDEBUG_TRACE_ALL, 0, 0) == 0, "NULL row");
    check(nesdebug_history_format(&e, buf, sizeof buf) > 0, "history rows format the same");

    /* ---- spaces ---- */
    check(strcmp(nesdebug_space_name(NESDEBUG_SPACE_PALETTE), "Palette") == 0 &&
          strcmp(nesdebug_space_name(NESDEBUG_SPACE_COUNT), "?") == 0, "space names");
    check(nesdebug_space_size(NULL, NESDEBUG_SPACE_CPU) == 0, "no engine, no space");
    {
        nesdebug_snapshot snap;
        check(nesdebug_stop_snapshot(NULL, &snap) == 0 && strcmp(snap.text, "running") == 0 && snap.address == -1,
              "no engine reads as running");
    }

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
