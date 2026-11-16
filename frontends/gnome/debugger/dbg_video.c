/*
 * The debugger's Video tab (GTK4/cairo).
 *
 * Everything that decides what to show lives in the core (nesvidtab.h): this file is
 * widgets; the old PPU tab's panes (nametables, patterns, sprites, palette, OAM list) are its
 * Decoded view's sources and its Sprites section. The picture is drawn from the controller's
 * cached buffer (0xFFRRGGBB, drawn as CAIRO_FORMAT_RGB24 with a nearest filter), so a mouse
 * move redraws only the marks. Only the section showing is filled in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "dbg_video.h"

#include <string.h>

#include "nesvidcap.h"
#include "nesvidtab.h"

#define NSWATCH 32          /* palette RAM: 16 background entries, 16 sprite entries */
#define MAX_OVERLAYS 16

typedef struct {
    nesvid_tab *tab;
    nessession *session;
    GtkDropDown *palette;
    GtkDropDown *view, *source, *zoom;
    GtkWidget *brk_top, *brk_pix;
    GtkCheckButton *ovl[MAX_OVERLAYS];
    int nov;
    GtkLabel *status;
    GtkDrawingArea *pic;
    GtkWidget *pic_scroll;
    int vp_w, vp_h;
    GtkNotebook *sections;
    GtkLabel *pixel_text, *pixel_note;
    GtkTextView *regs_text, *lines_text, *sprites_text;
    GtkLabel *regs_note;
    char *regs_sig, *lines_sig, *sprites_sig;
    GtkDrawingArea *swatch_box[NSWATCH];
    GtkLabel *swatch_lbl[NSWATCH];
    GtkLabel *timing_text;
    GtkProgressBar *beam;
    gboolean updating, primed, was_mapped;
    guint tick_id;
    gboolean down;
} Vid;

static GtkWidget *mono_label(void)
{
    GtkWidget *lbl = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_label_set_selectable(GTK_LABEL(lbl), TRUE);
    gtk_widget_add_css_class(lbl, "monospace");
    return lbl;
}

static GtkWidget *padded_box(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    return box;
}

static void set_rgb(cairo_t *ctx, uint32_t rgb)
{
    cairo_set_source_rgb(ctx, ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0);
}

/* ---- the picture ---------------------------------------------------------- */

