/*
 * The Controllers window: the Atari 7800's two controller ports and its front
 * panel drawn with cairo from the core's layout (a7800session_controls_layout,
 * hit-tested with a7800session_controls_hit), from photos of the real things
 * (DESIGN-7800.md). Each port shows what is plugged into it: the ProLine
 * two-button stick, a 2600 CX40, the XG-1 light gun (captions only: the picture
 * is its target) or an empty socket; under them the console's silver band with
 * PAUSE / SELECT / RESET and the two difficulty slides.
 *
 * Every press is the raw press and release of a gesture, not a button's
 * "clicked": the console samples its inputs once per frame, so a value present
 * only for the instant of a click falls between two frames. A part on screen is
 * HELD for as long as the mouse button is down; the stick follows the pointer
 * while it is dragged (eight 45-degree sectors from the stick's centre) and is
 * let go on release. A difficulty slide flips on the press. Everything the
 * pointer holds is let go (a7800session_release_pointer) on mouse-up, on the
 * gesture's cancel, on focus loss, when the window hides or maps and when Map
 * is armed.
 *
 * SINGLETON, hidden rather than destroyed, so a remap survives closing it. Not
 * resizable: the layout has the same extent for every controller type, so the
 * canvas never changes size.
 *
 * MAP MODE: Map arms a two-step rebind -- click a part (on the stick, the
 * direction under the click) to pick the target, then press a keyboard key OR a
 * gamepad button; Esc cancels. Clear unbinds the picked target; Defaults
 * restores the table; Show bindings paints each part's binding on it. While a
 * target is picked the other parts dim to 40%. The two system keys (Reboot to
 * CONFIG, stop in the debugger) are not on the picture: the "System key" list
 * picks them.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controllers_window.h"

#include "../window.h"
#include "../help/help_window.h"

#include <math.h>
#include <string.h>

#define SCALE   22.0        /* pixels per layout unit */
#define MARGIN  16.0
#define MAX_PADS 8

#define HELD_R  (((A7800SESSION_HELD_RGB >> 16) & 255) / 255.0)
#define HELD_G  (((A7800SESSION_HELD_RGB >> 8) & 255) / 255.0)
#define HELD_B  ((A7800SESSION_HELD_RGB & 255) / 255.0)

/* ---- singleton state ------------------------------------------------------ */

static GtkWidget *g_window;
static GtkWindow *g_parent;     /* the A7800Window: typed keys go through it */
static a7800session *g_session;
static GtkWidget *g_area;

static const a7800_control *g_layout;
static int g_count;
static int g_view[2] = { -1, -1 };   /* the effective types the layout was built for */
static float g_lw, g_lh;
static double g_offx;           /* the picture is centred in the canvas */

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 the picked
 * target waiting for a key or pad button. */
static int g_map_state = -2;
static GtkWidget *g_map_button, *g_clear_button, *g_defaults_button, *g_show_check;
static GtkWidget *g_map_hint, *g_system_dd;
static guint g_capture_timer, g_info_timer;
static guint g_swallow_code;    /* the key Map took: its repeats and release too */
static GtkWidget *g_type_combo[2], *g_type_now[2];
static GtkWidget *g_pad_box;
static GtkWidget *g_pad_port[MAX_PADS];
static int g_npad_rows;
static unsigned g_pad_generation = (unsigned)-1;
static gboolean g_syncing;
static gboolean g_show_bindings;

static int g_drag_idx = -1;     /* the layout part the drag started on, or -1 */
static guint64 g_last_sig;

/* ---- helpers --------------------------------------------------------------- */

static void set_hint(const char *text)
{
    gtk_label_set_text(GTK_LABEL(g_map_hint), text);
}

static void redraw(void)
{
    if (g_area)
        gtk_widget_queue_draw(g_area);
}

static void release_pointer(void)
{
    if (g_session)
        a7800session_release_pointer(g_session);
    g_drag_idx = -1;
    redraw();
}

static const a7800_control *part(int idx)
{
    return idx >= 0 && idx < g_count ? &g_layout[idx] : NULL;
}

static double px(double u) { return g_offx + u * SCALE; }
static double py(double u) { return MARGIN + u * SCALE; }

/* The direction bit a vector from a stick's centre points at, for Map: the
 * dominant axis (-1 within the dead centre). */
static int map_direction(double dx, double dy, double radius)
{
    if (a7800session_stick_from_vector((float)dx, (float)dy, (float)radius) == 0)
        return -1;
    if (fabs(dx) >= fabs(dy))
        return dx < 0 ? A7800_ACT_LEFT : A7800_ACT_RIGHT;
    return dy < 0 ? A7800_ACT_UP : A7800_ACT_DOWN;
}

/* The Map target a click at canvas point (x, y) picks: a part's target; on a
 * stick the direction under the click. */
static int map_target_at(int idx, double x, double y)
{
    const a7800_control *c = part(idx);
    double rad, cx, cy;
    int dir;
    if (!c)
        return -1;
    if (c->kind != A7800_CTL_STICK)
        return c->target;
    rad = c->w / 2 * SCALE;
    cx = px(c->x + c->w / 2);
    cy = py(c->y + c->h / 2);
    dir = map_direction(x - cx, y - cy, rad);
    return dir < 0 ? -1 : A7800_TARGET_PORT(c->port, dir);
}

/* ---- gamepad capture and Map state ---------------------------------------- */

static void stop_capture(void)
{
    if (g_capture_timer) {
        g_source_remove(g_capture_timer);
        g_capture_timer = 0;
    }
    a7800session_gamepad_capture_cancel(g_session);
}

static void refresh_clear_button(void)
{
    gtk_widget_set_sensitive(g_clear_button, g_map_state >= 0);
}

static void set_map_state(int state);

