/*
 * vidtab_stubs -- the machine and debugger vidtab.c calls, as a recorder, so
 * vidtab_test runs with no emulator, no SDL and no ROM (and links on every
 * toolchain). The test reads the recorded breakpoints and sets the stop state.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nesdebug.h"
#include "nesvidcap.h"
#include "nessession.h"

int stub_stopped;                   /* what nesdebug_is_stopped answers */
unsigned stub_generation;
int stub_capture_enabled;           /* net nesvid_host_capture_enable calls */
int stub_fill_ok = 1;

#define STUB_MAX 64
nesdebug_breakpoint stub_bps[STUB_MAX];
int stub_nbps;
int stub_next_id = 1;
const char *(*stub_video_text)(const char *section);

static char s_dummy_session[1];
static char s_dummy_debugger[1];

nessession *stub_session(void) { return (nessession *)s_dummy_session; }
nesdebug *stub_debugger(void) { return (nesdebug *)s_dummy_debugger; }

nesdebug *nessession_debugger(nessession *session)
{
    (void)session;
    return stub_debugger();
}

void nesvid_host_capture_enable(nessession *session, int enable)
{
    (void)session;
    stub_capture_enabled += enable ? 1 : -1;
}

int nesvid_host_capture_get(nessession *session, nesvid_capture *dst, uint64_t *serial_inout)
{
    (void)session; (void)dst; (void)serial_inout;
    return 0;
}

int nesvid_host_capture_fill(nessession *session, nesvid_capture *dst)
{
    (void)session; (void)dst;
    return stub_fill_ok;
}

void nesvid_host_capture_stats(nessession *session, uint64_t *batches, uint64_t *resyncs)
{
    (void)session;
    if (batches) *batches = 0;
    if (resyncs) *resyncs = 0;
}

int nesdebug_is_stopped(nesdebug *dbg) { (void)dbg; return stub_stopped; }
unsigned nesdebug_generation(nesdebug *dbg) { (void)dbg; return stub_generation; }

int nesdebug_breakpoint_list(nesdebug *dbg, nesdebug_breakpoint *out, int max)
{
    int num = stub_nbps < max ? stub_nbps : max;
    (void)dbg;
    memcpy(out, stub_bps, (size_t)num * sizeof *out);
    return num;
}

int nesdebug_breakpoint_add_in(nesdebug *dbg, int space, int type, uint32_t start, uint32_t end,
                               const char *condition, char *err, int errsz)
{
    nesdebug_breakpoint *entry;
    (void)dbg; (void)err; (void)errsz;
    if (stub_nbps >= STUB_MAX) return -1;
    entry = &stub_bps[stub_nbps++];
    memset(entry, 0, sizeof *entry);
    entry->id = stub_next_id++;
    entry->type = type;
    entry->space = space;
    entry->start = (uint16_t)start;
    entry->end = (uint16_t)end;
    entry->enabled = 1;
    snprintf(entry->condition, sizeof entry->condition, "%s", condition ? condition : "");
    return entry->id;
}

void nesdebug_set_video_text(const char *(*func)(const char *section))
{
    stub_video_text = func;
}
