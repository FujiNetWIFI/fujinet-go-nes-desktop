/*
 * KeyForward -- see the header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "KeyForward.h"

#import <Carbon/Carbon.h>   /* kVK_* virtual key codes */

#include "nessession.h"

uint32_t NESKeysymFromEvent(NSEvent *event)
{
    const uint32_t ks = nessession_keysym_from_macos_keycode([event keyCode]);
    if (ks) return ks;

    switch ([event keyCode]) {
    case kVK_UpArrow:    return NES_KEYSYM_UP;
    case kVK_DownArrow:  return NES_KEYSYM_DOWN;
    case kVK_LeftArrow:  return NES_KEYSYM_LEFT;
    case kVK_RightArrow: return NES_KEYSYM_RIGHT;
    case kVK_Tab:        return NES_KEYSYM_TAB;
    case kVK_Escape:     return NES_KEYSYM_ESCAPE;
    case kVK_Return:     return NES_KEYSYM_RETURN;
    case kVK_Space:      return NES_KEYSYM_SPACE;
    case kVK_Delete:     return NES_KEYSYM_BACKSPACE;
    default: break;
    }

    /* Anything the table does not know: the character typed. */
    NSString *chars = [event charactersIgnoringModifiers];
    if ([chars length] == 1) {
        const unichar c = [chars characterAtIndex:0];
        if (c >= 0x20 && c <= 0x7e) {
            if (c >= 'A' && c <= 'Z') return (uint32_t)(c - 'A' + 'a');
            return (uint32_t)c;
        }
        if (c >= NSF1FunctionKey && c <= NSF12FunctionKey)
            return NES_KEYSYM_F1 + (uint32_t)(c - NSF1FunctionKey);
    }
    return 0;
}

/* Modifier keys arrive as flagsChanged, not keyDown/keyUp. The key code
 * says which key; the corresponding flag says whether it went down. */
uint32_t NESKeysymFromFlagsChange(NSEvent *event, int *down)
{
    const NSEventModifierFlags f = [event modifierFlags];
    switch ([event keyCode]) {
    case kVK_Shift:        *down = (f & NSEventModifierFlagShift) != 0;   return NES_KEYSYM_LSHIFT;
    case kVK_RightShift:   *down = (f & NSEventModifierFlagShift) != 0;   return NES_KEYSYM_RSHIFT;
    case kVK_Control:      *down = (f & NSEventModifierFlagControl) != 0; return NES_KEYSYM_LCTRL;
    case kVK_RightControl: *down = (f & NSEventModifierFlagControl) != 0; return NES_KEYSYM_RCTRL;
    /* Option is where a Mac keyboard puts the key a PC calls Alt. */
    case kVK_Option:       *down = (f & NSEventModifierFlagOption) != 0;  return NES_KEYSYM_LALT;
    case kVK_RightOption:  *down = (f & NSEventModifierFlagOption) != 0;  return NES_KEYSYM_RALT;
    default: *down = 0; return 0;
    }
}

NSColor *NESAccentColor(void)
{
    return [NSColor colorWithSRGBRed:((NESSESSION_ACCENT_RGB >> 16) & 0xff) / 255.0
                               green:((NESSESSION_ACCENT_RGB >> 8) & 0xff) / 255.0
                                blue:(NESSESSION_ACCENT_RGB & 0xff) / 255.0
                               alpha:1.0];
}
