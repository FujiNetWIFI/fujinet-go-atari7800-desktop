/*
 * The Win32 Controllers window (F9): the Atari 7800's two controller ports as
 * the real parts look -- the ProLine's black slab with its grey stick and two
 * red buttons, the 2600's CX40, the XG-1 light gun, an empty socket -- each
 * drawn for what is plugged in (the session's effective type, so Auto follows
 * the running game), over the console's front strip (silver band, rainbow,
 * PAUSE / SELECT / RESET and the two A / B difficulty slides). Everything is
 * drawn from the session's layout (a7800session_controls_layout), so every
 * frontend shows the same parts. Under them: one type selector per port, the
 * gamepads with the player each drives, and Map mode.
 *
 * MOUSE: a press on a button or a console key holds it for as long as the
 * mouse button is down (the console samples its controllers once per frame, so
 * a value present only for the instant of a click falls between frames); the
 * stick is dragged, the pointer's offset from the dish's centre being the
 * direction (a7800session_pointer_set); a click on a difficulty slide flips
 * it. The mouse is captured on press; letting go anywhere, losing the capture,
 * hiding, Map being armed or deactivating the window releases whatever the
 * pointer holds (a7800session_release_pointer). The wheel does nothing here:
 * the 7800 has no knob.
 *
 * What the console sees held -- keyboard, gamepads and this window together --
 * is polled every 40 ms and drawn as a ring and a 35% fill in the held colour,
 * so the window doubles as an input tester.
 *
 * FIXED SIZE: laid out once, in a caption-only tool window with no thick frame
 * and no maximize box, so neither Map mode, a long gamepad name nor a port's
 * controller changing size it.
 *
 * MAP MODE: Map arms a two-step rebind -- click a control (a stick click picks
 * its direction) to pick the target, then press a host key OR a gamepad
 * button; Esc cancels. While armed the controls show what drives them, the
 * picked target is lit and the rest are drawn at 40%, and typing does not reach
 * the console. Clear takes the picked target's key and button away. "Show
 * bindings" shows the same legends outside Map mode. F9, F11 and F12 are the
 * app's and cannot be bound.
 *
 * The art is drawn at twice the size into a memory bitmap and shrunk with
 * HALFTONE (GDI has no anti-aliasing of its own); the text goes on after at
 * the window's own size.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controller_window.h"

#include <math.h>

/* GET_X_LPARAM / GET_Y_LPARAM live here, not in windows.h. */
#include <windowsx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../help/help_window.h"
#include "../key_forward.h"
#include "../resource.h"

#define PANEL_CLASS   "FujiNetGoAtari7800Controllers"
#define PANEL_STYLE   (WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN)
#define PANEL_EXSTYLE WS_EX_TOOLWINDOW

/* Sizes at 96 dpi; scaled() maps them to the window's. */
#define UNIT96      16      /* pixels per layout unit */
#define MARGIN      12
#define CASE_PAD    10      /* the mat round the art */
#define HEAD_H      22
#define ROW_H       26      /* one gamepad's row */
#define ROW_GAP     3
#define PAD_ROWS    3       /* gamepad rows the window has room for */
#define SEG_AUTO_W  76      /* Automatic */
#define SEG_W       62      /* Player 1, Player 2 */
#define TYPE_H      26      /* a port's type selector */
#define BTN_H       28
#define HINT_H      20

#define IDT_CAPTURE 1
#define IDT_HELD    2
#define IDT_PADS    3

#define PI_VALUE 3.14159265358979323846

enum { SB_MAP, SB_CLEAR, SB_DEFAULTS, SB_SHOW, SB_HELP, SB_COUNT };

typedef struct {
    unsigned generation;
    int count, assign[PAD_ROWS], effective[PAD_ROWS];
} pads_state;

typedef struct {
    unsigned held[A7800_PORTS];     /* a7800_action bits the console sees held */
    unsigned switches;              /* bit per a7800_switch: held, or the slide at A */
    int view[A7800_PORTS];          /* the controller drawn on each port */
    int detected[A7800_PORTS];
    int dragging[A7800_PORTS];      /* the pointer's stick drag, as a flag */
} held_state;

static HWND g_panel, g_parent;
static HWND g_combo[A7800_PORTS];
static a7800session *g_session;
static int g_dpi = 96;

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad button to bind to that target. */
static int g_map_state = -2;
static int g_show_bindings;
static WPARAM g_swallow_vk;     /* the key Map took: its repeats and release too */
static char g_hint[300];
static int g_hover_part = -1;   /* the part under the idle pointer */
static int g_tracking;

/* The layout (static table owned by the core), and where it sits in the client. */
static const a7800_control *g_layout;
static int g_numlayout;
static float g_layw, g_layh;
static double g_unit, g_orgx, g_orgy;

/* What the pointer holds: a pressed part (layout index), a dragged stick. */
static int g_mouse_part = -1;
static double g_stick_dx, g_stick_dy;   /* the dragged cap's offset, -1..1 of the reach */

/* What was last painted. */
static held_state g_shown;
static pads_state g_pads_shown;
static int g_total_w, g_total_h;

static RECT g_mat_rc, g_gp_rc, g_gp_head_rc, g_gp_row_rc[PAD_ROWS];
static RECT g_btn_rc[SB_COUNT], g_hint_rc;

static HFONT g_font, g_bold, g_legend, g_tiny, g_caption, g_title, g_logo, g_key;

static void refresh_held(void);

static int scaled(int val) { return a7800_scale(val, g_dpi); }

/* ---- fonts ------------------------------------------------------------------ */

static void free_fonts(void)
{
    HFONT *const slots[] = { &g_font, &g_bold, &g_legend, &g_tiny, &g_caption, &g_title, &g_logo, &g_key };
    size_t idx;
    for (idx = 0; idx < sizeof slots / sizeof slots[0]; idx++)
        if (*slots[idx]) { DeleteObject(*slots[idx]); *slots[idx] = NULL; }
}

static HFONT face_font(int px, int weight, int italic, int escapement)
{
    return CreateFontW(-MulDiv(px, g_dpi, 96), 0, escapement, escapement, weight, italic, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

static void make_fonts(void)
{
    free_fonts();
    g_font = a7800_make_font(g_dpi, 0, FW_NORMAL, NULL);
    g_bold = a7800_make_font(g_dpi, 0, FW_BOLD, NULL);
    g_legend = face_font(9, FW_BOLD, 0, 0);         /* console key legends, slide letters */
    g_tiny = a7800_make_font(g_dpi, 9, FW_NORMAL, NULL);   /* binding legends */
    g_caption = face_font(12, FW_BOLD, 0, 0);       /* Player 1 / Player 2 on the mat */
    g_title = face_font(10, FW_BOLD, 1, 0);         /* 7800 PROSYSTEM */
    g_logo = face_font(11, FW_BOLD, 0, 150);        /* ATARI, 15 degrees up */
    g_key = face_font(10, FW_BOLD, 0, 0);           /* the buttons' numerals */
}

/* ---- text -------------------------------------------------------------------- */

/* UTF-8 (key legends, gamepad names) through DrawTextW. */
static int text_out(HDC hdc, const char *utf8, RECT *rect, UINT fmt)
{
    wchar_t wide[256];
    if (!utf8 || !*utf8) return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, (int)(sizeof wide / sizeof wide[0])) <= 0) return 0;
    return DrawTextW(hdc, wide, -1, rect, fmt | DT_NOPREFIX);
}

static void text_line(HDC hdc, const char *utf8, const RECT *rect, UINT fmt)
{
    RECT copy = *rect;
    text_out(hdc, utf8, &copy, fmt | DT_SINGLELINE | DT_VCENTER);
}

/* ---- colours ----------------------------------------------------------------- */

static COLORREF rgb_of(unsigned val)
{
    return RGB((val >> 16) & 0xff, (val >> 8) & 0xff, val & 0xff);
}

#define HX(val) rgb_of(val)

#define NOFILL ((COLORREF)0xFFFFFFFFu)

#define COL_HELD_RING   HX(A7800SESSION_HELD_RGB)
#define COL_HELD_BUTTON HX(A7800SESSION_HELD_BUTTON_FILL_RGB)
#define COL_MAT_TOP     HX(0x8E897C)    /* the desk: light enough that black plastic reads */
#define COL_MAT_BOT     HX(0x6B675C)
#define COL_MAT_EDGE    HX(0x3D3A33)
#define COL_SHADOW      HX(0x403D36)
#define COL_CAPTION     HX(0xFBF8EC)
#define COL_DIM_TO      HX(0x7C776B)    /* unpicked parts fade toward the mat */

static COLORREF mix_color(COLORREF start, COLORREF finish, double frac)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    return RGB((int)(GetRValue(start) + (GetRValue(finish) - GetRValue(start)) * frac + 0.5),
               (int)(GetGValue(start) + (GetGValue(finish) - GetGValue(start)) * frac + 0.5),
               (int)(GetBValue(start) + (GetBValue(finish) - GetBValue(start)) * frac + 0.5));
}

/* An unpicked part in Map mode: 40% of its own colours over the mat's. */
static COLORREF tone(COLORREF col, int dim)
{
    return dim ? mix_color(col, COL_DIM_TO, 0.6) : col;
}

/* ---- drawing primitives ---------------------------------------------------------
 * The art is drawn at g_art_k times the window's size into a bitmap that holds
 * the pixel rectangle starting at (g_art_x, g_art_y). */
static int g_art_k = 1, g_art_x, g_art_y, g_art_rows;

static int art_xpos(double val) { return (int)floor((val - g_art_x) * g_art_k + 0.5); }
static int art_ypos(double val) { return (int)floor((val - g_art_y) * g_art_k + 0.5); }
static int art_len(double val) { return (int)floor(val * g_art_k + 0.5); }

static POINT apt(double xpos, double ypos)
{
    POINT pnt;
    pnt.x = art_xpos(xpos);
    pnt.y = art_ypos(ypos);
    return pnt;
}

static void poly(HDC hdc, const POINT *pnts, int num, COLORREF fill, COLORREF line, double width)
{
    HBRUSH brush = fill == NOFILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, width > 0 ? art_len(width) : 0, line);
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, width > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    Polygon(hdc, pnts, num);
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    if (fill != NOFILL) DeleteObject(brush);
}

static void ellipse(HDC hdc, double ctrx, double ctry, double radx, double rady, COLORREF fill, COLORREF line,
                    double width)
{
    HBRUSH brush = fill == NOFILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, width > 0 ? art_len(width) : 0, line);
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, width > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    Ellipse(hdc, art_xpos(ctrx - radx), art_ypos(ctry - rady), art_xpos(ctrx + radx), art_ypos(ctry + rady));
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    if (fill != NOFILL) DeleteObject(brush);
}

static void rrect(HDC hdc, double xpos, double ypos, double width, double height, double radius, COLORREF fill,
                  COLORREF line, double linew)
{
    HBRUSH brush = fill == NOFILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, linew > 0 ? art_len(linew) : 0, line);
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, linew > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    RoundRect(hdc, art_xpos(xpos), art_ypos(ypos), art_xpos(xpos + width), art_ypos(ypos + height),
              art_len(radius * 2), art_len(radius * 2));
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    if (fill != NOFILL) DeleteObject(brush);
}