static gboolean capture_tick(gpointer d)
{
    int button;
    (void)d;
    if (g_map_state < 0) { g_capture_timer = 0; return G_SOURCE_REMOVE; }
    if (a7800session_gamepad_capture_poll(g_session, &button)) {
        char stolen[128], msg[200];
        stolen[0] = '\0';
        a7800session_binding_set_button(g_session, g_map_state, button, stolen, sizeof stolen);
        if (stolen[0])
            g_snprintf(msg, sizeof msg, "Bound %s (was %s)", a7800_pad_button_name(button), stolen);
        else
            g_snprintf(msg, sizeof msg, "Bound %s", a7800_pad_button_name(button));
        g_capture_timer = 0;
        set_map_state(-1);          /* back to armed: remapping several in a row is normal */
        set_hint(msg);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void set_map_state(int state)
{
    if (state != -2) {
        release_pointer();                       /* a part held into Map would never be released */
        a7800session_release_all(g_session);     /* Map swallows key-ups: drop keyboard holds too */
    }
    g_map_state = state;
    if (state == -2) {
        stop_capture();
        g_swallow_code = 0;
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Map");
        gtk_widget_remove_css_class(g_map_button, "a7800-accent");
        set_hint("Drag the stick, hold a button or a key. Click a slide to flip it.");
    } else if (state == -1) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Done");
        gtk_widget_add_css_class(g_map_button, "a7800-accent");
        set_hint("Click a control to remap (on a stick, a direction). Esc backs out");
    } else {
        char msg[128];
        g_snprintf(msg, sizeof msg, "Press a key or gamepad button for %s (Esc cancels)",
                   a7800_target_name(state));
        set_hint(msg);
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Done");
        gtk_widget_add_css_class(g_map_button, "a7800-accent");
        stop_capture();
        a7800session_gamepad_capture_begin(g_session);
        g_capture_timer = g_timeout_add(50, capture_tick, NULL);
    }
    refresh_clear_button();
    redraw();
}

/* ---- drawing ---------------------------------------------------------------- */

static void rgb(cairo_t *cr, unsigned hex)
{
    cairo_set_source_rgb(cr, ((hex >> 16) & 255) / 255.0, ((hex >> 8) & 255) / 255.0,
                         (hex & 255) / 255.0);
}

static void rgba(cairo_t *cr, unsigned hex, double a)
{
    cairo_set_source_rgba(cr, ((hex >> 16) & 255) / 255.0, ((hex >> 8) & 255) / 255.0,
                          (hex & 255) / 255.0, a);
}

static void stop_rgb(cairo_pattern_t *p, double at, unsigned hex)
{
    cairo_pattern_add_color_stop_rgb(p, at, ((hex >> 16) & 255) / 255.0,
                                     ((hex >> 8) & 255) / 255.0, (hex & 255) / 255.0);
}

static void rrect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

static void disc(cairo_t *cr, double cx, double cy, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
}

/* Text centred on a point, shrunk to fit its width. */
static void text_fit(cairo_t *cr, const char *text, double cx, double cy, double maxw,
                     double size, cairo_font_slant_t slant, cairo_font_weight_t weight)
{
    cairo_text_extents_t ext;
    cairo_select_font_face(cr, "Sans", slant, weight);
    cairo_set_font_size(cr, size);
    cairo_text_extents(cr, text, &ext);
    if (ext.width > maxw && ext.width > 0) {
        cairo_set_font_size(cr, size * maxw / ext.width);
        cairo_text_extents(cr, text, &ext);
    }
    cairo_move_to(cr, cx - ext.width / 2 - ext.x_bearing, cy - ext.height / 2 - ext.y_bearing);
    cairo_show_text(cr, text);
}

/* What a part shows in Map mode and with Show bindings: its key and pad button
 * (a dash when there is none). */
static void binding_text(int target, char *key, int keysz, char *pad, int padsz)
{
    a7800_binding b = a7800session_binding_get(g_session, target);
    key[0] = pad[0] = 0;
    a7800session_keysym_name(b.keysym, key, keysz);
    if (!key[0])
        g_strlcpy(key, "\xe2\x80\x94", keysz);
    if (b.button != A7800_PAD_BTN_NONE)
        g_strlcpy(pad, a7800_pad_button_name(b.button), padsz);
}

static gboolean bindings_on(void)
{
    return g_map_state != -2 || g_show_bindings;
}

/* The net direction a port's stick is held in, as a unit vector (0, 0 centred). */
static void stick_dir(int port, double *dx, double *dy)
{
    const unsigned held = a7800session_buttons_held(g_session, port);
    double len;
    *dx = (int)((held >> A7800_ACT_RIGHT) & 1) - (int)((held >> A7800_ACT_LEFT) & 1);
    *dy = (int)((held >> A7800_ACT_DOWN) & 1) - (int)((held >> A7800_ACT_UP) & 1);
    len = hypot(*dx, *dy);
    if (len > 0) {
        *dx /= len;
        *dy /= len;
    }
}

/* ---- captions and the console's bodies ----------------------------------------- */

static void draw_caption(cairo_t *cr, const a7800_control *c, const GdkRGBA *fg)
{
    const double x = px(c->x), y = py(c->y), w = c->w * SCALE, h = c->h * SCALE;
    if (!strcmp(c->label, "7800 PROSYSTEM")) {
        rgb(cr, A7800SESSION_BAND_TEXT_RGB);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_ITALIC, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 13);
        cairo_move_to(cr, x, y + h * 0.78);
        cairo_show_text(cr, c->label);
    } else if (!strcmp(c->label, "No controller")) {
        cairo_set_source_rgba(cr, fg->red, fg->green, fg->blue, 0.6);
        text_fit(cr, c->label, x + w / 2, y + h / 2, w, 13, CAIRO_FONT_SLANT_NORMAL,
                 CAIRO_FONT_WEIGHT_NORMAL);
    } else {                      /* "Player N", the gun's two lines */
        const gboolean title = !strncmp(c->label, "Player", 6);
        cairo_set_source_rgba(cr, fg->red, fg->green, fg->blue, title ? 0.85 : 0.7);
        text_fit(cr, c->label, x + w / 2, y + h / 2, w, 12,
                 CAIRO_FONT_SLANT_NORMAL, title ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    }
}

/* The Atari fuji: three strokes spreading from a common base. */
static void draw_fuji(cairo_t *cr, double cx, double base, double top, unsigned colour)
{
    const double d = (base - top) * 0.45;
    int i;
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    rgb(cr, colour);
    cairo_set_line_width(cr, 1.4);
    for (i = -1; i <= 1; i++) {
        cairo_move_to(cr, cx + i * d * 0.8, base);
        cairo_curve_to(cr, cx + i * d * 0.5, base - (base - top) * 0.5,
                       cx + i * d * 0.25, top + (base - top) * 0.2, cx + i * d * 0.12, top);
        cairo_stroke(cr);
    }
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
}

/* The ProLine: a low black slab, the raised triangle over the stick, the two
 * red buttons' recesses with their swooshes, the embossed logo. The stick's
 * dish and the buttons themselves are separate parts drawn on top. */
static void draw_proline(cairo_t *cr, const a7800_control *c)
{
    const double ox = c->x, oy = c->y;
    static const double button_x[2] = { 8.4, 12.4 };
    cairo_pattern_t *p;
    int i;

    /* shadow, then the slab with its bevel */
    rgba(cr, 0x000000, 0.35);
    rrect(cr, px(ox) + 1, py(oy) + 3, c->w * SCALE, c->h * SCALE, 0.5 * SCALE);
    cairo_fill(cr);
    rgb(cr, A7800SESSION_CASE_BEVEL_RGB);
    rrect(cr, px(ox), py(oy), c->w * SCALE, c->h * SCALE, 0.5 * SCALE);
    cairo_fill(cr);
    p = cairo_pattern_create_linear(0, py(oy), 0, py(oy + c->h));
    stop_rgb(p, 0.0, 0x232326);
    stop_rgb(p, 0.8, A7800SESSION_CASE_RGB);
    stop_rgb(p, 1.0, A7800SESSION_CASE_SHADOW_RGB);
    cairo_set_source(cr, p);
    rrect(cr, px(ox) + 2, py(oy) + 2, c->w * SCALE - 4, c->h * SCALE - 4, 0.45 * SCALE);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(p);

    cairo_save(cr);
    cairo_clip(cr);

    /* the thick front lip */
    rgb(cr, A7800SESSION_CASE_SHADOW_RGB);
    cairo_rectangle(cr, px(ox), py(oy + 7.35), c->w * SCALE, 0.65 * SCALE);
    cairo_fill(cr);

    /* the raised triangle over the stick */
    cairo_move_to(cr, px(ox + 0.2), py(oy + 0.2));
    cairo_line_to(cr, px(ox + 7.8), py(oy + 0.2));
    cairo_line_to(cr, px(ox + 0.8), py(oy + 7.2));
    cairo_line_to(cr, px(ox + 0.2), py(oy + 7.2));
    cairo_close_path(cr);
    rgb(cr, 0x242427);
    cairo_fill_preserve(cr);
    rgb(cr, A7800SESSION_PANEL_EDGE_RGB);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    rgba(cr, 0xFFFFFF, 0.10);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, px(ox + 7.8), py(oy + 0.2) + 1.5);
    cairo_line_to(cr, px(ox + 0.8) + 1.5, py(oy + 7.2));
    cairo_stroke(cr);

    /* each red button's recess and the ribbed swoosh running down-right */
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (i = 0; i < 2; i++) {
        rgb(cr, A7800SESSION_SWOOSH_RGB);
        cairo_set_line_width(cr, 2.0 * SCALE);
        cairo_move_to(cr, px(ox + button_x[i]), py(oy + 5.2));
        cairo_line_to(cr, px(ox + button_x[i] + 2.6), py(oy + 7.6));
        cairo_stroke(cr);
        rgba(cr, 0xFFFFFF, 0.07);
        cairo_set_line_width(cr, 1);
        cairo_move_to(cr, px(ox + button_x[i] + 0.9), py(oy + 4.7));
        cairo_line_to(cr, px(ox + button_x[i] + 3.6), py(oy + 7.0));
        cairo_stroke(cr);
    }
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
    for (i = 0; i < 2; i++) {
        rgb(cr, A7800SESSION_BUTTON_RECESS_RGB);
        disc(cr, px(ox + button_x[i]), py(oy + 5.2), 1.5 * SCALE);
        cairo_fill(cr);
    }
    cairo_restore(cr);

    /* the embossed 1 and 2 above-left of each button */
    rgb(cr, A7800SESSION_NUMERAL_RGB);
    for (i = 0; i < 2; i++)
        text_fit(cr, i ? "2" : "1", px(ox + button_x[i] - 1.5), py(oy + 3.5), 14, 10,
                 CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);

    /* the silver fuji and ATARI, tilted about -15 degrees */
    cairo_save(cr);
    cairo_translate(cr, px(ox + 12.8), py(oy + 2.0));
    cairo_rotate(cr, -15.0 * G_PI / 180.0);
    draw_fuji(cr, -2.2 * SCALE * 0.55, 0.45 * SCALE, -0.45 * SCALE, A7800SESSION_LOGO_RGB);
    rgb(cr, A7800SESSION_LOGO_RGB);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 0.95 * SCALE);
    cairo_move_to(cr, -1.5 * SCALE * 0.55, 0.42 * SCALE);
    cairo_show_text(cr, "ATARI");
    cairo_restore(cr);

    /* the cable leaving the top edge */
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    rgb(cr, A7800SESSION_CABLE_RGB);
    cairo_set_line_width(cr, 0.4 * SCALE);
    cairo_move_to(cr, px(ox + 13.5), py(oy + 0.3));
    cairo_curve_to(cr, px(ox + 13.7), py(oy - 0.4), px(ox + 14.4), py(oy - 0.2), px(ox + 14.6), py(oy - 0.8));
    cairo_stroke(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
}