static void draw_message(cairo_t *ctx, int width, int height, const char *msg)
{
    cairo_text_extents_t ext;
    cairo_set_source_rgb(ctx, 0.65, 0.65, 0.68);
    cairo_select_font_face(ctx, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(ctx, 14);
    cairo_text_extents(ctx, msg, &ext);
    cairo_move_to(ctx, (width - ext.width) / 2 - ext.x_bearing, (height - ext.height) / 2 - ext.y_bearing);
    cairo_show_text(ctx, msg);
}

/* A pixel can be one device pixel wide: outline it from outside, dark under light. */
static void draw_mark(cairo_t *ctx, const int box[4], double red, double green, double blue)
{
    cairo_set_line_width(ctx, 3);
    cairo_set_source_rgba(ctx, 0, 0, 0, 0.85);
    cairo_rectangle(ctx, box[0] - 1.5, box[1] - 1.5, box[2] + 3, box[3] + 3);
    cairo_stroke(ctx);
    cairo_set_line_width(ctx, 1);
    cairo_set_source_rgb(ctx, red, green, blue);
    cairo_rectangle(ctx, box[0] - 1.5, box[1] - 1.5, box[2] + 3, box[3] + 3);
    cairo_stroke(ctx);
}

static void draw_pic(GtkDrawingArea *area, cairo_t *ctx, int width, int height, gpointer data)
{
    Vid *vid = data;
    int picw, pich, marks, hover[4] = { 0 }, pin[4] = { 0 };
    const uint32_t *pixels;
    (void)area;
    cairo_set_source_rgb(ctx, 0.07, 0.07, 0.09);
    cairo_paint(ctx);
    pixels = nesvid_tab_picture(vid->tab, &picw, &pich);
    if (!pixels) {
        const char *why = nesvid_tab_why(vid->tab);
        draw_message(ctx, width, height, why && *why ? why : "Nothing to show");
        return;
    }
    {
        cairo_surface_t *str = cairo_image_surface_create_for_data((unsigned char *)pixels, CAIRO_FORMAT_RGB24,
                                                                 picw, pich, picw * 4);
        cairo_set_source_surface(ctx, str, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(ctx), CAIRO_FILTER_NEAREST);
        cairo_paint(ctx);
        cairo_surface_destroy(str);
    }
    marks = nesvid_tab_marks(vid->tab, hover, pin);
    if (marks & 1) draw_mark(ctx, hover, 1, 1, 1);
    if (marks & 2) draw_mark(ctx, pin, 1.0, 0.85, 0.1);
}

static void fill_sprites(Vid *vid);

static void update_pixel_section(Vid *vid)
{
    const nesvid_pixel *pix = nesvid_tab_pixel(vid->tab);
    const gboolean can = nesvid_tab_can_break(vid->tab) != 0;
    gtk_widget_set_sensitive(vid->brk_top, can);
    gtk_widget_set_sensitive(vid->brk_pix, can);
    gtk_label_set_text(vid->pixel_text, pix ? pix->text
        : "Move over the picture to inspect a pixel.\nClick to pin it (click again to unpin);\n"
          "double-click to break on writes to what it is made from.");
    {   /* the swatches the inspected pixel uses are outlined */
        int index;
        for (index = 0; index < NSWATCH; index++)
            gtk_widget_queue_draw(GTK_WIDGET(vid->swatch_box[index]));
    }
    if (gtk_notebook_get_current_page(vid->sections) == NESVID_S_SPRITES) fill_sprites(vid);
}

static void do_break_pixel(Vid *vid)
{
    const nesvid_pixel *pix = nesvid_tab_pixel(vid->tab);
    int bpid = nesvid_tab_break_on_pixel(vid->tab);
    GString *msg = g_string_new(NULL);
    if (bpid >= 0 && pix && pix->nbreak > 0) {
        static const char *const spaces[] = { "the CPU bus", "PPU memory", "palette RAM", "OAM" };
        int idx;
        g_string_append(msg, "Break on writes to");
        for (idx = 0; idx < pix->nbreak; idx++) {
            const int space = pix->break_space[idx];
            g_string_append_printf(msg, "%s %s $%04X", idx ? ";" : "",
                                   space >= 0 && space < 4 ? spaces[space] : "memory", pix->break_lo[idx]);
            if (pix->break_hi[idx] != pix->break_lo[idx])
                g_string_append_printf(msg, "-$%04X", pix->break_hi[idx]);
        }
    } else {
        g_string_append(msg, "Nothing to break on (inspect a tile, a sprite, a palette entry or a band edge, "
                             "not the backdrop)");
    }
    /* the message lives in the Pixel section: a line under the picture would take room from
     * Fit and change the zoom */
    gtk_label_set_text(vid->pixel_note, msg->str);
    g_string_free(msg, TRUE);
    gtk_notebook_set_current_page(vid->sections, NESVID_S_PIXEL);
}

static void on_motion(GtkEventControllerMotion *ctrl, double posx, double posy, gpointer data)
{
    Vid *vid = data;
    (void)ctrl;
    if (nesvid_tab_hover(vid->tab, (int)posx, (int)posy)) {
        gtk_widget_queue_draw(GTK_WIDGET(vid->pic));
        update_pixel_section(vid);
    }
}

static void on_leave(GtkEventControllerMotion *ctrl, gpointer data)
{
    Vid *vid = data;
    (void)ctrl;
    if (nesvid_tab_hover(vid->tab, -1, -1)) {
        gtk_widget_queue_draw(GTK_WIDGET(vid->pic));
        update_pixel_section(vid);
    }
}

static void on_click(GtkGestureClick *gest, int clicks, double posx, double posy, gpointer data)
{
    Vid *vid = data;
    (void)gest;
    if (clicks == 1) {
        nesvid_tab_pin(vid->tab, (int)posx, (int)posy);
    } else if (clicks == 2) {
        /* the first click of the pair toggled the pin; the second restores it */
        nesvid_tab_hover(vid->tab, (int)posx, (int)posy);
        nesvid_tab_pin(vid->tab, (int)posx, (int)posy);
        do_break_pixel(vid);
    }
    gtk_widget_queue_draw(GTK_WIDGET(vid->pic));
    update_pixel_section(vid);
}

/* ---- text tables ------------------------------------------------------------- */

/* Replace first text view'str contents when they (or the highlighted rows) changed;
 * the scroll position stays put. lit[idx] != 0 highlights line idx. */
static void put_table(GtkTextView *view, char **sigp, GString *text, const GString *lit)
{
    GString *sig = g_string_new(NULL);
    g_string_append_len(sig, lit->str, (gssize)lit->len);
    g_string_append_c(sig, '|');
    g_string_append_len(sig, text->str, (gssize)text->len);
    if (!*sigp || strcmp(*sigp, sig->str)) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(view);
        GtkAdjustment *adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(view));
        double pos = adj ? gtk_adjustment_get_value(adj) : 0;
        char *clean = g_utf8_make_valid(text->str, -1);
        gsize idx;
        gtk_text_buffer_set_text(buf, clean, -1);
        g_free(clean);
        for (idx = 0; idx < lit->len; idx++) {
            if (lit->str[idx] == '1') {
                GtkTextIter first, last;
                gtk_text_buffer_get_iter_at_line(buf, &first, (int)idx);
                last = first;
                gtk_text_iter_forward_to_line_end(&last);
                gtk_text_buffer_apply_tag_by_name(buf, "changed", &first, &last);
            }
        }
        if (adj) gtk_adjustment_set_value(adj, pos);
        g_free(*sigp);
        *sigp = g_strdup(sig->str);
    }
    g_string_free(sig, TRUE);
}