static void line_seg(HDC hdc, double x0, double y0, double x1, double y1, COLORREF col, double width)
{
    HPEN pen = CreatePen(PS_SOLID, art_len(width) > 0 ? art_len(width) : 1, col);
    HGDIOBJ oldp = SelectObject(hdc, pen);
    MoveToEx(hdc, art_xpos(x0), art_ypos(y0), NULL);
    LineTo(hdc, art_xpos(x1), art_ypos(y1));
    SelectObject(hdc, oldp);
    DeleteObject(pen);
}

/* A pie slice of a circle, from clockwise-from-up angle first to last (degrees). */
static void sector(HDC hdc, double ctrx, double ctry, double rad, double first, double last, COLORREF fill,
                   COLORREF line, double linew)
{
    const double ra = first * PI_VALUE / 180.0, rb = last * PI_VALUE / 180.0;
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, linew > 0 ? art_len(linew) : 0, line);
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, linew > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    /* GDI draws a pie counter-clockwise from its first radial to its second */
    Pie(hdc, art_xpos(ctrx - rad), art_ypos(ctry - rad), art_xpos(ctrx + rad), art_ypos(ctry + rad),
        art_xpos(ctrx + rad * 2 * sin(ra)), art_ypos(ctry - rad * 2 * cos(ra)),
        art_xpos(ctrx + rad * 2 * sin(rb)), art_ypos(ctry - rad * 2 * cos(rb)));
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    DeleteObject(brush);
}

/* A rounded rectangle filled with a vertical gradient, then outlined. */
static void grad_rrect(HDC hdc, double xpos, double ypos, double width, double height, double radius,
                       COLORREF top, COLORREF bottom, COLORREF edge, double edgew)
{
    const int left = art_xpos(xpos), right = art_xpos(xpos + width);
    const int first = art_ypos(ypos), last = art_ypos(ypos + height);
    HRGN clip = CreateRoundRectRgn(left, first, right + 1, last + 1, art_len(radius * 2), art_len(radius * 2));
    int row;
    HGDIOBJ oldb;
    SelectClipRgn(hdc, clip);
    oldb = SelectObject(hdc, GetStockObject(DC_BRUSH));
    for (row = first < 0 ? 0 : first; row <= last && row < g_art_rows; row++) {
        SetDCBrushColor(hdc, mix_color(top, bottom, last > first ? (double)(row - first) / (last - first) : 0));
        PatBlt(hdc, left, row, right - left + 1, 1, PATCOPY);
    }
    SelectObject(hdc, oldb);
    SelectClipRgn(hdc, NULL);
    DeleteObject(clip);
    if (edgew > 0) rrect(hdc, xpos, ypos, width, height, radius, NOFILL, edge, edgew);
}

/* The pixel rectangle of a layout part. */
static void part_box(const a7800_control *ctl, double *xpos, double *ypos, double *width, double *height)
{
    *xpos = g_orgx + (double)ctl->x * g_unit;
    *ypos = g_orgy + (double)ctl->y * g_unit;
    *width = (double)ctl->w * g_unit;
    *height = (double)ctl->h * g_unit;
}

static void part_rect(const a7800_control *ctl, RECT *rect)
{
    double xpos, ypos, width, height;
    part_box(ctl, &xpos, &ypos, &width, &height);
    SetRect(rect, (int)floor(xpos), (int)floor(ypos), (int)ceil(xpos + width), (int)ceil(ypos + height));
}

/* ---- what a part shows ---------------------------------------------------------- */

static int binding_mode(void)
{
    return g_map_state != -2 || g_show_bindings;
}

static int pressable(const a7800_control *ctl)
{
    return ctl->kind == A7800_CTL_STICK || ctl->kind == A7800_CTL_ROUND_BUTTON || ctl->kind == A7800_CTL_BUTTON ||
           ctl->kind == A7800_CTL_SWITCH;
}

/* The port a target belongs to, -1 for the console's. */
static int target_port(int target)
{
    return target >= 0 && target < 2 * A7800_ACT_PER_PORT ? target / A7800_ACT_PER_PORT : -1;
}

/* The target is one of a port's four stick directions. */
static int is_dir_of(int target, int port)
{
    return target_port(target) == port && target % A7800_ACT_PER_PORT <= A7800_ACT_RIGHT;
}

/* A part shown held: what the console sees. In Map mode the picked target is
 * the one lit instead. */
static int part_lit(const a7800_control *ctl)
{
    if (g_map_state != -2) {
        if (g_map_state < 0 || !pressable(ctl)) return 0;
        if (ctl->kind == A7800_CTL_STICK) return is_dir_of(g_map_state, ctl->port);
        return ctl->target == g_map_state;
    }
    if (ctl->kind == A7800_CTL_STICK) return (g_shown.held[ctl->port] & 0xFu) != 0;
    if (ctl->kind == A7800_CTL_SWITCH) return 0;        /* a position, not a hold */
    if (ctl->target < 0) return 0;
    if (target_port(ctl->target) >= 0)
        return (g_shown.held[ctl->port] >> (ctl->target % A7800_ACT_PER_PORT)) & 1u;
    return (g_shown.switches >> (ctl->target - A7800_TARGET_SWITCH(0))) & 1u;
}

/* Map mode with a target picked: every other control is drawn at 40%. */
static int part_dim(const a7800_control *ctl)
{
    if (g_map_state < 0 || !pressable(ctl)) return 0;
    if (ctl->kind == A7800_CTL_STICK) return !is_dir_of(g_map_state, ctl->port);
    return ctl->target != g_map_state;
}

/* A device's case in Map mode with a target picked elsewhere. */
static int body_dim(int port)
{
    return g_map_state >= 0 && target_port(g_map_state) != port;
}

/* The direction bits a stick shows lit: the held ones, or in Map mode the
 * picked direction. */
static unsigned dirs_shown(int port)
{
    if (g_map_state != -2) {
        if (g_map_state >= 0 && is_dir_of(g_map_state, port)) return 1u << (g_map_state % A7800_ACT_PER_PORT);
        return 0;
    }
    return g_shown.held[port] & 0xFu;
}

/* The shaft cap's offset from rest, -1..1 of the reach: the pointer while
 * dragging, else what is held (diagonals normalised so the cap stays inside
 * the dish). */
static void stick_offset(int port, double *offx, double *offy)
{
    const unsigned held = dirs_shown(port);
    int dirx, diry;
    if (g_shown.dragging[port] && g_map_state == -2) { *offx = g_stick_dx; *offy = g_stick_dy; return; }
    dirx = (int)((held >> A7800_ACT_RIGHT) & 1u) - (int)((held >> A7800_ACT_LEFT) & 1u);
    diry = (int)((held >> A7800_ACT_DOWN) & 1u) - (int)((held >> A7800_ACT_UP) & 1u);
    *offx = dirx;
    *offy = diry;
    if (dirx && diry) { *offx *= 0.7071; *offy *= 0.7071; }
}

/* The angle (degrees clockwise from straight up) the shown directions point at. */
static int held_angle(int port, double *ang)
{
    const unsigned held = dirs_shown(port);
    const int dirx = (int)((held >> A7800_ACT_RIGHT) & 1u) - (int)((held >> A7800_ACT_LEFT) & 1u);
    const int diry = (int)((held >> A7800_ACT_DOWN) & 1u) - (int)((held >> A7800_ACT_UP) & 1u);
    if (!dirx && !diry) return 0;
    *ang = atan2((double)dirx, (double)-diry) * 180.0 / PI_VALUE;
    return 1;
}

static double angle_between(double one, double two)
{
    double delta = fmod(one - two, 360.0);
    if (delta > 180) delta -= 360;
    if (delta < -180) delta += 360;
    return fabs(delta);
}

/* What drives a target: the key and the pad button, one line each ("" when unbound). */
static void binding_label(int target, char *out, int outsz)
{
    a7800_binding bind;
    out[0] = '\0';
    if (target < 0) return;
    bind = a7800session_binding_get(g_session, target);
    if (bind.keysym) {
        a7800session_keysym_name(bind.keysym, out, outsz);
        /* the labels are small: "Keypad 5" reads "KP 5" */
        if (strncmp(out, "Keypad ", 7) == 0) {
            memmove(out + 3, out + 7, strlen(out + 7) + 1);
            memcpy(out, "KP ", 3);
        }
    }
    if (!out[0]) snprintf(out, (size_t)outsz, "%s", "\xe2\x80\x94");
}

/* ---- the ProLine ---------------------------------------------------------------- */

/* The slab, its raised triangle panel, the swooshes behind the buttons and the
 * ATARI mark: 16 x 8 u. */
static void art_proline_body(HDC hdc, const a7800_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = body_dim(ctl->port);
    POINT tri[4];
    int idx;
    part_box(ctl, &bx, &by, &bw, &bh);

    rrect(hdc, bx + u * 0.12, by + u * 0.2, bw, bh, u * 0.5, COL_SHADOW, COL_SHADOW, 0);
    rrect(hdc, bx, by, bw, bh, u * 0.5, tone(HX(A7800SESSION_CASE_RGB), dim), tone(HX(A7800SESSION_CASE_SHADOW_RGB), dim),
          1.2);
    rrect(hdc, bx + u * 0.15, by + u * 0.15, bw - u * 0.3, bh - u * 0.3, u * 0.4, NOFILL,
          tone(HX(A7800SESSION_CASE_BEVEL_RGB), dim), 0.8);
    /* the raised triangle over the stick's half: its hypotenuse runs from the top to the lower left */
    tri[0] = apt(bx + u * 0.2, by + u * 0.2);
    tri[1] = apt(bx + u * 7.8, by + u * 0.2);
    tri[2] = apt(bx + u * 0.8, by + u * 7.2);
    tri[3] = apt(bx + u * 0.2, by + u * 7.2);
    poly(hdc, tri, 4, tone(HX(0x232326), dim), tone(HX(A7800SESSION_PANEL_EDGE_RGB), dim), 1.0);
    /* the ribbed swoosh running down and to the right from each button */
    for (idx = 0; idx < 2; idx++) {
        const double fromx = bx + u * (8.4 + idx * 4.0), fromy = by + u * 5.2;
        POINT band[4];
        band[0] = apt(fromx + u * 0.5, fromy - u * 0.9);
        band[1] = apt(fromx + u * 1.5, fromy - u * 0.2);
        band[2] = apt(fromx + u * 3.4, fromy + u * 2.0);
        band[3] = apt(fromx + u * 0.3, fromy + u * 1.1);
        poly(hdc, band, 4, tone(HX(A7800SESSION_CASE_SHADOW_RGB), dim), tone(HX(A7800SESSION_SWOOSH_RGB), dim), 0.6);
    }
    /* the fuji mark of the ATARI logo, tilted 15 degrees up with its name */
    {
        const double pivotx = bx + u * 12.0, pivoty = by + u * 2.2;
        const double rot = -15.0 * PI_VALUE / 180.0;
        for (idx = 0; idx < 3; idx++) {
            const double offx = -u * 1.1 + idx * u * 0.28, lean = (1 - idx) * u * 0.22;
            const double xs[4] = { offx - u * 0.06 + lean, offx + u * 0.06 + lean, offx + u * 0.06, offx - u * 0.06 };
            const double ys[4] = { -u * 0.8, -u * 0.8, u * 0.0, u * 0.0 };
            POINT pts[4];
            int pnt;
            for (pnt = 0; pnt < 4; pnt++)
                pts[pnt] = apt(pivotx + xs[pnt] * cos(rot) - ys[pnt] * sin(rot),
                               pivoty + xs[pnt] * sin(rot) + ys[pnt] * cos(rot));
            poly(hdc, pts, 4, tone(HX(A7800SESSION_LOGO_RGB), dim), tone(HX(A7800SESSION_LOGO_RGB), dim), 0);
        }
    }
}

