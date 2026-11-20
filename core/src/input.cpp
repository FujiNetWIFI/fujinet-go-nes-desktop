/*
 * input.cpp -- the NES controllers: each port's pad and the console's RESET
 * key, as the frontends' targets, the keyboard's bindings, the gamepads and
 * the Controllers window's pointer.
 *
 * Every way of holding an input is a source of its own (input_merge.h) and the
 * console sees their merge, under the session's input mutex (a leaf lock). So
 * a key release, a window drag letting go or an unplugged pad can only ever
 * release what that source itself held. The merged state is handed on to
 * MesenHost (one atomic per button, read by Mesen's emulation thread); nothing
 * here takes a lock Mesen takes, and the lock is never held across the
 * machine's reset.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <cstring>

#include "MesenHost.h"

extern "C" {
#include "session_internal.h"
}

namespace {

MesenHost* host_of(struct nessession* s)
{
  return static_cast<MesenHost*>(s->host);
}

const unsigned kVert = (1u << NES_ACT_UP) | (1u << NES_ACT_DOWN);
const unsigned kHorz = (1u << NES_ACT_LEFT) | (1u << NES_ACT_RIGHT);
const unsigned kActMask = (1u << NES_ACT_PER_PORT) - 1;

// Every source's bits on a port, opposed pairs cancelled (a pad's cross
// cannot close both ways). input_mtx held.
unsigned merged_pad(const struct nessession* s, int port)
{
  unsigned bits = 0;
  for(int src = 0; src < NESSESSION_SRC_COUNT; src++)
    bits |= s->pad_src[port][src];
  if((bits & kVert) == kVert) bits &= ~kVert;
  if((bits & kHorz) == kHorz) bits &= ~kHorz;
  return bits;
}

unsigned merged_switches(const struct nessession* s)
{
  unsigned bits = 0;
  for(int src = 0; src < NESSESSION_SRC_COUNT; src++)
    bits |= s->sw_src[src];
  return bits & ((1u << NES_SW_COUNT) - 1);
}

// Tell MesenHost what changed. input_mtx held, so the order the sources
// changed in is the order the machine hears it.
void forward_locked(struct nessession* s)
{
  MesenHost* host = host_of(s);
  if(!host || !s->running) return;
  for(int port = 0; port < 2; port++)
  {
    const unsigned now = merged_pad(s, port);
    const unsigned diff = now ^ s->fwd_pad[port];
    for(int act = 0; act < NES_ACT_PER_PORT; act++)
      if(diff & (1u << act))
        host->SetButton(port, act, ((now >> act) & 1u) != 0);
    s->fwd_pad[port] = static_cast<uint16_t>(now);
  }
}

void clear_source_locked(struct nessession* s, int src)
{
  for(int port = 0; port < 2; port++)
    s->pad_src[port][src] = 0;
  s->sw_src[src] = 0;
  forward_locked(s);
}

} // namespace

// Called by nessession_start / nessession_stop: nothing is held across a
// power-up, and the host's keys start released.
extern "C" void input_reset(struct nessession* s)
{
  pthread_mutex_lock(&s->input_mtx);
  memset(s->pad_src, 0, sizeof s->pad_src);
  memset(s->sw_src, 0, sizeof s->sw_src);
  memset(s->fwd_pad, 0, sizeof s->fwd_pad);
  memset(s->held_keysym, 0, sizeof s->held_keysym);
  pthread_mutex_unlock(&s->input_mtx);
}

extern "C" void input_press(struct nessession* s, int src, int target, int down)
{
  if(!s || src < 0 || src >= NESSESSION_SRC_COUNT || target < 0 ||
     target >= NES_TARGET_SYSACT(0))
    return;
  bool reset_now = false;
  pthread_mutex_lock(&s->input_mtx);
  if(target < 2 * NES_ACT_PER_PORT)
  {
    const int port = target / NES_ACT_PER_PORT;
    const unsigned bit = 1u << (target % NES_ACT_PER_PORT);
    if(down) s->pad_src[port][src] |= static_cast<uint16_t>(bit);
    else s->pad_src[port][src] &= static_cast<uint16_t>(~bit);
  }
  else
  {
    const int sw = target - 2 * NES_ACT_PER_PORT;
    const unsigned before = merged_switches(s);
    if(down) s->sw_src[src] |= static_cast<uint8_t>(1u << sw);
    else s->sw_src[src] &= static_cast<uint8_t>(~(1u << sw));
    reset_now = sw == NES_SW_RESET && !(before & (1u << sw)) &&
                (merged_switches(s) & (1u << sw));
  }
  forward_locked(s);
  pthread_mutex_unlock(&s->input_mtx);
  // the reset itself runs with the lock let go: the pad thread calls this
  if(reset_now) nessession_reset_game(s);
}

extern "C" void input_set_port(struct nessession* s, int src, int port, unsigned mask)
{
  if(!s || src < 0 || src >= NESSESSION_SRC_COUNT || port < 0 || port > 1) return;
  pthread_mutex_lock(&s->input_mtx);
  s->pad_src[port][src] = static_cast<uint16_t>(mask & kActMask);
  forward_locked(s);
  pthread_mutex_unlock(&s->input_mtx);
}

extern "C" void input_pad_clear(struct nessession* s, int owner)
{
  if(!s || owner < 0 || owner >= NESSESSION_MAX_PADS) return;
  pthread_mutex_lock(&s->input_mtx);
  clear_source_locked(s, NESSESSION_SRC_PAD(owner));
  clear_source_locked(s, NESSESSION_SRC_STICK(owner));
  pthread_mutex_unlock(&s->input_mtx);
}

// ---- public ------------------------------------------------------------------

// A target held by the Controllers window's pointer (a pressed part), or a
// system action fired. The keyboard's and the pads' holds come through
// nessession_key and the gamepad thread.
extern "C" void nessession_press(nessession* s, int target, int down)
{
  if(!s || target < 0 || target >= NES_TARGET_COUNT) return;
  if(target < NES_TARGET_SYSACT(0))
    input_press(s, NESSESSION_SRC_POINTER, target, down);
  else if(down)
    nessession_sysaction(s, target - NES_TARGET_SYSACT(0));
}

// Replaces the pointer's four direction bits only: the button bits held by
// nessession_press (another finger, another pointer) are kept.
extern "C" void nessession_pointer_set(nessession* s, int port, unsigned mask)
{
  if(!s || port < 0 || port > 1) return;
  pthread_mutex_lock(&s->input_mtx);
  const unsigned keep = s->pad_src[port][NESSESSION_SRC_POINTER] & ~(kVert | kHorz);
  s->pad_src[port][NESSESSION_SRC_POINTER] =
    static_cast<uint16_t>(keep | (mask & (kVert | kHorz)));
  forward_locked(s);
  pthread_mutex_unlock(&s->input_mtx);
}

// Lets go of the window's pointer: every part and the cross it holds. The
// keyboard and the gamepads are left alone.
extern "C" void nessession_release_pointer(nessession* s)
{
  if(!s) return;
  pthread_mutex_lock(&s->input_mtx);
  clear_source_locked(s, NESSESSION_SRC_POINTER);
  pthread_mutex_unlock(&s->input_mtx);
}

// Lets go of the host keyboard's holds only: the controller bindings and the
// host keys typed into the expansion keyboard. The window's mouse (also on the
// on-screen keyboard) and the pads keep theirs.
extern "C" void nessession_release_keyboard(nessession* s)
{
  if(!s) return;
  pthread_mutex_lock(&s->input_mtx);
  memset(s->held_keysym, 0, sizeof s->held_keysym);
  clear_source_locked(s, NESSESSION_SRC_KEYBOARD);
  pthread_mutex_unlock(&s->input_mtx);
  session_keyboard_release_host(s);
}

// Focus loss in the Controllers window, entering Map mode: the keyboard's and
// the pointer's holds, and the on-screen expansion keyboard's. The gamepads
// stay: each knows when it lets go.
extern "C" void nessession_release_all(nessession* s)
{
  if(!s) return;
  pthread_mutex_lock(&s->input_mtx);
  memset(s->held_keysym, 0, sizeof s->held_keysym);
  clear_source_locked(s, NESSESSION_SRC_KEYBOARD);
  clear_source_locked(s, NESSESSION_SRC_POINTER);
  pthread_mutex_unlock(&s->input_mtx);
  session_keyboard_release_all(s);
}

extern "C" unsigned nessession_buttons_held(nessession* s, int port)
{
  if(!s || port < 0 || port > 1) return 0;
  pthread_mutex_lock(&s->input_mtx);
  const unsigned bits = merged_pad(s, port);
  pthread_mutex_unlock(&s->input_mtx);
  return bits;
}

extern "C" int nessession_target_held(nessession* s, int target)
{
  if(!s || target < 0) return 0;
  if(target < 2 * NES_ACT_PER_PORT)
    return (nessession_buttons_held(s, target / NES_ACT_PER_PORT) >>
            (target % NES_ACT_PER_PORT)) & 1;
  if(target >= NES_TARGET_SWITCH(0) && target < NES_TARGET_SYSACT(0))
  {
    pthread_mutex_lock(&s->input_mtx);
    const unsigned bits = merged_switches(s);
    pthread_mutex_unlock(&s->input_mtx);
    return (bits >> (target - NES_TARGET_SWITCH(0))) & 1;
  }
  return 0;
}
