/*
 * controls -- the layout of the on-screen controllers, for every toolkit.
 *
 * One table (a7800session_controls_layout) says what the Controllers window
 * draws and where, in layout units (DESIGN-7800.md's u) from the top-left; the
 * toolkits draw the art from it and ask a7800session_controls_hit what a click
 * landed on, so the shapes cannot drift apart between GTK, Qt, AppKit and
 * Win32. It has a view per port, following what is plugged in (the ProLine
 * two-button stick, the 2600's CX40, the XG-1 light gun, nothing), over a
 * console strip with the front keys and the two difficulty slides. Every view
 * has the same extent, so the window never changes size when a port's type or
 * the running game changes it.
 *
 * The CX40 geometry is the 2600 desktop's (DESIGN-atari.md section 2), placed
 * centred in the port's slot.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <pthread.h>
#include <string.h>

#include "session_internal.h"

/* ---- geometry (layout units) ------------------------------------------------------ */

#define CAPTION_H   0.9f
#define PORT_W     16.0f
#define PORT_H      8.0f
#define PORT_GAP    1.0f
#define CONSOLE_W  33.0f
#define CONSOLE_H   5.5f
#define CONSOLE_GAP 0.6f
#define LAYOUT_W   (2.0f * PORT_W + PORT_GAP)
#define LAYOUT_H   (CAPTION_H + PORT_H + CONSOLE_GAP + CONSOLE_H + 0.3f)

#define MAX_CONTROLS 40

typedef struct {
    a7800_control ctl[MAX_CONTROLS];
    int n;
} layout;

static layout g_layout[A7800_CTRL_COUNT][A7800_CTRL_COUNT];
static int g_built[A7800_CTRL_COUNT][A7800_CTRL_COUNT];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void add(layout *lay, int kind, const char *label, const char *legend,
                int port, int target, int device,
                float x, float y, float w, float h)
{
    a7800_control *c;
    if (lay->n >= MAX_CONTROLS) return;
    c = &lay->ctl[lay->n++];
    c->kind = kind;
    c->label = label;
    c->legend = legend;
    c->port = port;
    c->target = target;
    c->device = device;
    c->x = x; c->y = y; c->w = w; c->h = h;
}

/* A view's type: AUTO (not yet detected) is drawn as the ProLine. */
static int view_type(int type)
{
    return type >= A7800_CTRL_PROLINE && type < A7800_CTRL_COUNT ? type : A7800_CTRL_PROLINE;
}

/* The two-button ProLine stick, top view, in its 16 x 8 box: the grey dish
 * (centre (3.0, 3.0), r 2.5: press and drag, a quarter of the reach is nothing,
 * then eight 45-degree sectors) and the two red buttons side by side on the
 * lower right (centres (8.4, 5.2) and (12.4, 5.2), hit as circles of r 1.4):
 * the left one is Button 1, the right one Button 2. */
static void add_proline(layout *lay, int port, float ox, float oy)
{
    const int dev = A7800_CTRL_PROLINE;
    add(lay, A7800_CTL_BODY, "ProLine", NULL, port, -1, dev, ox, oy, PORT_W, PORT_H);
    add(lay, A7800_CTL_STICK, "STICK", NULL, port, -1, dev, ox + 0.5f, oy + 0.5f, 5.0f, 5.0f);
    add(lay, A7800_CTL_ROUND_BUTTON, "1", "left button", port,
        A7800_TARGET_PORT(port, A7800_ACT_BUTTON1), dev, ox + 7.0f, oy + 3.8f, 2.8f, 2.8f);
    add(lay, A7800_CTL_ROUND_BUTTON, "2", "right button", port,
        A7800_TARGET_PORT(port, A7800_ACT_BUTTON2), dev, ox + 11.0f, oy + 3.8f, 2.8f, 2.8f);
}

/* The CX40 in its 8.0 x 8.0 case, centred in the 16 x 8 slot: the ring (centre
 * (4.5, 4.5), r 2.1: the stick) and the one red fire button in the top-left
 * corner (centre (1.7, 1.7), r 0.95). The fire is Button 1. */