/* The grey dish with its four notches, the held sector, the shaft cap tilted
 * toward what is held: round, r 2.5 u. */
static void art_proline_stick(HDC hdc, const a7800_control *ctl)
{
    double bx, by, bw, bh, offx, offy, capx, capy, ang;
    const double u = g_unit;
    const int dim = part_dim(ctl), lit = part_lit(ctl);
    part_box(ctl, &bx, &by, &bw, &bh);
    stick_offset(ctl->port, &offx, &offy);
    {
        const double len = sqrt(offx * offx + offy * offy);
        if (len > 1.0) { offx /= len; offy /= len; }
    }
    {
        const double ctrx = bx + bw / 2, ctry = by + bh / 2;
        ellipse(hdc, ctrx, ctry, u * 2.5, u * 2.5, tone(HX(A7800SESSION_DISH_RGB), dim), tone(HX(0x4A4A4C), dim), 1.0);
        /* the four notches of the cross-shaped recess */
        rrect(hdc, ctrx - u * 0.4, ctry - u * 2.0, u * 0.8, u * 0.7, u * 0.08, tone(HX(A7800SESSION_DISH_RECESS_RGB), dim),
              tone(HX(0x4A4A4C), dim), 0.6);
        rrect(hdc, ctrx - u * 0.4, ctry + u * 1.3, u * 0.8, u * 0.7, u * 0.08, tone(HX(A7800SESSION_DISH_RECESS_RGB), dim),
              tone(HX(0x4A4A4C), dim), 0.6);
        rrect(hdc, ctrx - u * 2.0, ctry - u * 0.4, u * 0.7, u * 0.8, u * 0.08, tone(HX(A7800SESSION_DISH_RECESS_RGB), dim),
                  tone(HX(0x4A4A4C), dim), 0.6);
        rrect(hdc, ctrx + u * 1.3, ctry - u * 0.4, u * 0.7, u * 0.8, u * 0.08, tone(HX(A7800SESSION_DISH_RECESS_RGB), dim),
              tone(HX(0x4A4A4C), dim), 0.6);
        /* a held direction tints its 45-degree sector of the dish */
        if (lit && held_angle(ctl->port, &ang)) {
            sector(hdc, ctrx, ctry, u * 2.5, ang + 22.5, ang - 22.5, mix_color(HX(A7800SESSION_DISH_RGB), COL_HELD_RING, 0.35),
                   COL_HELD_RING, 0.12);
        }
        capx = ctrx + offx * u * 0.9;
        capy = ctry + offy * u * 0.9;
        ellipse(hdc, capx + u * 0.08, capy + u * 0.12, u * 1.0, u * 1.0, tone(HX(0x5A5A5C), dim), tone(HX(0x5A5A5C), dim), 0);
        ellipse(hdc, capx, capy, u * 1.0, u * 1.0, tone(HX(A7800SESSION_CAP_RGB), dim), tone(HX(0x606062), dim), 1.0);
        ellipse(hdc, capx - u * 0.25, capy - u * 0.3, u * 0.45, u * 0.32, tone(HX(A7800SESSION_CAP_HI_RGB), dim),
                tone(HX(A7800SESSION_CAP_HI_RGB), dim), 0);
        if (lit && g_map_state == -2)
            ellipse(hdc, capx, capy, u * 1.12, u * 1.12, NOFILL, COL_HELD_RING, u * 0.1);
    }
}

/* A red fire button in its dark recess: the ProLine's two, the CX40's one. */
static void art_round_button(HDC hdc, const a7800_control *ctl)
{
    double bx, by, bw, bh, ctrx, ctry;
    const double u = g_unit;
    const int dim = part_dim(ctl), lit = part_lit(ctl), pressed = lit && g_map_state == -2;
    const int cx40 = ctl->device == A7800_CTRL_JOY2600;
    const double rad = cx40 ? 0.8 : 1.25;
    part_box(ctl, &bx, &by, &bw, &bh);
    ctrx = bx + bw / 2;
    ctry = by + bh / 2;
    if (!cx40)
        ellipse(hdc, ctrx, ctry, u * 1.5, u * 1.5, tone(HX(A7800SESSION_BUTTON_RECESS_RGB), dim),
                tone(HX(A7800SESSION_BUTTON_RECESS_RGB), dim), 0);
    {
        const COLORREF base = cx40 ? HX(A7800SESSION_CX40_FIRE_RGB) : HX(A7800SESSION_BUTTON_RGB);
        const COLORREF hi = cx40 ? HX(A7800SESSION_CX40_FIRE_HI_RGB) : HX(A7800SESSION_BUTTON_HI_RGB);
        ellipse(hdc, ctrx, ctry, u * rad, u * rad, tone(pressed ? COL_HELD_BUTTON : base, dim),
                tone(pressed ? COL_HELD_BUTTON : mix_color(base, HX(0x000000), 0.35), dim), 1.0);
        if (!pressed)
            ellipse(hdc, ctrx - u * rad * 0.3, ctry - u * rad * 0.33, u * rad * 0.38, u * rad * 0.28, tone(hi, dim),
                    tone(hi, dim), 0);
    }
    if (lit)
        ellipse(hdc, ctrx, ctry, u * (rad + 0.2), u * (rad + 0.2), NOFILL, COL_HELD_RING, u * 0.1);
}

/* ---- the CX40 -------------------------------------------------------------------- */

/* The case, the fire button's recess, the ring of ticks: 8.0 x 8.0 u. */
static void art_cx40_body(HDC hdc, const a7800_control *ctl)
{
    double bx, by, bw, bh, ang = 0;
    const double u = g_unit;
    const int dim = body_dim(ctl->port), have = held_angle(ctl->port, &ang);
    const COLORREF plastic = HX(A7800SESSION_CX40_CASE_RGB), recess = HX(0x101010);
    int idx;
    part_box(ctl, &bx, &by, &bw, &bh);

    rrect(hdc, bx + u * 0.12, by + u * 0.2, bw, bh, u * 0.9, COL_SHADOW, COL_SHADOW, 0);
    rrect(hdc, bx, by, bw, bh, u * 0.9, tone(plastic, dim), tone(recess, dim), 1.2);
    rrect(hdc, bx + u * 0.25, by + u * 0.25, bw - u * 0.5, bh - u * 0.5, u * 0.7, NOFILL,
          tone(HX(A7800SESSION_CX40_BEVEL_RGB), dim), 1.0);
    {   /* the teardrop: the button's recess and its neck to the ring */
        const double fromx = bx + u * 1.7, fromy = by + u * 1.7, tox = bx + u * 4.5, toy = by + u * 4.5;
        const double len = hypot(tox - fromx, toy - fromy);
        const double dirx = (tox - fromx) / len, diry = (toy - fromy) / len, half = u * 0.45;
        POINT neck[4];
        neck[0] = apt(fromx - diry * half, fromy + dirx * half);
        neck[1] = apt(tox - diry * half, toy + dirx * half);
        neck[2] = apt(tox + diry * half, toy - dirx * half);
        neck[3] = apt(fromx + diry * half, fromy - dirx * half);
        poly(hdc, neck, 4, tone(recess, dim), tone(recess, dim), 0);
        ellipse(hdc, fromx, fromy, u * 1.1, u * 1.1, tone(recess, dim), tone(recess, dim), 0);
    }
    ellipse(hdc, bx + u * 4.5, by + u * 4.5, u * 3.0, u * 3.0, tone(recess, dim), tone(HX(0x070707), dim), 1.0);
    for (idx = 0; idx < 36; idx++) {    /* the ring's tick marks; a held direction lights its sector */
        const double deg = idx * 10.0, rad = deg * PI_VALUE / 180.0;
        const int lit = have && angle_between(deg, ang) <= 22.6;
        line_seg(hdc, bx + u * 4.5 + u * 2.52 * sin(rad), by + u * 4.5 - u * 2.52 * cos(rad),
                 bx + u * 4.5 + u * 2.86 * sin(rad), by + u * 4.5 - u * 2.86 * cos(rad),
                 lit ? COL_HELD_RING : tone(HX(A7800SESSION_CX40_TICK_RGB), dim), lit ? u * 0.11 : u * 0.07);
    }
}

/* The ribbed boot and the octagonal shaft cap, tilted toward what is held. */
static void art_cx40_stick(HDC hdc, const a7800_control *ctl)
{
    double bx, by, bw, bh, offx, offy, capx, capy;
    const double u = g_unit;
    const int dim = part_dim(ctl), lit = part_lit(ctl);
    POINT oct[8];
    int idx;
    part_box(ctl, &bx, &by, &bw, &bh);
    stick_offset(ctl->port, &offx, &offy);
    {
        const double len = sqrt(offx * offx + offy * offy);
        if (len > 1.0) { offx /= len; offy /= len; }
    }
    capx = bx + bw / 2 + offx * u * 1.0;
    capy = by + bh / 2 + offy * u * 1.0;
    /* the boot's rings squash toward the tilt for depth */
    ellipse(hdc, bx + bw / 2 + offx * u * 0.08, by + bh / 2 + offy * u * 0.08, u * 2.1, u * 2.1, tone(HX(0x151515), dim),
            tone(HX(0x0A0A0A), dim), 1.0);
    ellipse(hdc, bx + bw / 2 + offx * u * 0.2, by + bh / 2 + offy * u * 0.2, u * 1.7, u * 1.7, tone(HX(0x262626), dim),
            tone(HX(0x151515), dim), 1.0);
    ellipse(hdc, bx + bw / 2 + offx * u * 0.35, by + bh / 2 + offy * u * 0.35, u * 1.3, u * 1.3, tone(HX(0x151515), dim),
            tone(HX(0x262626), dim), 1.0);
    for (idx = 0; idx < 8; idx++) {     /* the octagonal shaft cap */
        const double rad = (idx * 45.0 + 22.5) * PI_VALUE / 180.0;
        oct[idx] = apt(capx + u * 0.9 * sin(rad), capy - u * 0.9 * cos(rad));
    }
    poly(hdc, oct, 8, tone(HX(0x0C0C0C), dim), tone(HX(A7800SESSION_CX40_BEVEL_RGB), dim), u * 0.1);
    ellipse(hdc, capx - u * 0.15, capy - u * 0.18, u * 0.28, u * 0.2, tone(HX(0x232323), dim), tone(HX(0x232323), dim), 0);
    if (lit && g_map_state == -2)
        ellipse(hdc, capx, capy, u * 1.02, u * 1.02, NOFILL, COL_HELD_RING, u * 0.1);
}

/* ---- the XG-1 and the empty socket ------------------------------------------------- */

/* The light gun's silhouette in its 9.0 x 4.6 u box, from the photo of the
 * real one (beige-grey plastic, dark grey trigger), muzzle up and left. */
