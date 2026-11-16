/*
 * nesdebug -- the debugger contract: MesenCE's own debugger engine (its
 * NES disassembler, breakpoint manager, expression evaluator, label manager
 * and memory dumper) behind a C API the four native debugger windows share.
 *
 * The engine only exists while a window holds it attached (it costs
 * emulation speed): nesdebug_attach() when the window opens -- which also
 * stops the machine, as on every sibling -- and nesdebug_detach() when it
 * closes, which lets the machine run on.
 *
 * Calls are safe from the UI thread at any time. Inspection is meant for
 * the stopped state; while running it answers with whatever is current.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NESDEBUG_H
#define NESDEBUG_H

#include <stdint.h>

#include "nessession.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Lazily created; lives as long as the session. */
nesdebug *nesdebug_get(nessession *s);

/* ---- attach / stop / go ------------------------------------------------------ */
/* Bring the engine up and stop at the next instruction. Idempotent. */
void nesdebug_attach(nesdebug *d);
/* Clear the engine away and let the machine run. Breakpoints are kept for
 * the next attach. */
void nesdebug_detach(nesdebug *d);
int  nesdebug_is_attached(nesdebug *d);

int  nesdebug_is_stopped(nesdebug *d);
void nesdebug_stop(nesdebug *d);         /* break at the next instruction */
void nesdebug_resume(nesdebug *d);       /* "run" */
/* Why the machine last stopped ("stopped", "breakpoint: exec $C123",
 * "step", "BRK", ...) and the PC, -1 if none. Returns length. */
int  nesdebug_stop_reason(nesdebug *d, char *dst, int dstsz, int *address);
/* Bumped whenever a command ran or the machine stopped/resumed, so a window
 * knows when to refresh. */
unsigned nesdebug_generation(nesdebug *d);
/* Bump it by hand (a command ran that the engine cannot see change). */
void nesdebug_touch(nesdebug *d);

/* ---- the prompt -------------------------------------------------------------- */
/* Run one command. "help" lists them: step [n], over, out, run, stop,
 * frame [n], scanline [n], runto <addr>, break <addr> [cond],
 * bpr/bpw/bpa [ppu|pal|oam] <addr>[-<end>] [cond], delete <id|addr>,
 * enable/disable <id>, clear, hits [clear], breaks, print <expr> (any Mesen
 * expression: a, x, y, sp, pc, ps, cycle, scanline, frame, [$addr],
 * {$addr}, labels...), regs, set <reg> <expr>, mem [space] <addr> [len],
 * poke [space] <addr> <val...>, ppu <addr> [len], disasm [addr] [n],
 * find [space] [hex] <bytes> | find [space] text <string>,
 * trace [on|off|clear|n], watch [expr|lo-hi], unwatch <n|all>,
 * symbols [text], label <addr> <name>, labels <file>, map, cart, io, sound,
 * video [section], save <dis|prg|chr|ram|wram> <file>.
 * Output text into dst; returns its length. */
int  nesdebug_command(nesdebug *d, const char *command, char *dst, int dstsz);
/* Completions for a prefix (commands and labels), one per line. Returns
 * the count. */
int  nesdebug_completions(nesdebug *d, const char *prefix, char *dst, int dstsz);

/* ---- stepping shortcuts (the toolbar) --------------------------------------- */
void nesdebug_step(nesdebug *d);          /* F7: one instruction */
void nesdebug_step_over(nesdebug *d);     /* F8: over a JSR */
void nesdebug_step_out(nesdebug *d);      /* Shift+F8: to the RTS/RTI */
void nesdebug_step_count(nesdebug *d, int n);   /* n instructions */
void nesdebug_scanline(nesdebug *d, int n);
void nesdebug_frame(nesdebug *d, int n);
/* Run until PC == addr. */
void nesdebug_run_to(nesdebug *d, uint16_t addr);

/* ---- CPU --------------------------------------------------------------------- */
typedef struct {
    int pc, sp, a, x, y, ps;
    int n, v, d, i, z, c;
    int irq, nmi;               /* lines pending */
    uint64_t total_cycles;
    int scanline, dot;          /* where the PPU is */
    uint32_t frame;
} nesdebug_cpu;
void nesdebug_cpu_get(nesdebug *d, nesdebug_cpu *out);
typedef enum { NES_REG_PC, NES_REG_SP, NES_REG_A, NES_REG_X, NES_REG_Y,
               NES_REG_PS, NES_FLAG_N, NES_FLAG_V, NES_FLAG_D,
               NES_FLAG_I, NES_FLAG_Z, NES_FLAG_C } nesdebug_reg;