/* The CX40's case, its fire button's teardrop recess and its cable. The ring
 * and the shaft are the stick part. */
static void draw_cx40(cairo_t *cr, const a7800_control *c)
{
    const double x = px(c->x), y = py(c->y), s = c->w * SCALE;
    cairo_pattern_t *p;

    rgba(cr, 0x000000, 0.35);
    rrect(cr, x + 1, y + 3, s, s, 0.9 * SCALE);
    cairo_fill(cr);
    rgb(cr, A7800SESSION_CX40_BEVEL_RGB);
    rrect(cr, x, y, s, s, 0.9 * SCALE);
    cairo_fill(cr);
    p = cairo_pattern_create_linear(x, y, x + s, y + s);
    stop_rgb(p, 0.0, 0x232323);
    stop_rgb(p, 1.0, A7800SESSION_CX40_CASE_RGB);
    cairo_set_source(cr, p);
    rrect(cr, x + 0.25 * SCALE, y + 0.25 * SCALE, s - 0.5 * SCALE, s - 0.5 * SCALE, 0.7 * SCALE);
    cairo_fill(cr);
    cairo_pattern_destroy(p);

    /* the fire button's recess: a teardrop joined to the ring by a neck */
    rgb(cr, A7800SESSION_CX40_RING_RGB);
    disc(cr, x + 1.7 * SCALE, y + 1.7 * SCALE, 1.1 * SCALE);
    cairo_fill(cr);
    cairo_set_line_width(cr, 0.9 * SCALE);
    cairo_move_to(cr, x + 1.7 * SCALE, y + 1.7 * SCALE);
    cairo_line_to(cr, x + 3.2 * SCALE, y + 3.2 * SCALE);
    cairo_stroke(cr);

    /* "TOP" embossed above the ring, and the cable */
    rgb(cr, A7800SESSION_CX40_BEVEL_RGB);
    text_fit(cr, "TOP", x + 4.5 * SCALE, y + 0.75 * SCALE, 30, 8, CAIRO_FONT_SLANT_NORMAL,
             CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    rgb(cr, A7800SESSION_CABLE_RGB);
    cairo_set_line_width(cr, 0.35 * SCALE);
    cairo_move_to(cr, x + 0.6 * SCALE, y + 0.3 * SCALE);
    cairo_curve_to(cr, x + 0.5 * SCALE, y - 0.3 * SCALE, x + 0.2 * SCALE, y - 0.3 * SCALE, x + 0.1 * SCALE, y - 0.8 * SCALE);
    cairo_stroke(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
}

/* The XG-1: drawn in the photo's own pixels (muzzle at the left, the ribbed
 * grip under the rear half), scaled into its box. */
static void draw_gun(cairo_t *cr, const a7800_control *c)
{
    const double k = c->h * SCALE / 990.0;
    const double ox = px(c->x) + (c->w * SCALE - 1240.0 * k) / 2 - 20.0 * k, oy = py(c->y) - 20.0 * k;
    cairo_pattern_t *p;
    double yy;

    cairo_save(cr);
    cairo_translate(cr, ox, oy);
    cairo_scale(cr, k, k);

    /* barrel, muzzle and front sight */
    rgb(cr, 0xA29E94);
    cairo_rectangle(cr, 240, 120, 150, 110);
    cairo_fill(cr);
    p = cairo_pattern_create_linear(0, 95, 0, 215);
    stop_rgb(p, 0.0, 0xD0CCC2);
    stop_rgb(p, 1.0, 0x9C988E);
    cairo_set_source(cr, p);
    rrect(cr, 25, 95, 235, 125, 28);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    rgb(cr, 0xB7B2A6);
    cairo_rectangle(cr, 140, 38, 52, 62);
    cairo_fill(cr);
    rgb(cr, 0x8E8A80);
    rrect(cr, 62, 140, 120, 28, 12);
    cairo_fill(cr);

    /* the body */
    p = cairo_pattern_create_linear(0, 190, 0, 520);
    stop_rgb(p, 0.0, 0xCFCBC1);
    stop_rgb(p, 1.0, 0xA8A398);
    cairo_set_source(cr, p);
    cairo_move_to(cr, 370, 185);
    cairo_line_to(cr, 610, 205);
    cairo_line_to(cr, 1195, 375);
    cairo_line_to(cr, 1240, 430);
    cairo_line_to(cr, 1230, 520);
    cairo_line_to(cr, 990, 495);
    cairo_line_to(cr, 760, 440);
    cairo_line_to(cr, 400, 300);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    rgba(cr, 0x000000, 0.18);
    cairo_set_line_width(cr, 5);
    cairo_move_to(cr, 540, 245);
    cairo_line_to(cr, 1030, 400);
    cairo_stroke(cr);

    /* the grip with its ribs, and the dark trigger */
    rgb(cr, 0xB7B2A6);
    cairo_move_to(cr, 870, 430);
    cairo_line_to(cr, 1005, 460);
    cairo_line_to(cr, 985, 885);
    cairo_line_to(cr, 822, 830);
    cairo_line_to(cr, 840, 560);
    cairo_close_path(cr);
    cairo_fill(cr);
    rgba(cr, 0x000000, 0.22);
    cairo_set_line_width(cr, 4);
    for (yy = 560; yy < 830; yy += 24) {
        cairo_move_to(cr, 836 + (yy - 560) * 0.0, yy);
        cairo_line_to(cr, 990, yy + 5);
    }
    cairo_stroke(cr);
    rgb(cr, 0x6E6C66);
    cairo_move_to(cr, 775, 465);
    cairo_line_to(cr, 835, 490);
    cairo_line_to(cr, 875, 585);
    cairo_line_to(cr, 845, 595);
    cairo_line_to(cr, 795, 548);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_restore(cr);
}

/* An empty port: a dashed socket outline and its row of pin holes. */
static void draw_socket(cairo_t *cr, const a7800_control *c)
{
    static const double dash[2] = { 6, 4 };
    const double x = px(c->x), y = py(c->y), w = c->w * SCALE, h = c->h * SCALE;
    int i;
    rgb(cr, A7800SESSION_SOCKET_RGB);
    cairo_set_line_width(cr, 2);
    cairo_set_dash(cr, dash, 2, 0);
    rrect(cr, x, y, w, h, 0.8 * SCALE);
    cairo_stroke(cr);
    cairo_set_dash(cr, NULL, 0, 0);
    for (i = 0; i < 9; i++) {
        disc(cr, x + w * (0.2 + 0.075 * i), y + 0.9 * SCALE, 2.2);
        cairo_fill(cr);
        disc(cr, x + w * (0.2 + 0.075 * i), y + h - 0.9 * SCALE, 2.2);
        cairo_fill(cr);
    }
}

static void draw_band(cairo_t *cr, const a7800_control *c)
{
    const double x = px(c->x), y = py(c->y), w = c->w * SCALE, h = c->h * SCALE;
    cairo_pattern_t *p = cairo_pattern_create_linear(0, y, 0, y + h);
    int i;
    stop_rgb(p, 0.0, A7800SESSION_BAND_HI_RGB);
    stop_rgb(p, 0.55, 0xC4C4C9);
    stop_rgb(p, 1.0, A7800SESSION_BAND_LO_RGB);
    cairo_set_source(cr, p);
    rrect(cr, x, y, w, h, 3);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    /* brushed hairlines */
    rgba(cr, 0xFFFFFF, 0.22);
    cairo_set_line_width(cr, 0.5);
    for (i = 3; i < h; i += 3) {
        cairo_move_to(cr, x + 3, y + i + 0.5);
        cairo_line_to(cr, x + w - 3, y + i + 0.5);
    }
    cairo_stroke(cr);
}

/* The rainbow line across the band: orange, violet, blue, green. */
static void draw_rainbow(cairo_t *cr, const a7800_control *c)
{
    const double x = px(c->x), y = py(c->y), w = c->w * SCALE, h = c->h * SCALE;
    cairo_pattern_t *p = cairo_pattern_create_linear(x, 0, x + w, 0);
    stop_rgb(p, 0.0, A7800SESSION_RAINBOW_ORANGE_RGB);
    stop_rgb(p, 0.40, A7800SESSION_RAINBOW_VIOLET_RGB);
    stop_rgb(p, 0.70, A7800SESSION_RAINBOW_BLUE_RGB);
    stop_rgb(p, 1.0, A7800SESSION_RAINBOW_GREEN_RGB);
    cairo_set_source(cr, p);
    cairo_rectangle(cr, x, y, w, h < 2 ? 2 : h);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
}

/* ---- parts ------------------------------------------------------------------------ */

/* A raised key cap: gradient face, dark edge, light top edge. Held or picked
 * parts get the held ring and a 35% fill. */
static void cap(cairo_t *cr, double x, double y, double w, double h, double r,
                unsigned base, unsigned hi, int lit)
{
    cairo_pattern_t *p;
    rgba(cr, 0x000000, 0.55);
    rrect(cr, x, y + 1.5, w, h, r);
    cairo_fill(cr);
    p = cairo_pattern_create_linear(x, y, x, y + h);
    stop_rgb(p, 0, hi);
    stop_rgb(p, 1, base);
    cairo_set_source(cr, p);
    rrect(cr, x, y, w, h, r);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(p);
    rgba(cr, 0xFFFFFF, 0.18);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    if (lit) {
        cairo_set_source_rgba(cr, HELD_R, HELD_G, HELD_B, 0.35);
        rrect(cr, x, y, w, h, r);
        cairo_fill(cr);
        rgb(cr, A7800SESSION_HELD_RGB);
        cairo_set_line_width(cr, 2);
        rrect(cr, x + 1, y + 1, w - 2, h - 2, r);
        cairo_stroke(cr);
    }
}

/* PAUSE / SELECT / RESET: flat black keys, white small-caps legends. */
static void draw_key(cairo_t *cr, const a7800_control *c, int lit, double alpha)
{
    const double x = px(c->x), y = py(c->y), w = c->w * SCALE, h = c->h * SCALE;
    char key[64], pad[64];

    cairo_push_group(cr);
    cap(cr, x, y, w, h, 3, A7800SESSION_KEY_RGB, 0x2A2A2E, lit);
    if (bindings_on() && c->target >= 0) {
        binding_text(c->target, key, sizeof key, pad, sizeof pad);
        rgb(cr, A7800SESSION_KEY_LABEL_RGB);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 7);
        cairo_move_to(cr, x + 4, y + 8.5);
        cairo_show_text(cr, c->label);
        rgb(cr, 0xEDEDE6);
        text_fit(cr, key, x + w / 2, y + h * (pad[0] ? 0.50 : 0.62), w - 6, 10,
                 CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        if (pad[0]) {
            rgb(cr, 0xA9A9B2);
            text_fit(cr, pad, x + w / 2, y + h * 0.82, w - 6, 8, CAIRO_FONT_SLANT_NORMAL,
                     CAIRO_FONT_WEIGHT_NORMAL);
        }
    } else {
        rgb(cr, A7800SESSION_KEY_LABEL_RGB);
        text_fit(cr, c->label, x + w / 2, y + h / 2, w - 8, 10, CAIRO_FONT_SLANT_NORMAL,
                 CAIRO_FONT_WEIGHT_BOLD);
    }
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, alpha);
}

/* POWER: drawn like the others but dimmed, and nothing to press. */
static void draw_power(cairo_t *cr, const a7800_control *c)
{
    const double x = px(c->x), y = py(c->y), w = c->w * SCALE, h = c->h * SCALE;
    cairo_push_group(cr);
    cap(cr, x, y, w, h, 3, A7800SESSION_KEY_RGB, 0x2A2A2E, 0);
    rgb(cr, A7800SESSION_KEY_LABEL_RGB);
    text_fit(cr, c->label, x + w / 2, y + h / 2, w - 8, 10, CAIRO_FONT_SLANT_NORMAL,
             CAIRO_FONT_WEIGHT_BOLD);
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, 0.4);
    /* the power LED's pinhole */
    rgb(cr, A7800SESSION_SWOOSH_RGB);
    disc(cr, x + w + 0.6 * SCALE, y + h / 2, 2.5);
    cairo_fill(cr);
}

