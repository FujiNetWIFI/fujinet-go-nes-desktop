/*
 * The app's accent as Qt colours, from the session header's macros: the
 * fill, the ink on it, and the accent as text or lines (the light variant on
 * a dark theme, where the fill itself is only ~1.5:1).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QColor>
#include <QPalette>
#include <QWidget>

extern "C" {
#include "nessession.h"
}

inline QColor nesDebugRgb(unsigned rgb)
{
    return QColor(int((rgb >> 16) & 0xFF), int((rgb >> 8) & 0xFF), int(rgb & 0xFF));
}

inline QColor nesDebugAccentColor() { return nesDebugRgb(NESSESSION_ACCENT_RGB); }
inline QColor nesDebugAccentInk() { return nesDebugRgb(NESSESSION_ACCENT_INK_RGB); }

/* The accent for text and lines: light on a dark theme, the fill otherwise. */
inline QColor nesDebugAccentTextColor(const QWidget *widget)
{
    if (widget && widget->palette().color(QPalette::Window).lightness() < 128)
        return nesDebugRgb(NESSESSION_ACCENT_LIGHT_RGB);
    return nesDebugAccentColor();
}

/* A highlight fill and the ink on it: the icon red with white ink on a light
 * theme; on a dark one the pure red barely shows, so the light red with
 * black ink. */
inline bool nesDebugDarkTheme(const QWidget *widget)
{
    return widget && widget->palette().color(QPalette::Window).lightness() < 128;
}
inline QColor nesDebugAccentFill(const QWidget *widget)
{
    return nesDebugDarkTheme(widget) ? nesDebugRgb(NESSESSION_ACCENT_LIGHT_RGB) : nesDebugAccentColor();
}
inline QColor nesDebugAccentFillInk(const QWidget *widget)
{
    return nesDebugDarkTheme(widget) ? QColor(Qt::black) : nesDebugAccentInk();
}