static void add_cx40(layout *lay, int port, float ox, float oy)
{
    const int dev = A7800_CTRL_JOY2600;
    const float cx = ox + (PORT_W - 8.0f) * 0.5f;
    add(lay, A7800_CTL_BODY, "CX40", NULL, port, -1, dev, cx, oy, 8.0f, 8.0f);
    add(lay, A7800_CTL_STICK, "STICK", NULL, port, -1, dev, cx + 2.4f, oy + 2.4f, 4.2f, 4.2f);
    add(lay, A7800_CTL_ROUND_BUTTON, "FIRE", NULL, port,
        A7800_TARGET_PORT(port, A7800_ACT_BUTTON1), dev, cx + 0.75f, oy + 0.75f, 1.9f, 1.9f);
}

/* The XG-1 light gun: a small silhouette and two lines of text; nothing in it
 * is pressable (the picture is the target, see a7800session_pointer). */
static void add_gun(layout *lay, int port, float ox, float oy)
{
    const int dev = A7800_CTRL_LIGHTGUN;
    add(lay, A7800_CTL_BODY, "XG-1", NULL, port, -1, dev, ox + 3.5f, oy + 0.4f, 9.0f, 4.6f);
    add(lay, A7800_CTL_CAPTION, "Aim on the picture", NULL, port, -1, dev, ox, oy + 5.2f, PORT_W, 1.2f);
    add(lay, A7800_CTL_CAPTION, "Click to fire", NULL, port, -1, dev, ox, oy + 6.5f, PORT_W, 1.0f);
}

/* An empty port: the socket and its words. */
static void add_none(layout *lay, int port, float ox, float oy)
{
    const int dev = A7800_CTRL_NONE;
    add(lay, A7800_CTL_BODY, "Socket", NULL, port, -1, dev, ox + 2.0f, oy + 1.0f, 12.0f, 6.0f);
    add(lay, A7800_CTL_CAPTION, "No controller", NULL, port, -1, dev, ox + 2.0f, oy + 3.2f, 12.0f, 1.6f);
}

/* The console strip, 33.0 x 5.5: a black bar with the silver band, its rainbow
 * line and "7800 PROSYSTEM"; under them the front keys as on the console (POWER
 * and PAUSE on the left, SELECT and RESET on the right; POWER is drawn dimmed
 * and is no target, the emulator is on) and the two difficulty slides between,
 * each a slot with its B and A letters printed either side (B at the left, A at
 * the right). */
static void add_console(layout *lay, float oy)
{
    const float key_y = oy + 3.3f, key_h = 1.4f, key_w = 3.6f;
    add(lay, A7800_CTL_BODY, "Console", NULL, -1, -1, -1, 0.0f, oy, CONSOLE_W, CONSOLE_H);
    add(lay, A7800_CTL_BODY, "Band", NULL, -1, -1, -1, 0.5f, oy + 0.4f, 32.0f, 2.0f);
    add(lay, A7800_CTL_CAPTION, "7800 PROSYSTEM", NULL, -1, -1, -1, 1.5f, oy + 0.7f, 12.0f, 1.4f);
    add(lay, A7800_CTL_BODY, "Rainbow", NULL, -1, -1, -1, 0.5f, oy + 2.5f, 32.0f, 0.3f);
    add(lay, A7800_CTL_BODY, "POWER", "not interactive", -1, -1, -1, 1.0f, key_y, key_w, key_h);
    add(lay, A7800_CTL_BUTTON, "PAUSE", "pause", -1,
        A7800_TARGET_SWITCH(A7800_SW_PAUSE), -1, 5.0f, key_y, key_w, key_h);
    add(lay, A7800_CTL_SWITCH, "A / B", "left difficulty", -1,
        A7800_TARGET_SWITCH(A7800_SW_LEFT_DIFF), -1, 10.0f, key_y, 4.6f, key_h);
    add(lay, A7800_CTL_SWITCH, "A / B", "right difficulty", -1,
        A7800_TARGET_SWITCH(A7800_SW_RIGHT_DIFF), -1, 18.4f, key_y, 4.6f, key_h);
    add(lay, A7800_CTL_BUTTON, "SELECT", "select", -1,
        A7800_TARGET_SWITCH(A7800_SW_SELECT), -1, 24.4f, key_y, key_w, key_h);
    add(lay, A7800_CTL_BUTTON, "RESET", "reset", -1,
        A7800_TARGET_SWITCH(A7800_SW_RESET), -1, 28.4f, key_y, key_w, key_h);
}