void nesdebug_cpu_set(nesdebug *d, int reg, int value);

/* ---- PPU --------------------------------------------------------------------- */
typedef struct {
    uint8_t ctrl, mask, status, oam_addr;   /* $2000 $2001 $2002 $2003 as last written/read */
    uint16_t vram_addr, tmp_addr;           /* v, t */
    uint8_t fine_x, write_toggle;
    int scanline, dot;
    uint32_t frame;
    int nmi_on_vblank, sprite_size_16, bg_table_1000, spr_table_1000, increment_32;
    int show_bg, show_spr, show_bg_left, show_spr_left, grayscale;
    int vblank, sprite0_hit, sprite_overflow;
    char mirroring[16];                     /* "Vertical", "Horizontal", "Screen A"... */
} nesdebug_ppu;
void nesdebug_ppu_get(nesdebug *d, nesdebug_ppu *out);

/* The PPU's views, as XRGB images. Returns 1 when drawn.
 *   NAMETABLES: 512x480, the four nametables through the current
 *               mirroring, with the scroll window outlined.
 *   PATTERNS:   256x128, $0000 and $1000 side by side, in BG palette
 *               `palette` (0-7; 4-7 are the sprite palettes).
 *   SPRITES:    256x240, the 64 OAM sprites where they sit on screen.
 *   PALETTE:    256x32, the 32 palette RAM entries as 16x16 swatches
 *               (BG top row, sprites bottom). */
typedef enum { NESDEBUG_VIEW_NAMETABLES = 0, NESDEBUG_VIEW_PATTERNS,
               NESDEBUG_VIEW_SPRITES, NESDEBUG_VIEW_PALETTE } nesdebug_view;
#define NESDEBUG_VIEW_MAX_PIXELS (512 * 480)
int  nesdebug_ppu_view(nesdebug *d, int view, int palette, uint32_t *dst,
                       int *width, int *height);
/* One OAM entry, for the sprite list. */
typedef struct { uint8_t y, tile, attr, x; } nesdebug_sprite;
void nesdebug_oam_get(nesdebug *d, nesdebug_sprite out[64]);
/* Palette index (0-63) to XRGB, through the running video filter's palette. */
uint32_t nesdebug_color(nesdebug *d, uint8_t index);

/* ---- APU / controllers ------------------------------------------------------- */
typedef struct {
    int pulse1_period, pulse1_volume, pulse1_duty, pulse1_enabled;
    int pulse2_period, pulse2_volume, pulse2_duty, pulse2_enabled;
    int triangle_period, triangle_enabled;
    int noise_period, noise_volume, noise_enabled;
    int dmc_enabled, dmc_bytes_left;
    int frame_irq, dmc_irq;
    uint8_t pad[2];          /* buttons held, bit 0 = A ... 7 = Right */
    int keyboard;            /* nes_keyboard on the expansion port */
    int tape;                /* nes_tape_state */
    char keys_held[96];      /* the keyboard's held keys, space-separated labels */
} nesdebug_apu;
void nesdebug_apu_get(nesdebug *d, nesdebug_apu *out);

/* ---- memory ------------------------------------------------------------------ */
/* CPU-bus reads without side effects. */
int  nesdebug_read(nesdebug *d, uint16_t addr, uint8_t *dst, int n);
/* A debugger edit: RAM, WRAM, PRG/CHR SRAM and the mailbox arena all take
 * it (it is written into the memory behind the address, not onto the bus). */
void nesdebug_write(nesdebug *d, uint16_t addr, uint8_t value);
/* The 2K of console RAM ($0000-$07FF). */
void nesdebug_ram_get(nesdebug *d, uint8_t out[2048]);
/* PPU-bus reads ($0000-$3FFF) without side effects. */
int  nesdebug_ppu_read(nesdebug *d, uint16_t addr, uint8_t *dst, int n);