static void fill_regs(Vid *vid)
{
    const nesvid_reg *rec;
    int num = nesvid_tab_regs(vid->tab, &rec), idx;
    GString *str = g_string_new(NULL), *lit = g_string_new(NULL);
    g_string_append(str, "Register     Value    Writes  Meaning\n");
    g_string_append_c(lit, '0');
    for (idx = 0; idx < num; idx++) {
        char writes[24];
        if (rec[idx].writes > 0) g_snprintf(writes, sizeof writes, "%dx@%d", rec[idx].writes, rec[idx].first_line);
        else g_strlcpy(writes, "-", sizeof writes);
        g_string_append_printf(str, "%-12s %-8s %-7s %s\n", rec[idx].name, rec[idx].value, writes, rec[idx].meaning);
        g_string_append_c(lit, (rec[idx].age < NESVID_REG_FADE || rec[idx].writes > 0) ? '1' : '0');
    }
    if (!num) g_string_append(str, "The machine is not running.\n");
    put_table(vid->regs_text, &vid->regs_sig, str, lit);
    g_string_free(str, TRUE);
    g_string_free(lit, TRUE);
}

static void fill_lines(Vid *vid)
{
    const nesvid_band *rec;
    int num = nesvid_tab_lines(vid->tab, &rec), idx;
    GString *str = g_string_new(NULL), *lit = g_string_new(NULL);
    g_string_append(str, "Bands of display lines drawn from the same registers\n");
    g_string_append_c(lit, '0');
    for (idx = 0; idx < num; idx++) {
        g_string_append_printf(str, "%s\n", rec[idx].text);
        g_string_append_c(lit, rec[idx].writes > 0 ? '1' : '0');
    }
    if (!num) g_string_append(str, "The machine is not running.\n");
    put_table(vid->lines_text, &vid->lines_sig, str, lit);
    g_string_free(str, TRUE);
    g_string_free(lit, TRUE);
}

/* ---- colours ------------------------------------------------------------------ */

static void draw_swatch(GtkDrawingArea *area, cairo_t *ctx, int width, int height, gpointer data)
{
    Vid *vid = data;
    const nesvid_swatch *swatch;
    int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(area), "idx"));
    (void)width; (void)height;
    if (nesvid_tab_swatches(vid->tab, &swatch) <= index) return;
    set_rgb(ctx, swatch[index].rgb);
    cairo_rectangle(ctx, 0, 0, 22, 22);
    cairo_fill(ctx);
    cairo_set_source_rgba(ctx, 0.5, 0.5, 0.5, 0.8);
    cairo_set_line_width(ctx, 1);
    cairo_rectangle(ctx, 0.5, 0.5, 21, 21);
    cairo_stroke(ctx);
    if (swatch[index].inspected) {      /* the inspected pixel uses this colour */
        cairo_set_source_rgb(ctx, 1, 0.85, 0.1);
        cairo_set_line_width(ctx, 2);
        cairo_rectangle(ctx, 1, 1, 20, 20);
        cairo_stroke(ctx);
    }
}

static void fill_colours(Vid *vid)
{
    const nesvid_swatch *swatch;
    int num = nesvid_tab_swatches(vid->tab, &swatch), index;
    for (index = 0; index < NSWATCH; index++) {
        char buf[200];
        if (index < num)
            g_snprintf(buf, sizeof buf, "%2d  %-11s $%02X #%06X %7ld px  %s", swatch[index].slot,
                       swatch[index].name, swatch[index].colour, swatch[index].rgb & 0xffffff,
                       swatch[index].uses, swatch[index].role);
        else
            g_snprintf(buf, sizeof buf, "-");
        gtk_label_set_text(vid->swatch_lbl[index], buf);
        gtk_widget_queue_draw(GTK_WIDGET(vid->swatch_box[index]));
    }
}

static void fill_timing(Vid *vid)
{
    const char *const *lines;
    int num = nesvid_tab_timing(vid->tab, &lines), idx;
    GString *str = g_string_new(NULL);
    for (idx = 0; idx < num; idx++) {
        const char *tab = strchr(lines[idx], '\t');
        if (tab) g_string_append_printf(str, "%-14.*s %s\n", (int)(tab - lines[idx]), lines[idx], tab + 1);
        else g_string_append_printf(str, "%s\n", lines[idx]);
    }
    gtk_label_set_text(vid->timing_text, str->len ? str->str : "The machine is not running.");
    g_string_free(str, TRUE);
    gtk_progress_bar_set_fraction(vid->beam, nesvid_tab_beam_permille(vid->tab) / 1000.0);
}

