/*
 * ppumodel.c -- see ppumodel.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ppumodel.h"

#include <string.h>

#define KEY(line, dot) (((int)(line) + 1) * 1024 + (int)(dot))

void nesvid_model_seed(nesvid_model *model, uint8_t ctrl, uint8_t mask, uint16_t vaddr, uint16_t taddr,
                       uint8_t fine_x, uint8_t wtoggle)
{
    model->ctrl = ctrl;
    model->mask = mask;
    model->vaddr = (uint16_t)(vaddr & 0x7FFF);
    model->taddr = (uint16_t)(taddr & 0x7FFF);
    model->fine_x = (uint8_t)(fine_x & 7);
    model->wtoggle = wtoggle ? 1 : 0;
}

static int rendering(const nesvid_model *model)
{
    return (model->mask & 0x18) != 0;
}

static void y_increment(nesvid_model *model)
{
    unsigned vaddr = model->vaddr;
    if ((vaddr & 0x7000) != 0x7000) {
        vaddr += 0x1000;
    } else {
        unsigned coarsey;
        vaddr &= ~0x7000u;
        coarsey = (vaddr & 0x03E0) >> 5;
        if (coarsey == 29) { coarsey = 0; vaddr ^= 0x0800; }
        else if (coarsey == 31) coarsey = 0;
        else coarsey++;
        vaddr = (vaddr & ~0x03E0u) | (coarsey << 5);
    }
    model->vaddr = (uint16_t)vaddr;
}

static void data_step(nesvid_model *model, int line)
{
    /* during rendering the access also steps coarse X and Y in a way the
     * origin model does not follow; the host re-seeds from the real PPU */
    if (rendering(model) && line >= -1 && line <= 239) return;
    model->vaddr = (uint16_t)((model->vaddr + ((model->ctrl & 0x04) ? 32 : 1)) & 0x7FFF);
}

static void set_bit(uint8_t *map, unsigned index) { map[index >> 3] |= (uint8_t)(1u << (index & 7)); }

static void mark_heat(nesvid_frame_log *out, unsigned addr)
{
    addr &= 0x3FFF;
    if (addr < NESVID_CHR) set_bit(out->wr_chr, addr);
    else if (addr < 0x3000) set_bit(out->wr_nt, addr - 0x2000);
    else if (addr < 0x3F00) set_bit(out->wr_nt, addr - 0x3000);
    else set_bit(out->wr_pal, (addr & 0x1F) & ~((addr & 3) == 0 ? 0x10u : 0u));
}

static void add_event(nesvid_frame_log *out, int kind, int line, int dot)
{
    if (out->nevents < NESVID_MAX_EVENTS) {
        nesvid_event *evp = &out->events[out->nevents++];
        evp->kind = (uint8_t)kind;
        evp->line = (int16_t)line;
        evp->dot = (uint16_t)dot;
    }
}

static void sat_inc(uint8_t *count)
{
    if (*count < 255) (*count)++;
}

/* ---- the marks: what the PPU does by itself, in order ---------------------------------------- */

typedef struct { int line, stage; } cursor;      /* the next mark; line 240 is the end */

static int mark_key(const cursor *cur)
{
    static const int pre_dots[3] = { 256, 257, 304 };
    static const int vis_dots[3] = { 1, 256, 257 };
    return KEY(cur->line, cur->line < 0 ? pre_dots[cur->stage] : vis_dots[cur->stage]);
}

static void mark_next(cursor *cur)
{
    if (++cur->stage > 2) {
        cur->stage = 0;
        cur->line++;
    }
}

static void mark_apply(nesvid_model *model, const cursor *cur, nesvid_frame_log *out)
{
    if (cur->line < 0) {
        if (!rendering(model)) return;
        if (cur->stage == 0) y_increment(model);
        else if (cur->stage == 1) model->vaddr = (uint16_t)((model->vaddr & ~0x041Fu) | (model->taddr & 0x041Fu));
        else model->vaddr = (uint16_t)((model->vaddr & ~0x7BE0u) | (model->taddr & 0x7BE0u));
        return;
    }
    if (cur->stage == 0) {
        nesvid_line *line = &out->line[cur->line];
        line->valid = 1;
        line->ctrl = model->ctrl;
        line->mask = model->mask;
        line->fine_x = model->fine_x;
        line->vaddr = model->vaddr;
        line->taddr = model->taddr;
        out->last_line = cur->line;
        return;
    }
    if (!rendering(model)) return;
    if (cur->stage == 1) y_increment(model);
    else model->vaddr = (uint16_t)((model->vaddr & ~0x041Fu) | (model->taddr & 0x041Fu));
}

/* apply every mark before `key`, but not past the stop */
static void marks_until(nesvid_model *model, cursor *cur, int key, int stop_key, nesvid_frame_log *out)
{
    while (cur->line < NESVID_H) {
        const int when = mark_key(cur);
        if (when >= key || when > stop_key) break;
        mark_apply(model, cur, out);
        mark_next(cur);
    }
}

/* ---- the accesses ------------------------------------------------------------------------------ */