/* ---- disassembly --------------------------------------------------------------- */
typedef struct {
    uint16_t address;
    int is_pc;
    int has_breakpoint;     /* an execute breakpoint here, enabled or not */
    int breakpoint_enabled; /* ... and it is enabled */
    int is_code;            /* executed or reachable code (else data) */
    char bytes[16];
    char label[48];
    char disasm[64];
    char comment[64];
} nesdebug_line;
/* Up to `max` lines starting at the row that holds `addr`. Returns the
 * count; *pc_line is the index of the PC's line in `out`, -1 when not
 * among them. Mesen lists only memory that exists (RAM, PRG, WRAM, the
 * loader): from an address with nothing behind it ($2000-$4FFF) the first
 * line is the next backed one, so out[0].address can be above `addr`. */
int  nesdebug_disassemble(nesdebug *d, uint16_t addr, nesdebug_line *out,
                          int max, int *pc_line);
/* The address `rows` disassembly rows before (negative) or after `addr` --
 * for scrolling, and for "Follow PC" (PC a third of the way down). */
int  nesdebug_row_address(nesdebug *d, uint16_t addr, int rows);
/* Address of a label, or -1. Label of an address into dst (may be empty). */
int  nesdebug_label_address(nesdebug *d, const char *label);
int  nesdebug_address_label(nesdebug *d, uint16_t addr, char *dst, int dstsz);
int  nesdebug_set_label(nesdebug *d, uint16_t addr, const char *label);
/* Load a symbol file: ld65 -Ln / VICE ("al C:1234 .name"), a ca65 .dbg, or
 * Mesen's .mlb. With path NULL, <cart>.dbg/.lbl/.mlb/.sym next to the
 * opened cartridge. Message into msg; returns labels loaded or -1. */
int  nesdebug_load_symbols(nesdebug *d, const char *path, char *msg, int msgsz);

/* ---- the cartridge ------------------------------------------------------------ */
typedef struct {
    int present, link_up, busy, mailbox_live, sram_enabled, loading, booted_image;
    int load_pct;
    int mapper;
    char mapper_name[32];
    uint8_t prg_slot[4];
    uint16_t chr_slot[8];
    char mirroring[16];
    int wram_enabled, wram_protected, chr_writable;
    int irq_enabled, irq_line, irq_latch, irq_counter;
    uint8_t ackseq, status, last_error, boot_state, boot_pct, boot_err, diag_rmw;
    uint32_t queue_depth;
    char link_error[128];
} nesdebug_cart;
void nesdebug_cart_get(nesdebug *d, nesdebug_cart *out);
/* One line: "FujiNet cartridge: CONFIG, mapper 0 (NROM), link up". */
int  nesdebug_cart_info(nesdebug *d, char *dst, int dstsz);

/* ---- FujiNet: the cartridge's link, as FujiBus packets ---------------------- */
typedef struct {
    uint64_t serial;
    int      to_host;       /* 1: FujiNet -> NES, 0: NES -> FujiNet */
    int      device, command, length;   /* from the FujiBus header, -1 if short */
    int      nbytes;
    uint8_t  data[64];      /* the decoded packet's first bytes */
    char     text[96];      /* "NES -> FujiNet 70 F4 READ HOST SLOTS (6 bytes)" */
    uint32_t repeat;        /* the same exchange this many times in a row (xN) */
} nesdebug_fuji_packet;
/* Always on from the session's start, attached or not: the newest `max`
 * packets, oldest first. Returns the count. */
int  nesdebug_fujinet_trace(nesdebug *d, nesdebug_fuji_packet *out, int max);
void nesdebug_fujinet_trace_clear(nesdebug *d);
/* The link's state for the FujiNet tab, as text lines. Returns length. */
int  nesdebug_fujinet_state(nesdebug *d, char *dst, int dstsz);

/* ---- breakpoints -------------------------------------------------------------- */
/* Address spaces: breakpoints take 0-3; the Memory tab and search take all.
 *   CPU      the 64K bus as mapped now
 *   PPU      pattern/CHR tables and nametables, $0000-$3EFF ($3F00-$3FFF
 *            reads as the palette when read with nesdebug_ppu_read)
 *   PALETTE  the 32 bytes of palette RAM
 *   OAM      the 256 bytes of sprite RAM
 *   PRG      the cartridge's PRG image (SRAM behind the bank table)
 *   CHR      the cartridge's CHR image (RAM)
 *   WRAM     the 8K at $6000
 *   RAM      the console's 2K */
