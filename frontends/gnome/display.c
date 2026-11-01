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

/* ---- frame buffer pool ----------------------------------------------------
 * The session copies a frame straight into a pooled buffer and the texture
 * borrows that buffer: one copy per frame, no allocation in steady state
 * (a 250 KB malloc is an mmap/munmap pair every frame). The buffer comes
 * back to the pool when GSK drops the texture's bytes, possibly after the
 * widget is gone, so the pool is reference counted and each outstanding
 * buffer holds a reference. */
typedef struct {
    gatomicrefcount refs;
    GMutex lock;
    GPtrArray *spare;      /* idle guint32 buffers, FB_W * FB_MAX_H each */
} FramePool;

typedef struct {
    FramePool *pool;
    guint32 *px;
} FrameLoan;

#define POOL_KEEP 4

static FramePool *pool_new(void)
{
    FramePool *p = g_new0(FramePool, 1);
    g_atomic_ref_count_init(&p->refs);
    g_mutex_init(&p->lock);
    p->spare = g_ptr_array_new_with_free_func(g_free);
    return p;
}

static void pool_unref(FramePool *p)
{
    if (!g_atomic_ref_count_dec(&p->refs))
        return;
    g_ptr_array_free(p->spare, TRUE);
    g_mutex_clear(&p->lock);
    g_free(p);
}

static guint32 *pool_take(FramePool *p)
{
    guint32 *px = NULL;

    g_mutex_lock(&p->lock);
    if (p->spare->len > 0)
        px = g_ptr_array_steal_index(p->spare, p->spare->len - 1);
    g_mutex_unlock(&p->lock);
    return px ? px : g_malloc((gsize)FB_W * FB_MAX_H * sizeof(guint32));
}

static void pool_give(FramePool *p, guint32 *px)
{
    g_mutex_lock(&p->lock);
    if (p->spare->len < POOL_KEEP) {
        g_ptr_array_add(p->spare, px);
        px = NULL;
    }
    g_mutex_unlock(&p->lock);
    g_free(px);
}

static void loan_release(gpointer data)
{
    FrameLoan *loan = data;

    pool_give(loan->pool, loan->px);
    pool_unref(loan->pool);
    g_free(loan);
}

struct _A7800Display {
    GtkWidget parent_instance;

    a7800session *session;
    FramePool *pool;
    int height;
    GdkTexture *texture;
    guint64 serial;
    guint tick_id;
    gulong suspended_id;   /* notify::suspended on the toplevel, while mapped */
    GtkWindow *toplevel;
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

/* Wrap px (XRGB8888 straight from the session, h lines) in the texture. The
 * previous texture, when it was the same size, goes in as the update source
 * so GSK can reuse its GPU texture instead of making a new one. The X byte is
 * ignored by GDK_MEMORY_B8G8R8X8, so the alpha never needs touching. Takes
 * ownership of px. */
static void publish_texture(A7800Display *self, guint32 *px, int h)
{
    GdkMemoryTextureBuilder *b = gdk_memory_texture_builder_new();
    FrameLoan *loan = g_new(FrameLoan, 1);
    GBytes *bytes;

    g_atomic_ref_count_inc(&self->pool->refs);
    loan->pool = self->pool;
    loan->px = px;
    bytes = g_bytes_new_with_free_func(px, (gsize)FB_W * h * sizeof(guint32),
                                       loan_release, loan);
    gdk_memory_texture_builder_set_bytes(b, bytes);
    gdk_memory_texture_builder_set_stride(b, (gsize)FB_W * 4);
    gdk_memory_texture_builder_set_width(b, FB_W);
    gdk_memory_texture_builder_set_height(b, h);
    gdk_memory_texture_builder_set_format(b, GDK_MEMORY_B8G8R8X8);
    if (self->texture && self->height == h) {
        cairo_rectangle_int_t all = { 0, 0, FB_W, h };
        cairo_region_t *region = cairo_region_create_rectangle(&all);

        gdk_memory_texture_builder_set_update_texture(b, self->texture);
        gdk_memory_texture_builder_set_update_region(b, region);
        cairo_region_destroy(region);
    }
    g_clear_object(&self->texture);
    self->texture = gdk_memory_texture_builder_build(b);
    g_bytes_unref(bytes);
    g_object_unref(b);
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
    guint32 *px = pool_take(self->pool);
    int h = 0;
    (void)user_data;

    /* Hand the emulator the compositor's cadence. */
    a7800session_notify_vsync(self->session,
                            gdk_frame_clock_get_frame_time(clock) * 1000);

    if (a7800session_copy_frame(self->session, px, &h, &self->serial) &&
        h > 0 && h <= FB_MAX_H) {
        publish_texture(self, px, h);
        self->height = h;
        gtk_widget_queue_draw(widget);
    } else {
        pool_give(self->pool, px);
    }
    sync_gun(self);
    return G_SOURCE_CONTINUE;
}

/* The tick runs only while somebody can see the result. A hidden or
 * suspended window gets no frame callbacks from the compositor anyway, but a
 * minimised one on some stacks keeps ticking, and every tick is a wakeup.
 * The emulator does not care: with no ticks the vsync stream goes stale
 * after a quarter second and the session paces on the wall clock, which is
 * what it does for a PAL console anyway. The first tick after the window
 * comes back copies whatever frame is newest. */
static void update_tick(A7800Display *self)
{
    gboolean want = gtk_widget_get_mapped(GTK_WIDGET(self)) &&
                    !(self->toplevel && gtk_window_is_suspended(self->toplevel));

    if (want && !self->tick_id)
        self->tick_id = gtk_widget_add_tick_callback(GTK_WIDGET(self), on_tick,
                                                     NULL, NULL);
    else if (!want && self->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }
}

static void on_suspended(GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    (void)obj; (void)pspec;
    update_tick(A7800_DISPLAY(user_data));
}

static void a7800_display_map(GtkWidget *widget)
{
    A7800Display *self = A7800_DISPLAY(widget);
    GtkRoot *root;

    GTK_WIDGET_CLASS(a7800_display_parent_class)->map(widget);
    root = gtk_widget_get_root(widget);
    if (GTK_IS_WINDOW(root)) {
        self->toplevel = GTK_WINDOW(root);
        self->suspended_id = g_signal_connect(self->toplevel, "notify::suspended",
                                              G_CALLBACK(on_suspended), self);
    }
    update_tick(self);
}

static void a7800_display_unmap(GtkWidget *widget)
{
    A7800Display *self = A7800_DISPLAY(widget);

    if (self->toplevel && self->suspended_id)
        g_signal_handler_disconnect(self->toplevel, self->suspended_id);
    self->suspended_id = 0;
    self->toplevel = NULL;
    GTK_WIDGET_CLASS(a7800_display_parent_class)->unmap(widget);
    update_tick(self);
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
    if (self->pool) {
        pool_unref(self->pool);
        self->pool = NULL;
    }

    G_OBJECT_CLASS(a7800_display_parent_class)->dispose(object);
}

static void a7800_display_class_init(A7800DisplayClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = a7800_display_dispose;
    GTK_WIDGET_CLASS(klass)->snapshot = a7800_display_snapshot;
    GTK_WIDGET_CLASS(klass)->map = a7800_display_map;
    GTK_WIDGET_CLASS(klass)->unmap = a7800_display_unmap;
}

static void a7800_display_init(A7800Display *self)
{
    GtkEventController *motion;
    GtkGesture *click;

    self->pool = pool_new();
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