/* A difficulty slide: B at the left, A at the right, the silver slider where
 * the switch is. A position, not a held light; picked in Map it gets the ring. */
static void draw_slide(cairo_t *cr, const a7800_control *c, int picked, double alpha)
{
    const int sw = c->target - A7800_TARGET_SWITCH(0);
    const gboolean at_a = a7800session_switch_get(g_session, sw) != 0;
    const double x = px(c->x), y = py(c->y), w = c->w * SCALE, h = c->h * SCALE;
    const double slot_w = 3.0 * SCALE, slot_h = 1.2 * SCALE;
    const double sx = x + (w - slot_w) / 2, sy = y + (h - slot_h) / 2;
    const double kw = 1.5 * SCALE, kh = slot_h - 4;
    const double kx = at_a ? sx + slot_w - kw - 2 : sx + 2;
    cairo_pattern_t *p;

    cairo_push_group(cr);
    rgb(cr, A7800SESSION_SLIDE_SLOT_RGB);
    rrect(cr, sx, sy, slot_w, slot_h, 3);
    cairo_fill(cr);
    rgba(cr, 0xFFFFFF, 0.14);
    cairo_set_line_width(cr, 1);
    rrect(cr, sx + 0.5, sy + 0.5, slot_w - 1, slot_h - 1, 3);
    cairo_stroke(cr);
    p = cairo_pattern_create_linear(0, sy + 2, 0, sy + 2 + kh);
    stop_rgb(p, 0.0, 0xE4E4E4);
    stop_rgb(p, 0.5, A7800SESSION_SLIDE_KNOB_RGB);
    stop_rgb(p, 1.0, 0x8E8E8E);
    cairo_set_source(cr, p);
    rrect(cr, kx, sy + 2, kw, kh, 2.5);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    /* the grip ridges */
    rgba(cr, 0x000000, 0.3);
    cairo_set_line_width(cr, 1);
    {
        int i;
        for (i = -1; i <= 1; i++) {
            cairo_move_to(cr, kx + kw / 2 + i * 4 + 0.5, sy + 5);
            cairo_line_to(cr, kx + kw / 2 + i * 4 + 0.5, sy + slot_h - 5);
        }
        cairo_stroke(cr);
    }
    /* the letters either side: the one the slider is at lights white */
    rgb(cr, !at_a ? 0xFFFFFF : 0x8C8C92);
    text_fit(cr, "B", x + (w - slot_w) / 4, y + h / 2, 12, 11, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    rgb(cr, at_a ? 0xFFFFFF : 0x8C8C92);
    text_fit(cr, "A", x + w - (w - slot_w) / 4, y + h / 2, 12, 11, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    /* what it is, or its binding, under it */
    if (bindings_on()) {
        char key[64], pad[64], both[140];
        binding_text(c->target, key, sizeof key, pad, sizeof pad);
        g_snprintf(both, sizeof both, "%s%s%s", key, pad[0] ? " / " : "", pad);
        rgb(cr, 0xEDEDE6);
        text_fit(cr, both, x + w / 2, y + h + 0.38 * SCALE, w, 9, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    } else {
        rgb(cr, 0x8C8C92);
        text_fit(cr, strstr(c->legend, "left") ? "LEFT DIFFICULTY" : "RIGHT DIFFICULTY",
                 x + w / 2, y + h + 0.38 * SCALE, w + 20, 8, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    }
    if (picked) {
        rgb(cr, A7800SESSION_HELD_RGB);
        cairo_set_line_width(cr, 2);
        rrect(cr, x + 1, y + 1, w - 2, h - 2, 4);
        cairo_stroke(cr);
    }
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, alpha);
}

/* One of the four directions' wedge of a stick, +-45 degrees. */
static void dir_wedge(cairo_t *cr, double cx, double cy, double r, int dir)
{
    static const double ang[4] = { -G_PI / 2, G_PI / 2, G_PI, 0 };   /* UP, DOWN, LEFT, RIGHT */
    cairo_move_to(cr, cx, cy);
    cairo_arc(cr, cx, cy, r, ang[dir] - G_PI / 4, ang[dir] + G_PI / 4);
    cairo_close_path(cr);
}

/* The ProLine's stick: the grey dish with its cross-shaped recess, and the
 * round cap shifted up to 0.9 u toward the held direction. */
static void draw_dish(cairo_t *cr, const a7800_control *c, double alpha)
{
    const double r = c->w * SCALE / 2, cx = px(c->x) + r, cy = py(c->y) + r;
    double dx, dy;
    cairo_pattern_t *p;
    int i;

    stick_dir(c->port, &dx, &dy);
    cairo_push_group(cr);
    /* its shadow and the plate, lit from the top left */
    rgba(cr, 0x000000, 0.4);
    disc(cr, cx + 1, cy + 2.5, r);
    cairo_fill(cr);
    p = cairo_pattern_create_radial(cx - r * 0.4, cy - r * 0.4, r * 0.1, cx, cy, r * 1.1);
    stop_rgb(p, 0.0, 0x8C8C8E);
    stop_rgb(p, 1.0, A7800SESSION_DISH_RGB);
    cairo_set_source(cr, p);
    disc(cr, cx, cy, r);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    rgb(cr, A7800SESSION_DISH_RECESS_RGB);
    cairo_set_line_width(cr, 1.5);
    disc(cr, cx, cy, r - 0.75);
    cairo_stroke(cr);
    /* the cross-shaped recess: four notches at N, S, E, W */
    rgb(cr, A7800SESSION_DISH_RECESS_RGB);
    cairo_rectangle(cr, cx - 0.78 * r, cy - 0.17 * r, 1.56 * r, 0.34 * r);
    cairo_fill(cr);
    cairo_rectangle(cr, cx - 0.17 * r, cy - 0.78 * r, 0.34 * r, 1.56 * r);
    cairo_fill(cr);
    /* each held direction tints its quadrant */
    for (i = 0; i < 4; i++) {
        static const int dirs[4] = { A7800_ACT_UP, A7800_ACT_DOWN, A7800_ACT_LEFT, A7800_ACT_RIGHT };
        if (g_map_state == A7800_TARGET_PORT(c->port, dirs[i])) {
            cairo_set_source_rgba(cr, HELD_R, HELD_G, HELD_B, 0.55);
            dir_wedge(cr, cx, cy, r, dirs[i]);
            cairo_fill(cr);
        } else if ((a7800session_buttons_held(g_session, c->port) >> dirs[i]) & 1) {
            cairo_set_source_rgba(cr, HELD_R, HELD_G, HELD_B, 0.35);
            dir_wedge(cr, cx, cy, r, dirs[i]);
            cairo_fill(cr);
        }
    }
    {
        const double sh = 0.9 * SCALE, capr = 1.0 * SCALE;
        const double kx = cx + dx * sh, ky = cy + dy * sh;
        /* the shaft under the cap, then the cap */
        rgb(cr, 0x4A4A4C);
        disc(cr, cx + dx * sh * 0.5, cy + dy * sh * 0.5 + 1, capr * 0.75);
        cairo_fill(cr);
        rgba(cr, 0x000000, 0.35);
        disc(cr, kx + 1, ky + 2, capr);
        cairo_fill(cr);
        p = cairo_pattern_create_radial(kx - capr * 0.3, ky - capr * 0.3, capr * 0.1, kx, ky, capr);
        if (dx || dy) {
            stop_rgb(p, 0.0, 0xFFD08A);
            stop_rgb(p, 1.0, A7800SESSION_HELD_RGB);
        } else {
            stop_rgb(p, 0.0, A7800SESSION_CAP_HI_RGB);
            stop_rgb(p, 1.0, A7800SESSION_CAP_RGB);
        }
        cairo_set_source(cr, p);
        disc(cr, kx, ky, capr);
        cairo_fill(cr);
        cairo_pattern_destroy(p);
        rgba(cr, 0x000000, 0.25);
        cairo_set_line_width(cr, 1);
        disc(cr, kx, ky, capr);
        cairo_stroke(cr);
    }
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, alpha);
}

/* The CX40's ring, ribbed boot and octagonal shaft cap (DESIGN-atari.md
 * section 2): the 36 ticks nearest the held direction light. */
static void draw_ring(cairo_t *cr, const a7800_control *c, double alpha)
{
    const double r = c->w * SCALE / 2 * (3.0 / 2.1), cx = px(c->x) + c->w * SCALE / 2,
                 cy = py(c->y) + c->h * SCALE / 2;
    double dx, dy, held_ang = 0;
    gboolean held;
    int i;

    stick_dir(c->port, &dx, &dy);
    held = dx || dy;
    if (held)
        held_ang = atan2(dy, dx);
    cairo_push_group(cr);
    rgb(cr, A7800SESSION_CX40_RING_RGB);
    disc(cr, cx, cy, r);
    cairo_fill(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cr, 1.8);
    for (i = 0; i < 36; i++) {
        const double a = i * G_PI / 18.0;
        double diff = fmod(fabs(a - held_ang), 2 * G_PI);
        if (diff > G_PI)
            diff = 2 * G_PI - diff;
        if (held && diff < 0.37)
            rgb(cr, A7800SESSION_HELD_RGB);
        else
            rgb(cr, A7800SESSION_CX40_TICK_RGB);
        cairo_move_to(cr, cx + cos(a) * r * 0.86, cy + sin(a) * r * 0.86);
        cairo_line_to(cr, cx + cos(a) * r * 0.95, cy + sin(a) * r * 0.95);
        cairo_stroke(cr);
    }
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
    for (i = 0; i < 3; i++) {
        static const double ring[3] = { 2.1, 1.7, 1.3 };
        rgb(cr, i % 2 ? 0x262626 : 0x151515);
        disc(cr, cx + dx * 0.15 * SCALE, cy + dy * 0.15 * SCALE, ring[i] * SCALE);
        cairo_fill(cr);
    }
    /* the octagon cap, up to 1.0 u toward the tilt */
    {
        const double kx = cx + dx * SCALE, ky = cy + dy * SCALE, o = 0.85 * SCALE;
        for (i = 0; i < 8; i++) {
            const double a = G_PI / 8 + i * G_PI / 4;
            if (i) cairo_line_to(cr, kx + cos(a) * o, ky + sin(a) * o);
            else cairo_move_to(cr, kx + cos(a) * o, ky + sin(a) * o);
        }
        cairo_close_path(cr);
        rgb(cr, 0x0C0C0C);
        cairo_fill_preserve(cr);
        if (held) rgb(cr, A7800SESSION_HELD_RGB);
        else rgb(cr, A7800SESSION_CX40_BEVEL_RGB);
        cairo_set_line_width(cr, 0.1 * SCALE + 0.5);
        cairo_stroke(cr);
    }
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, alpha);
}

/* The stick's bindings (Map and Show bindings) and the picked direction (CX40
 * only: the ProLine tints its dish). Drawn over the stick. */
static void draw_stick_overlay(cairo_t *cr, const a7800_control *c)
{
    static const int dirs[4] = { A7800_ACT_UP, A7800_ACT_RIGHT, A7800_ACT_DOWN, A7800_ACT_LEFT };
    static const double dx[4] = { 0, 1, 0, -1 }, dy[4] = { -1, 0, 1, 0 };
    const double r = c->w * SCALE / 2, cx = px(c->x) + r, cy = py(c->y) + r;
    const gboolean proline = c->device == A7800_CTRL_PROLINE;
    int i;

    for (i = 0; i < 4; i++) {
        const int target = A7800_TARGET_PORT(c->port, dirs[i]);
        char key[64], pad[64];
        if (!proline && g_map_state == target) {
            cairo_set_source_rgba(cr, HELD_R, HELD_G, HELD_B, 0.4);
            dir_wedge(cr, cx, cy, r * 1.4, dirs[i]);
            cairo_fill(cr);
        }
        if (bindings_on()) {
            const double ex = cx + dx[i] * r * (proline ? 0.62 : 0.9);
            const double ey = cy + dy[i] * r * (proline ? 0.62 : 0.9);
            const double alpha = g_map_state >= 0 && g_map_state != target ? 0.4 : 1.0;
            binding_text(target, key, sizeof key, pad, sizeof pad);
            if (proline)
                rgba(cr, 0x111111, alpha);
            else
                rgba(cr, 0xEDEDE6, alpha);
            text_fit(cr, key, ex, ey, i % 2 ? 24 : 34, 9, CAIRO_FONT_SLANT_NORMAL,
                     CAIRO_FONT_WEIGHT_BOLD);
        }
    }
}

/* A fire button: the ProLine's red dome in its recess, or the CX40's in the
 * corner. Held (or picked) it gets the held ring and a darker fill. */
static void draw_fire(cairo_t *cr, const a7800_control *c, int lit, double alpha)
{
    const gboolean proline = c->device == A7800_CTRL_PROLINE;
    const double cx = px(c->x + c->w / 2), cy = py(c->y + c->h / 2);
    const double r = (proline ? 1.25 : 0.8) * SCALE;
    cairo_pattern_t *p;
    char key[64], pad[64];

    cairo_push_group(cr);
    p = cairo_pattern_create_radial(cx - r * 0.35, cy - r * 0.4, r * 0.1, cx, cy, r);
    if (lit) {
        stop_rgb(p, 0.0, A7800SESSION_BUTTON_HI_RGB);
        stop_rgb(p, 1.0, A7800SESSION_HELD_BUTTON_FILL_RGB);
    } else {
        stop_rgb(p, 0.0, proline ? A7800SESSION_BUTTON_HI_RGB : A7800SESSION_CX40_FIRE_HI_RGB);
        stop_rgb(p, 1.0, proline ? A7800SESSION_BUTTON_RGB : A7800SESSION_CX40_FIRE_RGB);
    }
    cairo_set_source(cr, p);
    disc(cr, cx, cy, r);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    if (lit) {
        cairo_set_source_rgba(cr, HELD_R, HELD_G, HELD_B, 0.35);
        disc(cr, cx, cy, r);
        cairo_fill(cr);
        rgb(cr, A7800SESSION_HELD_RGB);
        cairo_set_line_width(cr, 2);
        disc(cr, cx, cy, r + 1);
        cairo_stroke(cr);
    } else {
        rgba(cr, 0xFFFFFF, 0.5);
        disc(cr, cx - r * 0.35, cy - r * 0.4, r * 0.16);
        cairo_fill(cr);
    }
    if (bindings_on()) {
        binding_text(c->target, key, sizeof key, pad, sizeof pad);
        rgb(cr, 0xFFFFFF);
        text_fit(cr, key, cx, cy, r * 1.6, 9, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    }
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, alpha);
}

static void draw_all(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer d)
{
    GdkRGBA fg;
    int i;
    (void)d; (void)height;

    gtk_widget_get_color(GTK_WIDGET(area), &fg);
    g_offx = (width - g_lw * SCALE) / 2;
    for (i = 0; i < g_count; i++) {
        const a7800_control *c = &g_layout[i];
        const int held = c->target >= 0 && c->kind != A7800_CTL_SWITCH &&
                         a7800session_target_held(g_session, c->target);
        const int picked = g_map_state >= 0 && c->target == g_map_state;
        const double alpha = g_map_state >= 0 && !picked ? 0.4 : 1.0;

        switch (c->kind) {
        case A7800_CTL_CAPTION:
            draw_caption(cr, c, &fg);
            break;
        case A7800_CTL_BODY:
            if (!strcmp(c->label, "ProLine")) draw_proline(cr, c);
            else if (!strcmp(c->label, "CX40")) draw_cx40(cr, c);
            else if (!strcmp(c->label, "XG-1")) draw_gun(cr, c);
            else if (!strcmp(c->label, "Socket")) draw_socket(cr, c);
            else if (!strcmp(c->label, "Console")) {
                rgb(cr, A7800SESSION_CASE_BEVEL_RGB);
                rrect(cr, px(c->x), py(c->y), c->w * SCALE, c->h * SCALE, 0.7 * SCALE);
                cairo_fill(cr);
                rgb(cr, A7800SESSION_CASE_RGB);
                rrect(cr, px(c->x) + 1.5, py(c->y) + 1.5, c->w * SCALE - 3, c->h * SCALE - 3, 0.6 * SCALE);
                cairo_fill(cr);
            } else if (!strcmp(c->label, "Band")) draw_band(cr, c);
            else if (!strcmp(c->label, "Rainbow")) draw_rainbow(cr, c);
            else if (!strcmp(c->label, "POWER")) draw_power(cr, c);
            break;
        case A7800_CTL_STICK: {
            /* dimmed in Map mode unless the picked target is one of its directions */
            const int mine = g_map_state >= A7800_TARGET_PORT(c->port, 0) &&
                             g_map_state <= A7800_TARGET_PORT(c->port, A7800_ACT_RIGHT);
            const double a = g_map_state >= 0 && !mine ? 0.4 : 1.0;
            if (c->device == A7800_CTRL_PROLINE) draw_dish(cr, c, a);
            else draw_ring(cr, c, a);
            draw_stick_overlay(cr, c);
            break;
        }
        case A7800_CTL_ROUND_BUTTON:
            draw_fire(cr, c, held || picked, alpha);
            break;
        case A7800_CTL_BUTTON:
            draw_key(cr, c, held || picked, alpha);
            break;
        case A7800_CTL_SWITCH:
            draw_slide(cr, c, picked, alpha);
            break;
        }
    }
}

/* ---- sizing ---------------------------------------------------------------- */

/* The layout for the controllers plugged in now. Its extent is the same for
 * every type, so only the contents change. */
static void relayout(void)
{
    g_view[0] = a7800session_view_type(g_session, 0);
    g_view[1] = a7800session_view_type(g_session, 1);
    g_layout = a7800session_controls_layout(g_view[0], g_view[1], &g_count, &g_lw, &g_lh);
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(g_area), (int)(g_lw * SCALE + 2 * MARGIN));
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g_area), (int)(g_lh * SCALE + 2 * MARGIN));
}

