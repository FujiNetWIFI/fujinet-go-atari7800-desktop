/*
 * display.c -- see display.h.
 *
 * Three things here are load bearing and none is obvious:
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
 *  - The light gun is the mouse. While a port has an XG-1 the pointer over
 *    the picture is a crosshair, and where it is (in the frame's own pixels,
 *    letterboxing and scaling undone) and which buttons are down go to the
 *    session every time either changes. Otherwise the pointer is left alone.
 *
 * MARIA's frame is 320 pixels wide and 224 (NTSC) or 260 (PAL) lines tall;
 * the texture is rebuilt at the frame's own height each time, so nothing
 * here assumes either.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "display.h"

#include <string.h>

#define FB_W A7800SESSION_FB_WIDTH
#define FB_MAX_H A7800SESSION_FB_MAX_HEIGHT

struct _A7800Display {
    GtkWidget parent_instance;

    a7800session *session;
    guint32 *fb;           /* XRGB8888 straight from the session */
    guint32 *bgra;         /* with alpha forced opaque for GdkMemoryTexture */
    int height;
    GdkTexture *texture;
    guint64 serial;
    guint tick_id;
    gboolean tv_aspect;
    gboolean smooth;

    /* where the picture was last drawn, for mapping the pointer */
    double pic_x, pic_y, pic_w, pic_h;
    /* the light gun */
    gboolean gun;          /* a port has one: crosshair and forwarding */
    double ptr_x, ptr_y;
    gboolean ptr_in;
    unsigned buttons;
};

G_DEFINE_FINAL_TYPE(A7800Display, a7800_display, GTK_TYPE_WIDGET)

static void rebuild_texture(A7800Display *self)
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

/* ---- the light gun ---------------------------------------------------------- */

static void send_pointer(A7800Display *self)
{
    int x = 0, y = 0, inside = 0;

    if (self->ptr_in && self->pic_w > 0 && self->pic_h > 0 && self->height > 0) {
        double fx = (self->ptr_x - self->pic_x) * FB_W / self->pic_w;
        double fy = (self->ptr_y - self->pic_y) * self->height / self->pic_h;
        if (fx >= 0 && fx < FB_W && fy >= 0 && fy < self->height) {
            x = (int)fx;
            y = (int)fy;
            inside = 1;
        }
    }
    a7800session_pointer(self->session, x, y, inside, self->buttons);
}

/* Follows the session: a game with a gun on AUTO, or Preferences, can plug
 * one in or take it away at any time. */
static void sync_gun(A7800Display *self)
{
    gboolean gun = a7800session_lightgun_active(self->session) != 0;
    if (gun == self->gun)
        return;
    self->gun = gun;
    gtk_widget_set_cursor_from_name(GTK_WIDGET(self), gun ? "crosshair" : NULL);
    if (gun) {
        send_pointer(self);
    } else {
        self->buttons = 0;
        a7800session_pointer(self->session, 0, 0, 0, 0);
    }
}

static void on_motion(GtkEventControllerMotion *c, double x, double y, gpointer user_data)
{
    A7800Display *self = user_data;
    (void)c;
    self->ptr_x = x;
    self->ptr_y = y;
    self->ptr_in = TRUE;
    if (self->gun)
        send_pointer(self);
}

static void on_leave(GtkEventControllerMotion *c, gpointer user_data)
{
    A7800Display *self = user_data;
    (void)c;
    self->ptr_in = FALSE;
    if (self->gun)
        send_pointer(self);
}

/* The primary button pulls the trigger; the secondary fires off-screen,
 * which is how a gun game is told to reload. */
static unsigned button_bit(GtkGestureClick *g)
{
    switch (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g))) {
    case GDK_BUTTON_PRIMARY: return 1u;
    case GDK_BUTTON_SECONDARY: return 2u;
    default: return 0u;
    }
}

static void on_pressed(GtkGestureClick *g, int n, double x, double y, gpointer user_data)
{
    A7800Display *self = user_data;
    (void)n;
    gtk_widget_grab_focus(GTK_WIDGET(self));
    self->ptr_x = x;
    self->ptr_y = y;
    self->ptr_in = TRUE;
    self->buttons |= button_bit(g);
    if (self->gun)
        send_pointer(self);
}

static void on_released(GtkGestureClick *g, int n, double x, double y, gpointer user_data)
{
    A7800Display *self = user_data;
    (void)n; (void)x; (void)y;
    self->buttons &= ~button_bit(g);
    if (self->gun)
        send_pointer(self);
}

