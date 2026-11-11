/*
 * The debugger's Video tab (GTK4/cairo).
 *
 * Everything that decides what to show lives in the core (a7800vidtab.h): this file is
 * widgets. The picture is drawn from the controller's cached buffer (0xFFRRGGBB, drawn as
 * CAIRO_FORMAT_RGB24 with a nearest filter), so a mouse move redraws only the marks. Only the
 * section showing is filled in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "dbg_video.h"

#include <string.h>

#include "a7800vidtab.h"

#define NSWATCH 32          /* MARIA palette registers $20-$3F */
#define MAX_OVERLAYS 16

typedef struct {
    a7800vid_tab *tab;
    GtkDropDown *view, *source, *zoom;
    GtkCheckButton *ov[MAX_OVERLAYS];
    int nov;
    GtkLabel *status, *note;
    GtkDrawingArea *pic;
    GtkWidget *pic_scroll;
    int vp_w, vp_h;
    GtkNotebook *sections;
    GtkLabel *pixel_text, *pixel_note;
    GtkTextView *regs_text, *lines_text;
    char *regs_sig, *lines_sig;
    GtkDrawingArea *swatch_box[NSWATCH];
    GtkLabel *swatch_lbl[NSWATCH];
    GtkLabel *timing_text;
    GtkProgressBar *beam;
    gboolean updating, primed, was_mapped;
} Vid;

static GtkWidget *mono_label(void)
{
    GtkWidget *l = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_widget_add_css_class(l, "monospace");
    return l;
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

static void set_rgb(cairo_t *cr, uint32_t rgb)
{
    cairo_set_source_rgb(cr, ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0);
}

/* ---- the picture ---------------------------------------------------------- */

static void draw_message(cairo_t *cr, int w, int h, const char *msg)
{
    cairo_text_extents_t ext;
    cairo_set_source_rgb(cr, 0.65, 0.65, 0.68);
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 14);
    cairo_text_extents(cr, msg, &ext);
    cairo_move_to(cr, (w - ext.width) / 2 - ext.x_bearing, (h - ext.height) / 2 - ext.y_bearing);
    cairo_show_text(cr, msg);
}

/* A pixel can be one device pixel wide: outline it from outside, dark under light. */
static void draw_mark(cairo_t *cr, const int r[4], double red, double green, double blue)
{
    cairo_set_line_width(cr, 3);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.85);
    cairo_rectangle(cr, r[0] - 1.5, r[1] - 1.5, r[2] + 3, r[3] + 3);
    cairo_stroke(cr);
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgb(cr, red, green, blue);
    cairo_rectangle(cr, r[0] - 1.5, r[1] - 1.5, r[2] + 3, r[3] + 3);
    cairo_stroke(cr);
}