typedef enum { NESDEBUG_SPACE_CPU = 0, NESDEBUG_SPACE_PPU = 1, NESDEBUG_SPACE_PALETTE = 2,
               NESDEBUG_SPACE_OAM = 3, NESDEBUG_SPACE_PRG = 4, NESDEBUG_SPACE_CHR = 5,
               NESDEBUG_SPACE_WRAM = 6, NESDEBUG_SPACE_RAM = 7, NESDEBUG_SPACE_COUNT } nesdebug_space;
/* "CPU", "PPU", "Palette", "OAM", "PRG", "CHR", "WRAM", "RAM", or "?" */
const char *nesdebug_space_name(int space);

typedef enum { NESDEBUG_BP_EXEC = 1, NESDEBUG_BP_READ = 2, NESDEBUG_BP_WRITE = 4,
               /* read and write as one entry: one id, one hit count */
               NESDEBUG_BP_ACCESS = 6 } nesdebug_bp_type;
#define NESDEBUG_MAX_BREAKPOINTS 128
typedef struct {
    int id;
    int type;              /* nesdebug_bp_type bits */
    int space;             /* NESDEBUG_SPACE_CPU .. _OAM */
    uint16_t start, end;   /* inclusive, in the space's own addresses */
    int enabled;
    unsigned hits;         /* times this entry stopped the machine */
    char condition[128];   /* "" for none */
} nesdebug_breakpoint;
/* Execute breakpoint at addr: added if absent, removed if present. Returns
 * 1 when one is now set. */
int  nesdebug_breakpoint_toggle(nesdebug *d, uint16_t addr);
/* 1 when an execute breakpoint is at addr (enabled or not). */
int  nesdebug_breakpoint_check(nesdebug *d, uint16_t addr);
/* A CPU-space breakpoint. Returns the new id, or -1 if the condition does
 * not parse. */
int  nesdebug_breakpoint_add(nesdebug *d, int type, uint16_t start, uint16_t end,
                             const char *condition);
/* Any space (EXEC only in CPU space). Returns the id, or -1 with the reason
 * in err (a condition that does not parse, `old` outside CPU space, a bad
 * range or type, too many). The condition is a Mesen expression (registers
 * a x y sp ps pc, [addr], {addr}, labels, frame cycle scanline, ...) with
 * three words for the access: value (the byte moved), address (where) and,
 * in CPU space, old (the byte a write replaced; for a read the value). Write-
 * only registers ($2000-$2007, $4000-$4017) read as 0 in `old`. */
int  nesdebug_breakpoint_add_in(nesdebug *d, int space, int type, uint32_t start, uint32_t end,
                                const char *condition, char *err, int errsz);
void nesdebug_breakpoint_remove(nesdebug *d, int id);
void nesdebug_breakpoint_enable(nesdebug *d, int id, int enabled);
/* Replaces the condition of an entry; "" removes it. 1, or 0 with err. */
int  nesdebug_breakpoint_set_condition(nesdebug *d, int id, const char *condition,
                                       char *err, int errsz);
int  nesdebug_breakpoint_list(nesdebug *d, nesdebug_breakpoint *out, int max);
void nesdebug_breakpoint_clear(nesdebug *d);
void nesdebug_breakpoint_clear_hits(nesdebug *d);

/* ---- the stop, in words -------------------------------------------------------
 * "paused at $C005", "step at $C005", "frame at $C005", "stopped at $C005"
 * (a one-shot: step over/out, run to), "breakpoint: exec $C005",
 * "wrote $2000 = $90 (was $10) at $C123", "read $0300 = $12 at $C123",
 * "wrote ppu $2005 = $0F at $C123" (PPU/palette/OAM accesses name the space),
 * "BRK at $C123", "CPU crash (illegal opcode) at $C123". While running it
 * says "running" and *address is -1. A CPU-space read/write stops BEFORE the
 * instruction that makes the access (so the stop address is that
 * instruction); an access of the PPU's own (rendering reads, OAM DMA) stops
 * where the CPU is. "was" is left out when the old byte cannot be known
 * (write-only registers other than $2000 $2001 $2003 $4015). Returns length. */