/* A signature of everything that decides how the canvas looks. */
static guint64 paint_signature(void)
{
    guint64 sig = (guint64)(g_view[0] * 8 + g_view[1]);
    int port, sw;
    for (port = 0; port < 2; port++)
        sig = sig * 1099511628211ull + a7800session_buttons_held(g_session, port);
    for (sw = 0; sw < A7800_SW_COUNT; sw++)
        sig = sig * 1099511628211ull + (guint64)(a7800session_switch_held(g_session, sw) + 1);
    return sig;
}

/* Every frame while the window is on screen: follow what the console sees held
 * (keyboard, gamepads and this window together), and the layout when a port's
 * type or the game's changes it. */
static gboolean tick_cb(GtkWidget *w, GdkFrameClock *clock, gpointer d)
{
    guint64 sig;
    (void)w; (void)clock; (void)d;
    if (a7800session_view_type(g_session, 0) != g_view[0] ||
        a7800session_view_type(g_session, 1) != g_view[1]) {
        release_pointer();
        relayout();
    }
    sig = paint_signature();
    if (sig != g_last_sig) {
        g_last_sig = sig;
        redraw();
    }
    return G_SOURCE_CONTINUE;
}

/* ---- the pointer -------------------------------------------------------------- */

static void stick_to_pointer(const a7800_control *c, double x, double y)
{
    const double rad = c->w * SCALE / 2;
    const double cx = px(c->x) + rad, cy = py(c->y) + rad;
    a7800session_pointer_set(g_session, c->port,
        a7800session_stick_from_vector((float)(x - cx), (float)(y - cy), (float)rad));
}

