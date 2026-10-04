/*
 * display.c -- see display.h.
 *
 * Two things here are load bearing and neither is obvious:
 *
 *  - The frame clock is the emulator's clock. GTK4 presents on the
 *    compositor's vsync, so its tick callback is the most accurate ~60 Hz
 *    signal available, and feeding it to the session lets the emulator run
 *    one frame per tick instead of racing its own timer against the panel's.
 *
 *  - Frames are pulled by serial, not pushed. copy_frame does nothing when
 *    the emulator has not produced a new frame, so a tick that arrives
 *    between frames costs a mutex and no memcpy.
 *
 * The PPU's frame is 256 x 240; the texture is rebuilt at the frame's own
 * height each time all the same, so nothing here assumes it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "display.h"

#include <string.h>

#define FB_W NESSESSION_FB_WIDTH
#define FB_MAX_H NESSESSION_FB_MAX_HEIGHT

struct _NESDisplay {
    GtkWidget parent_instance;

    nessession *session;
    guint32 *fb;           /* XRGB8888 straight from the session */
    guint32 *bgra;         /* with alpha forced opaque for GdkMemoryTexture */
    int height;
    GdkTexture *texture;
    guint64 serial;
    guint tick_id;
    gboolean tv_aspect;
    gboolean smooth;
};

G_DEFINE_FINAL_TYPE(NESDisplay, nes_display, GTK_TYPE_WIDGET)

static void rebuild_texture(NESDisplay *self)
{
    GBytes *bytes;
    int i, n = FB_W * self->height;

    /* The session's pixels are 0x00RRGGBB; GDK_MEMORY_B8G8R8A8 on a little-endian
     * host reads the same bytes as B,G,R,A -- only the alpha needs setting. */
    for (i = 0; i < n; i++)
        self->bgra[i] = self->fb[i] | 0xFF000000u;

    bytes = g_bytes_new_static(self->bgra, (gsize)n * 4);
    g_clear_object(&self->texture);
    self->texture = gdk_memory_texture_new(FB_W, self->height,
                                           GDK_MEMORY_B8G8R8A8, bytes,
                                           (gsize)FB_W * 4);
    g_bytes_unref(bytes);
}

static gboolean on_tick(GtkWidget *widget, GdkFrameClock *clock,
                        gpointer user_data)
{
    NESDisplay *self = NES_DISPLAY(widget);
    int h = 0;
    (void)user_data;

    /* Hand the emulator the compositor's cadence. */
    nessession_notify_vsync(self->session,
                              gdk_frame_clock_get_frame_time(clock) * 1000);

    if (nessession_copy_frame(self->session, self->fb, &h, &self->serial)) {
        if (h > 0 && h <= FB_MAX_H) {
            self->height = h;
            rebuild_texture(self);
            gtk_widget_queue_draw(widget);
        }
    }
    return G_SOURCE_CONTINUE;
}

static void nes_display_snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
    NESDisplay *self = NES_DISPLAY(widget);
    int w = gtk_widget_get_width(widget);
    int h = gtk_widget_get_height(widget);
    double want, sw, sh, x, y;
    graphene_rect_t rect;

    gtk_snapshot_append_color(snapshot, &(GdkRGBA){ 0, 0, 0, 1 },
                              &GRAPHENE_RECT_INIT(0, 0, w, h));
    if (!self->texture || w <= 0 || h <= 0 || self->height <= 0)
        return;

    /* An NTSC television showed each NES pixel 8:7 as wide as it is tall,
     * so the 256-pixel line spans 292 square pixels: TV mode shows the
     * frame at 292 x 240. Square pixels show the raw 256 x 240 buffer,
     * which is useful for pixel work and honest about nothing else. */
    want = (self->tv_aspect ? FB_W * 8.0 / 7.0 : (double)FB_W)
           / (double)self->height;

    if ((double)w / (double)h > want) {
        sh = h;
        sw = sh * want;
    } else {
        sw = w;
        sh = sw / want;
    }
    x = (w - sw) / 2.0;
    y = (h - sh) / 2.0;

    graphene_rect_init(&rect, (float)x, (float)y, (float)sw, (float)sh);
    gtk_snapshot_append_scaled_texture(
        snapshot, self->texture,
        self->smooth ? GSK_SCALING_FILTER_LINEAR : GSK_SCALING_FILTER_NEAREST,
        &rect);
}

static void nes_display_dispose(GObject *object)
{
    NESDisplay *self = NES_DISPLAY(object);

    if (self->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }
    g_clear_object(&self->texture);
    g_clear_pointer(&self->fb, g_free);
    g_clear_pointer(&self->bgra, g_free);

    G_OBJECT_CLASS(nes_display_parent_class)->dispose(object);
}

static void nes_display_class_init(NESDisplayClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = nes_display_dispose;
    GTK_WIDGET_CLASS(klass)->snapshot = nes_display_snapshot;
}

static void nes_display_init(NESDisplay *self)
{
    self->fb = g_new0(guint32, FB_W * FB_MAX_H);
    self->bgra = g_new0(guint32, FB_W * FB_MAX_H);
    self->tv_aspect = TRUE;
    self->smooth = FALSE;
    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
}

GtkWidget *nes_display_new(nessession *session)
{
    NESDisplay *self = g_object_new(NES_TYPE_DISPLAY, NULL);
    self->session = session;
    self->tick_id = gtk_widget_add_tick_callback(GTK_WIDGET(self), on_tick,
                                                 NULL, NULL);
    return GTK_WIDGET(self);
}

void nes_display_set_tv_aspect(NESDisplay *self, gboolean tv)
{
    self->tv_aspect = tv;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void nes_display_set_smooth(NESDisplay *self, gboolean smooth)
{
    self->smooth = smooth;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}
