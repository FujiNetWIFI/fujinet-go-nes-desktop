/*
 * KeyForward -- see KeyForward.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KeyForward.h"

#include "nessession.h"

uint32_t nesKeysymFromQt(const QKeyEvent *e)
{
    /* On X11 and Wayland Qt reports the X keycode, which is the evdev code
     * plus 8. */
    const quint32 sc = e->nativeScanCode();
    if (sc >= 8) {
        const uint32_t ks = nessession_keysym_from_evdev(sc - 8);
        if (ks) return ks;
    }

    switch (e->key()) {
    case Qt::Key_Up:      return NES_KEYSYM_UP;
    case Qt::Key_Down:    return NES_KEYSYM_DOWN;
    case Qt::Key_Left:    return NES_KEYSYM_LEFT;
    case Qt::Key_Right:   return NES_KEYSYM_RIGHT;
    case Qt::Key_Tab:     return NES_KEYSYM_TAB;
    case Qt::Key_Escape:  return NES_KEYSYM_ESCAPE;
    case Qt::Key_Return:  return NES_KEYSYM_RETURN;
    case Qt::Key_Enter:   return NES_KEYSYM_KP_ENTER;
    case Qt::Key_Backspace: return NES_KEYSYM_BACKSPACE;
    case Qt::Key_Space:   return NES_KEYSYM_SPACE;
    case Qt::Key_Shift:   return NES_KEYSYM_LSHIFT;
    case Qt::Key_Control: return NES_KEYSYM_LCTRL;
    case Qt::Key_Alt:     return NES_KEYSYM_LALT;
    case Qt::Key_ScrollLock: return NES_KEYSYM_SCROLL_LOCK;
    case Qt::Key_Asterisk: return '*';
    case Qt::Key_NumberSign: return '#';
    default: break;
    }
    const int k = e->key();
    if (k >= Qt::Key_F1 && k <= Qt::Key_F12)
        return NES_KEYSYM_F1 + (k - Qt::Key_F1);
    if (k >= Qt::Key_0 && k <= Qt::Key_9)
        return (uint32_t)('0' + (k - Qt::Key_0));
    if (k >= Qt::Key_A && k <= Qt::Key_Z)
        return (uint32_t)('a' + (k - Qt::Key_A));
    if (k >= 0x20 && k <= 0x7e)
        return (uint32_t)k;
    return 0;
}