int  nesdebug_stop_text(nesdebug *d, char *dst, int dstsz, int *address);

enum {
    NESDEBUG_STOP_NONE = 0,
    NESDEBUG_STOP_PAUSE,
    NESDEBUG_STOP_STEP,
    NESDEBUG_STOP_BREAKPOINT,      /* an EXEC entry */
    NESDEBUG_STOP_WATCH_READ,
    NESDEBUG_STOP_WATCH_WRITE,
    NESDEBUG_STOP_ONESHOT,         /* step over / out / run to */
    NESDEBUG_STOP_FRAME,           /* frame or scanline */
    NESDEBUG_STOP_OTHER            /* BRK, a CPU crash, an unofficial opcode */
};
typedef struct {
    int      reason;        /* NESDEBUG_STOP_* */
    int      bp_id;         /* the entry that fired, 0 if none */
    int      space;         /* NESDEBUG_SPACE_* of a watch */
    uint32_t addr;          /* the byte touched (watch), or the PC */
    int      value;         /* the byte moved, -1 when not a watch */
    int      old_value;     /* the byte a write replaced, -1 when not known */
    uint16_t pc;            /* the PC at the stop */
    uint64_t serial;        /* nesdebug_stop_serial at the stop */
} nesdebug_stop_info;
/* Returns 1 while the machine is stopped, 0 while it runs (out then holds
 * the last stop). */
int  nesdebug_last_stop(nesdebug *d, nesdebug_stop_info *out);
/* Bumped by every stop (pause, breakpoint, step); frontends recentre their
 * views on the PC when it changes. */
uint64_t nesdebug_stop_serial(nesdebug *d);
/* Everything about the current stop read in one critical section, so a
 * poller never sees a stop with the old serial or the old reason: paused,
 * the serial, the stop info and its text agree. Returns paused. */
typedef struct {
    int      paused;
    uint64_t serial;
    nesdebug_stop_info stop;
    char     text[160];
    int      address;       /* the PC to show, -1 when running */
} nesdebug_snapshot;
int  nesdebug_stop_snapshot(nesdebug *d, nesdebug_snapshot *out);

/* ---- registers, in bulk ----------------------------------------------------------
 * Writes pc sp a x y and ps of `regs` (ps is authoritative: the n v d i z c
 * fields are ignored; total_cycles, irq, nmi and the beam are read-only).
 * Only while stopped: returns 1, or 0 with a reason in err. */
int  nesdebug_regs_set(nesdebug *d, const nesdebug_cpu *regs, char *err, int errsz);
/* One register or flag by name: pc sp a x y ps (p), n v d i z c.
 * Same rules. */
int  nesdebug_register_set(nesdebug *d, const char *name, long value, char *err, int errsz);
/* Register names the expression grammar knows, NULL-terminated, for completion. */
extern const char *const nesdebug_reg_names[];

/* ---- expressions ---------------------------------------------------------------------
 * MesenCE's grammar: numbers ($C000 49152), a x y sp ps pc, flags, [addr] the
 * byte and {addr} the word at addr (as the CPU sees them, no side effects),
 * labels, frame cycle scanline, C's operators; for an access, value and
 * address (and old in a breakpoint condition: see nesdebug_breakpoint_add_in).
 * Returns 0 and *out, or -1 and a message. Needs the debugger attached. */
int  nesdebug_eval(nesdebug *d, const char *text, long *out, char *err, int errsz);

/* ---- memory in every space ------------------------------------------------------------ */
/* Bytes in a space (CPU 65536, PPU 16384, PALETTE 32, OAM 256, RAM 2048;
 * PRG/CHR/WRAM as the cartridge has them). 0 while the engine is not up. */
int  nesdebug_space_size(nesdebug *d, int space);
/* Side-effect-free reads/writes of n bytes; addresses wrap within the space.
 * Writes are debugger edits (see nesdebug_write). Returns bytes done. */
int  nesdebug_read_space(nesdebug *d, int space, uint32_t addr, uint8_t *dst, int n);
int  nesdebug_write_space(nesdebug *d, int space, uint32_t addr, const uint8_t *src, int n);