static void on_drag_begin(GtkGestureDrag *g, double x, double y, gpointer d)
{
    const int idx = a7800session_controls_hit(g_view[0], g_view[1], (float)((x - g_offx) / SCALE),
                                              (float)((y - MARGIN) / SCALE));
    const a7800_control *c = part(idx);
    (void)g; (void)d;
    if (!c)
        return;
    if (g_map_state != -2) {
        int target = map_target_at(idx, x, y);
        if (target >= 0)
            set_map_state(target);
        return;
    }
    release_pointer();
    g_drag_idx = idx;
    if (c->kind == A7800_CTL_STICK)
        stick_to_pointer(c, x, y);
    else
        a7800session_press(g_session, c->target, 1);
    redraw();
}

static void on_drag_update(GtkGestureDrag *g, double dx, double dy, gpointer d)
{
    const a7800_control *c = part(g_drag_idx);
    double sx, sy;
    (void)d;
    if (!c || c->kind != A7800_CTL_STICK || g_map_state != -2)
        return;
    gtk_gesture_drag_get_start_point(g, &sx, &sy);
    stick_to_pointer(c, sx + dx, sy + dy);
}

static void on_drag_end(GtkGestureDrag *g, double dx, double dy, gpointer d)
{
    const a7800_control *c = part(g_drag_idx);
    (void)g; (void)dx; (void)dy; (void)d;
    if (c && c->kind == A7800_CTL_STICK)
        a7800session_pointer_set(g_session, c->port, 0);
    else if (c && c->kind != A7800_CTL_SWITCH)
        a7800session_press(g_session, c->target, 0);
    release_pointer();
}

static void on_cancelled(GtkGesture *g, GdkEventSequence *seq, gpointer d)
{
    (void)g; (void)seq; (void)d;
    release_pointer();
}

/* ---- the ports' types ------------------------------------------------------ */

static void on_type_changed(GObject *o, GParamSpec *ps, gpointer user_data)
{
    const int port = GPOINTER_TO_INT(user_data);
    (void)ps;
    if (g_syncing)
        return;
    a7800session_set_port_type(g_session, port, (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(o)));
}

/* What each port is set to, and what AUTO resolves to. */
static void refresh_types(void)
{
    int port;
    for (port = 0; port < 2; port++) {
        const int type = a7800session_port_type(g_session, port);
        char text[64];
        if ((int)gtk_drop_down_get_selected(GTK_DROP_DOWN(g_type_combo[port])) != type) {
            g_syncing = TRUE;
            gtk_drop_down_set_selected(GTK_DROP_DOWN(g_type_combo[port]), (guint)type);
            g_syncing = FALSE;
        }
        if (type == A7800_CTRL_AUTO)
            g_snprintf(text, sizeof text, "now %s",
                       a7800_ctrl_type_name(a7800session_port_detected(g_session, port)));
        else
            text[0] = '\0';
        if (strcmp(gtk_label_get_text(GTK_LABEL(g_type_now[port])), text) != 0)
            gtk_label_set_text(GTK_LABEL(g_type_now[port]), text);
    }
}