static void on_cancelled(GtkGesture *g, GdkEventSequence *seq, gpointer user_data)
{
    A7800Display *self = user_data;
    (void)g; (void)seq;
    self->buttons = 0;
    if (self->gun)
        send_pointer(self);
}

/* ---- frames ------------------------------------------------------------------ */

static gboolean on_tick(GtkWidget *widget, GdkFrameClock *clock,
                        gpointer user_data)
{
    A7800Display *self = A7800_DISPLAY(widget);
    int h = 0;
    (void)user_data;

    /* Hand the emulator the compositor's cadence. */
    a7800session_notify_vsync(self->session,
                              gdk_frame_clock_get_frame_time(clock) * 1000);

    if (a7800session_copy_frame(self->session, self->fb, &h, &self->serial)) {
        if (h > 0 && h <= FB_MAX_H) {
            self->height = h;
            rebuild_texture(self);
            gtk_widget_queue_draw(widget);
        }
    }
    sync_gun(self);
    return G_SOURCE_CONTINUE;
}

static void a7800_display_snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
    A7800Display *self = A7800_DISPLAY(widget);
    int w = gtk_widget_get_width(widget);
    int h = gtk_widget_get_height(widget);
    double want, sw, sh, x, y;
    graphene_rect_t rect;

    gtk_snapshot_append_color(snapshot, &(GdkRGBA){ 0, 0, 0, 1 },
                              &GRAPHENE_RECT_INIT(0, 0, w, h));
    if (!self->texture || w <= 0 || h <= 0 || self->height <= 0)
        return;

    /* A television showed MARIA's frame at 4:3, NTSC or PAL alike, as MAME
     * does. Square pixels show the raw 320-pixel lines, which is useful for
     * pixel work and honest about nothing else. */
    want = self->tv_aspect ? 4.0 / 3.0 : (double)FB_W / (double)self->height;

    if ((double)w / (double)h > want) {
        sh = h;
        sw = sh * want;
    } else {
        sw = w;
        sh = sw / want;
    }
    x = (w - sw) / 2.0;
    y = (h - sh) / 2.0;
    self->pic_x = x;
    self->pic_y = y;
    self->pic_w = sw;
    self->pic_h = sh;

    graphene_rect_init(&rect, (float)x, (float)y, (float)sw, (float)sh);
    gtk_snapshot_append_scaled_texture(
        snapshot, self->texture,
        self->smooth ? GSK_SCALING_FILTER_LINEAR : GSK_SCALING_FILTER_NEAREST,
        &rect);
}

static void a7800_display_dispose(GObject *object)
{
    A7800Display *self = A7800_DISPLAY(object);

    if (self->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }
    g_clear_object(&self->texture);
    g_clear_pointer(&self->fb, g_free);
    g_clear_pointer(&self->bgra, g_free);

    G_OBJECT_CLASS(a7800_display_parent_class)->dispose(object);
}

static void a7800_display_class_init(A7800DisplayClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = a7800_display_dispose;
    GTK_WIDGET_CLASS(klass)->snapshot = a7800_display_snapshot;
}

static void a7800_display_init(A7800Display *self)
{
    GtkEventController *motion;
    GtkGesture *click;

    self->fb = g_new0(guint32, FB_W * FB_MAX_H);
    self->bgra = g_new0(guint32, FB_W * FB_MAX_H);
    self->tv_aspect = TRUE;
    self->smooth = FALSE;
    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);

    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "enter", G_CALLBACK(on_motion), self);
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), self);
    g_signal_connect(motion, "leave", G_CALLBACK(on_leave), self);
    gtk_widget_add_controller(GTK_WIDGET(self), motion);

    click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);   /* every button */
    g_signal_connect(click, "pressed", G_CALLBACK(on_pressed), self);
    g_signal_connect(click, "released", G_CALLBACK(on_released), self);
    g_signal_connect(click, "cancel", G_CALLBACK(on_cancelled), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(click));
}

GtkWidget *a7800_display_new(a7800session *session)
{
    A7800Display *self = g_object_new(A7800_TYPE_DISPLAY, NULL);
    self->session = session;
    self->tick_id = gtk_widget_add_tick_callback(GTK_WIDGET(self), on_tick,
                                                 NULL, NULL);
    return GTK_WIDGET(self);
}

void a7800_display_set_tv_aspect(A7800Display *self, gboolean tv)
{
    self->tv_aspect = tv;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void a7800_display_set_smooth(A7800Display *self, gboolean smooth)
{
    self->smooth = smooth;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}