static void draw_pic(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer ud)
{
    Vid *v = ud;
    int pw, ph, marks, hover[4] = { 0 }, pin[4] = { 0 };
    const uint32_t *px;
    (void)area;
    cairo_set_source_rgb(cr, 0.07, 0.07, 0.09);
    cairo_paint(cr);
    px = a7800vid_tab_picture(v->tab, &pw, &ph);
    if (!px) {
        const char *why = a7800vid_tab_why(v->tab);
        draw_message(cr, w, h, why && *why ? why : "Nothing to show");
        return;
    }
    {
        cairo_surface_t *s = cairo_image_surface_create_for_data((unsigned char *)px, CAIRO_FORMAT_RGB24,
                                                                 pw, ph, pw * 4);
        cairo_set_source_surface(cr, s, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
        cairo_paint(cr);
        cairo_surface_destroy(s);
    }
    marks = a7800vid_tab_marks(v->tab, hover, pin);
    if (marks & 1) draw_mark(cr, hover, 1, 1, 1);
    if (marks & 2) draw_mark(cr, pin, 1.0, 0.85, 0.1);
}

static void update_pixel_section(Vid *v)
{
    const a7800vid_pixel *p = a7800vid_tab_pixel(v->tab);
    gtk_label_set_text(v->pixel_text, p ? p->text
        : "Move over the picture to inspect a pixel.\nClick to pin it (click again to unpin);\n"
          "double-click to break on writes to it.");
    {   /* the swatch the inspected pixel uses is outlined */
        int index;
        for (index = 0; index < NSWATCH; index++)
            gtk_widget_queue_draw(GTK_WIDGET(v->swatch_box[index]));
    }
}

static void do_break_pixel(Vid *v)
{
    const a7800vid_pixel *p = a7800vid_tab_pixel(v->tab);
    int id = a7800vid_tab_break_on_pixel(v->tab);
    char buf[160];
    if (id >= 0 && p)
        g_snprintf(buf, sizeof buf, "Break on writes to the pixel's object headers, its DLL entry $%04X "
                   "and palette register $%02X", p->dll, 0x20 + p->reg_index);
    else
        g_snprintf(buf, sizeof buf, "Nothing to break on (inspect a pixel of a row MARIA drew)");
    gtk_label_set_text(v->note, buf);
    gtk_label_set_text(v->pixel_note, buf);
}

static void on_motion(GtkEventControllerMotion *c, double x, double y, gpointer ud)
{
    Vid *v = ud;
    (void)c;
    if (a7800vid_tab_hover(v->tab, (int)x, (int)y)) {
        gtk_widget_queue_draw(GTK_WIDGET(v->pic));
        update_pixel_section(v);
    }
}

static void on_leave(GtkEventControllerMotion *c, gpointer ud)
{
    Vid *v = ud;
    (void)c;
    if (a7800vid_tab_hover(v->tab, -1, -1)) {
        gtk_widget_queue_draw(GTK_WIDGET(v->pic));
        update_pixel_section(v);
    }
}

static void on_click(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    Vid *v = ud;
    (void)g;
    if (n == 1) {
        a7800vid_tab_pin(v->tab, (int)x, (int)y);
    } else if (n == 2) {
        /* the first click of the pair toggled the pin; the second restores it */
        a7800vid_tab_hover(v->tab, (int)x, (int)y);
        a7800vid_tab_pin(v->tab, (int)x, (int)y);
        do_break_pixel(v);
    }
    gtk_widget_queue_draw(GTK_WIDGET(v->pic));
    update_pixel_section(v);
}

/* ---- text tables ------------------------------------------------------------- */

/* Replace a text view's contents when they (or the highlighted rows) changed;
 * the scroll position stays put. hl[i] != 0 highlights line i. */
static void put_table(GtkTextView *tv, char **sigp, GString *text, const GString *hl)
{
    GString *sig = g_string_new(NULL);
    g_string_append_len(sig, hl->str, (gssize)hl->len);
    g_string_append_c(sig, '|');
    g_string_append_len(sig, text->str, (gssize)text->len);
    if (!*sigp || strcmp(*sigp, sig->str)) {
        GtkTextBuffer *b = gtk_text_view_get_buffer(tv);
        GtkAdjustment *adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tv));
        double pos = adj ? gtk_adjustment_get_value(adj) : 0;
        char *clean = g_utf8_make_valid(text->str, -1);
        gsize i;
        gtk_text_buffer_set_text(b, clean, -1);
        g_free(clean);
        for (i = 0; i < hl->len; i++) {
            if (hl->str[i] == '1') {
                GtkTextIter a, e;
                gtk_text_buffer_get_iter_at_line(b, &a, (int)i);
                e = a;
                gtk_text_iter_forward_to_line_end(&e);
                gtk_text_buffer_apply_tag_by_name(b, "changed", &a, &e);
            }
        }
        if (adj) gtk_adjustment_set_value(adj, pos);
        g_free(*sigp);
        *sigp = g_strdup(sig->str);
    }
    g_string_free(sig, TRUE);
}

static void fill_regs(Vid *v)
{
    const a7800vid_reg *r;
    int n = a7800vid_tab_regs(v->tab, &r), i;
    GString *s = g_string_new(NULL), *hl = g_string_new(NULL);
    g_string_append(s, "Register   Value  Meaning\n");
    g_string_append_c(hl, '0');
    for (i = 0; i < n; i++) {
        g_string_append_printf(s, "%-10s %-6s %s\n", r[i].name, r[i].value, r[i].meaning);
        g_string_append_c(hl, r[i].age < A7800VID_REG_FADE ? '1' : '0');
    }
    if (!n) g_string_append(s, "The machine is not running.\n");
    put_table(v->regs_text, &v->regs_sig, s, hl);
    g_string_free(s, TRUE);
    g_string_free(hl, TRUE);
}

