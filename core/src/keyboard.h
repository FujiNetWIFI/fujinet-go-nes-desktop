/*
 * keyboard -- private interface to core/src/keyboard.c for the session.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NES_KEYBOARD_H
#define NES_KEYBOARD_H

#ifdef __cplusplus
extern "C" {
#endif

/* How many key indices the keyboard type has (MesenCE reads CustomKeys[0..n-1]). */
int keyboard_key_count(int type);

/* The most any keyboard has: the host reserves this many virtual key codes. */
#define NES_KBD_MAX_KEYS 128

#ifdef __cplusplus
}
#endif

#endif /* NES_KEYBOARD_H */