static GtkWidget *build_type_half(int port)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new("Plugged in:");
    const char *names[A7800_CTRL_COUNT + 1];
    int i;

    gtk_widget_add_css_class(label, "dim-label");
    for (i = 0; i < A7800_CTRL_COUNT; i++)
        names[i] = a7800_ctrl_type_name(i);
    names[A7800_CTRL_COUNT] = NULL;
    g_type_combo[port] = gtk_drop_down_new_from_strings(names);
    gtk_widget_set_focus_on_click(g_type_combo[port], FALSE);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(g_type_combo[port]),
                               (guint)a7800session_port_type(g_session, port));
    g_signal_connect(g_type_combo[port], "notify::selected",
                     G_CALLBACK(on_type_changed), GINT_TO_POINTER(port));
    g_type_now[port] = gtk_label_new("");
    gtk_widget_add_css_class(g_type_now[port], "dim-label");
    gtk_label_set_width_chars(GTK_LABEL(g_type_now[port]), 12);
    gtk_label_set_xalign(GTK_LABEL(g_type_now[port]), 0);
    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), g_type_combo[port]);
    gtk_box_append(GTK_BOX(box), g_type_now[port]);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    return box;
}

/* ---- gamepads ------------------------------------------------------------- */

static void drives_text(int idx, char *text, int size)
{
    const int effective = a7800session_gamepad_effective_port(g_session, idx);
    if (effective >= 0) g_snprintf(text, size, "drives player %d", effective + 1);
    else g_snprintf(text, size, "drives nothing");
}

static void refresh_pad_ports(void)
{
    int i;
    for (i = 0; i < g_npad_rows; i++) {
        char text[40];
        drives_text(i, text, sizeof text);
        if (strcmp(gtk_label_get_text(GTK_LABEL(g_pad_port[i])), text) != 0)
            gtk_label_set_text(GTK_LABEL(g_pad_port[i]), text);
    }
}

static void on_assign_changed(GObject *o, GParamSpec *ps, gpointer user_data)
{
    const int idx = GPOINTER_TO_INT(user_data);
    const guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(o));
    (void)ps;
    if (g_syncing)
        return;
    /* 0 = automatic (connection order), 1 = player 1, 2 = player 2 */
    a7800session_gamepad_assign(g_session, idx, sel == 0 ? -1 : (int)sel - 1);
    refresh_pad_ports();
}

/* Rebuilt whenever a pad comes or goes (the session's generation counter): one
 * row per pad, its name, the player it drives and the player it is set to. */
static void refresh_pads(void)
{
    static const char *const choices[] = { "Automatic", "Player 1", "Player 2", NULL };
    GtkWidget *child;
    int n, i;

    while ((child = gtk_widget_get_first_child(g_pad_box)) != NULL)
        gtk_box_remove(GTK_BOX(g_pad_box), child);
    g_npad_rows = 0;

    n = a7800session_gamepad_count(g_session);
    if (n == 0) {
        GtkWidget *none = gtk_label_new("No gamepads connected");
        gtk_widget_add_css_class(none, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(none), 0);
        gtk_box_append(GTK_BOX(g_pad_box), none);
        return;
    }
    for (i = 0; i < n; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *name, *dd, *eff;
        char text[128], drives[40];
        const int assign = a7800session_gamepad_assignment(g_session, i);

        a7800session_gamepad_name(g_session, i, text, sizeof text);
        name = gtk_label_new(text);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(name), 24);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 40);
        gtk_widget_set_tooltip_text(name, text);
        dd = gtk_drop_down_new_from_strings(choices);
        g_syncing = TRUE;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), assign < 0 || assign > 1 ? 0 : (guint)assign + 1);
        g_syncing = FALSE;
        gtk_widget_set_focus_on_click(dd, FALSE);
        g_signal_connect(dd, "notify::selected", G_CALLBACK(on_assign_changed), GINT_TO_POINTER(i));
        drives_text(i, drives, sizeof drives);
        eff = gtk_label_new(drives);
        gtk_widget_add_css_class(eff, "dim-label");
        gtk_label_set_width_chars(GTK_LABEL(eff), 16);
        gtk_label_set_xalign(GTK_LABEL(eff), 0);
        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), eff);
        gtk_box_append(GTK_BOX(row), dd);
        gtk_box_append(GTK_BOX(g_pad_box), row);
        if (g_npad_rows < MAX_PADS)
            g_pad_port[g_npad_rows++] = eff;
    }
}

/* Every 250 ms while the window is on screen: hot-plugged pads, the ports'
 * types and what AUTO resolved to. */
static gboolean info_tick(gpointer d)
{
    unsigned gen;
    (void)d;
    if (!g_window || !gtk_widget_get_visible(g_window)) return G_SOURCE_CONTINUE;
    refresh_types();
    gen = a7800session_gamepad_generation(g_session);
    if (gen != g_pad_generation) {
        g_pad_generation = gen;
        refresh_pads();
    } else {
        refresh_pad_ports();
    }
    return G_SOURCE_CONTINUE;
}

/* ---- keyboard ------------------------------------------------------------- */

static gboolean on_key(guint keyval, guint code, GdkModifierType st, gboolean down)
{
    /* an open drop-down (a type or a gamepad's player) has the arrows and Return */
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(g_window));
    if (focus && gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER))
        return FALSE;

    /* F9, F11, F12 and Ctrl chords are never Map's: they go where they always do */
    if (g_map_state != -2 && !(keyval == GDK_KEY_F9 || keyval == GDK_KEY_F11 ||
                               keyval == GDK_KEY_F12 || (st & GDK_CONTROL_MASK))) {
        if (g_swallow_code && code == g_swallow_code) {
            if (!down) g_swallow_code = 0;
            return TRUE;
        }
        if (!down)
            return TRUE;
        if (keyval == GDK_KEY_Escape) {
            g_swallow_code = code;
            set_map_state(g_map_state >= 0 ? -1 : -2);
            return TRUE;
        }
        if (g_map_state < 0)
            return TRUE;           /* armed, nothing picked: keys do nothing */
        {
            guint32 keysym = code >= 8 ? a7800session_keysym_from_evdev(code - 8) : 0;
            char stolen[128], msg[200], name[32];
            if (!keysym) keysym = keyval;
            if (a7800session_key_reserved(keysym)) {
                set_hint("F9, F11 and F12 can't be bound");
                g_swallow_code = code;
                return TRUE;
            }
            stolen[0] = '\0';
            a7800session_binding_set_key(g_session, g_map_state, keysym, stolen, sizeof stolen);
            a7800session_keysym_name(keysym, name, sizeof name);
            g_swallow_code = code;
            set_map_state(-1);     /* back to armed */
            if (stolen[0])
                g_snprintf(msg, sizeof msg, "Bound %s (was %s)", name, stolen);
            else
                g_snprintf(msg, sizeof msg, "Bound %s", name);
            set_hint(msg);
            return TRUE;
        }
    }
    /* Otherwise exactly what the main window does with it. */
    if (!g_parent) return FALSE;
    return a7800_window_key(A7800_WINDOW(g_parent), keyval, code, st, down);
}

static gboolean on_key_pressed(GtkEventControllerKey *c, guint keyval,
                               guint code, GdkModifierType st, gpointer d)
{
    (void)c; (void)d;
    return on_key(keyval, code, st, TRUE);
}

static void on_key_released(GtkEventControllerKey *c, guint keyval,
                            guint code, GdkModifierType st, gpointer d)
{
    (void)c; (void)d;
    on_key(keyval, code, st, FALSE);
}

/* Keys and mouse buttons held into a focus change must not stay down: the
 * release goes to another window. */
static void on_focus_leave(GtkEventControllerFocus *c, gpointer d)
{
    (void)c; (void)d;
    release_pointer();
    a7800session_release_all(g_session);
    g_swallow_code = 0;
}

static void on_active_changed(GObject *o, GParamSpec *ps, gpointer d)
{
    (void)ps; (void)d;
    if (!gtk_window_is_active(GTK_WINDOW(o))) {
        release_pointer();
        a7800session_release_all(g_session);
    }
}

/* ---- the window ----------------------------------------------------------- */

static void on_map_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    set_map_state(g_map_state == -2 ? -1 : -2);
}

static void on_clear_clicked(GtkButton *b, gpointer d)
{
    char msg[160];
    (void)b; (void)d;
    if (g_map_state < 0) return;
    a7800session_binding_clear(g_session, g_map_state);
    g_snprintf(msg, sizeof msg, "Cleared %s", a7800_target_name(g_map_state));
    set_hint(msg);
    redraw();
}

static void on_defaults_clicked(GtkButton *b, gpointer d)
{
    int i;
    const int n = a7800session_gamepad_count(g_session);
    (void)b; (void)d;
    a7800session_bindings_reset(g_session);
    for (i = 0; i < n; i++)
        a7800session_gamepad_assign(g_session, i, -1);
    refresh_pads();
    set_hint("Back to the defaults");
    redraw();
}