static void fill_lines(Vid *v)
{
    const a7800vid_line_row *r;
    int n = a7800vid_tab_lines(v->tab, &r), i;
    GString *s = g_string_new(NULL), *hl = g_string_new(NULL);
    g_string_append(s, "Row Line CTRL CB DPP  DLL  DL   Off H D Obj Cyc BG Notes\n");
    g_string_append_c(hl, '0');
    for (i = 0; i < n; i++) {
        g_string_append_printf(s, "%3d %4d  %02X   %02X  %04X %04X %04X  %d  %d %d %3d %3d %02X ", r[i].row,
                               r[i].scanline, r[i].ctrl, r[i].charbase, r[i].dpp, r[i].dll, r[i].dl,
                               r[i].offset, r[i].holey, r[i].dli, r[i].objects, r[i].cycles, r[i].background);
        g_string_append_printf(s, "%s%s%s%s%s", r[i].dli ? "DLI " : "",
                               r[i].changed_mask & A7800VID_LC_ZONE ? "zone " : "",
                               r[i].changed_mask & A7800VID_LC_PALETTE ? "palette " : "",
                               r[i].changed_mask & A7800VID_LC_CTRL ? "CTRL " : "",
                               r[i].changed_mask & A7800VID_LC_DPP ? "DPP " : "");
        g_string_append_c(s, '\n');
        /* a zone change is why the row is listed; the registers moving is the news */
        g_string_append_c(hl, i > 0 && (r[i].changed_mask &
                                         (A7800VID_LC_PALETTE | A7800VID_LC_CTRL | A7800VID_LC_DPP)) ? '1' : '0');
    }
    if (!n) g_string_append(s, "The machine is not running.\n");
    put_table(v->lines_text, &v->lines_sig, s, hl);
    g_string_free(s, TRUE);
    g_string_free(hl, TRUE);
}

/* ---- colours ------------------------------------------------------------------ */

static void draw_swatch(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer ud)
{
    Vid *v = ud;
    const a7800vid_swatch *sw;
    int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(area), "idx"));
    (void)w; (void)h;
    if (a7800vid_tab_swatches(v->tab, &sw) <= index) return;
    set_rgb(cr, sw[index].rgb);
    cairo_rectangle(cr, 0, 0, 22, 22);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.8);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, 0.5, 0.5, 21, 21);
    cairo_stroke(cr);
    if (sw[index].inspected) {      /* the inspected pixel uses this colour */
        cairo_set_source_rgb(cr, 1, 0.85, 0.1);
        cairo_set_line_width(cr, 2);
        cairo_rectangle(cr, 1, 1, 20, 20);
        cairo_stroke(cr);
    }
}

static void on_swatch_break(GtkButton *b, gpointer ud)
{
    Vid *v = ud;
    int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "idx"));
    const a7800vid_swatch *sw;
    int id = a7800vid_tab_break_on_swatch(v->tab, index), n = a7800vid_tab_swatches(v->tab, &sw);
    char buf[120];
    if (id >= 0 && index < n)
        g_snprintf(buf, sizeof buf, "Break on writes to $%02X (%s)", sw[index].address, sw[index].name);
    else
        g_snprintf(buf, sizeof buf, "Could not add a breakpoint");
    gtk_label_set_text(v->note, buf);
}

static void fill_colours(Vid *v)
{
    const a7800vid_swatch *sw;
    int n = a7800vid_tab_swatches(v->tab, &sw), index;
    for (index = 0; index < NSWATCH; index++) {
        char buf[96];
        if (index < n)
            g_snprintf(buf, sizeof buf, "$%02X %-8s %02X  #%06X  %ld px", sw[index].address, sw[index].name,
                       sw[index].value, sw[index].rgb & 0xffffff, sw[index].uses);
        else
            g_snprintf(buf, sizeof buf, "-");
        gtk_label_set_text(v->swatch_lbl[index], buf);
        gtk_widget_queue_draw(GTK_WIDGET(v->swatch_box[index]));
    }
}

static void fill_timing(Vid *v)
{
    const char *const *lines;
    int n = a7800vid_tab_timing(v->tab, &lines), i;
    GString *s = g_string_new(NULL);
    for (i = 0; i < n; i++) {
        const char *tab = strchr(lines[i], '\t');
        if (tab) g_string_append_printf(s, "%-14.*s %s\n", (int)(tab - lines[i]), lines[i], tab + 1);
        else g_string_append_printf(s, "%s\n", lines[i]);
    }
    gtk_label_set_text(v->timing_text, s->len ? s->str : "The machine is not running.");
    g_string_free(s, TRUE);
    gtk_progress_bar_set_fraction(v->beam, a7800vid_tab_beam_permille(v->tab) / 1000.0);
}