static void art_gun(HDC hdc, const a7800_control *ctl)
{
    static const int body[][2] = { { 30, 100 }, { 370, 180 }, { 630, 205 }, { 1190, 375 }, { 1250, 470 }, { 1230, 520 },
                                   { 990, 500 }, { 990, 885 }, { 825, 830 }, { 830, 555 }, { 740, 470 }, { 380, 310 },
                                   { 250, 230 }, { 30, 200 } };
    static const int trigger[][2] = { { 770, 470 }, { 860, 560 }, { 850, 600 }, { 800, 565 } };
    static const int stripe[][2] = { { 540, 240 }, { 1020, 410 } };
    double bx, by, bw, bh;
    const double scale = 4.6 * g_unit / 1215.0;
    double ox;
    const int dim = body_dim(ctl->port);
    POINT pts[16];
    size_t idx;
    part_box(ctl, &bx, &by, &bw, &bh);
    ox = bx + (bw - 1250 * scale) / 2.0 - 30 * scale;
#define GUN_X(px) (ox + (px) * scale)
#define GUN_Y(py) (by + (py) * scale - 10 * scale)
    for (idx = 0; idx < sizeof body / sizeof body[0]; idx++)
        pts[idx] = apt(GUN_X(body[idx][0]) + g_unit * 0.1, GUN_Y(body[idx][1]) + g_unit * 0.15);
    poly(hdc, pts, (int)(sizeof body / sizeof body[0]), COL_SHADOW, COL_SHADOW, 0);
    for (idx = 0; idx < sizeof body / sizeof body[0]; idx++)
        pts[idx] = apt(GUN_X(body[idx][0]), GUN_Y(body[idx][1]));
    poly(hdc, pts, (int)(sizeof body / sizeof body[0]), tone(HX(0xB9B5AB), dim), tone(HX(0x8E8A80), dim), 1.0);
    for (idx = 0; idx < sizeof trigger / sizeof trigger[0]; idx++)
        pts[idx] = apt(GUN_X(trigger[idx][0]), GUN_Y(trigger[idx][1]));
    poly(hdc, pts, 4, tone(HX(0x5D5E60), dim), tone(HX(0x5D5E60), dim), 0);
    line_seg(hdc, GUN_X(stripe[0][0]), GUN_Y(stripe[0][1]), GUN_X(stripe[1][0]), GUN_Y(stripe[1][1]),
             tone(HX(0xA29E94), dim), 0.12);
    for (idx = 0; idx < 7; idx++)       /* the grip's ribs */
        line_seg(hdc, GUN_X(850), GUN_Y(600 + (int)idx * 40), GUN_X(985), GUN_Y(600 + (int)idx * 40),
                 tone(HX(0x9A968C), dim), 0.05);
    ellipse(hdc, GUN_X(75), GUN_Y(150), g_unit * 0.22, g_unit * 0.3, tone(HX(0x6E6B63), dim), tone(HX(0x8E8A80), dim), 0.5);
#undef GUN_X
#undef GUN_Y
}

/* An empty port: the socket's outline with its row of pin holes. */
static void art_socket(HDC hdc, const a7800_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = body_dim(ctl->port);
    int idx;
    part_box(ctl, &bx, &by, &bw, &bh);
    rrect(hdc, bx, by, bw, bh, u * 0.8, tone(HX(0x55524A), dim), tone(HX(A7800SESSION_SOCKET_RGB), dim), 1.5);
    for (idx = 0; idx < 9; idx++) {
        ellipse(hdc, bx + u * (2.2 + idx * 0.95), by + u * 4.6, u * 0.22, u * 0.22, tone(HX(0x1A1A1C), dim),
                tone(HX(0x1A1A1C), dim), 0);
        if (idx < 4)
            ellipse(hdc, bx + u * (2.7 + idx * 0.95), by + u * 5.2, u * 0.22, u * 0.22, tone(HX(0x1A1A1C), dim),
                    tone(HX(0x1A1A1C), dim), 0);
    }
}

/* ---- the console ----------------------------------------------------------------- */

static void art_console_body(HDC hdc, const a7800_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = body_dim(-1);
    part_box(ctl, &bx, &by, &bw, &bh);
    rrect(hdc, bx + u * 0.1, by + u * 0.16, bw, bh, u * 0.5, COL_SHADOW, COL_SHADOW, 0);
    rrect(hdc, bx, by, bw, bh, u * 0.5, tone(HX(A7800SESSION_CASE_RGB), dim), tone(HX(A7800SESSION_CASE_SHADOW_RGB), dim),
          1.2);
    rrect(hdc, bx + u * 0.15, by + u * 0.15, bw - u * 0.3, bh - u * 0.3, u * 0.4, NOFILL,
          tone(HX(A7800SESSION_CASE_BEVEL_RGB), dim), 0.8);
}

/* The brushed-silver band across the case. */
static void art_band(HDC hdc, const a7800_control *ctl)
{
    double bx, by, bw, bh;
    const int dim = body_dim(-1);
    part_box(ctl, &bx, &by, &bw, &bh);
    grad_rrect(hdc, bx, by, bw, bh, g_unit * 0.15, tone(HX(A7800SESSION_BAND_HI_RGB), dim),
               tone(HX(A7800SESSION_BAND_LO_RGB), dim), tone(HX(0x7A7A80), dim), 0.8);
}

/* The thin rainbow along the band's lower edge: orange, violet, blue, green. */
static void art_rainbow(HDC hdc, const a7800_control *ctl)
{
    static const unsigned stops[4] = { A7800SESSION_RAINBOW_ORANGE_RGB, A7800SESSION_RAINBOW_VIOLET_RGB,
                                       A7800SESSION_RAINBOW_BLUE_RGB, A7800SESSION_RAINBOW_GREEN_RGB };
    double bx, by, bw, bh;
    const int dim = body_dim(-1);
    int left, right, first, last, col;
    HGDIOBJ oldb;
    part_box(ctl, &bx, &by, &bw, &bh);
    left = art_xpos(bx);
    right = art_xpos(bx + bw);
    first = art_ypos(by + bh * 0.25);
    last = art_ypos(by + bh * 0.75);
    if (last <= first) last = first + 1;
    oldb = SelectObject(hdc, GetStockObject(DC_BRUSH));
    for (col = left; col < right; col++) {
        const double pos = (double)(col - left) / (right - left > 1 ? right - left - 1 : 1) * 3.0;
        int seg = (int)pos;
        if (seg > 2) seg = 2;
        SetDCBrushColor(hdc, tone(mix_color(HX(stops[seg]), HX(stops[seg + 1]), pos - seg), dim));
        PatBlt(hdc, col, first, 1, last - first, PATCOPY);
    }
    SelectObject(hdc, oldb);
}

/* A flat console key (PAUSE, SELECT, RESET), or POWER dimmed: it is not a target. */
static void art_console_key(HDC hdc, const a7800_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int power = ctl->kind == A7800_CTL_BODY;
    const int dim = power ? body_dim(-1) : part_dim(ctl);
    const int lit = !power && part_lit(ctl), pressed = lit && g_map_state == -2;
    COLORREF fill = HX(A7800SESSION_KEY_RGB);
    part_box(ctl, &bx, &by, &bw, &bh);
    if (pressed) fill = mix_color(fill, COL_HELD_RING, 0.35);
    rrect(hdc, bx, by, bw, bh, u * 0.12, tone(fill, dim), tone(HX(0x44444A), dim), 1.0);
    if (!pressed)
        line_seg(hdc, bx + u * 0.15, by + u * 0.1, bx + bw - u * 0.15, by + u * 0.1, tone(HX(0x2C2C30), dim), 0.08);
    if (lit)
        rrect(hdc, bx - u * 0.1, by - u * 0.1, bw + u * 0.2, bh + u * 0.2, u * 0.2, NOFILL, COL_HELD_RING, u * 0.1);
}

/* A difficulty slide: a slot with its silver slider at the A end (right, pro)
 * or the B end (left, novice). */
static void art_slide(HDC hdc, const a7800_control *ctl)
{
    double bx, by, bw, bh, knobx;
    const double u = g_unit;
    const int dim = part_dim(ctl), picked = g_map_state >= 0 && ctl->target == g_map_state;
    const int pro = (g_shown.switches >> (ctl->target - A7800_TARGET_SWITCH(0))) & 1u;
    part_box(ctl, &bx, &by, &bw, &bh);
    rrect(hdc, bx + u * 0.8, by + u * 0.1, u * 3.0, u * 1.2, u * 0.15, tone(HX(A7800SESSION_SLIDE_SLOT_RGB), dim),
          tone(HX(0x44444A), dim), 1.0);
    knobx = pro ? bx + u * 0.8 + u * 3.0 - u * 1.3 : bx + u * 0.8 + u * 0.1;
    rrect(hdc, knobx + u * 0.04, by + u * 0.22, u * 1.2, u * 1.0, u * 0.2, COL_SHADOW, COL_SHADOW, 0);
    rrect(hdc, knobx, by + u * 0.2, u * 1.2, u * 1.0, u * 0.2, tone(HX(A7800SESSION_SLIDE_KNOB_RGB), dim),
          tone(HX(0x8E8E8E), dim), 1.0);
    line_seg(hdc, knobx + u * 0.6, by + u * 0.3, knobx + u * 0.6, by + u * 1.1, tone(HX(0x8E8E8E), dim), 0.08);
    if (picked)
        rrect(hdc, bx + u * 0.65, by - u * 0.05, u * 3.3, u * 1.5, u * 0.2, NOFILL, COL_HELD_RING, u * 0.1);
}

/* The mat and every device in the layout, in layout order (so a body is drawn
 * before the parts on it). */
static void art_all(HDC hdc)
{
    int idx;
    grad_rrect(hdc, g_mat_rc.left, g_mat_rc.top, g_mat_rc.right - g_mat_rc.left, g_mat_rc.bottom - g_mat_rc.top,
               scaled(14), COL_MAT_TOP, COL_MAT_BOT, COL_MAT_EDGE, 1.5);
    for (idx = 0; idx < g_numlayout; idx++) {
        const a7800_control *ctl = &g_layout[idx];
        switch (ctl->kind) {
        case A7800_CTL_BODY:
            if (!strcmp(ctl->label, "ProLine")) art_proline_body(hdc, ctl);
            else if (!strcmp(ctl->label, "CX40")) art_cx40_body(hdc, ctl);
            else if (!strcmp(ctl->label, "XG-1")) art_gun(hdc, ctl);
            else if (!strcmp(ctl->label, "Socket")) art_socket(hdc, ctl);
            else if (!strcmp(ctl->label, "Console")) art_console_body(hdc, ctl);
            else if (!strcmp(ctl->label, "Band")) art_band(hdc, ctl);
            else if (!strcmp(ctl->label, "Rainbow")) art_rainbow(hdc, ctl);
            else if (!strcmp(ctl->label, "POWER")) art_console_key(hdc, ctl);
            break;
        case A7800_CTL_STICK:
            if (ctl->device == A7800_CTRL_JOY2600) art_cx40_stick(hdc, ctl);
            else art_proline_stick(hdc, ctl);
            break;
        case A7800_CTL_ROUND_BUTTON: art_round_button(hdc, ctl); break;
        case A7800_CTL_BUTTON: art_console_key(hdc, ctl); break;
        case A7800_CTL_SWITCH: art_slide(hdc, ctl); break;
        default: break;
        }
    }
}

/* ---- the text over the art ------------------------------------------------------------- */