/* ---- search ------------------------------------------------------------------------------- */
#define NESDEBUG_SEARCH_NOCASE 1
/* Start addresses in from..to of a space where (mem & mask) == (pat & mask)
 * for all `count` bytes. `to` bounds the START address. Returns the count
 * written, or -1 while the engine is down or on an error. */
int  nesdebug_search_in(nesdebug *d, int space, const uint8_t *pat, int count, uint8_t mask,
                        int flags, uint32_t from, uint32_t to, uint32_t *out, int max);
/* The CPU-space form. */
int  nesdebug_search(nesdebug *d, const uint8_t *pat, int count, uint8_t mask, int flags,
                     uint16_t from, uint16_t to, uint16_t *out, int max);
/* The next match after `from` (before it if backward), wrapping once.
 * Returns 1 and *found, 0 for none, -1 when the engine is down. */
int  nesdebug_search_next_in(nesdebug *d, int space, const uint8_t *pat, int count, uint8_t mask,
                             int flags, uint32_t from, int backward, uint32_t *found);
int  nesdebug_search_next(nesdebug *d, const uint8_t *pat, int count, uint8_t mask, int flags,
                          uint16_t from, int backward, uint16_t *found);

/* ---- the CPU memory map -------------------------------------------------------------------- */
typedef struct {
    uint16_t low, high;     /* inclusive */
    int      readable, writable;
    int      bank;          /* PRG window: the 8K bank number, else -1 */
    char     name[64];      /* "Console RAM (2K)", "PRG window $C000 (bank 3)" */
} nesdebug_region;
/* Ordered and gap-free from $0000 to $FFFF as the machine is mapped now: RAM
 * and its mirrors, the PPU registers and mirrors, the APU and I/O, the
 * expansion area, the mailbox arena and loader ROM, WRAM and the four PRG
 * windows with their bank numbers. Returns how many (up to max; at most 24). */
int  nesdebug_memory_map(nesdebug *d, nesdebug_region *out, int max);

/* ---- symbols, for a Symbols tab and the labels ------------------------------------------------
 * The built-in register/mailbox names, loaded files and user labels as one
 * list of CPU addresses. */
int  nesdebug_symbols_count(nesdebug *d);
/* Entry `index` (0 .. count-1), ordered by address then name. 1, or 0 past the end. */
int  nesdebug_symbols_get(nesdebug *d, int index, uint16_t *addr, char *name, int namesz);
/* Indices of the entries whose name contains `text` (case-insensitive; empty
 * matches all). Returns the count written. */
int  nesdebug_symbols_filter(nesdebug *d, const char *text, int *out, int max);
/* Bumped when symbols are loaded, labelled or cleared. */
unsigned nesdebug_symbols_generation(nesdebug *d);
/* Drops every loaded file and user label; the built-ins stay. */
void nesdebug_clear_symbols(nesdebug *d);

/* ---- trace and history --------------------------------------------------------------------------
 * Mesen's execution log (a ring of 30000 rows; it costs emulation speed
 * while on). Trace (newest first, live) and History (what ran before the
 * machine stopped, oldest first) read the one ring. Turning recording off,
 * or a power cycle, keeps what was recorded. Registers are the state BEFORE
 * the instruction ran. Safe while running (a read pauses the machine for a
 * moment). Recording needs the engine attached; asked for earlier, it
 * starts at attach. */
typedef struct {
    uint16_t pc;
    uint8_t  sp, a, x, y, ps;
    uint8_t  len;           /* instruction bytes, 1..3 */
    uint8_t  bytes[3];
    uint64_t cycles;        /* CPU cycles since power on */
    uint32_t frame;
    int16_t  scanline;
    uint16_t dot;
    char     disasm[48];    /* "INC $10" (with labels) */
} nesdebug_trace_entry;
#define NESDEBUG_TRACE_SIZE 30000
void nesdebug_trace_enable(nesdebug *d, int enable);
int  nesdebug_trace_enabled(nesdebug *d);
/* Copies up to max entries, newest first. Returns entries written. */
int  nesdebug_trace_read(nesdebug *d, nesdebug_trace_entry *out, int max);
void nesdebug_trace_clear(nesdebug *d);
/* Entries held. */
int  nesdebug_trace_count(nesdebug *d);
/* Entries recorded since the last clear: a tab polls this to see new lines. */
uint64_t nesdebug_trace_total(nesdebug *d);
/* "C005  INC $10          A=00 X=00 Y=00 SP=FD P=nv--dIzc". Returns the length. */
int  nesdebug_trace_format(const nesdebug_trace_entry *rec, char *dst, int dstsz);
enum { NESDEBUG_TRACE_ALL = 0, NESDEBUG_TRACE_BRANCHES, NESDEBUG_TRACE_RANGE };
/* 1 if the entry passes the filter (BRANCHES: jumps, calls, returns,
 * branches, BRK; RANGE: pc in low..high). */