/* The 64 OAM entries; the one under the inspected pixel is lit. */
static void fill_sprites(Vid *vid)
{
    const nesvid_spriterow *rec;
    const nesvid_pixel *pix = nesvid_tab_pixel(vid->tab);
    int num = nesvid_tab_sprites(vid->tab, &rec), idx;
    GString *str = g_string_new(NULL), *lit = g_string_new(NULL);
    g_string_append(str, "Sprites in OAM (those marked * are inside the picture)\n");
    g_string_append_c(lit, '0');
    for (idx = 0; idx < num; idx++) {
        g_string_append_printf(str, "%c %s\n", rec[idx].visible ? '*' : ' ', rec[idx].text);
        g_string_append_c(lit, pix && pix->sprite == rec[idx].index ? '1' : '0');
    }
    if (!num) g_string_append(str, "The machine is not running.\n");
    put_table(vid->sprites_text, &vid->sprites_sig, str, lit);
    g_string_free(str, TRUE);
    g_string_free(lit, TRUE);
}

/* ---- refresh ------------------------------------------------------------------- */

static void fill_section(Vid *vid, int sec)
{
    switch (sec) {
    case NESVID_S_REGISTERS: fill_regs(vid); break;
    case NESVID_S_COLOURS: fill_colours(vid); break;
    case NESVID_S_LINES: fill_lines(vid); break;
    case NESVID_S_TIMING: fill_timing(vid); break;
    case NESVID_S_SPRITES: fill_sprites(vid); break;
    default: update_pixel_section(vid); break;
    }
}

static void refresh_view(Vid *vid, unsigned what)
{
    if (what & (NESVID_TAB_PICTURE | NESVID_TAB_OFF)) {
        int picw = 0, pich = 0;
        if (nesvid_tab_picture(vid->tab, &picw, &pich)) {
            gtk_drawing_area_set_content_width(vid->pic, picw);
            gtk_drawing_area_set_content_height(vid->pic, pich);
        } else {
            gtk_drawing_area_set_content_width(vid->pic, vid->vp_w > 4 ? vid->vp_w : 480);
            gtk_drawing_area_set_content_height(vid->pic, vid->vp_h > 4 ? vid->vp_h : 300);
        }
        gtk_widget_queue_draw(GTK_WIDGET(vid->pic));
    }
    if (what & NESVID_TAB_TABLES) {
        gtk_label_set_text(vid->status, nesvid_tab_status(vid->tab));
        fill_section(vid, gtk_notebook_get_current_page(vid->sections));
        if (gtk_notebook_get_current_page(vid->sections) != NESVID_S_PIXEL)
            update_pixel_section(vid);    /* the Break on writes buttons follow the pixel */
    }
}

static void sync_viewport(Vid *vid)
{
    int wide = gtk_widget_get_width(vid->pic_scroll), high = gtk_widget_get_height(vid->pic_scroll);
    if (wide > 4 && high > 4 && (wide != vid->vp_w || high != vid->vp_h)) {
        vid->vp_w = wide;
        vid->vp_h = high;
        nesvid_tab_set_viewport(vid->tab, wide - 2, high - 2);
    }
}

void dbg_video_refresh(GtkWidget *page, gboolean force)
{
    Vid *vid = g_object_get_data(G_OBJECT(page), "vid");
    gboolean mapped;
    unsigned what;
    if (!vid || vid->down) return;
    mapped = gtk_widget_get_mapped(page);
    if (mapped && !vid->was_mapped) force = TRUE;      /* just shown */
    vid->was_mapped = mapped;
    if (!vid->primed) force = TRUE;
    vid->primed = TRUE;
    sync_viewport(vid);
    what = nesvid_tab_refresh(vid->tab, force);
    if (force) what |= NESVID_TAB_PICTURE | NESVID_TAB_TABLES;
    refresh_view(vid, what);
}