static void do_write(nesvid_model *model, const nesvid_ev *event, nesvid_frame_log *out)
{
    const int reg = event->addr & 7;
    const int line = event->line;
    const unsigned value = event->value;

    out->reg_writes[reg]++;
    if (line >= 0 && line < NESVID_H) {
        out->reg_vis[reg]++;
        if (out->reg_first[reg] < 0) out->reg_first[reg] = (int16_t)line;
    }
    switch (reg) {
    case 0:
        model->ctrl = (uint8_t)value;
        model->taddr = (uint16_t)((model->taddr & ~0x0C00u) | ((value & 3) << 10));
        break;
    case 1:
        model->mask = (uint8_t)value;
        break;
    case 5:
        if (!model->wtoggle) {
            model->taddr = (uint16_t)((model->taddr & ~0x001Fu) | (value >> 3));
            model->fine_x = (uint8_t)(value & 7);
            model->wtoggle = 1;
        } else {
            model->taddr = (uint16_t)((model->taddr & ~0x73E0u) | ((value & 7) << 12) | ((value >> 3) << 5));
            model->wtoggle = 0;
        }
        break;
    case 6:
        if (!model->wtoggle) {
            model->taddr = (uint16_t)((model->taddr & 0x00FFu) | ((value & 0x3F) << 8));
            model->wtoggle = 1;
        } else {
            model->taddr = (uint16_t)((model->taddr & 0xFF00u) | value);
            model->vaddr = model->taddr;
            model->wtoggle = 0;
        }
        break;
    case 7:
        mark_heat(out, event->ttype == NESVID_TT_PPU ? event->target : model->vaddr);
        data_step(model, line);
        break;
    case 4:
        if (event->ttype == NESVID_TT_OAM) set_bit(out->wr_oam, event->target & 0xFF);
        break;
    default:
        break;
    }
    if (reg != 4 && reg != 7 && out->nwrites < NESVID_MAX_WRITES) {
        nesvid_write *wrec = &out->writes[out->nwrites++];
        wrec->line = (int16_t)line;
        wrec->dot = event->dot;
        wrec->reg = (uint8_t)reg;
        wrec->value = (uint8_t)value;
    }
    if (reg != 4 && reg != 7 && line >= 0 && line < NESVID_H) {
        sat_inc(&out->line[line].writes);
        if (event->dot > 1) out->line[line].mid = 1;
    }
}

static void do_read(nesvid_model *model, const nesvid_ev *event, nesvid_frame_log *out)
{
    const int reg = event->addr & 7;
    out->reg_reads[reg]++;
    if (reg == 2) model->wtoggle = 0;
    else if (reg == 7) data_step(model, event->line);
}

void nesvid_model_run(nesvid_model *model, const nesvid_ev *events, int count,
                      int stop_line, int stop_dot, nesvid_frame_log *out)
{
    cursor cur = { -1, 0 };
    const int stop_key = stop_line < -1 ? 1 << 30 : KEY(stop_line, stop_dot);
    int last_key = 0;

    memset(out, 0, sizeof *out);
    out->last_line = -1;
    for (int reg = 0; reg < 8; reg++) out->reg_first[reg] = -1;
    if (!model) return;
    for (int item = 0; item < count; item++) {
        const nesvid_ev *evp = &events[item];
        int key = KEY(evp->line, evp->dot);
        if (key < last_key) key = last_key;         /* never go back in time */
        last_key = key;
        if (key > stop_key) break;
        marks_until(model, &cur, key, stop_key, out);
        switch (evp->kind) {
        case NESVID_EVK_WRITE:
            if (evp->addr >= 0x2000 && evp->addr <= 0x3FFF) do_write(model, evp, out);
            break;
        case NESVID_EVK_READ:
            if (evp->addr >= 0x2000 && evp->addr <= 0x3FFF) do_read(model, evp, out);
            break;
        case NESVID_EVK_MAPPER:
            out->mapper_writes++;
            if (evp->line >= 0 && evp->line < NESVID_H) {
                nesvid_line *line = &out->line[evp->line];
                sat_inc(&line->mapper_writes);
                line->mapper_value = evp->value;
                line->mapper_addr = evp->addr;
            }
            break;
        case NESVID_EVK_NMI: add_event(out, NESVID_EV_NMI, evp->line, evp->dot); break;
        case NESVID_EVK_IRQ: add_event(out, NESVID_EV_IRQ, evp->line, evp->dot); break;
        case NESVID_EVK_SPRITE0: add_event(out, NESVID_EV_SPRITE0, evp->line, evp->dot); break;
        default: break;
        }
    }
    marks_until(model, &cur, 1 << 30, stop_key, out);
    /* a write on a line lands on the record that line already has; record
     * counters kept above on lines whose mark had not fired yet are lost to
     * valid=0 only for lines the replay never reached */
}

void nesvid_line_scroll(const nesvid_line *line, int *outx, int *outy)
{
    const unsigned vaddr = line->vaddr;
    int posx = (int)(vaddr & 0x1F) * 8 + line->fine_x + (int)((vaddr >> 10) & 1) * 256;
    int posy = (int)((vaddr >> 5) & 0x1F) * 8 + (int)((vaddr >> 12) & 7) + (int)((vaddr >> 11) & 1) * 240;
    if (outx) *outx = posx % 512;
    if (outy) *outy = posy % 480;
}
