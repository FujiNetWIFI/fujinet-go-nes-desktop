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

/* ---- the prompt -------------------------------------------------------------- */
/* Run one command. "help" lists them: step [n], over, out, frame [n],
 * scanline [n], run, stop, runto <addr>, break <addr> [cond], bpr/bpw
 * <addr>[-<end>] [cond], delete <addr>, clear, print <expr> (any Mesen
 * expression: a, x, y, sp, pc, ps, cycle, scanline, frame, [$addr],
 * {$addr}, labels...), mem <addr> [len], poke <addr> <val>, ppu <addr>
 * [len], label <addr> <name>, labels <file>, disasm [addr] [n], cart.
 * Output text into dst; returns its length. */
int  nesdebug_command(nesdebug *d, const char *command, char *dst, int dstsz);
/* Completions for a prefix (commands and labels), one per line. Returns
 * the count. */
int  nesdebug_completions(nesdebug *d, const char *prefix, char *dst, int dstsz);

/* ---- stepping shortcuts (the toolbar) --------------------------------------- */
void nesdebug_step(nesdebug *d);          /* F7: one instruction */
void nesdebug_step_over(nesdebug *d);     /* F8: over a JSR */
void nesdebug_step_out(nesdebug *d);      /* Shift+F8: to the RTS/RTI */
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
    int has_breakpoint;
    int is_code;            /* executed or reachable code (else data) */
    char bytes[16];
    char label[48];
    char disasm[64];
    char comment[64];
} nesdebug_line;
/* Up to `max` lines starting at the row that holds `addr`. Returns the
 * count; *pc_line is the index of the PC's line in `out`, -1 when not
 * among them. */
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

/* ---- breakpoints -------------------------------------------------------------- */
typedef enum { NESDEBUG_BP_EXEC = 1, NESDEBUG_BP_READ = 2, NESDEBUG_BP_WRITE = 4 } nesdebug_bp_type;
typedef struct {
    int id;
    int type;              /* nesdebug_bp_type bits */
    uint16_t start, end;
    int enabled;
    char condition[128];
} nesdebug_breakpoint;
/* Execute breakpoint at addr: added if absent, removed if present. Returns
 * 1 when one is now set. */
int  nesdebug_breakpoint_toggle(nesdebug *d, uint16_t addr);
int  nesdebug_breakpoint_check(nesdebug *d, uint16_t addr);
/* Returns the new id, or -1 if the condition does not parse. */
int  nesdebug_breakpoint_add(nesdebug *d, int type, uint16_t start, uint16_t end,
                             const char *condition);
void nesdebug_breakpoint_remove(nesdebug *d, int id);
void nesdebug_breakpoint_enable(nesdebug *d, int id, int enabled);
int  nesdebug_breakpoint_list(nesdebug *d, nesdebug_breakpoint *out, int max);
void nesdebug_breakpoint_clear(nesdebug *d);

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