/* ---- refresh ------------------------------------------------------------------- */

static void fill_section(Vid *v, int sec)
{
    switch (sec) {
    case A7800VID_S_REGISTERS: fill_regs(v); break;
    case A7800VID_S_COLOURS: fill_colours(v); break;
    case A7800VID_S_LINES: fill_lines(v); break;
    case A7800VID_S_TIMING: fill_timing(v); break;
    default: update_pixel_section(v); break;
    }
}

static void refresh_view(Vid *v, unsigned what)
{
    if (what & (A7800VID_TAB_PICTURE | A7800VID_TAB_OFF)) {
        int pw = 0, ph = 0;
        if (a7800vid_tab_picture(v->tab, &pw, &ph)) {
            gtk_drawing_area_set_content_width(v->pic, pw);
            gtk_drawing_area_set_content_height(v->pic, ph);
        } else {
            gtk_drawing_area_set_content_width(v->pic, v->vp_w > 4 ? v->vp_w : 480);
            gtk_drawing_area_set_content_height(v->pic, v->vp_h > 4 ? v->vp_h : 300);
        }
        gtk_widget_queue_draw(GTK_WIDGET(v->pic));
    }
    if (what & A7800VID_TAB_TABLES) {
        gtk_label_set_text(v->status, a7800vid_tab_status(v->tab));
        fill_section(v, gtk_notebook_get_current_page(v->sections));
    }
}

static void sync_viewport(Vid *v)
{
    int w = gtk_widget_get_width(v->pic_scroll), h = gtk_widget_get_height(v->pic_scroll);
    if (w > 4 && h > 4 && (w != v->vp_w || h != v->vp_h)) {
        v->vp_w = w;
        v->vp_h = h;
        a7800vid_tab_set_viewport(v->tab, w - 2, h - 2);
    }
}

void dbg_video_refresh(GtkWidget *page, gboolean force)
{
    Vid *v = g_object_get_data(G_OBJECT(page), "vid");
    gboolean mapped;
    unsigned what;
    if (!v) return;
    mapped = gtk_widget_get_mapped(page);
    if (mapped && !v->was_mapped) force = TRUE;      /* just shown */
    v->was_mapped = mapped;
    if (!v->primed) force = TRUE;
    v->primed = TRUE;
    sync_viewport(v);
    what = a7800vid_tab_refresh(v->tab, force);
    if (force) what |= A7800VID_TAB_PICTURE | A7800VID_TAB_TABLES;
    refresh_view(v, what);
}