/* The picture's room changed (window resized, split dragged): Fit follows it. */
static gboolean on_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
    Vid *vid = data;
    int picw = gtk_widget_get_width(widget), pich = gtk_widget_get_height(widget);
    (void)clock;
    if (picw > 4 && pich > 4 && (picw != vid->vp_w || pich != vid->vp_h)) {
        sync_viewport(vid);
        if (vid->primed && nesvid_tab_zoom(vid->tab) == 0)
            refresh_view(vid, nesvid_tab_refresh(vid->tab, TRUE) | NESVID_TAB_PICTURE);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- controls ------------------------------------------------------------------- */

static void refresh_all(Vid *vid)
{
    refresh_view(vid, nesvid_tab_refresh(vid->tab, TRUE) | NESVID_TAB_PICTURE | NESVID_TAB_TABLES);
}

static void on_view(GObject *obj, GParamSpec *spec, gpointer data)
{
    Vid *vid = data;
    (void)obj; (void)spec;
    if (vid->updating) return;
    nesvid_tab_set_view(vid->tab, (int)gtk_drop_down_get_selected(vid->view));
    refresh_all(vid);
}

static void on_source(GObject *obj, GParamSpec *spec, gpointer data)
{
    Vid *vid = data;
    (void)obj; (void)spec;
    if (vid->updating) return;
    nesvid_tab_set_source(vid->tab, (int)gtk_drop_down_get_selected(vid->source));
    refresh_all(vid);
}

static void on_zoom(GObject *obj, GParamSpec *spec, gpointer data)
{
    Vid *vid = data;
    int num;
    const int *zoomval = nesvid_zooms(&num);
    guint idx = gtk_drop_down_get_selected(vid->zoom);
    (void)obj; (void)spec;
    if (vid->updating || (int)idx >= num) return;
    sync_viewport(vid);
    nesvid_tab_set_zoom(vid->tab, zoomval[idx]);
    refresh_all(vid);
}

static void on_palette(GObject *obj, GParamSpec *spec, gpointer data)
{
    Vid *vid = data;
    (void)obj; (void)spec;
    if (vid->updating) return;
    nesvid_tab_set_palette(vid->tab, (int)gtk_drop_down_get_selected(vid->palette));
    refresh_all(vid);
}

static void on_overlay(GtkCheckButton *btn, gpointer data)
{
    Vid *vid = data;
    unsigned mask = 0;
    int num, idx;
    const nesvid_overlay_info *info = nesvid_overlays(&num);
    (void)btn;
    if (vid->updating) return;
    for (idx = 0; idx < num && idx < vid->nov; idx++)
        if (gtk_check_button_get_active(vid->ovl[idx])) mask |= info[idx].bit;
    nesvid_tab_set_overlays(vid->tab, mask);
    refresh_all(vid);
}

static void on_unpin(GtkButton *btn, gpointer data)
{
    Vid *vid = data;
    (void)btn;
    nesvid_tab_unpin(vid->tab);
    gtk_widget_queue_draw(GTK_WIDGET(vid->pic));
    update_pixel_section(vid);
}

static void on_break(GtkButton *btn, gpointer data)
{
    (void)btn;
    do_break_pixel(data);
}

static void on_break_register(GtkButton *btn, gpointer data)
{
    Vid *vid = data;
    static const char *const names[] = { "PPUCTRL", "PPUMASK", "PPUSTATUS", "OAMADDR", "PPUSCROLL", "PPUADDR", "" };
    const int row = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "row"));
    char buf[160];
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
    if (nesvid_tab_break_on_register(vid->tab, row) >= 0)
        g_snprintf(buf, sizeof buf, "Breaking when the CPU writes %s (see Breakpoints).", names[row]);
    else
        g_snprintf(buf, sizeof buf, "The breakpoint was refused.");
    gtk_label_set_text(vid->regs_note, buf);
}

static void on_section(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer data)
{
    Vid *vid = data;
    (void)notebook; (void)page;
    fill_section(vid, (int)page_num);
}

/* Taking the page apart re-selects the sections' pages and resets the
 * choosers: the window calls this first, while every widget is still alive,
 * so nothing calls back into the page from then on. */
void dbg_video_shutdown(GtkWidget *page)
{
    Vid *vid = g_object_get_data(G_OBJECT(page), "vid");
    int idx;
    if (!vid || vid->down) return;
    vid->down = TRUE;
    vid->updating = TRUE;
    if (vid->tick_id) gtk_widget_remove_tick_callback(vid->pic_scroll, vid->tick_id);
    vid->tick_id = 0;
    g_signal_handlers_disconnect_by_data(vid->sections, vid);
    g_signal_handlers_disconnect_by_data(vid->view, vid);
    g_signal_handlers_disconnect_by_data(vid->source, vid);
    g_signal_handlers_disconnect_by_data(vid->zoom, vid);
    g_signal_handlers_disconnect_by_data(vid->palette, vid);
    for (idx = 0; idx < vid->nov; idx++)
        g_signal_handlers_disconnect_by_data(vid->ovl[idx], vid);
}

static void on_destroy(GtkWidget *widget, gpointer data)
{
    Vid *vid = data;
    (void)widget;
    nesvid_tab_free(vid->tab);
    g_free(vid->sprites_sig);
    g_free(vid->regs_sig);
    g_free(vid->lines_sig);
    g_free(vid);
}