int  nesdebug_trace_match(const nesdebug_trace_entry *rec, int filter, uint16_t low, uint16_t high);
typedef nesdebug_trace_entry nesdebug_hist_entry;
void nesdebug_history_enable(nesdebug *d, int on);
int  nesdebug_history_enabled(nesdebug *d);
void nesdebug_history_clear(nesdebug *d);
uint64_t nesdebug_history_count(nesdebug *d);
/* Up to max of the newest entries, oldest first; returns how many. */
int  nesdebug_history_copy(nesdebug *d, nesdebug_hist_entry *out, int max);
int  nesdebug_history_format(const nesdebug_hist_entry *rec, char *dst, int dstsz);
int  nesdebug_history_match(const nesdebug_hist_entry *rec, int filter, uint16_t low, uint16_t high);

/* ---- Cart & I/O, as text -------------------------------------------------------------------------
 * The cartridge, the controllers, the expansion-port keyboard and the data
 * recorder, as text lines for the tab and the `io` command. Returns length. */
int  nesdebug_io_text(nesdebug *d, char *dst, int dstsz);
/* The cartridge alone: mapper, PRG/CHR banks, mirroring, WRAM, IRQ and the
 * FujiNet mailbox registers. */
int  nesdebug_cart_text(nesdebug *d, char *dst, int dstsz);

/* ---- Sound: the 2A03 ----------------------------------------------------------------------------- */
typedef struct {
    int    enabled;
    int    period;          /* the timer, as programmed */
    double hz;              /* what it plays; 0 when silent */
    int    volume;          /* 0-15 as output now */
    int    duty;            /* pulse 0-3 */
    int    length;          /* length counter, 0 = expired */
    int    length_halt;
    int    env_constant, env_loop, env_volume, env_counter;   /* pulse and noise */
    int    sweep_enabled, sweep_negate, sweep_period, sweep_shift;   /* pulse */
    int    linear, linear_reload;  /* triangle */
    int    noise_mode;      /* 1: the short (93-step) sequence */
} nesdebug_voice;
typedef struct {
    int    present;
    nesdebug_voice pulse1, pulse2, triangle, noise;
    struct {
        int    enabled;     /* bytes remain */
        int    irq, loop;
        double rate_hz;     /* samples per second */
        int    period;
        uint16_t address;   /* sample address now / start */
        uint16_t start_address;
        uint16_t length;    /* programmed length (bytes) */
        int    bytes_left;
        int    volume;      /* output level 0-127 */
    } dmc;
    int    five_step, frame_irq, dmc_irq;
    uint8_t status;         /* $4015 as the CPU would read it */
    double cpu_hz;          /* the APU's clock */
} nesdebug_sound;
/* Returns 1 with a machine, 0 without. */
int  nesdebug_sound_get(nesdebug *d, nesdebug_sound *out);
int  nesdebug_sound_text(nesdebug *d, char *dst, int dstsz);

/* ---- the Video tab's text for the console `video` command -------------------------------------
 * func gets a section name (or "") and returns text that stays valid until its
 * next call, NULL when there is no frame yet. Set by whoever owns the Video
 * controller; until then `video` says there is no video state. */
void nesdebug_set_video_text(const char *(*func)(const char *section));

/* ---- files -------------------------------------------------------------------- */
/* Save with the path a native file picker supplied. kind: "dis" (the
 * disassembly of $8000-$FFFF), "prg" / "chr" (the cartridge SRAMs as far
 * as the image fills them), "ram" (console RAM), "wram". Message into msg;
 * returns 0 or -1. */
int  nesdebug_save(nesdebug *d, const char *kind, const char *path,
                   char *msg, int msgsz);

#ifdef __cplusplus
}
#endif

#endif /* NESDEBUG_H */