/* The picture's room changed (window resized, split dragged): Fit follows it. */
static gboolean on_tick(GtkWidget *w, GdkFrameClock *fc, gpointer ud)
{
    Vid *v = ud;
    int pw = gtk_widget_get_width(w), ph = gtk_widget_get_height(w);
    (void)fc;
    if (pw > 4 && ph > 4 && (pw != v->vp_w || ph != v->vp_h)) {
        sync_viewport(v);
        if (v->primed && a7800vid_tab_zoom(v->tab) == 0)
            refresh_view(v, a7800vid_tab_refresh(v->tab, TRUE) | A7800VID_TAB_PICTURE);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- controls ------------------------------------------------------------------- */

static void refresh_all(Vid *v)
{
    refresh_view(v, a7800vid_tab_refresh(v->tab, TRUE) | A7800VID_TAB_PICTURE | A7800VID_TAB_TABLES);
}

static void on_view(GObject *o, GParamSpec *p, gpointer ud)
{
    Vid *v = ud;
    (void)o; (void)p;
    if (v->updating) return;
    a7800vid_tab_set_view(v->tab, (int)gtk_drop_down_get_selected(v->view));
    refresh_all(v);
}

static void on_source(GObject *o, GParamSpec *p, gpointer ud)
{
    Vid *v = ud;
    (void)o; (void)p;
    if (v->updating) return;
    a7800vid_tab_set_source(v->tab, (int)gtk_drop_down_get_selected(v->source));
    refresh_all(v);
}

static void on_zoom(GObject *o, GParamSpec *p, gpointer ud)
{
    Vid *v = ud;
    int n;
    const int *z = a7800vid_zooms(&n);
    guint i = gtk_drop_down_get_selected(v->zoom);
    (void)o; (void)p;
    if (v->updating || (int)i >= n) return;
    sync_viewport(v);
    a7800vid_tab_set_zoom(v->tab, z[i]);
    refresh_all(v);
}

static void on_overlay(GtkCheckButton *b, gpointer ud)
{
    Vid *v = ud;
    unsigned m = 0;
    int n, i;
    const a7800vid_overlay_info *d = a7800vid_overlays(&n);
    (void)b;
    if (v->updating) return;
    for (i = 0; i < n && i < v->nov; i++)
        if (gtk_check_button_get_active(v->ov[i])) m |= d[i].bit;
    a7800vid_tab_set_overlays(v->tab, m);
    refresh_all(v);
}

static void on_unpin(GtkButton *b, gpointer ud)
{
    Vid *v = ud;
    (void)b;
    a7800vid_tab_unpin(v->tab);
    gtk_widget_queue_draw(GTK_WIDGET(v->pic));
    update_pixel_section(v);
}

static void on_break(GtkButton *b, gpointer ud)
{
    (void)b;
    do_break_pixel(ud);
}

static void on_section(GtkNotebook *nb, GtkWidget *page, guint n, gpointer ud)
{
    Vid *v = ud;
    (void)nb; (void)page;
    fill_section(v, (int)n);
}

static void on_destroy(GtkWidget *w, gpointer ud)
{
    Vid *v = ud;
    (void)w;
    a7800vid_tab_free(v->tab);
    g_free(v->regs_sig);
    g_free(v->lines_sig);
    g_free(v);
}

GtkWidget *dbg_video_new(a7800session *session)
{
    Vid *v = g_new0(Vid, 1);
    GtkWidget *root, *left, *bar, *right, *box, *btns, *b1, *b2, *scroll, *ovbtn, *pop, *ovbox, *grid, *brk;
    GtkEventController *motion;
    GtkGesture *click;
    GtkTextBuffer *tb;
    const a7800vid_overlay_info *od;
    const int *zv;
    const char *views[A7800VID_V_COUNT + 1], *sources[A7800VID_D_COUNT + 1], *zlabels[24];
    char zbuf[24][8];
    int n, nz, i;

    v->tab = a7800vid_tab_new(session);

    root = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    g_object_set_data(G_OBJECT(root), "vid", v);
    g_signal_connect(root, "destroy", G_CALLBACK(on_destroy), v);

    /* ---- left: toolbar, status, picture */
    left = padded_box();
    bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    for (i = 0; i < A7800VID_V_COUNT; i++) views[i] = a7800vid_view_name(i);
    views[A7800VID_V_COUNT] = NULL;
    v->view = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(views));
    gtk_widget_set_tooltip_text(GTK_WIDGET(v->view),
        "Display: the last finished frame, as the TV showed it.\n"
        "Decoded: the display list's objects or zones over the frame (see Source).\n"
        "Beam: this frame above the beam, the last one dimmed below.\n"
        "Values: the raw palette and colour numbers (cool = palettes 0-3, warm = 4-7).");
    for (i = 0; i < A7800VID_D_COUNT; i++) sources[i] = a7800vid_source_name(i);
    sources[A7800VID_D_COUNT] = NULL;
    v->source = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(sources));
    gtk_widget_set_tooltip_text(GTK_WIDGET(v->source),
        "What Decoded shows: each display list object's extent, the zone bands "
        "or the 32 palette registers.");
    zv = a7800vid_zooms(&nz);
    if (nz > 23) nz = 23;
    for (i = 0; i < nz; i++) {
        if (zv[i] == 0) g_strlcpy(zbuf[i], "Fit", sizeof zbuf[i]);
        else g_snprintf(zbuf[i], sizeof zbuf[i], "%d×", zv[i]);
        zlabels[i] = zbuf[i];
    }
    zlabels[nz] = NULL;
    v->zoom = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(zlabels));
    gtk_box_append(GTK_BOX(bar), gtk_label_new("View"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(v->view));
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Source"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(v->source));
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Zoom"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(v->zoom));

    od = a7800vid_overlays(&n);
    if (n > MAX_OVERLAYS) n = MAX_OVERLAYS;
    v->nov = n;
    ovbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    for (i = 0; i < n; i++) {
        v->ov[i] = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(od[i].label));
        g_signal_connect(v->ov[i], "toggled", G_CALLBACK(on_overlay), v);
        gtk_box_append(GTK_BOX(ovbox), GTK_WIDGET(v->ov[i]));
    }
    pop = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(pop), ovbox);
    ovbtn = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(ovbtn), "Overlays");
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(ovbtn), pop);
    gtk_box_append(GTK_BOX(bar), ovbtn);

    brk = gtk_button_new_with_label("Break on writes");
    gtk_widget_set_tooltip_text(brk, "Break when the CPU writes the inspected pixel's object headers, "
                                     "its DLL entry or the palette register it read");
    g_signal_connect(brk, "clicked", G_CALLBACK(on_break), v);
    gtk_box_append(GTK_BOX(bar), brk);
    gtk_box_append(GTK_BOX(left), bar);

    v->status = GTK_LABEL(mono_label());
    gtk_label_set_selectable(v->status, FALSE);
    gtk_box_append(GTK_BOX(left), GTK_WIDGET(v->status));

    v->pic = GTK_DRAWING_AREA(gtk_drawing_area_new());
    gtk_drawing_area_set_draw_func(v->pic, draw_pic, v, NULL);
    gtk_drawing_area_set_content_width(v->pic, 480);
    gtk_drawing_area_set_content_height(v->pic, 300);
    gtk_widget_set_halign(GTK_WIDGET(v->pic), GTK_ALIGN_START);
    gtk_widget_set_valign(GTK_WIDGET(v->pic), GTK_ALIGN_START);
    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), v);
    g_signal_connect(motion, "leave", G_CALLBACK(on_leave), v);
    gtk_widget_add_controller(GTK_WIDGET(v->pic), motion);
    click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_click), v);
    gtk_widget_add_controller(GTK_WIDGET(v->pic), GTK_EVENT_CONTROLLER(click));
    scroll = gtk_scrolled_window_new();
    v->pic_scroll = scroll;
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(v->pic));
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_add_tick_callback(scroll, on_tick, v, NULL);
    gtk_box_append(GTK_BOX(left), scroll);

    v->note = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(v->note, 0);
    gtk_label_set_wrap(v->note, TRUE);
    gtk_box_append(GTK_BOX(left), GTK_WIDGET(v->note));

    /* ---- right: the five sections, in a7800vid_section_name order */
    right = gtk_notebook_new();
    v->sections = GTK_NOTEBOOK(right);
    gtk_widget_set_size_request(right, 520, -1);

    /* Pixel */
    box = padded_box();
    v->pixel_text = GTK_LABEL(mono_label());
    gtk_label_set_wrap(v->pixel_text, TRUE);
    gtk_label_set_wrap_mode(v->pixel_text, PANGO_WRAP_WORD_CHAR);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->pixel_text));
    v->pixel_note = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(v->pixel_note, 0);
    gtk_label_set_wrap(v->pixel_note, TRUE);
    gtk_label_set_wrap_mode(v->pixel_note, PANGO_WRAP_WORD_CHAR);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->pixel_note));
    btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    b1 = gtk_button_new_with_label("Break on writes");
    b2 = gtk_button_new_with_label("Unpin");
    g_signal_connect(b1, "clicked", G_CALLBACK(on_break), v);
    g_signal_connect(b2, "clicked", G_CALLBACK(on_unpin), v);
    gtk_box_append(GTK_BOX(btns), b1);
    gtk_box_append(GTK_BOX(btns), b2);
    gtk_box_append(GTK_BOX(box), btns);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    gtk_notebook_append_page(v->sections, scroll, gtk_label_new(a7800vid_section_name(A7800VID_S_PIXEL)));

    /* Registers */
    v->regs_text = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(v->regs_text, FALSE);
    gtk_text_view_set_cursor_visible(v->regs_text, FALSE);
    gtk_text_view_set_monospace(v->regs_text, TRUE);
    gtk_text_view_set_wrap_mode(v->regs_text, GTK_WRAP_WORD_CHAR);
    tb = gtk_text_view_get_buffer(v->regs_text);
    gtk_text_buffer_create_tag(tb, "changed", "background", "#E4002B", "foreground", "#ffffff",
                               "weight", 700, NULL);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(v->regs_text));
    gtk_notebook_append_page(v->sections, scroll, gtk_label_new(a7800vid_section_name(A7800VID_S_REGISTERS)));

    /* Colours: the 32 palette registers, BACKGRND then P0C1 to P7C3 */
    box = padded_box();
    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 3);
    for (i = 0; i < NSWATCH; i++) {
        GtkWidget *bk = gtk_button_new_with_label("Break");
        const int column = 0, row = i;
        v->swatch_box[i] = GTK_DRAWING_AREA(gtk_drawing_area_new());
        gtk_drawing_area_set_content_width(v->swatch_box[i], 22);
        gtk_drawing_area_set_content_height(v->swatch_box[i], 22);
        g_object_set_data(G_OBJECT(v->swatch_box[i]), "idx", GINT_TO_POINTER(i));
        gtk_drawing_area_set_draw_func(v->swatch_box[i], draw_swatch, v, NULL);
        v->swatch_lbl[i] = GTK_LABEL(mono_label());
        gtk_label_set_selectable(v->swatch_lbl[i], FALSE);
        g_object_set_data(G_OBJECT(bk), "idx", GINT_TO_POINTER(i));
        gtk_widget_add_css_class(bk, "flat");
        gtk_widget_set_tooltip_text(bk, "Break when the program writes this palette register");
        g_signal_connect(bk, "clicked", G_CALLBACK(on_swatch_break), v);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(v->swatch_box[i]), column, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(v->swatch_lbl[i]), column + 1, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), bk, column + 2, row, 1, 1);
    }
    gtk_box_append(GTK_BOX(box), grid);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    gtk_notebook_append_page(v->sections, scroll, gtk_label_new(a7800vid_section_name(A7800VID_S_COLOURS)));

    /* Lines */
    v->lines_text = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(v->lines_text, FALSE);
    gtk_text_view_set_cursor_visible(v->lines_text, FALSE);
    gtk_text_view_set_monospace(v->lines_text, TRUE);
    tb = gtk_text_view_get_buffer(v->lines_text);
    gtk_text_buffer_create_tag(tb, "changed", "background", "#E4002B", "foreground", "#ffffff",
                               "weight", 700, NULL);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(v->lines_text));
    gtk_notebook_append_page(v->sections, scroll, gtk_label_new(a7800vid_section_name(A7800VID_S_LINES)));

    /* Timing */
    box = padded_box();
    v->beam = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_progress_bar_set_text(v->beam, "beam");
    gtk_progress_bar_set_show_text(v->beam, TRUE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->beam));
    v->timing_text = GTK_LABEL(mono_label());
    gtk_label_set_selectable(v->timing_text, FALSE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->timing_text));
    gtk_notebook_append_page(v->sections, box, gtk_label_new(a7800vid_section_name(A7800VID_S_TIMING)));

    gtk_paned_set_start_child(GTK_PANED(root), left);
    gtk_paned_set_end_child(GTK_PANED(root), right);
    gtk_paned_set_resize_start_child(GTK_PANED(root), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(root), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(root), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(root), FALSE);

    /* start from the controller's state (the A7800_DEBUGGER_VIDEO_* variables) */
    v->updating = TRUE;
    gtk_drop_down_set_selected(v->view, (guint)a7800vid_tab_view(v->tab));
    gtk_drop_down_set_selected(v->source, (guint)a7800vid_tab_source(v->tab));
    for (i = 0; i < nz; i++)
        if (zv[i] == a7800vid_tab_zoom(v->tab)) gtk_drop_down_set_selected(v->zoom, (guint)i);
    for (i = 0; i < v->nov; i++)
        gtk_check_button_set_active(v->ov[i], (a7800vid_tab_overlays(v->tab) & od[i].bit) != 0);
    gtk_notebook_set_current_page(v->sections, a7800vid_tab_env_section(v->tab));
    v->updating = FALSE;
    update_pixel_section(v);
    g_signal_connect(v->view, "notify::selected", G_CALLBACK(on_view), v);
    g_signal_connect(v->source, "notify::selected", G_CALLBACK(on_source), v);
    g_signal_connect(v->zoom, "notify::selected", G_CALLBACK(on_zoom), v);
    g_signal_connect(v->sections, "switch-page", G_CALLBACK(on_section), v);
    return root;
}