GtkWidget *dbg_video_new(nessession *session)
{
    Vid *vid = g_new0(Vid, 1);
    GtkWidget *root, *left, *bar, *right, *box, *btns, *brkbtn, *unpbtn, *scroll, *ovbtn, *pop, *ovbox, *grid, *brk;
    GtkEventController *motion;
    GtkGesture *click;
    GtkTextBuffer *buf;
    const nesvid_overlay_info *ovinfo;
    const int *zoomv;
    const char *views[NESVID_V_COUNT + 1], *zlabels[24];
    char zbuf[24][8];
    int num, nzoom, idx;

    vid->session = session;
    vid->tab = nesvid_tab_new(session);

    root = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    g_object_set_data(G_OBJECT(root), "vid", vid);
    g_signal_connect(root, "destroy", G_CALLBACK(on_destroy), vid);

    /* ---- left: toolbar, status, picture */
    left = padded_box();
    bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    for (idx = 0; idx < NESVID_V_COUNT; idx++) views[idx] = nesvid_view_name(idx);
    views[NESVID_V_COUNT] = NULL;
    vid->view = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(views));
    gtk_widget_set_tooltip_text(GTK_WIDGET(vid->view),
        "Display: the last finished frame, as the main window shows it.\n"
        "Decoded: one source rebuilt from the PPU's memory (see Source).\n"
        "Beam: the frame as the PPU is drawing it, the lines the beam has not reached dimmed (stopped).\n"
        "Values: each nametable entry as a flat false colour of its value.");
    {
        const char *sources[NESVID_SRC_COUNT + 1];
        int sidx;
        for (sidx = 0; sidx < NESVID_SRC_COUNT; sidx++) sources[sidx] = nesvid_source_name(sidx);
        sources[NESVID_SRC_COUNT] = NULL;
        vid->source = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(sources));
        gtk_widget_set_tooltip_text(GTK_WIDGET(vid->source),
            "What the Decoded view redraws: the four nametables with the scroll windows, "
            "the two pattern tables, the 64 sprites, or the palette.");
    }
    zoomv = nesvid_zooms(&nzoom);
    if (nzoom > 23) nzoom = 23;
    for (idx = 0; idx < nzoom; idx++) {
        if (zoomv[idx] == 0) g_strlcpy(zbuf[idx], "Fit", sizeof zbuf[idx]);
        else g_snprintf(zbuf[idx], sizeof zbuf[idx], "%d×", zoomv[idx]);
        zlabels[idx] = zbuf[idx];
    }
    zlabels[nzoom] = NULL;
    vid->zoom = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(zlabels));
    gtk_box_append(GTK_BOX(bar), gtk_label_new("View"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(vid->view));
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Source"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(vid->source));
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Zoom"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(vid->zoom));
    {
        static const char *const palettes[] = { "BG 0", "BG 1", "BG 2", "BG 3",
                                                "Sprite 0", "Sprite 1", "Sprite 2", "Sprite 3", NULL };
        vid->palette = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(palettes));
        gtk_widget_set_tooltip_text(GTK_WIDGET(vid->palette),
            "The palette the Patterns source draws in: four background, four sprite");
        gtk_box_append(GTK_BOX(bar), gtk_label_new("Palette"));
        gtk_box_append(GTK_BOX(bar), GTK_WIDGET(vid->palette));
    }

    ovinfo = nesvid_overlays(&num);
    if (num > MAX_OVERLAYS) num = MAX_OVERLAYS;
    vid->nov = num;
    ovbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    for (idx = 0; idx < num; idx++) {
        vid->ovl[idx] = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(ovinfo[idx].label));
        g_signal_connect(vid->ovl[idx], "toggled", G_CALLBACK(on_overlay), vid);
        gtk_box_append(GTK_BOX(ovbox), GTK_WIDGET(vid->ovl[idx]));
    }
    pop = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(pop), ovbox);
    ovbtn = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(ovbtn), "Overlays");
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(ovbtn), pop);
    gtk_box_append(GTK_BOX(bar), ovbtn);

    brk = gtk_button_new_with_label("Break on writes");
    gtk_widget_set_tooltip_text(brk, "Break when the CPU writes what the inspected pixel is made from: "
                                     "its nametable entry, attribute or pattern bytes, its palette entries "
                                     "or its sprite bytes (or, at a band edge, the PPU register that changed there)");
    vid->brk_top = brk;
    g_signal_connect(brk, "clicked", G_CALLBACK(on_break), vid);
    gtk_box_append(GTK_BOX(bar), brk);
    gtk_box_append(GTK_BOX(left), bar);

    vid->status = GTK_LABEL(mono_label());
    gtk_label_set_selectable(vid->status, FALSE);
    gtk_label_set_ellipsize(vid->status, PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(left), GTK_WIDGET(vid->status));

    vid->pic = GTK_DRAWING_AREA(gtk_drawing_area_new());
    gtk_drawing_area_set_draw_func(vid->pic, draw_pic, vid, NULL);
    gtk_drawing_area_set_content_width(vid->pic, 480);
    gtk_drawing_area_set_content_height(vid->pic, 300);
    gtk_widget_set_halign(GTK_WIDGET(vid->pic), GTK_ALIGN_START);
    gtk_widget_set_valign(GTK_WIDGET(vid->pic), GTK_ALIGN_START);
    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), vid);
    g_signal_connect(motion, "leave", G_CALLBACK(on_leave), vid);
    gtk_widget_add_controller(GTK_WIDGET(vid->pic), motion);
    click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_click), vid);
    gtk_widget_add_controller(GTK_WIDGET(vid->pic), GTK_EVENT_CONTROLLER(click));
    scroll = gtk_scrolled_window_new();
    vid->pic_scroll = scroll;
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(vid->pic));
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);
    vid->tick_id = gtk_widget_add_tick_callback(scroll, on_tick, vid, NULL);
    gtk_box_append(GTK_BOX(left), scroll);


    /* ---- right: the six sections, in nesvid_section_name order */
    right = gtk_notebook_new();
    vid->sections = GTK_NOTEBOOK(right);
    gtk_notebook_set_scrollable(vid->sections, TRUE);
    gtk_widget_set_size_request(right, 400, -1);

    /* Pixel */
    box = padded_box();
    vid->pixel_text = GTK_LABEL(mono_label());
    gtk_label_set_wrap(vid->pixel_text, TRUE);
    gtk_label_set_wrap_mode(vid->pixel_text, PANGO_WRAP_WORD_CHAR);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(vid->pixel_text));
    vid->pixel_note = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(vid->pixel_note, 0);
    gtk_label_set_wrap(vid->pixel_note, TRUE);
    gtk_label_set_wrap_mode(vid->pixel_note, PANGO_WRAP_WORD_CHAR);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(vid->pixel_note));
    btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    brkbtn = gtk_button_new_with_label("Break on writes");
    vid->brk_pix = brkbtn;
    unpbtn = gtk_button_new_with_label("Unpin");
    g_signal_connect(brkbtn, "clicked", G_CALLBACK(on_break), vid);
    g_signal_connect(unpbtn, "clicked", G_CALLBACK(on_unpin), vid);
    gtk_box_append(GTK_BOX(btns), brkbtn);
    gtk_box_append(GTK_BOX(btns), unpbtn);
    gtk_box_append(GTK_BOX(box), btns);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    gtk_notebook_append_page(vid->sections, scroll, gtk_label_new(nesvid_section_name(NESVID_S_PIXEL)));

    /* Registers */
    vid->regs_text = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(vid->regs_text, FALSE);
    gtk_text_view_set_cursor_visible(vid->regs_text, FALSE);
    gtk_text_view_set_monospace(vid->regs_text, TRUE);
    gtk_text_view_set_wrap_mode(vid->regs_text, GTK_WRAP_NONE);   /* the columns stay lined up; it scrolls */
    buf = gtk_text_view_get_buffer(vid->regs_text);
    gtk_text_buffer_create_tag(buf, "changed", "background", "#C8B58C", "foreground", "#000000",
                               "weight", 700, NULL);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(vid->regs_text));
    gtk_widget_set_vexpand(scroll, TRUE);
    {
        static const struct { const char *name; int row; } regbreaks[] = {
            { "PPUCTRL ($2000)", 0 }, { "PPUMASK ($2001)", 1 }, { "OAMADDR ($2003)", 3 },
            { "PPUSCROLL ($2005)", 4 }, { "PPUADDR ($2006)", 5 } };
        GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        GtkWidget *rbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *rmenu = gtk_menu_button_new(), *rpop = gtk_popover_new();
        GtkWidget *rlist = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        size_t reg;
        vid->regs_note = GTK_LABEL(gtk_label_new(""));
        gtk_label_set_xalign(vid->regs_note, 0);
        gtk_label_set_ellipsize(vid->regs_note, PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(GTK_WIDGET(vid->regs_note), "dim-label");
        for (reg = 0; reg < G_N_ELEMENTS(regbreaks); reg++) {
            GtkWidget *btn = gtk_button_new_with_label(regbreaks[reg].name);
            gtk_widget_add_css_class(btn, "flat");
            g_object_set_data(G_OBJECT(btn), "row", GINT_TO_POINTER(regbreaks[reg].row));
            g_signal_connect(btn, "clicked", G_CALLBACK(on_break_register), vid);
            gtk_box_append(GTK_BOX(rlist), btn);
        }
        gtk_popover_set_child(GTK_POPOVER(rpop), rlist);
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(rmenu), rpop);
        gtk_menu_button_set_label(GTK_MENU_BUTTON(rmenu), "Break on writes to\xe2\x80\xa6");
        gtk_box_append(GTK_BOX(rbar), rmenu);
        gtk_box_append(GTK_BOX(page), scroll);
        gtk_box_append(GTK_BOX(page), rbar);
        gtk_box_append(GTK_BOX(page), GTK_WIDGET(vid->regs_note));
        gtk_notebook_append_page(vid->sections, page, gtk_label_new(nesvid_section_name(NESVID_S_REGISTERS)));
    }

    /* Colours: the 32 palette RAM entries, background then sprites */
    box = padded_box();
    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 3);
    for (idx = 0; idx < NSWATCH; idx++) {
        vid->swatch_box[idx] = GTK_DRAWING_AREA(gtk_drawing_area_new());
        gtk_drawing_area_set_content_width(vid->swatch_box[idx], 22);
        gtk_drawing_area_set_content_height(vid->swatch_box[idx], 22);
        g_object_set_data(G_OBJECT(vid->swatch_box[idx]), "idx", GINT_TO_POINTER(idx));
        gtk_drawing_area_set_draw_func(vid->swatch_box[idx], draw_swatch, vid, NULL);
        vid->swatch_lbl[idx] = GTK_LABEL(mono_label());
        gtk_label_set_selectable(vid->swatch_lbl[idx], FALSE);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(vid->swatch_box[idx]), 0, idx, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(vid->swatch_lbl[idx]), 1, idx, 1, 1);
    }
    gtk_box_append(GTK_BOX(box), grid);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    gtk_notebook_append_page(vid->sections, scroll, gtk_label_new(nesvid_section_name(NESVID_S_COLOURS)));

    /* Lines */
    vid->lines_text = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(vid->lines_text, FALSE);
    gtk_text_view_set_cursor_visible(vid->lines_text, FALSE);
    gtk_text_view_set_monospace(vid->lines_text, TRUE);
    buf = gtk_text_view_get_buffer(vid->lines_text);
    gtk_text_buffer_create_tag(buf, "changed", "background", "#C8B58C", "foreground", "#000000",
                               "weight", 700, NULL);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(vid->lines_text));
    gtk_notebook_append_page(vid->sections, scroll, gtk_label_new(nesvid_section_name(NESVID_S_LINES)));

    /* Timing */
    box = padded_box();
    vid->beam = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_progress_bar_set_text(vid->beam, "beam");
    gtk_progress_bar_set_show_text(vid->beam, TRUE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(vid->beam));
    vid->timing_text = GTK_LABEL(mono_label());
    gtk_label_set_selectable(vid->timing_text, FALSE);
    gtk_label_set_wrap(vid->timing_text, TRUE);
    gtk_label_set_wrap_mode(vid->timing_text, PANGO_WRAP_WORD_CHAR);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(vid->timing_text));
    gtk_notebook_append_page(vid->sections, box, gtk_label_new(nesvid_section_name(NESVID_S_TIMING)));

    /* Sprites: the 64 OAM entries */
    vid->sprites_text = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(vid->sprites_text, FALSE);
    gtk_text_view_set_cursor_visible(vid->sprites_text, FALSE);
    gtk_text_view_set_monospace(vid->sprites_text, TRUE);
    gtk_text_view_set_wrap_mode(vid->sprites_text, GTK_WRAP_NONE);
    buf = gtk_text_view_get_buffer(vid->sprites_text);
    gtk_text_buffer_create_tag(buf, "changed", "background", "#C8B58C", "foreground", "#000000",
                               "weight", 700, NULL);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(vid->sprites_text));
    gtk_notebook_append_page(vid->sections, scroll, gtk_label_new(nesvid_section_name(NESVID_S_SPRITES)));

    gtk_paned_set_start_child(GTK_PANED(root), left);
    gtk_paned_set_end_child(GTK_PANED(root), right);
    gtk_paned_set_resize_start_child(GTK_PANED(root), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(root), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(root), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(root), FALSE);

    /* start from the controller's state (the NES_DEBUGGER_VIDEO_* variables) */
    vid->updating = TRUE;
    gtk_drop_down_set_selected(vid->view, (guint)nesvid_tab_view(vid->tab));
    gtk_drop_down_set_selected(vid->source, (guint)nesvid_tab_source(vid->tab));
    gtk_drop_down_set_selected(vid->palette, (guint)nesvid_tab_palette(vid->tab));
    for (idx = 0; idx < nzoom; idx++)
        if (zoomv[idx] == nesvid_tab_zoom(vid->tab)) gtk_drop_down_set_selected(vid->zoom, (guint)idx);
    for (idx = 0; idx < vid->nov; idx++)
        gtk_check_button_set_active(vid->ovl[idx], (nesvid_tab_overlays(vid->tab) & ovinfo[idx].bit) != 0);
    gtk_notebook_set_current_page(vid->sections, nesvid_tab_env_section(vid->tab));
    vid->updating = FALSE;
    update_pixel_section(vid);
    g_signal_connect(vid->view, "notify::selected", G_CALLBACK(on_view), vid);
    g_signal_connect(vid->source, "notify::selected", G_CALLBACK(on_source), vid);
    g_signal_connect(vid->palette, "notify::selected", G_CALLBACK(on_palette), vid);
    g_signal_connect(vid->zoom, "notify::selected", G_CALLBACK(on_zoom), vid);
    g_signal_connect(vid->sections, "switch-page", G_CALLBACK(on_section), vid);
    return root;
}