static void build(layout *lay, int left, int right)
{
    static const char *const k_caption[A7800_PORTS] = { "Player 1", "Player 2" };
    int port;
    memset(lay, 0, sizeof *lay);
    for (port = 0; port < A7800_PORTS; port++) {
        const float ox = (float)port * (PORT_W + PORT_GAP);
        const float oy = CAPTION_H;
        add(lay, A7800_CTL_CAPTION, k_caption[port], NULL, port, -1, -1, ox, 0.0f, PORT_W, CAPTION_H);
        switch (port ? right : left) {
        case A7800_CTRL_JOY2600:  add_cx40(lay, port, ox, oy); break;
        case A7800_CTRL_LIGHTGUN: add_gun(lay, port, ox, oy); break;
        case A7800_CTRL_NONE:     add_none(lay, port, ox, oy); break;
        default:                  add_proline(lay, port, ox, oy); break;
        }
    }
    add_console(lay, CAPTION_H + PORT_H + CONSOLE_GAP);
}

const a7800_control *a7800session_controls_layout(int left_type, int right_type, int *n,
                                                  float *width, float *height)
{
    const int lt = view_type(left_type), rt = view_type(right_type);
    layout *lay;

    pthread_mutex_lock(&g_lock);
    if (!g_built[lt][rt]) {
        build(&g_layout[lt][rt], lt, rt);
        g_built[lt][rt] = 1;
    }
    lay = &g_layout[lt][rt];
    pthread_mutex_unlock(&g_lock);
    if (n) *n = lay->n;
    if (width) *width = LAYOUT_W;
    if (height) *height = LAYOUT_H;
    return lay->ctl;
}

static int is_round(int kind)
{
    return kind == A7800_CTL_STICK || kind == A7800_CTL_ROUND_BUTTON;
}

static int is_pressable(int kind)
{
    return kind == A7800_CTL_STICK || kind == A7800_CTL_ROUND_BUTTON
        || kind == A7800_CTL_BUTTON || kind == A7800_CTL_SWITCH;
}

int a7800session_controls_hit(int left_type, int right_type, float x, float y)
{
    int n, i;
    const a7800_control *c = a7800session_controls_layout(left_type, right_type, &n, NULL, NULL);

    /* the topmost: later entries are drawn over earlier ones */
    for (i = n - 1; i >= 0; i--) {
        if (!is_pressable(c[i].kind)) continue;
        if (x < c[i].x || x >= c[i].x + c[i].w || y < c[i].y || y >= c[i].y + c[i].h)
            continue;
        if (is_round(c[i].kind)) {
            const float rx = c[i].w * 0.5f, ry = c[i].h * 0.5f;
            const float dx = (x - (c[i].x + rx)) / rx, dy = (y - (c[i].y + ry)) / ry;
            if (dx * dx + dy * dy > 1.0f) continue;
        }
        return i;
    }
    return -1;
}

unsigned a7800session_stick_from_vector(float dx, float dy, float radius)
{
    const float ax = fabsf(dx), ay = fabsf(dy);
    unsigned mask = 0;
    /* tan(22.5 degrees): the edge of a 45-degree sector */
    const float edge = 0.41421356f;

    if (radius <= 0.f || ax * ax + ay * ay < (0.25f * radius) * (0.25f * radius))
        return 0;
    if (ay <= ax * edge)       mask = dx < 0.f ? 1u << A7800_ACT_LEFT : 1u << A7800_ACT_RIGHT;
    else if (ax <= ay * edge)  mask = dy < 0.f ? 1u << A7800_ACT_UP : 1u << A7800_ACT_DOWN;
    else {
        mask = dx < 0.f ? 1u << A7800_ACT_LEFT : 1u << A7800_ACT_RIGHT;
        mask |= dy < 0.f ? 1u << A7800_ACT_UP : 1u << A7800_ACT_DOWN;
    }
    return mask;
}

int a7800session_view_type(a7800session *s, int port)
{
    int type;
    if (!s || port < 0 || port >= A7800_PORTS) return A7800_CTRL_PROLINE;
    type = a7800session_port_type(s, port);
    if (type == A7800_CTRL_AUTO)
        type = a7800session_port_detected(s, port);
    return view_type(type);
}