/* A target's binding, printed on its part. */
static void text_binding_on(HDC hdc, const a7800_control *ctl, const RECT *rect, int dim)
{
    char legend[96];
    RECT box = *rect;
    if (!binding_mode() || ctl->target < 0) return;
    binding_label(ctl->target, legend, sizeof legend);
    SelectObject(hdc, g_tiny);
    box.left -= (int)(g_unit * 0.15);
    box.right += (int)(g_unit * 0.15);
    SetTextColor(hdc, g_map_state == ctl->target ? COL_HELD_RING : tone(RGB(0xFF, 0xFF, 0xFF), dim));
    text_line(hdc, legend, &box, DT_CENTER | DT_END_ELLIPSIS);
}

/* The four directions' keys on a stick: inside the ProLine's dish, round the
 * CX40's ring. */
static void text_stick_bindings(HDC hdc, const a7800_control *ctl, const RECT *rect)
{
    static const int dirs[4] = { A7800_ACT_LEFT, A7800_ACT_RIGHT, A7800_ACT_UP, A7800_ACT_DOWN };
    const double u = g_unit;
    const int cx40 = ctl->device == A7800_CTRL_JOY2600;
    const double reach = cx40 ? 2.5 : 1.6;
    const int midx = (rect->left + rect->right) / 2, midy = (rect->top + rect->bottom) / 2;
    int dir;
    SelectObject(hdc, g_tiny);
    for (dir = 0; dir < 4; dir++) {
        const int target = A7800_TARGET_PORT(ctl->port, dirs[dir]);
        const int half = (int)(u * 0.75);
        int posx = midx, posy = midy;
        RECT box;
        char legend[96];
        if (dirs[dir] == A7800_ACT_LEFT) posx = midx - (int)(u * (reach + (cx40 ? 0.0 : 0.1)));
        if (dirs[dir] == A7800_ACT_RIGHT) posx = midx + (int)(u * (reach + (cx40 ? 0.0 : 0.1)));
        if (dirs[dir] == A7800_ACT_UP) posy = midy - (int)(u * (reach - (cx40 ? 0.0 : 0.15)));
        if (dirs[dir] == A7800_ACT_DOWN) posy = midy + (int)(u * (reach - (cx40 ? 0.0 : 0.15)));
        SetRect(&box, posx - half, posy - scaled(7), posx + half, posy + scaled(7));
        binding_label(target, legend, sizeof legend);
        SetTextColor(hdc, g_map_state == target ? COL_HELD_RING : RGB(0xFF, 0xFF, 0xFF));
        text_line(hdc, legend, &box, DT_CENTER | DT_END_ELLIPSIS);
    }
}

static void text_parts(HDC hdc)
{
    int idx;
    SetBkMode(hdc, TRANSPARENT);
    for (idx = 0; idx < g_numlayout; idx++) {
        const a7800_control *ctl = &g_layout[idx];
        RECT rect;
        const double u = g_unit;
        part_rect(ctl, &rect);
        switch (ctl->kind) {
        case A7800_CTL_CAPTION:
            if (ctl->port < 0) {
                if (!strcmp(ctl->label, "7800 PROSYSTEM")) {
                    SelectObject(hdc, g_title);
                    SetTextColor(hdc, tone(HX(A7800SESSION_BAND_TEXT_RGB), body_dim(-1)));
                    text_line(hdc, ctl->label, &rect, DT_LEFT);
                }
            } else if (ctl->device < 0) {
                SelectObject(hdc, g_caption);
                SetTextColor(hdc, COL_CAPTION);
                text_line(hdc, ctl->label, &rect, DT_CENTER);
            } else if (ctl->device == A7800_CTRL_LIGHTGUN) {
                SelectObject(hdc, g_font);
                SetTextColor(hdc, tone(COL_CAPTION, body_dim(ctl->port)));
                text_line(hdc, ctl->label, &rect, DT_CENTER);
            } else {
                SelectObject(hdc, g_font);
                SetTextColor(hdc, tone(COL_CAPTION, body_dim(ctl->port)));
                text_line(hdc, ctl->label, &rect, DT_CENTER);
            }
            break;
        case A7800_CTL_BODY:
            if (!strcmp(ctl->label, "ProLine")) {
                static const wchar_t atari[] = L"ATARI";
                const int dim = body_dim(ctl->port);
                int btn;
                /* the embossed ATARI, rising 15 degrees to the right, and the buttons' numerals */
                SelectObject(hdc, g_logo);
                SetTextColor(hdc, tone(HX(A7800SESSION_LOGO_RGB), dim));
                SetTextAlign(hdc, TA_LEFT | TA_BASELINE);
                TextOutW(hdc, rect.left + (int)(u * 12.2), rect.top + (int)(u * 2.35), atari, 5);
                SetTextAlign(hdc, TA_LEFT | TA_TOP);
                SelectObject(hdc, g_key);
                SetTextColor(hdc, tone(HX(A7800SESSION_NUMERAL_RGB), dim));
                for (btn = 0; btn < 2; btn++) {
                    RECT num;
                    SetRect(&num, rect.left + (int)(u * (6.5 + btn * 4.0)), rect.top + (int)(u * 2.9),
                            rect.left + (int)(u * (7.5 + btn * 4.0)), rect.top + (int)(u * 3.7));
                    text_line(hdc, btn ? "2" : "1", &num, DT_CENTER);
                }
            } else if (!strcmp(ctl->label, "CX40")) {
                RECT top = rect;
                SelectObject(hdc, g_tiny);
                SetTextColor(hdc, tone(HX(0x3A3A3A), body_dim(ctl->port)));
                top.left += (int)(u * 3.3);
                top.right -= (int)(u * 3.3);
                top.top += (int)(u * 1.25);
                top.bottom = top.top + (int)(u * 0.5);
                text_line(hdc, "TOP", &top, DT_CENTER);
            } else if (!strcmp(ctl->label, "XG-1")) {
                /* nothing printed on the gun */
            } else if (!strcmp(ctl->label, "POWER")) {
                RECT face = rect;
                SelectObject(hdc, g_legend);
                SetTextColor(hdc, tone(HX(0x5C5C62), body_dim(-1)));
                text_line(hdc, ctl->label, &face, DT_CENTER);
            }
            break;
        case A7800_CTL_ROUND_BUTTON:
            if (binding_mode() && ctl->target >= 0) {
                char legend[96];
                RECT box = rect;
                InflateRect(&box, (int)(u * 0.2), 0);
                binding_label(ctl->target, legend, sizeof legend);
                SelectObject(hdc, g_tiny);
                SetTextColor(hdc, g_map_state == ctl->target ? COL_HELD_RING : tone(RGB(0xFF, 0xFF, 0xFF), part_dim(ctl)));
                text_line(hdc, legend, &box, DT_CENTER | DT_END_ELLIPSIS);
            }
            break;
        case A7800_CTL_BUTTON: {
            RECT face = rect;
            const int lit = part_lit(ctl), dim = part_dim(ctl);
            if (binding_mode()) {
                char legend[96];
                binding_label(ctl->target, legend, sizeof legend);
                SelectObject(hdc, g_tiny);
                SetTextColor(hdc, g_map_state == ctl->target ? COL_HELD_RING : tone(HX(A7800SESSION_KEY_LABEL_RGB), dim));
                text_line(hdc, legend, &face, DT_CENTER | DT_END_ELLIPSIS);
            } else {
                SelectObject(hdc, g_legend);
                SetTextColor(hdc, lit ? RGB(255, 255, 255) : tone(HX(A7800SESSION_KEY_LABEL_RGB), dim));
                text_line(hdc, ctl->label, &face, DT_CENTER);
            }
            break;
        }
        case A7800_CTL_SWITCH: {
            const int dim = part_dim(ctl);
            const int pro = (g_shown.switches >> (ctl->target - A7800_TARGET_SWITCH(0))) & 1u;
            RECT letter, below = rect;
            SelectObject(hdc, g_legend);
            SetRect(&letter, rect.left, rect.top, rect.left + (int)(u * 0.8), rect.bottom);
            SetTextColor(hdc, !pro ? RGB(255, 255, 255) : tone(HX(0x7C7C82), dim));
            text_line(hdc, "B", &letter, DT_CENTER);
            SetRect(&letter, rect.left + (int)(u * 3.8), rect.top, rect.right, rect.bottom);
            SetTextColor(hdc, pro ? RGB(255, 255, 255) : tone(HX(0x7C7C82), dim));
            text_line(hdc, "A", &letter, DT_CENTER);
            /* under the slot: what it is, or in binding mode what drives it */
            below.top = rect.bottom;
            below.bottom = rect.bottom + (int)(u * 0.8);
            below.left -= (int)(u * 0.6);
            below.right += (int)(u * 0.6);
            if (binding_mode()) {
                text_binding_on(hdc, ctl, &below, dim);
            } else {
                SelectObject(hdc, g_tiny);
                SetTextColor(hdc, tone(HX(0x9A9AA0), dim));
                text_line(hdc, ctl->legend && strstr(ctl->legend, "left") ? "LEFT DIFF" : "RIGHT DIFF", &below, DT_CENTER);
            }
            break;
        }
        case A7800_CTL_STICK:
            if (binding_mode()) text_stick_bindings(hdc, ctl, &rect);
            break;
        default:
            break;
        }
    }
}

/* ---- the strip: type selectors, gamepads, Map, hint -------------------------------------- */

static void draw_face(HDC hdc, const RECT *bounds, int pushed, int accent, int inactive)
{
    RECT rect = *bounds;
    if (accent) {
        HBRUSH back = CreateSolidBrush(rgb_of(A7800SESSION_ACCENT_RGB));
        HBRUSH edge = CreateSolidBrush(rgb_of(A7800SESSION_ACCENT_DARK_RGB));
        FillRect(hdc, &rect, back);
        FrameRect(hdc, &rect, edge);
        DeleteObject(back);
        DeleteObject(edge);
        SetTextColor(hdc, RGB(255, 255, 255));
    } else {
        DrawFrameControl(hdc, &rect, DFC_BUTTON,
                         DFCS_BUTTONPUSH | (pushed ? DFCS_PUSHED : 0) | (inactive ? DFCS_INACTIVE : 0));
        SetTextColor(hdc, GetSysColor(inactive ? COLOR_GRAYTEXT : COLOR_BTNTEXT));
    }
}

static const char *strip_label(int btn)
{
    switch (btn) {
    case SB_MAP: return g_map_state == -2 ? "Map" : "Done";
    case SB_CLEAR: return "Clear";
    case SB_DEFAULTS: return "Defaults";
    case SB_SHOW: return "Show bindings";
    default: return "Help";
    }
}

/* The choice at the right of a gamepad's row: segment 0 is Automatic, n the
 * player n. */
static void segment_rect(const RECT *row, int seg, RECT *rect)
{
    const int gap = scaled(4);
    const int total = scaled(SEG_AUTO_W) + A7800_PORTS * scaled(SEG_W) + A7800_PORTS * gap;
    int left = row->right - total, idx;
    for (idx = 0; idx < seg; idx++)
        left += (idx == 0 ? scaled(SEG_AUTO_W) : scaled(SEG_W)) + gap;
    SetRect(rect, left, row->top, left + (seg == 0 ? scaled(SEG_AUTO_W) : scaled(SEG_W)), row->bottom - 1);
}

/* One line per gamepad: its name and the player it drives now, then the
 * Automatic / Player 1 / Player 2 choice, the current one in the accent. */
