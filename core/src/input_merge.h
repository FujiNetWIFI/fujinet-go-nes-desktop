/*
 * input_merge -- every way of holding an NES input is a separate SOURCE, and
 * what the console sees is their merge. So letting go of one source (a key
 * release, the Controllers window's mouse, a gamepad that is unplugged) can
 * never cancel another's hold, and two pads on one port do not fight.
 *
 * Sources: the host keyboard, the Controllers window's pointer, and for each
 * connected pad (by its stable owner number, 0 to NESSESSION_MAX_PADS - 1) its
 * buttons and its stick / D-pad separately.
 *
 * Per port the ten actions are merged with opposed pairs cancelled (Up with
 * Down, Left with Right: a real pad's cross cannot close both); the console's
 * RESET key is a plain OR of the sources, and the machine resets when it goes
 * from nobody holding it to somebody.
 *
 * The functions are C so the gamepad thread (gamepad_sdl.c) can call them;
 * they live in input.cpp.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NES_INPUT_MERGE_H
#define NES_INPUT_MERGE_H

#ifdef __cplusplus
extern "C" {
#endif

#define NESSESSION_MAX_PADS 8

#define NESSESSION_SRC_KEYBOARD  0
#define NESSESSION_SRC_POINTER   1
#define NESSESSION_SRC_PAD0      2                                         /* + owner: its buttons */
#define NESSESSION_SRC_STICK0    (NESSESSION_SRC_PAD0 + NESSESSION_MAX_PADS) /* + owner: stick, D-pad */
#define NESSESSION_SRC_COUNT     (NESSESSION_SRC_STICK0 + NESSESSION_MAX_PADS)
#define NESSESSION_SRC_PAD(owner)   (NESSESSION_SRC_PAD0 + (owner))
#define NESSESSION_SRC_STICK(owner) (NESSESSION_SRC_STICK0 + (owner))

struct nessession;

/* Hold or release a port action or the RESET key (target below
 * NES_TARGET_SYSACT(0)) for a source. RESET resets the machine on the
 * merged press, from whichever thread calls. Anything else is ignored. */
void input_press(struct nessession *s, int src, int target, int down);

/* Set a source's whole mask of port actions (1 << nes_action) at once. */
void input_set_port(struct nessession *s, int src, int port, unsigned mask);

/* Let go of everything a connected pad holds (its buttons' and its stick's
 * sources). Called when a pad goes away or changes port. */
void input_pad_clear(struct nessession *s, int owner);

#ifdef __cplusplus
}
#endif

#endif /* NES_INPUT_MERGE_H */