static void on_show_toggled(GtkCheckButton *b, gpointer d)
{
    (void)d;
    g_show_bindings = gtk_check_button_get_active(b);
    a7800session_set_int(g_session, "controllers_show_bindings", g_show_bindings);
    redraw();
}

/* The system keys are not on the picture: picking one here arms Map for it. */
static void on_system_changed(GObject *o, GParamSpec *ps, gpointer d)
{
    const guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(o));
    (void)ps; (void)d;
    if (g_syncing || sel == 0 || sel == GTK_INVALID_LIST_POSITION)
        return;
    set_map_state(A7800_TARGET_SYSACT((int)sel - 1));
    g_syncing = TRUE;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(o), 0);
    g_syncing = FALSE;
}

static void hide_window(void)
{
    set_map_state(-2);
    release_pointer();
    gtk_widget_set_visible(g_window, FALSE);
}

static gboolean on_close(GtkWindow *w, gpointer d)
{
    (void)w; (void)d;
    hide_window();
    return TRUE;
}

/* The info timer runs only while the window is on screen: closing only hides
 * it, and a hidden window has nobody to update. */
static void on_window_map(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    release_pointer();
    if (!g_info_timer) g_info_timer = g_timeout_add(250, info_tick, NULL);
}

static void on_window_unmap(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    release_pointer();
    if (g_info_timer) g_source_remove(g_info_timer);
    g_info_timer = 0;
}

static void on_destroy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (g_capture_timer) g_source_remove(g_capture_timer);
    if (g_info_timer) g_source_remove(g_info_timer);
    g_capture_timer = g_info_timer = 0;
    g_window = NULL;
    g_area = NULL;
    g_npad_rows = 0;
    g_map_state = -2;
    g_view[0] = g_view[1] = -1;
}

static GtkWidget *plain_button(const char *label, GCallback cb)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_widget_set_focus_on_click(b, FALSE);
    g_signal_connect(b, "clicked", cb, NULL);
    return b;
}

static void build_window(GtkWindow *parent)
{
    GtkWidget *root, *maprow, *typerow, *toolbar, *header, *padtitle, *padscroll;
    GtkStringList *system;
    GtkEventController *keys, *focus;
    GtkGesture *drag;
    int i;

    g_parent = parent;
    g_window = adw_window_new();
    gtk_window_set_title(GTK_WINDOW(g_window), "Controllers (F9)");
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(g_window), parent);
        /* Same app-id as the main window, so the shell groups and icons it. */
        gtk_window_set_application(GTK_WINDOW(g_window), gtk_window_get_application(parent));
    }
    gtk_window_set_destroy_with_parent(GTK_WINDOW(g_window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(g_window), FALSE);
    g_signal_connect(g_window, "close-request", G_CALLBACK(on_close), NULL);
    g_signal_connect(g_window, "destroy", G_CALLBACK(on_destroy), NULL);
    g_signal_connect(g_window, "notify::is-active", G_CALLBACK(on_active_changed), NULL);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(root, 8);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);

    g_area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_area), draw_all, NULL, NULL);
    gtk_widget_set_halign(g_area, GTK_ALIGN_CENTER);
    gtk_widget_set_focusable(g_area, FALSE);
    drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), NULL);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), NULL);
    g_signal_connect(drag, "cancel", G_CALLBACK(on_cancelled), NULL);
    gtk_widget_add_controller(g_area, GTK_EVENT_CONTROLLER(drag));
    gtk_widget_add_tick_callback(g_area, tick_cb, NULL, NULL);
    gtk_box_append(GTK_BOX(root), g_area);
    relayout();

    /* each port's type, under its picture */
    typerow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_set_homogeneous(GTK_BOX(typerow), TRUE);
    gtk_widget_set_size_request(typerow, (int)(g_lw * SCALE + 2 * MARGIN), -1);
    gtk_widget_set_halign(typerow, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(typerow), build_type_half(0));
    gtk_box_append(GTK_BOX(typerow), build_type_half(1));
    gtk_box_append(GTK_BOX(root), typerow);

    maprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_map_button = plain_button("Map", G_CALLBACK(on_map_clicked));
    gtk_widget_set_size_request(g_map_button, 72, -1);
    g_clear_button = plain_button("Clear", G_CALLBACK(on_clear_clicked));
    g_defaults_button = plain_button("Defaults", G_CALLBACK(on_defaults_clicked));
    g_show_check = gtk_check_button_new_with_label("Show bindings");
    g_show_bindings = a7800session_get_int(g_session, "controllers_show_bindings", 0) != 0;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_show_check), g_show_bindings);
    gtk_widget_set_focus_on_click(g_show_check, FALSE);
    g_signal_connect(g_show_check, "toggled", G_CALLBACK(on_show_toggled), NULL);
    system = gtk_string_list_new(NULL);
    gtk_string_list_append(system, "System keys\xe2\x80\xa6");
    for (i = 0; i < A7800_SYSACT_COUNT; i++)
        gtk_string_list_append(system, a7800_target_name(A7800_TARGET_SYSACT(i)));
    g_system_dd = gtk_drop_down_new(G_LIST_MODEL(system), NULL);
    gtk_widget_set_focus_on_click(g_system_dd, FALSE);
    gtk_widget_set_halign(g_system_dd, GTK_ALIGN_END);
    gtk_widget_set_hexpand(g_system_dd, TRUE);
    gtk_widget_set_tooltip_text(g_system_dd,
        "Reboot to CONFIG and the debugger stop are not on the picture: pick one to remap it");
    g_signal_connect(g_system_dd, "notify::selected", G_CALLBACK(on_system_changed), NULL);
    gtk_box_append(GTK_BOX(maprow), g_map_button);
    gtk_box_append(GTK_BOX(maprow), g_clear_button);
    gtk_box_append(GTK_BOX(maprow), g_defaults_button);
    gtk_box_append(GTK_BOX(maprow), g_show_check);
    gtk_box_append(GTK_BOX(maprow), g_system_dd);
    gtk_box_append(GTK_BOX(root), maprow);
    g_object_unref(system);

    g_map_hint = gtk_label_new("");
    gtk_widget_add_css_class(g_map_hint, "dim-label");
    gtk_widget_set_hexpand(g_map_hint, TRUE);
    /* a long hint must not widen the window */
    gtk_label_set_ellipsize(GTK_LABEL(g_map_hint), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(g_map_hint), 24);
    gtk_label_set_max_width_chars(GTK_LABEL(g_map_hint), 90);
    gtk_label_set_xalign(GTK_LABEL(g_map_hint), 0);
    gtk_box_append(GTK_BOX(root), g_map_hint);

    padtitle = gtk_label_new("Gamepads");
    gtk_widget_add_css_class(padtitle, "heading");
    gtk_label_set_xalign(GTK_LABEL(padtitle), 0);
    gtk_box_append(GTK_BOX(root), padtitle);
    g_pad_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    /* two pad rows tall whatever is plugged in (more scroll), so a pad coming
     * or going never resizes the window */
    padscroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(padscroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(padscroll), FALSE);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(padscroll), 84);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(padscroll), 84);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(padscroll), g_pad_box);
    gtk_box_append(GTK_BOX(root), padscroll);

    header = adw_header_bar_new();
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), a7800_help_button("controllers"));
    a7800_help_set_context(GTK_WINDOW(g_window), "controllers");
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root);
    adw_window_set_content(ADW_WINDOW(g_window), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), NULL);
    gtk_widget_add_controller(g_window, keys);

    focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), NULL);
    gtk_widget_add_controller(g_window, focus);

    g_signal_connect(g_window, "map", G_CALLBACK(on_window_map), NULL);
    g_signal_connect(g_window, "unmap", G_CALLBACK(on_window_unmap), NULL);
    set_map_state(-2);
}

void a7800_controllers_window_show(GtkWindow *parent, a7800session *session)
{
    g_session = session;
    if (!g_window)
        build_window(parent);
    else if (parent)
        g_parent = parent;
    if (a7800session_view_type(g_session, 0) != g_view[0] ||
        a7800session_view_type(g_session, 1) != g_view[1])
        relayout();
    refresh_types();
    g_pad_generation = a7800session_gamepad_generation(g_session);
    refresh_pads();
    gtk_window_present(GTK_WINDOW(g_window));
}

void a7800_controllers_window_toggle(GtkWindow *parent, a7800session *session)
{
    g_session = session;
    if (g_window && gtk_widget_get_visible(g_window))
        hide_window();
    else
        a7800_controllers_window_show(parent, session);
}

gboolean a7800_controllers_window_is_visible(void)
{
    return g_window && gtk_widget_get_visible(g_window);
}

/* Bindings changed outside the window (Preferences' Reset Controls): redraw it
 * and rebuild what the pad rows offer. */
void a7800_controllers_window_refresh(void)
{
    if (!g_window)
        return;
    g_pad_generation = (unsigned)-1;
    redraw();
}