static void paint_gamepads(HDC hdc)
{
    static const char *const labels[A7800_PORTS + 1] = { "Automatic", "Player 1", "Player 2" };
    const int num = a7800session_gamepad_count(g_session);
    int idx, seg;

    SelectObject(hdc, g_bold);
    SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
    text_line(hdc, "Gamepads", &g_gp_head_rc, DT_LEFT);
    SelectObject(hdc, g_font);
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    if (num == 0) {
        text_line(hdc, "No gamepads connected - plug one in", &g_gp_row_rc[0], DT_LEFT);
        return;
    }
    if (num > PAD_ROWS) {
        char more[48];
        snprintf(more, sizeof more, "%d more not shown", num - PAD_ROWS);
        text_line(hdc, more, &g_gp_head_rc, DT_RIGHT);
    }
    for (idx = 0; idx < num && idx < PAD_ROWS; idx++) {
        const int assign = a7800session_gamepad_assignment(g_session, idx);
        const int effective = a7800session_gamepad_effective_port(g_session, idx);
        const int chosen = assign < 0 || assign >= A7800_PORTS ? 0 : assign + 1;
        char name[128], line[200];
        RECT name_rc, first;

        segment_rect(&g_gp_row_rc[idx], 0, &first);
        name_rc = g_gp_row_rc[idx];
        name_rc.right = first.left - scaled(6);
        a7800session_gamepad_name(g_session, idx, name, sizeof name);
        if (effective >= 0) snprintf(line, sizeof line, "%s  (player %d)", name, effective + 1);
        else snprintf(line, sizeof line, "%s  (no player)", name);
        SelectObject(hdc, g_font);
        SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
        text_line(hdc, line, &name_rc, DT_LEFT | DT_END_ELLIPSIS);
        for (seg = 0; seg <= A7800_PORTS; seg++) {
            RECT rect;
            segment_rect(&g_gp_row_rc[idx], seg, &rect);
            draw_face(hdc, &rect, 0, seg == chosen, 0);
            text_line(hdc, labels[seg], &rect, DT_CENTER);
        }
    }
}

/* What the hint line says about the part under an idle pointer. */
static void hover_text(int part, char *out, int outsz)
{
    const a7800_control *ctl;
    char key[64];
    a7800_binding bind;
    out[0] = '\0';
    if (part < 0 || part >= g_numlayout) return;
    ctl = &g_layout[part];
    if (ctl->kind == A7800_CTL_STICK) {
        snprintf(out, (size_t)outsz, "Player %d stick: drag it to push the stick", ctl->port + 1);
        return;
    }
    if (ctl->target < 0) return;
    bind = a7800session_binding_get(g_session, ctl->target);
    key[0] = '\0';
    if (bind.keysym) a7800session_keysym_name(bind.keysym, key, sizeof key);
    snprintf(out, (size_t)outsz, "%s   Key: %s   Gamepad: %s", a7800_target_name(ctl->target), key[0] ? key : "none",
             bind.button != A7800_PAD_BTN_NONE ? a7800_pad_button_name(bind.button) : "none");
}

static void paint_strip(HDC hdc)
{
    int btn;
    char hover[200];
    SelectObject(hdc, g_font);
    for (btn = 0; btn < SB_COUNT; btn++) {
        const int armed = (btn == SB_MAP && g_map_state != -2) || (btn == SB_SHOW && g_show_bindings);
        const int inactive = btn == SB_CLEAR && g_map_state < 0;
        draw_face(hdc, &g_btn_rc[btn], 0, armed, inactive);
        text_line(hdc, strip_label(btn), &g_btn_rc[btn], DT_CENTER);
    }
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    hover_text(g_map_state >= 0 ? -1 : g_hover_part, hover, sizeof hover);
    text_line(hdc, hover[0] ? hover : g_hint, &g_hint_rc, DT_LEFT | DT_END_ELLIPSIS);
}

/* The whole window: the controllers' art supersampled, their text, the strips.
 * Only what the update region needs is drawn. */
