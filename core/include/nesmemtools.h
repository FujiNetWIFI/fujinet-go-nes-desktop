/*
 * Watches, range parsing, memory-search queries and breakpoint descriptions,
 * kept in the core so the debugger windows only lay them out.
 *
 * A watch is text the user typed (an address, a label, an expression, or a
 * range low-high / low..high) shown with its live value. It is stored as text and
 * resolved every time it is shown, so loading symbols later makes a label
 * start resolving.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NESMEMTOOLS_H
#define NESMEMTOOLS_H

#include <stdint.h>

#include "nesdebug.h"

#ifdef __cplusplus
extern "C" {
#endif

/* "low..high", "low-high" (plain numbers or labels on both sides), or one
 * expression (an address, label, label+3, [ptr]). A bare token of 1-4 hex
 * digits ("C000") is hex unless it names a label; "$C000" and expressions
 * work as in the console. With d NULL (or the engine down) only numbers
 * resolve. Returns 1 and fills low and high (inclusive, low <= high), 0 if
 * the text means nothing. */
int nessym_parse_range(nesdebug *d, const char *text, uint16_t *low, uint16_t *high);

/* ---- watches ---- */
#define NESWATCH_MAX 64
/* The process-wide list, like the symbol tables: it outlives the window. */
int  neswatch_count(void);
/* Returns the new index, or -1 if empty / full. A watch that does not resolve
 * yet is kept (it shows "?"). */
int  neswatch_add(const char *text);
void neswatch_remove(int index);
void neswatch_clear(void);
const char *neswatch_text(int index);
unsigned neswatch_generation(void);
/* "score  $C010  $42 (66)". While the machine runs the value is "-"; a range
 * shows its first 8 bytes. Returns the length. */
int  neswatch_format(nesdebug *d, int index, char *dst, int dstsz);
/* The range a watch covers (for "show in Memory"). 1 if it resolves. */
int  neswatch_range(nesdebug *d, int index, uint16_t *low, uint16_t *high);

/* ---- memory search ---- */
enum { NESSEARCH_HEX = 0, NESSEARCH_TEXT };
#define NESSEARCH_MAX 32
/* Hex: byte pairs, optional "$" / "0x" / spaces. Text: one byte per
 * character, case-insensitive. Fills pat, count, mask (0xFF) and flags for
 * nesdebug_search. Returns 1, or 0 for empty / malformed text. */
int nessearch_parse(int kind, const char *text, uint8_t *pat, int *count, uint8_t *mask, int *flags);

/* ---- breakpoints ---- */
/* "Write  $C010 (score)  if value != old"; a PPU, palette or OAM entry names
 * its space ("Write ppu $2005"). Returns the length. */
int nesbp_describe(nesdebug *d, const nesdebug_breakpoint *bpt, char *dst, int dstsz);

/* ---- memory map ---- */
/* The name of the region of `map` (from nesdebug_memory_map) holding addr
 * ("PRG window $C000 (bank 3)"), for labelling Memory rows. Fetch the map
 * once per refresh and look rows up in it. Returns the length, 0 and "" when
 * no region covers addr. */
int nesmem_region_name(const nesdebug_region *map, int count, uint16_t addr, char *dst, int dstsz);

#ifdef __cplusplus
}
#endif

#endif /* NESMEMTOOLS_H */