static void paint_rect(HDC hdc, const RECT *inval)
{
    RECT art;
    const int scale = 2;

    FillRect(hdc, inval, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(hdc, TRANSPARENT);

    IntersectRect(&art, inval, &g_mat_rc);
    if (!IsRectEmpty(&art)) {
        const int width = art.right - art.left, height = art.bottom - art.top;
        BITMAPINFO bmi;
        void *bits = NULL;
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp;
        memset(&bmi, 0, sizeof bmi);
        bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
        bmi.bmiHeader.biWidth = width * scale;
        bmi.bmiHeader.biHeight = -height * scale;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (bmp) {
            HBRUSH face = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
            RECT all = { 0, 0, width * scale, height * scale };
            HGDIOBJ old = SelectObject(mem, bmp);
            FillRect(mem, &all, face);
            DeleteObject(face);
            SetBkMode(mem, TRANSPARENT);
            g_art_k = scale;
            g_art_x = art.left;
            g_art_y = art.top;
            g_art_rows = height * scale;
            art_all(mem);
            g_art_k = 1;
            SetStretchBltMode(hdc, HALFTONE);
            SetBrushOrgEx(hdc, 0, 0, NULL);
            StretchBlt(hdc, art.left, art.top, width, height, mem, 0, 0, width * scale, height * scale, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
        }
        DeleteDC(mem);
        SelectObject(hdc, g_font);
        text_parts(hdc);
    }
    paint_gamepads(hdc);
    paint_strip(hdc);
}

/* ---- pressing ------------------------------------------------------------------- */

static void invalidate_art(void)
{
    if (g_panel) InvalidateRect(g_panel, &g_mat_rc, FALSE);
}

/* Lets go of what the pointer holds in this window (only that: the main
 * window releases its own keys). */
static void mouse_release(void)
{
    g_mouse_part = -1;
    if (g_session) a7800session_release_pointer(g_session);
    g_stick_dx = g_stick_dy = 0;
    if (g_panel) {
        refresh_held();
        invalidate_art();
    }
}

static void set_hint(const char *text)
{
    snprintf(g_hint, sizeof g_hint, "%s", text);
    g_hover_part = -1;
    if (g_panel) InvalidateRect(g_panel, &g_hint_rc, FALSE);
}

#define IDLE_HINT "Press a button or key to hold it; drag a stick; click a slide to flip it. Map rebinds."

static void set_map_state(int state)
{
    g_map_state = state;
    if (state != -2) {
        mouse_release();                    /* a held part must not outlive Map being armed */
        a7800session_release_all(g_session);   /* Map swallows key-ups, so drop keyboard holds too */
    }
    if (state < 0) {
        if (g_panel) KillTimer(g_panel, IDT_CAPTURE);
        a7800session_gamepad_capture_cancel(g_session);
    }
    if (state == -2) {
        set_hint(IDLE_HINT);
    } else if (state == -1) {
        set_hint("Click a button, a key or a stick direction to remap it. Esc finishes.");
    } else {
        char msg[200];
        snprintf(msg, sizeof msg, "Press a key or a gamepad button for %s. Esc cancels.", a7800_target_name(state));
        set_hint(msg);
        a7800session_gamepad_capture_begin(g_session);
        if (g_panel) SetTimer(g_panel, IDT_CAPTURE, 50, NULL);
    }
    if (g_panel) {
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

/* After a rebind: what it is now, and whom it was taken from. */
static void bound_hint(int target, const char *what, const char *was)
{
    char msg[300], name[96];
    snprintf(name, sizeof name, "%s", a7800_target_name(target));   /* a static buffer */
    if (was && was[0]) snprintf(msg, sizeof msg, "Bound %s to %s (was %s)", name, what, was);
    else snprintf(msg, sizeof msg, "Bound %s to %s", name, what);
    set_hint(msg);
}

/* ---- layout --------------------------------------------------------------------- */


static int g_view[A7800_PORTS];     /* the controller drawn on each port */

static void fetch_layout(void)
{
    int port;
    for (port = 0; port < A7800_PORTS; port++) g_view[port] = a7800session_view_type(g_session, port);
    g_layout = a7800session_controls_layout(g_view[0], g_view[1], &g_numlayout, &g_layw, &g_layh);
}

/* The mat with the controllers in it, the type selectors, the gamepad rows and
 * the Map row: the window is this size at this DPI, always. */
static void layout_static(void)
{
    static const int widths[SB_COUNT] = { 56, 56, 76, 108, 56 };
    const int left = scaled(MARGIN);
    int ypos = scaled(MARGIN), idx, xpos, content_w, port;

    fetch_layout();
    g_unit = (double)UNIT96 * g_dpi / 96.0;
    content_w = (int)ceil((double)g_layw * g_unit) + 2 * scaled(CASE_PAD);
    g_total_w = 2 * left + content_w;

    g_orgx = left + scaled(CASE_PAD);
    g_orgy = ypos + scaled(CASE_PAD);
    SetRect(&g_mat_rc, left, ypos, left + content_w, (int)ceil(g_orgy + (double)g_layh * g_unit) + scaled(CASE_PAD));
    ypos = g_mat_rc.bottom + scaled(8);

    /* a type selector under each port, as wide as its view */
    for (port = 0; port < A7800_PORTS; port++) {
        if (!g_combo[port]) continue;
        MoveWindow(g_combo[port], (int)(g_orgx + port * 17.0 * g_unit), ypos, (int)(16.0 * g_unit),
                   scaled(TYPE_H) + scaled(220), TRUE);
        SendMessageA(g_combo[port], WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageA(g_combo[port], CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)scaled(TYPE_H - 6));
    }
    ypos += scaled(TYPE_H) + scaled(10);

    SetRect(&g_gp_head_rc, left, ypos, left + content_w, ypos + scaled(HEAD_H));
    ypos += scaled(HEAD_H);
    for (idx = 0; idx < PAD_ROWS; idx++) {
        SetRect(&g_gp_row_rc[idx], left, ypos, left + content_w, ypos + scaled(ROW_H));
        ypos += scaled(ROW_H + ROW_GAP);
    }
    SetRect(&g_gp_rc, left, g_gp_head_rc.top, left + content_w, ypos);
    ypos += scaled(6);

    xpos = left;
    for (idx = 0; idx < SB_HELP; idx++) {
        SetRect(&g_btn_rc[idx], xpos, ypos, xpos + scaled(widths[idx]), ypos + scaled(BTN_H));
        xpos += scaled(widths[idx]) + scaled(6);
    }
    SetRect(&g_btn_rc[SB_HELP], left + content_w - scaled(widths[SB_HELP]), ypos, left + content_w, ypos + scaled(BTN_H));
    ypos += scaled(BTN_H) + scaled(6);
    SetRect(&g_hint_rc, left, ypos, left + content_w, ypos + scaled(HINT_H));
    g_total_h = ypos + scaled(HINT_H) + scaled(MARGIN);
}

/* "Auto (now: ProLine)" names what Auto resolved to for the running game. */
static void sync_combos(void)
{
    int port;
    for (port = 0; port < A7800_PORTS; port++) {
        char want[96], have[96];
        const char *now = a7800_ctrl_type_name(a7800session_port_detected(g_session, port));
        int sel;
        if (!g_combo[port]) continue;
        snprintf(want, sizeof want, "Auto (now: %s)", now ? now : "?");
        have[0] = '\0';
        if (SendMessageA(g_combo[port], CB_GETLBTEXTLEN, 0, 0) < (LRESULT)sizeof have)
            SendMessageA(g_combo[port], CB_GETLBTEXT, 0, (LPARAM)have);
        sel = a7800session_port_type(g_session, port);
        if (strcmp(have, want) != 0) {
            SendMessageA(g_combo[port], CB_DELETESTRING, 0, 0);
            SendMessageA(g_combo[port], CB_INSERTSTRING, 0, (LPARAM)want);
        }
        if ((int)SendMessageA(g_combo[port], CB_GETCURSEL, 0, 0) != sel)
            SendMessageA(g_combo[port], CB_SETCURSEL, (WPARAM)sel, 0);
    }
}

static void rebuild(void)
{
    mouse_release();
    if (g_panel && GetCapture() == g_panel) ReleaseCapture();
    make_fonts();
    layout_static();
    memset(&g_shown, 0, sizeof g_shown);
    if (g_panel) {
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

/* ---- polling -------------------------------------------------------------------- */

/* What the console sees held, the controller on each port, the slides: redrawn
 * when any of it changed. */
static void refresh_held(void)
{
    held_state now;
    int port, sw;
    if (!g_panel || !g_session) return;
    memset(&now, 0, sizeof now);
    for (port = 0; port < A7800_PORTS; port++) {
        now.held[port] = a7800session_buttons_held(g_session, port);
        now.view[port] = a7800session_view_type(g_session, port);
        now.detected[port] = a7800session_port_detected(g_session, port);
    }
    if (g_mouse_part >= 0 && g_mouse_part < g_numlayout && g_layout[g_mouse_part].kind == A7800_CTL_STICK)
        now.dragging[g_layout[g_mouse_part].port] = 1;
    for (sw = 0; sw < A7800_SW_COUNT; sw++) {
        const int on = sw == A7800_SW_LEFT_DIFF || sw == A7800_SW_RIGHT_DIFF ? a7800session_switch_get(g_session, sw)
                                                                           : a7800session_switch_held(g_session, sw);
        if (on) now.switches |= 1u << sw;
    }
    if (memcmp(&now, &g_shown, sizeof now) != 0) {
        const int views_changed = now.view[0] != g_shown.view[0] || now.view[1] != g_shown.view[1];
        const int detect_changed = now.detected[0] != g_shown.detected[0] || now.detected[1] != g_shown.detected[1];
        g_shown = now;
        if (views_changed) {
            /* a port's controller changed under the pointer: whatever it held is gone with it */
            if (g_mouse_part >= 0) {
                g_mouse_part = -1;
                a7800session_release_pointer(g_session);
                if (GetCapture() == g_panel) ReleaseCapture();
            }
            fetch_layout();
        }
        if (views_changed || detect_changed) sync_combos();
        invalidate_art();
    }
}

/* Hot-plugged pads and auto-assignment changes. */
static void refresh_pads(void)
{
    pads_state now;
    int idx;
    if (!g_session) return;
    memset(&now, 0, sizeof now);
    now.generation = a7800session_gamepad_generation(g_session);
    now.count = a7800session_gamepad_count(g_session);
    for (idx = 0; idx < PAD_ROWS && idx < now.count; idx++) {
        now.assign[idx] = a7800session_gamepad_assignment(g_session, idx);
        now.effective[idx] = a7800session_gamepad_effective_port(g_session, idx);
    }
    if (memcmp(&now, &g_pads_shown, sizeof now) != 0) {
        g_pads_shown = now;
        if (g_panel) InvalidateRect(g_panel, &g_gp_rc, FALSE);
    }
}

/* ---- keys ----------------------------------------------------------------------- */

static void hide_panel(void)
{
    set_map_state(-2);
    mouse_release();
    if (GetCapture() == g_panel) ReleaseCapture();
    g_swallow_vk = 0;
    ShowWindow(g_panel, SW_HIDE);
}

/* The app's own keys, never the machine's or Map's. */
static int app_key(WPARAM vk)
{
    return vk == VK_F9 || vk == VK_F11 || vk == VK_F12;
}

/* A key while Map is armed: 1 if Map took it. Waiting for a target, a key is
 * not typing; waiting for a binding, the key is the binding (not F9, F11 or
 * F12, which are the app's). */
static int map_key(UINT msg, WPARAM vk, LPARAM lp)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;

    if (g_swallow_vk && vk == g_swallow_vk) {
        if (!down) g_swallow_vk = 0;
        return 1;
    }
    if (g_map_state == -2) return 0;
    if (app_key(vk) || vk == VK_F10) return 0;
    if (msg == WM_SYSKEYDOWN && vk == VK_F4) return 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) return 0;       /* Ctrl chords are the app's */
    if (!down || (lp & (1 << 30))) return 1;
    if (vk == VK_ESCAPE) {
        set_map_state(g_map_state >= 0 ? -1 : -2);
        g_swallow_vk = vk;
        return 1;
    }
    if (g_map_state >= 0) {
        const int target = g_map_state;
        const uint32_t keysym = a7800_keysym_from_msg(vk, lp);
        char was[128], name[48];
        if (!keysym) return 1;          /* nothing to call it: keep waiting */
        if (a7800session_key_reserved(keysym)) {
            set_hint("F9, F11 and F12 are the app's and cannot be bound.");
            g_swallow_vk = vk;
            return 1;
        }
        was[0] = '\0';
        a7800session_binding_set_key(g_session, target, keysym, was, sizeof was);
        a7800session_keysym_name(keysym, name, sizeof name);
        g_swallow_vk = vk;
        set_map_state(-1);              /* stay armed: remapping several in a row is normal */
        bound_hint(target, name, was);
    }
    return 1;
}

/* A key with the window focused: 1 if taken. Anything Map does not take goes to
 * the main window's key path, so a key plays exactly as typed there. */
static int panel_key(UINT msg, WPARAM vk, LPARAM lp)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    if (map_key(msg, vk, lp)) return 1;
    if (vk == VK_F9) {
        if (down && !(lp & (1 << 30))) hide_panel();
        return 1;
    }
    if (msg == WM_SYSKEYDOWN && (vk == VK_F4 || vk == VK_F10)) return 0;
    if (g_parent) {
        SendMessageA(g_parent, msg, vk, lp);
        return 1;
    }
    return 0;
}

/* Exactly the laid-out client size, whatever the frame is at this DPI. */
static void fit_client(void)
{
    RECT wnd, client;
    GetWindowRect(g_panel, &wnd);
    GetClientRect(g_panel, &client);
    if (client.right == g_total_w && client.bottom == g_total_h) return;
    SetWindowPos(g_panel, NULL, 0, 0, (wnd.right - wnd.left) - client.right + g_total_w,
                 (wnd.bottom - wnd.top) - client.bottom + g_total_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* The window's outer size for the laid-out client. */
static void window_size(HWND hwnd, SIZE *dims)
{
    RECT wnd, client;
    if (hwnd && GetWindowRect(hwnd, &wnd) && GetClientRect(hwnd, &client) && client.right > 0) {
        dims->cx = (wnd.right - wnd.left) - client.right + g_total_w;
        dims->cy = (wnd.bottom - wnd.top) - client.bottom + g_total_h;
    } else {
        SetRect(&wnd, 0, 0, g_total_w, g_total_h);
        AdjustWindowRectEx(&wnd, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
        dims->cx = wnd.right - wnd.left;
        dims->cy = wnd.bottom - wnd.top;
    }
}

static void rescale(int dpi)
{
    g_dpi = dpi;
    rebuild();
}

/* ---- the mouse ------------------------------------------------------------------ */

/* The layout part under a client point, or -1. */
static int part_at(POINT pnt)
{
    if (!g_numlayout) return -1;
    return a7800session_controls_hit(g_view[0], g_view[1], (float)((pnt.x - g_orgx) / g_unit),
                                     (float)((pnt.y - g_orgy) / g_unit));
}

/* A stick part's centre and drag radius, in pixels. */
static void stick_geometry(int part, double *ctrx, double *ctry, double *rad)
{
    double xpos, ypos, width, height;
    part_box(&g_layout[part], &xpos, &ypos, &width, &height);
    *ctrx = xpos + width / 2;
    *ctry = ypos + height / 2;
    *rad = width / 2;
}

/* Where the pointer is on a stick, as the direction the console is given. */
static void stick_follow(int part, POINT pnt)
{
    double ctrx, ctry, rad, len;
    stick_geometry(part, &ctrx, &ctry, &rad);
    g_stick_dx = (pnt.x - ctrx) / rad;
    g_stick_dy = (pnt.y - ctry) / rad;
    len = sqrt(g_stick_dx * g_stick_dx + g_stick_dy * g_stick_dy);
    if (len > 1.0) { g_stick_dx /= len; g_stick_dy /= len; }
    a7800session_pointer_set(g_session, g_layout[part].port,
                             a7800session_stick_from_vector((float)(pnt.x - ctrx), (float)(pnt.y - ctry), (float)rad));
    refresh_held();
    invalidate_art();
}

/* What a Map click on a part picks: for a stick, the side of it clicked. */
static int map_target_for(const a7800_control *ctl, POINT pnt, int part)
{
    if (ctl->kind == A7800_CTL_STICK) {
        double ctrx, ctry, rad, delx, dely;
        stick_geometry(part, &ctrx, &ctry, &rad);
        delx = pnt.x - ctrx;
        dely = pnt.y - ctry;
        if (fabs(delx) > fabs(dely)) return A7800_TARGET_PORT(ctl->port, delx < 0 ? A7800_ACT_LEFT : A7800_ACT_RIGHT);
        return A7800_TARGET_PORT(ctl->port, dely < 0 ? A7800_ACT_UP : A7800_ACT_DOWN);
    }
    return ctl->target;
}

static void strip_click(HWND hwnd, int btn)
{
    switch (btn) {
    case SB_MAP: set_map_state(g_map_state == -2 ? -1 : -2); break;
    case SB_CLEAR:
        if (g_map_state >= 0) {
            a7800session_binding_clear(g_session, g_map_state);
            set_hint("Cleared. Press a key or a gamepad button to bind it again");
            a7800session_gamepad_capture_begin(g_session);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;
    case SB_DEFAULTS: {
        const int count = a7800session_gamepad_count(g_session);
        int pad;
        a7800session_bindings_reset(g_session);
        for (pad = 0; pad < count; pad++) a7800session_gamepad_assign(g_session, pad, -1);
        refresh_pads();
        set_hint("Every key and button is back to its default");
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }
    case SB_SHOW:
        g_show_bindings = !g_show_bindings;
        a7800session_set_int(g_session, "controllers_show_bindings", g_show_bindings);
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    default: a7800_help_show(hwnd, "controllers"); break;
    }
}

static void mouse_down(HWND hwnd, POINT pnt)
{
    int part, row, seg, btn;
    SetFocus(hwnd);
    if (g_hover_part >= 0) {
        g_hover_part = -1;
        InvalidateRect(hwnd, &g_hint_rc, FALSE);
    }

    for (btn = 0; btn < SB_COUNT; btn++)
        if (PtInRect(&g_btn_rc[btn], pnt)) { strip_click(hwnd, btn); return; }
    for (row = 0; row < PAD_ROWS && row < a7800session_gamepad_count(g_session); row++)
        for (seg = 0; seg <= A7800_PORTS; seg++) {
            RECT rect;
            segment_rect(&g_gp_row_rc[row], seg, &rect);
            if (PtInRect(&rect, pnt)) {
                /* 0 = automatic (connection order), 1..2 = that player */
                a7800session_gamepad_assign(g_session, row, seg - 1);
                refresh_pads();
                InvalidateRect(hwnd, &g_gp_rc, FALSE);
                return;
            }
        }

    part = part_at(pnt);
    if (part < 0) return;
    {
        const a7800_control *ctl = &g_layout[part];
        if (g_map_state != -2) {
            /* armed: picks (or changes) the target; on a stick, the direction clicked */
            const int target = map_target_for(ctl, pnt, part);
            if (target >= 0) set_map_state(target);
            return;
        }
        mouse_release();
        if (ctl->kind == A7800_CTL_SWITCH) {
            a7800session_press(g_session, ctl->target, 1);      /* flips on the click; the release is nothing */
            refresh_held();
            return;
        }
        g_mouse_part = part;
        if (ctl->kind == A7800_CTL_STICK) stick_follow(part, pnt);
        else a7800session_press(g_session, ctl->target, 1);
        SetCapture(hwnd);
        refresh_held();
        invalidate_art();
    }
}

static void mouse_move(HWND hwnd, POINT pnt)
{
    if (g_mouse_part >= 0) {
        if (g_layout[g_mouse_part].kind == A7800_CTL_STICK) {
            stick_follow(g_mouse_part, pnt);        /* a disc is dragged, never "dragged off" */
            return;
        }
        /* dragged off what it holds: let go, as a button does */
        if (part_at(pnt) != g_mouse_part) {
            mouse_release();
            if (GetCapture() == hwnd) ReleaseCapture();
        }
        return;
    }
    {   /* idle: the hint line says what drives the part under the pointer */
        const int part = part_at(pnt);
        if (!g_tracking) {
            TRACKMOUSEEVENT track;
            memset(&track, 0, sizeof track);
            track.cbSize = sizeof track;
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            g_tracking = TrackMouseEvent(&track) != 0;
        }
        if (part != g_hover_part) {
            g_hover_part = part;
            InvalidateRect(hwnd, &g_hint_rc, FALSE);
        }
    }
}

static void mouse_up(HWND hwnd)
{
    if (g_mouse_part >= 0) mouse_release();
    if (GetCapture() == hwnd) ReleaseCapture();
}

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC hdc = BeginPaint(hwnd, &paint);
        /* Off-screen first: the held highlights repaint at 25 Hz. */
        RECT inval = paint.rcPaint;
        const int width = inval.right - inval.left, height = inval.bottom - inval.top;
        if (width > 0 && height > 0) {
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, width, height);
            HGDIOBJ oldbmp = SelectObject(mem, bmp);
            HGDIOBJ oldfont = SelectObject(mem, g_font);
            SetViewportOrgEx(mem, -inval.left, -inval.top, NULL);
            paint_rect(mem, &inval);
            SetViewportOrgEx(mem, 0, 0, NULL);
            BitBlt(hdc, inval.left, inval.top, width, height, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldfont);
            SelectObject(mem, oldbmp);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        mouse_down(hwnd, pnt);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        mouse_move(hwnd, pnt);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tracking = 0;
        if (g_hover_part >= 0) {
            g_hover_part = -1;
            InvalidateRect(hwnd, &g_hint_rc, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        /* released wherever the mouse ended up: capture means this arrives
         * even if the pointer left the part */
        mouse_up(hwnd);
        return 0;
    case WM_CAPTURECHANGED:
        /* lost the mouse some other way (Alt+Tab mid-press): let go */
        if ((HWND)lparam != hwnd && g_mouse_part >= 0) mouse_release();
        return 0;

    case WM_COMMAND:
        if (LOWORD(wparam) == IDC_CTRL_TYPE0 || LOWORD(wparam) == IDC_CTRL_TYPE1) {
            const int port = LOWORD(wparam) == IDC_CTRL_TYPE1;
            if (HIWORD(wparam) == CBN_SELCHANGE) {
                const int sel = (int)SendMessageA(g_combo[port], CB_GETCURSEL, 0, 0);
                if (sel >= 0) a7800session_set_port_type(g_session, port, sel);
                SetFocus(hwnd);             /* keys go to the console again, not to the list */
                refresh_held();
            } else if (HIWORD(wparam) == CBN_CLOSEUP) {
                SetFocus(hwnd);
            }
            return 0;
        }
        break;

    case WM_TIMER:
        if (wparam == IDT_CAPTURE) {
            int button;
            if (g_map_state < 0) {
                KillTimer(hwnd, IDT_CAPTURE);
            } else if (a7800session_gamepad_capture_poll(g_session, &button)) {
                char was[128];
                const int target = g_map_state;
                was[0] = '\0';
                a7800session_binding_set_button(g_session, target, button, was, sizeof was);
                set_map_state(-1);          /* stay armed: remapping several in a row is normal */
                bound_hint(target, a7800_pad_button_name(button), was);
                InvalidateRect(hwnd, NULL, FALSE);
            }
        } else if (wparam == IDT_HELD) {
            refresh_held();
        } else if (wparam == IDT_PADS) {
            refresh_pads();
        }
        return 0;

    /* WM_SYSKEY* too: a bound Alt is the console's */
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
        if (panel_key(msg, wparam, lparam)) return 0;
        break;
    case WM_SYSCHAR:
        /* Alt+letter: the main window's menu mnemonics, as when it has the focus */
        if (wparam != ' ' && g_parent) { SendMessageA(g_parent, msg, wparam, lparam); return 0; }
        break;
    case WM_ENTERMENULOOP:
        a7800session_release_all(g_session);
        break;

    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) {
            const HWND other = (HWND)lparam;
            mouse_release();                /* a7800session_release_pointer: the pointer's holds go */
            if (GetCapture() == hwnd) ReleaseCapture();
            g_swallow_vk = 0;               /* its release goes elsewhere */
            /* Leaving the app: nothing typed here stays held (within it, the
             * window that gets the key-up lets go). */
            if (!other || GetWindowThreadProcessId(other, NULL) != GetCurrentThreadId())
                a7800session_release_all(g_session);
        }
        break;                              /* DefWindowProc gives the window the focus */
    case WM_SHOWWINDOW:
        if (wparam) {
            refresh_held();
            SetTimer(hwnd, IDT_HELD, 40, NULL);
            SetTimer(hwnd, IDT_PADS, 250, NULL);
        } else {
            KillTimer(hwnd, IDT_HELD);
            KillTimer(hwnd, IDT_PADS);
            mouse_release();
        }
        break;
    case WM_GETMINMAXINFO:
        /* Fixed: the size limits say so too, so neither maximizing nor a
         * compositor (Wine passes them on) makes it any other size. */
        if (g_total_w) {
            MINMAXINFO *limits = (MINMAXINFO *)lparam;
            SIZE dims;
            window_size(hwnd, &dims);
            limits->ptMinTrackSize.x = limits->ptMaxTrackSize.x = limits->ptMaxSize.x = dims.cx;
            limits->ptMinTrackSize.y = limits->ptMaxTrackSize.y = limits->ptMaxSize.y = dims.cy;
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        const RECT *suggested = (const RECT *)lparam;
        rescale(HIWORD(wparam));
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        fit_client();
        return 0;
    }
    case WM_CLOSE:
        /* Hide, do not destroy: a remap and the position survive closing it. */
        hide_panel();
        return 0;
    case WM_DESTROY:
        /* with the main window: the next toggle starts afresh */
        mouse_release();
        set_map_state(-2);
        g_panel = NULL;
        g_combo[0] = g_combo[1] = NULL;
        g_numlayout = 0;
        free_fonts();
        break;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static int create_panel(void)
{
    static int registered;
    HINSTANCE inst = GetModuleHandleA(NULL);
    RECT want;
    int port, type;

    g_show_bindings = a7800session_get_int(g_session, "controllers_show_bindings", 0) > 0;

    if (!registered) {
        WNDCLASSEXA wcls;
        memset(&wcls, 0, sizeof wcls);
        wcls.cbSize = sizeof wcls;
        wcls.lpfnWndProc = panel_proc;      /* no CS_DBLCLKS: fast clicks stay press/release pairs */
        wcls.hInstance = inst;
        wcls.hCursor = LoadCursor(NULL, IDC_ARROW);
        wcls.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wcls.lpszClassName = PANEL_CLASS;
        registered = RegisterClassExA(&wcls) != 0;
    }
    g_dpi = a7800_window_dpi(g_parent);
    make_fonts();
    layout_static();

    SetRect(&want, 0, 0, g_total_w, g_total_h);
    /* A tool window with a caption and no thick frame or maximize box: it is
     * exactly the size of its contents, and stays off the taskbar. */
    AdjustWindowRectEx(&want, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
    g_panel = CreateWindowExA(PANEL_EXSTYLE, PANEL_CLASS, "Controllers (F9)", PANEL_STYLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left, want.bottom - want.top,
                              g_parent, NULL, inst, NULL);
    if (!g_panel) return 0;
    for (port = 0; port < A7800_PORTS; port++) {
        g_combo[port] = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                                        0, 0, 100, 100, g_panel, (HMENU)(INT_PTR)(IDC_CTRL_TYPE0 + port), inst, NULL);
        if (!g_combo[port]) continue;
        for (type = 0; type < A7800_CTRL_COUNT; type++) {
            const char *name = a7800_ctrl_type_name(type);
            SendMessageA(g_combo[port], CB_ADDSTRING, 0, (LPARAM)(name ? name : "?"));
        }
        SendMessageA(g_combo[port], CB_SETCURSEL, (WPARAM)a7800session_port_type(g_session, port), 0);
    }
    a7800_help_set_context(g_panel, "controllers", 0);    /* F1 is the console's SELECT here */
    rescale(a7800_window_dpi(g_panel));    /* placed on another monitor than the main window */
    fit_client();
    sync_combos();
    memset(&g_pads_shown, 0, sizeof g_pads_shown);
    g_hover_part = -1;
    set_map_state(-2);
    return 1;
}

static void show_panel(void)
{
    refresh_pads();
    ShowWindow(g_panel, SW_SHOW);
    SetForegroundWindow(g_panel);
}

void a7800_controller_window_toggle(HWND parent, a7800session *session)
{
    g_session = session;
    if (parent) g_parent = parent;
    if (!g_panel && !create_panel()) return;
    if (IsWindowVisible(g_panel)) hide_panel();
    else show_panel();
}

void a7800_controller_window_show(HWND parent, a7800session *session)
{
    g_session = session;
    if (parent) g_parent = parent;
    if (!g_panel && !create_panel()) return;
    if (!IsWindowVisible(g_panel)) show_panel();
    else SetForegroundWindow(g_panel);
}

int a7800_controller_window_visible(void)
{
    return g_panel && IsWindowVisible(g_panel);
}

void a7800_controller_window_gamepads_changed(void)
{
    if (!g_panel) return;
    sync_combos();
    if (IsWindowVisible(g_panel)) {
        refresh_pads();
        refresh_held();
        InvalidateRect(g_panel, NULL, FALSE);
    }
}

int a7800_controller_pretranslate(MSG *msg)
{
    /* Keys reach the window proc directly (the only focusable children are
     * the type selectors, which give the focus back); nothing to steal. */
    (void)msg;
    return 0;
}
