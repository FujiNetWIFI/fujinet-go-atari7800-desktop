/*
 * The debugger's Video tab (Win32).
 *
 * Everything that decides what to show lives in the core (a7800vidtab.h): this file is
 * controls and a GDI picture. The picture is the controller's cached 0xFFRRGGBB buffer drawn
 * 1:1 with StretchDIBits (BI_RGB ignores the alpha byte), double buffered; a mouse move
 * redraws only the marks. Only the section showing is filled in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "dbg_video.h"

#include <commctrl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a7800vidtab.h"

#define PAGE_CLASS L"FujiNetGoA7800VideoPage"
#define PIC_CLASS L"FujiNetGoA7800VideoPic"
#define COL_CLASS L"FujiNetGoA7800VideoColours"

#define ACCENT RGB(0xF0, 0x93, 0x2F)      /* the 7800 icon orange */
#define MAX_OVERLAYS 16
#define PAL_COLS 8          /* a column a palette; a row a colour (0 is BACKGRND's mirror) */
#define PAL_ROWS 4

enum {
    IDV_VIEW = 3000, IDV_SOURCE, IDV_LABEL_SOURCE, IDV_ZOOM, IDV_BREAK, IDV_BREAK_REG, IDV_STATUS, IDV_NOTE, IDV_SEC, IDV_PIXEL, IDV_UNPIN,
    IDV_REGS, IDV_LINES, IDV_BEAM, IDV_TIMING, IDV_PIC, IDV_COLS, IDV_LABEL_VIEW, IDV_LABEL_ZOOM,
    IDV_OV0 = 3200
};

#define MAX_ZOOMS 16

typedef struct {
    unsigned char *hot;
    int nhot;
    uint64_t sig;
    int have_sig;
} listfill;

typedef struct {
    HWND page, view, zoom, brk, status, note, pic, sec;
    HWND ov[MAX_OVERLAYS];
    unsigned ov_bit[MAX_OVERLAYS];
    int nov;
    HWND label_view, label_zoom, label_source, source, brk_reg;
    HWND pixel, unpin, regs, lines, cols, beam, timing;
    int reg_selected;              /* the swatch Break on register write acts on, -1 none */
    HFONT ui, mono;
    int dpi;
    a7800vid_tab *tab;
    int updating;
    int scroll_x, scroll_y;
    int tracking;
    int pal_hover, col_tracking;
    int zoom_val[MAX_ZOOMS], nzoom;
    listfill rf, lf;
    BITMAPINFO bmi;
} vidwin;

#define S(v, n) MulDiv((n), (v)->dpi, 96)

/* ---- text ------------------------------------------------------------------------ */

/* UTF-8 to UTF-16; '\n' becomes CR LF (multi-line controls want it) */
static wchar_t *wide(const char *utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0), i;
    wchar_t *w = malloc(((size_t)n + 1) * sizeof *w), *o, *d;
    if (!w) return NULL;
    o = malloc(((size_t)n * 2 + 2) * sizeof *w);
    if (!o) {
        free(w);
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n);
    d = o;
    for (i = 0; w[i]; i++) {
        if (w[i] == L'\n') *d++ = L'\r';
        *d++ = w[i];
    }
    *d = 0;
    free(w);
    return o;
}

/* a control's text, set only when it changed (no flicker on the live refresh) */
static void set_text(HWND h, const char *utf8)
{
    wchar_t *w = wide(utf8), cur[2048];
    if (!w) return;
    GetWindowTextW(h, cur, 2048);
    if (wcscmp(cur, w) != 0) SetWindowTextW(h, w);
    free(w);
}

static uint64_t fnv(uint64_t h, const void *data, size_t n)
{
    const unsigned char *p = data;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

/* ---- the picture --------------------------------------------------------------------- */

static void pic_update_scroll(vidwin *v)
{
    RECT rc;
    SCROLLINFO si;
    int pw = 0, ph = 0;
    GetClientRect(v->pic, &rc);
    a7800vid_tab_picture(v->tab, &pw, &ph);
    memset(&si, 0, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = pw > 0 ? pw - 1 : 0;
    si.nPage = (UINT)rc.right;
    if (v->scroll_x > pw - rc.right) v->scroll_x = pw > rc.right ? pw - rc.right : 0;
    si.nPos = v->scroll_x;
    SetScrollInfo(v->pic, SB_HORZ, &si, TRUE);
    si.nMax = ph > 0 ? ph - 1 : 0;
    si.nPage = (UINT)rc.bottom;
    if (v->scroll_y > ph - rc.bottom) v->scroll_y = ph > rc.bottom ? ph - rc.bottom : 0;
    si.nPos = v->scroll_y;
    SetScrollInfo(v->pic, SB_VERT, &si, TRUE);
}

static void pic_paint(vidwin *v, HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps), mem;
    RECT rc;
    HBITMAP bmp, old;
    int pw = 0, ph = 0, marks, hover[4] = { 0 }, pin[4] = { 0 };
    const uint32_t *px;
    HBRUSH bg = CreateSolidBrush(RGB(18, 18, 24));
    GetClientRect(h, &rc);
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, rc.right > 0 ? rc.right : 1, rc.bottom > 0 ? rc.bottom : 1);
    old = SelectObject(mem, bmp);
    FillRect(mem, &rc, bg);
    px = a7800vid_tab_picture(v->tab, &pw, &ph);
    if (!px) {
        const char *why = a7800vid_tab_why(v->tab);
        wchar_t *w = wide(why && *why ? why : "Nothing to show");
        RECT tr = rc;
        InflateRect(&tr, -S(v, 10), -S(v, 10));
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(140, 140, 148));
        SelectObject(mem, v->ui);
        if (w) DrawTextW(mem, w, -1, &tr, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        free(w);
    } else {
        HPEN pen_h, pen_p, pen_k, oldpen;
        HGDIOBJ oldbr;
        v->bmi.bmiHeader.biWidth = pw;      /* the buffer is pw words per row, whatever the view */
        v->bmi.bmiHeader.biHeight = -ph;    /* top-down */
        SetStretchBltMode(mem, COLORONCOLOR);
        StretchDIBits(mem, -v->scroll_x, -v->scroll_y, pw, ph, 0, 0, pw, ph, px, &v->bmi, DIB_RGB_COLORS, SRCCOPY);
        marks = a7800vid_tab_marks(v->tab, hover, pin);
        oldbr = SelectObject(mem, GetStockObject(NULL_BRUSH));
        pen_h = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
        pen_p = CreatePen(PS_SOLID, 2, RGB(255, 217, 26));
        pen_k = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
        if (marks & 1) {            /* hover: a white box just outside the pixel, over a dark one */
            oldpen = SelectObject(mem, pen_k);
            Rectangle(mem, hover[0] - v->scroll_x - 2, hover[1] - v->scroll_y - 2, hover[0] - v->scroll_x + hover[2] + 2,
                      hover[1] - v->scroll_y + hover[3] + 2);
            SelectObject(mem, pen_h);
            Rectangle(mem, hover[0] - v->scroll_x - 1, hover[1] - v->scroll_y - 1, hover[0] - v->scroll_x + hover[2] + 1,
                      hover[1] - v->scroll_y + hover[3] + 1);
            SelectObject(mem, oldpen);
        }
        if (marks & 2) {            /* pin: a thick yellow box */
            oldpen = SelectObject(mem, pen_k);
            Rectangle(mem, pin[0] - v->scroll_x - 3, pin[1] - v->scroll_y - 3, pin[0] - v->scroll_x + pin[2] + 3,
                      pin[1] - v->scroll_y + pin[3] + 3);
            SelectObject(mem, pen_p);
            Rectangle(mem, pin[0] - v->scroll_x - 1, pin[1] - v->scroll_y - 1, pin[0] - v->scroll_x + pin[2] + 1,
                      pin[1] - v->scroll_y + pin[3] + 1);
            SelectObject(mem, oldpen);
        }
        SelectObject(mem, oldbr);
        DeleteObject(pen_h);
        DeleteObject(pen_p);
        DeleteObject(pen_k);
    }
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    DeleteObject(bg);
    EndPaint(h, &ps);
}

static void update_pixel(vidwin *v)
{
    const a7800vid_pixel *p = a7800vid_tab_pixel(v->tab);
    set_text(v->pixel, p && p->text[0] ? p->text
                                       : "Move over the picture to inspect a pixel.\n"
                                         "Click to pin it; click again to unpin.\n"
                                         "Double-click to break on writes to it.");
    if (v->cols) InvalidateRect(v->cols, NULL, FALSE);
}

static void set_note(vidwin *v, const char *text)
{
    set_text(v->note, text);
}

/* Break on writes to the inspected pixel's objects, zone entry and palette register, and say what was done. */
static void break_on_pixel(vidwin *v)
{
    char msg[160];
    int id = a7800vid_tab_break_on_pixel(v->tab);
    if (id >= 0) snprintf(msg, sizeof msg, "Break on writes to the pixel's display list objects, zone and palette register (see Breakpoints)");
    else snprintf(msg, sizeof msg, "Nothing to break on: pin or hover a pixel inside the rows MARIA draws");
    set_note(v, msg);
}

static LRESULT CALLBACK pic_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    vidwin *v = (vidwin *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!v) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        pic_paint(v, h);
        return 0;
    case WM_SIZE:
        pic_update_scroll(v);
        return 0;
    case WM_HSCROLL:
    case WM_VSCROLL: {
        SCROLLINFO si;
        int bar = msg == WM_HSCROLL ? SB_HORZ : SB_VERT, pos;
        memset(&si, 0, sizeof si);
        si.cbSize = sizeof si;
        si.fMask = SIF_ALL;
        GetScrollInfo(h, bar, &si);
        pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINELEFT: pos -= 16; break;
        case SB_LINERIGHT: pos += 16; break;
        case SB_PAGELEFT: pos -= (int)si.nPage; break;
        case SB_PAGERIGHT: pos += (int)si.nPage; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        default: break;
        }
        if (pos < 0) pos = 0;
        if (bar == SB_HORZ) v->scroll_x = pos; else v->scroll_y = pos;
        pic_update_scroll(v);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL:
        v->scroll_y -= GET_WHEEL_DELTA_WPARAM(wp) / 3;
        if (v->scroll_y < 0) v->scroll_y = 0;
        pic_update_scroll(v);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_MOUSEMOVE: {
        int x = (short)LOWORD(lp) + v->scroll_x, y = (short)HIWORD(lp) + v->scroll_y;
        if (!v->tracking) {
            TRACKMOUSEEVENT t;
            memset(&t, 0, sizeof t);
            t.cbSize = sizeof t;
            t.dwFlags = TME_LEAVE;
            t.hwndTrack = h;
            TrackMouseEvent(&t);
            v->tracking = 1;
        }
        if (a7800vid_tab_hover(v->tab, x, y)) {
            InvalidateRect(h, NULL, FALSE);
            update_pixel(v);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        v->tracking = 0;
        if (a7800vid_tab_hover(v->tab, -1, -1)) {
            InvalidateRect(h, NULL, FALSE);
            update_pixel(v);
        }
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(h);
        a7800vid_tab_hover(v->tab, (short)LOWORD(lp) + v->scroll_x, (short)HIWORD(lp) + v->scroll_y);
        a7800vid_tab_pin(v->tab, (short)LOWORD(lp) + v->scroll_x, (short)HIWORD(lp) + v->scroll_y);
        InvalidateRect(h, NULL, FALSE);
        update_pixel(v);
        return 0;
    case WM_LBUTTONDBLCLK:
        a7800vid_tab_hover(v->tab, (short)LOWORD(lp) + v->scroll_x, (short)HIWORD(lp) + v->scroll_y);
        break_on_pixel(v);
        InvalidateRect(h, NULL, FALSE);
        update_pixel(v);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ---- the Colours panel: the 32 palette registers as a grid, a description and Break on register write ---- */

#define PAL_MAX_CELL 36

/* the grid's cell size and top, from the panel's size */
static void col_metrics(vidwin *v, const RECT *rc, int *cell, int *top, int *detail_h)
{
    int cell_size = rc->right / PAL_COLS;
    if (cell_size > S(v, PAL_MAX_CELL)) cell_size = S(v, PAL_MAX_CELL);
    if (cell_size < 6) cell_size = 6;
    *cell = cell_size;
    *top = 0;
    *detail_h = S(v, 16) * 3;
}

/* "P3C1 ($2D): $2A, #RRGGBB, N pixels" */
static void swatch_describe(const a7800vid_swatch *swatch, char *out, size_t cap)
{
    snprintf(out, cap, "%s ($%02X): $%02X  #%06X  %ld pixels%s", swatch->name, swatch->address, swatch->value,
             swatch->rgb & 0xFFFFFF, swatch->uses, swatch->inspected ? "  - used by the inspected pixel" : "");
}

static void col_paint(vidwin *v, HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps), mem;
    HBITMAP bmp, old;
    RECT rc;
    const a7800vid_swatch *swatches;
    int count = a7800vid_tab_swatches(v->tab, &swatches);
    int cell, top, detail_height, index;
    HFONT oldf;
    GetClientRect(h, &rc);
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, rc.right > 0 ? rc.right : 1, rc.bottom > 0 ? rc.bottom : 1);
    old = SelectObject(mem, bmp);
    FillRect(mem, &rc, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(mem, TRANSPARENT);
    oldf = SelectObject(mem, v->mono);
    col_metrics(v, &rc, &cell, &top, &detail_height);
    for (index = 0; index < count && index < PAL_COLS * PAL_ROWS; index++) {
        const a7800vid_swatch *swatch = &swatches[index];
        RECT cell_rect;
        HBRUSH fill = CreateSolidBrush(RGB((swatch->rgb >> 16) & 255, (swatch->rgb >> 8) & 255, swatch->rgb & 255));
        cell_rect.left = (index / PAL_ROWS) * cell;
        cell_rect.top = top + (index % PAL_ROWS) * (cell + S(v, 6));
        cell_rect.right = cell_rect.left + cell - 1;
        cell_rect.bottom = cell_rect.top + cell - 1;
        FillRect(mem, &cell_rect, fill);
        DeleteObject(fill);
        if (swatch->inspected) {
            HBRUSH accent_brush = CreateSolidBrush(ACCENT), white_brush = CreateSolidBrush(RGB(255, 255, 255));
            RECT outline = cell_rect;
            InflateRect(&outline, 1, 1);
            FrameRect(mem, &outline, accent_brush);
            InflateRect(&outline, -1, -1);
            FrameRect(mem, &outline, white_brush);
            DeleteObject(accent_brush);
            DeleteObject(white_brush);
        }
        if (index == v->reg_selected || index == v->pal_hover) {
            HBRUSH edge = CreateSolidBrush(index == v->reg_selected ? ACCENT : RGB(0, 0, 0));
            RECT outline = cell_rect;
            InflateRect(&outline, 2, 2);
            FrameRect(mem, &outline, edge);
            DeleteObject(edge);
        }
    }
    {
        char text[300];
        RECT text_rect = { 0, top + PAL_ROWS * (cell + S(v, 6)) + S(v, 4), rc.right, rc.bottom };
        wchar_t *wide_text;
        int shown = v->pal_hover >= 0 ? v->pal_hover : v->reg_selected;
        text[0] = 0;
        if (shown < 0)
            for (index = 0; index < count; index++)
                if (swatches[index].inspected) shown = index;
        if (shown >= 0 && shown < count) swatch_describe(&swatches[shown], text, sizeof text);
        else snprintf(text, sizeof text, "A column a palette (0-7), a row a colour (0-3; row 0 mirrors BACKGRND).\nHover a swatch for its value; click one, then Break on register write.");
        wide_text = wide(text);
        SetTextColor(mem, GetSysColor(COLOR_WINDOWTEXT));
        if (wide_text) DrawTextW(mem, wide_text, -1, &text_rect, DT_WORDBREAK | DT_NOPREFIX);
        free(wide_text);
    }
    SelectObject(mem, oldf);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(h, &ps);
}

/* the swatch under a point of the panel, or -1 */
static int col_hit(vidwin *v, HWND h, int px, int py)
{
    RECT rc;
    int cell, top, detail_height, column, row;
    GetClientRect(h, &rc);
    col_metrics(v, &rc, &cell, &top, &detail_height);
    if (px < 0 || py < top || px >= PAL_COLS * cell) return -1;
    row = (py - top) / (cell + S(v, 6));
    if (row >= PAL_ROWS || (py - top) % (cell + S(v, 6)) >= cell) return -1;
    column = px / cell;
    return column * PAL_ROWS + row;
}

static LRESULT CALLBACK col_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    vidwin *v = (vidwin *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!v) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        col_paint(v, h);
        return 0;
    case WM_LBUTTONDOWN: {
        int index = col_hit(v, h, (short)LOWORD(lp), (short)HIWORD(lp));
        v->reg_selected = index == v->reg_selected ? -1 : index;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int index = col_hit(v, h, (short)LOWORD(lp), (short)HIWORD(lp));
        if (!v->col_tracking) {
            TRACKMOUSEEVENT track;
            memset(&track, 0, sizeof track);
            track.cbSize = sizeof track;
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = h;
            TrackMouseEvent(&track);
            v->col_tracking = 1;
        }
        if (index != v->pal_hover) {
            v->pal_hover = index;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        v->col_tracking = 0;
        if (v->pal_hover != -1) {
            v->pal_hover = -1;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* Break on a write to the selected swatch's register (or the inspected pixel's own), and say what was done. */
static void break_on_cram(vidwin *v)
{
    char msg[160];
    int index = v->reg_selected >= 0 ? v->reg_selected : a7800vid_tab_pixel_register(v->tab);
    int id = index >= 0 ? a7800vid_tab_break_on_swatch(v->tab, index) : -1;
    if (id >= 0) snprintf(msg, sizeof msg, "Break on writes to palette register %d added (see Breakpoints)", index);
    else snprintf(msg, sizeof msg, "Select a swatch (or pin a pixel) first");
    set_note(v, msg);
}

/* ---- sections ----------------------------------------------------------------------- */

static void lv_cell(HWND lv, int row, int col, const char *text)
{
    wchar_t *w = wide(text);
    LVITEMW it;
    memset(&it, 0, sizeof it);
    it.iSubItem = col;
    it.pszText = w ? w : L"";
    if (col == 0) {
        it.mask = LVIF_TEXT;
        it.iItem = row;
        SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
    } else {
        SendMessageW(lv, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&it);
    }
    free(w);
}

static void mark_hot(listfill *lf, int n, int i, int hot)
{
    if (i == 0) {
        free(lf->hot);
        lf->hot = calloc((size_t)(n > 0 ? n : 1), 1);
        lf->nhot = lf->hot ? n : 0;
    }
    if (lf->hot && i < lf->nhot) lf->hot[i] = (unsigned char)hot;
}

static void fill_regs(vidwin *v)
{
    const a7800vid_reg *r;
    int n = a7800vid_tab_regs(v->tab, &r), i;
    uint64_t sig = 1469598103934665603ull;
    for (i = 0; i < n; i++) {
        int hot = r[i].age < A7800VID_REG_FADE;
        sig = fnv(sig, r[i].name, strlen(r[i].name));
        sig = fnv(sig, r[i].value, strlen(r[i].value));
        sig = fnv(sig, r[i].meaning, strlen(r[i].meaning));
        sig = fnv(sig, &hot, sizeof hot);
    }
    if (v->rf.have_sig && v->rf.sig == sig) return;
    v->rf.sig = sig;
    v->rf.have_sig = 1;
    SendMessageW(v->regs, WM_SETREDRAW, FALSE, 0);
    SendMessageW(v->regs, LVM_DELETEALLITEMS, 0, 0);
    for (i = 0; i < n; i++) {
        lv_cell(v->regs, i, 0, r[i].name);
        lv_cell(v->regs, i, 1, r[i].value);
        lv_cell(v->regs, i, 2, r[i].meaning);
        mark_hot(&v->rf, n, i, r[i].age < A7800VID_REG_FADE);
    }
    if (n == 0) mark_hot(&v->rf, 0, 0, 0);
    SendMessageW(v->regs, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(v->regs, NULL, TRUE);
}

static void fill_lines(vidwin *v)
{
    const a7800vid_line_row *r;
    int n = a7800vid_tab_lines(v->tab, &r), i;
    uint64_t sig = 1469598103934665603ull;
    for (i = 0; i < n; i++) {
        sig = fnv(sig, &r[i], sizeof r[i]);
    }
    sig = fnv(sig, &n, sizeof n);
    if (v->lf.have_sig && v->lf.sig == sig) return;
    v->lf.sig = sig;
    v->lf.have_sig = 1;
    SendMessageW(v->lines, WM_SETREDRAW, FALSE, 0);
    SendMessageW(v->lines, LVM_DELETEALLITEMS, 0, 0);
    for (i = 0; i < n; i++) {
        char text[64];
        snprintf(text, sizeof text, "%d", r[i].row);
        lv_cell(v->lines, i, 0, text);
        snprintf(text, sizeof text, "%d", r[i].scanline);
        lv_cell(v->lines, i, 1, text);
        snprintf(text, sizeof text, "%02X", r[i].ctrl);
        lv_cell(v->lines, i, 2, text);
        snprintf(text, sizeof text, "%02X", r[i].charbase);
        lv_cell(v->lines, i, 3, text);
        snprintf(text, sizeof text, "%04X", r[i].dpp);
        lv_cell(v->lines, i, 4, text);
        snprintf(text, sizeof text, "%04X", r[i].dll);
        lv_cell(v->lines, i, 5, text);
        snprintf(text, sizeof text, "%04X", r[i].dl);
        lv_cell(v->lines, i, 6, text);
        snprintf(text, sizeof text, "%d%s%s", r[i].offset, r[i].holey ? " holey" : "", r[i].dli ? " DLI" : "");
        lv_cell(v->lines, i, 7, text);
        snprintf(text, sizeof text, "%d", r[i].objects);
        lv_cell(v->lines, i, 8, text);
        snprintf(text, sizeof text, "%d", r[i].cycles);
        lv_cell(v->lines, i, 9, text);
        snprintf(text, sizeof text, "%02X", r[i].background);
        lv_cell(v->lines, i, 10, text);
        mark_hot(&v->lf, n, i, (r[i].changed_mask & (A7800VID_LC_PALETTE | A7800VID_LC_CTRL | A7800VID_LC_DPP)) != 0);
    }
    if (n == 0) mark_hot(&v->lf, 0, 0, 0);
    SendMessageW(v->lines, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(v->lines, NULL, TRUE);
}

static void fill_timing(vidwin *v)
{
    const char *const *lines;
    int n = a7800vid_tab_timing(v->tab, &lines), i;
    char buf[2048];
    size_t len = 0;
    buf[0] = 0;
    for (i = 0; i < n && len < sizeof buf - 160; i++) {
        const char *tab = strchr(lines[i], '\t');
        if (tab) len += (size_t)snprintf(buf + len, sizeof buf - len, "%-14.*s %s\n", (int)(tab - lines[i]), lines[i], tab + 1);
        else len += (size_t)snprintf(buf + len, sizeof buf - len, "%s\n", lines[i]);
    }
    if (!n) snprintf(buf, sizeof buf, "No machine");
    set_text(v->timing, buf);
    SendMessageW(v->beam, PBM_SETPOS, (WPARAM)a7800vid_tab_beam_permille(v->tab), 0);
}

static int cur_section(vidwin *v)
{
    int s = (int)SendMessageW(v->sec, TCM_GETCURSEL, 0, 0);
    return s < 0 ? 0 : s;
}

static void show_section(vidwin *v)
{
    int s = cur_section(v);
    ShowWindow(v->pixel, s == A7800VID_S_PIXEL ? SW_SHOW : SW_HIDE);
    ShowWindow(v->unpin, s == A7800VID_S_PIXEL ? SW_SHOW : SW_HIDE);
    ShowWindow(v->regs, s == A7800VID_S_REGISTERS ? SW_SHOW : SW_HIDE);
    ShowWindow(v->cols, s == A7800VID_S_COLOURS ? SW_SHOW : SW_HIDE);
    ShowWindow(v->brk_reg, s == A7800VID_S_COLOURS ? SW_SHOW : SW_HIDE);
    ShowWindow(v->lines, s == A7800VID_S_LINES ? SW_SHOW : SW_HIDE);
    ShowWindow(v->beam, s == A7800VID_S_TIMING ? SW_SHOW : SW_HIDE);
    ShowWindow(v->timing, s == A7800VID_S_TIMING ? SW_SHOW : SW_HIDE);
}

/* ---- refresh -------------------------------------------------------------------------- */

#define R_RESET 0x8000u         /* the view or zoom changed: scroll back to the corner */

static void refresh_view(vidwin *v, unsigned what)
{
    if (what & (A7800VID_TAB_PICTURE | A7800VID_TAB_OFF)) {
        RECT rc;
        GetClientRect(v->pic, &rc);
        if (rc.right > 4 && rc.bottom > 4) a7800vid_tab_set_viewport(v->tab, rc.right - 2, rc.bottom - 2);
        if (what & R_RESET) v->scroll_x = v->scroll_y = 0;
        pic_update_scroll(v);
        InvalidateRect(v->pic, NULL, FALSE);
    }
    if (what & A7800VID_TAB_TABLES) {
        set_text(v->status, a7800vid_tab_status(v->tab));
        switch (cur_section(v)) {
        case A7800VID_S_REGISTERS: fill_regs(v); break;
        case A7800VID_S_COLOURS: InvalidateRect(v->cols, NULL, FALSE); break;
        case A7800VID_S_LINES: fill_lines(v); break;
        case A7800VID_S_TIMING: fill_timing(v); break;
        default: break;
        }
        update_pixel(v);
    }
}

void dbg_video_refresh(HWND page, int force)
{
    vidwin *v = (vidwin *)GetWindowLongPtrW(page, GWLP_USERDATA);
    unsigned what;
    if (!v) return;
    what = a7800vid_tab_refresh(v->tab, force);
    if (force) {
        what |= A7800VID_TAB_PICTURE | A7800VID_TAB_TABLES;
        v->rf.have_sig = v->lf.have_sig = 0;
    }
    refresh_view(v, what);
}

/* ---- layout --------------------------------------------------------------------------- */

static int text_w(vidwin *v, HFONT f, const wchar_t *s)
{
    HDC dc = GetDC(v->page);
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz;
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    SelectObject(dc, old);
    ReleaseDC(v->page, dc);
    return sz.cx;
}

static void layout(vidwin *v)
{
    RECT rc, tr;
    int W, H, rw, lw, x, y, i, row_h = S(v, 24), lh = S(v, 18), gap = S(v, 6), mono_h;
    int right_x, tab_h, top;
    TEXTMETRICW tm;
    HDC dc;
    GetClientRect(v->page, &rc);
    W = rc.right;
    H = rc.bottom;
    if (W < 100 || H < 100) return;
    dc = GetDC(v->page);
    SelectObject(dc, v->mono);
    mono_h = GetTextMetricsW(dc, &tm) ? tm.tmHeight : S(v, 15);
    ReleaseDC(v->page, dc);

    /* toolbar: View, Zoom, Break on writes; then the overlay checkboxes, flowing */
    x = 0;
    y = 0;
    MoveWindow(v->label_view, x, y + (row_h - lh) / 2, S(v, 36), lh, TRUE);
    x += S(v, 36);
    MoveWindow(v->view, x, y, S(v, 130), S(v, 300), TRUE);
    x += S(v, 130) + gap * 2;
    MoveWindow(v->label_source, x, y + (row_h - lh) / 2, S(v, 50), lh, TRUE);
    x += S(v, 50);
    MoveWindow(v->source, x, y, S(v, 130), S(v, 300), TRUE);
    x += S(v, 130) + gap * 2;
    MoveWindow(v->label_zoom, x, y + (row_h - lh) / 2, S(v, 40), lh, TRUE);
    x += S(v, 40);
    MoveWindow(v->zoom, x, y, S(v, 80), S(v, 300), TRUE);
    x += S(v, 80) + gap * 3;
    {
        int bw = text_w(v, v->ui, L"Break on writes") + S(v, 28);
        MoveWindow(v->brk, x, y - S(v, 1), bw, row_h, TRUE);
    }
    y += row_h + gap;
    x = 0;
    for (i = 0; i < v->nov; i++) {
        char dummy[1];
        wchar_t lbl[64];
        int bw;
        (void)dummy;
        GetWindowTextW(v->ov[i], lbl, 64);
        bw = text_w(v, v->ui, lbl) + S(v, 26);
        if (x + bw > W && x > 0) {
            x = 0;
            y += row_h - S(v, 2);
        }
        MoveWindow(v->ov[i], x, y, bw, row_h - S(v, 4), TRUE);
        x += bw + gap;
    }
    y += row_h;
    MoveWindow(v->status, 0, y, W, mono_h * 2 + S(v, 4), TRUE);
    y += mono_h * 2 + S(v, 8);
    top = y;

    /* the split: picture left, sections right */
    rw = S(v, 480);
    if (rw > W * 50 / 100) rw = W * 48 / 100;
    lw = W - rw - gap;
    right_x = lw + gap;
    MoveWindow(v->pic, 0, top, lw, H - top - lh - gap > 20 ? H - top - lh - gap : 20, TRUE);
    MoveWindow(v->note, 0, H - lh, W, lh, TRUE);

    MoveWindow(v->sec, right_x, top, rw, H - top - lh - gap, TRUE);
    SetRect(&tr, 0, 0, rw, H - top - lh - gap);
    SendMessageW(v->sec, TCM_ADJUSTRECT, FALSE, (LPARAM)&tr);
    tab_h = tr.top;
    {
        int cx = right_x + S(v, 6), cy = top + tab_h + S(v, 6), cw = rw - S(v, 12);
        int ch = (H - lh - gap) - cy - S(v, 6);
        MoveWindow(v->pixel, cx, cy, cw, ch - S(v, 34) > 20 ? ch - S(v, 34) : 20, TRUE);
        MoveWindow(v->unpin, cx, cy + (ch > S(v, 60) ? ch - S(v, 28) : S(v, 32)), S(v, 80), S(v, 26), TRUE);
        MoveWindow(v->regs, cx, cy, cw, ch, TRUE);
        MoveWindow(v->cols, cx, cy, cw, ch - S(v, 34) > 20 ? ch - S(v, 34) : 20, TRUE);
        MoveWindow(v->brk_reg, cx, cy + (ch > S(v, 60) ? ch - S(v, 28) : S(v, 32)), S(v, 150), S(v, 26), TRUE);
        MoveWindow(v->lines, cx, cy, cw, ch, TRUE);
        MoveWindow(v->beam, cx, cy, cw, S(v, 20), TRUE);
        MoveWindow(v->timing, cx, cy + S(v, 30), cw, ch - S(v, 30), TRUE);
        SendMessageW(v->regs, LVM_SETCOLUMNWIDTH, 0, S(v, 80));
        SendMessageW(v->regs, LVM_SETCOLUMNWIDTH, 1, S(v, 60));
        SendMessageW(v->regs, LVM_SETCOLUMNWIDTH, 2, S(v, 520));
        {
            static const int lw_[11] = { 32, 38, 40, 40, 46, 46, 46, 80, 38, 44, 34 };
            for (i = 0; i < 11; i++) SendMessageW(v->lines, LVM_SETCOLUMNWIDTH, (WPARAM)i, S(v, lw_[i]));
        }
    }
    pic_update_scroll(v);
}

/* ---- the page -------------------------------------------------------------------------- */

static HWND mk_in(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style, int id, HFONT font)
{
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id,
                             (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE), NULL);
    if (font) SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
    return h;
}

#define mk(v, cls, text, style, id, font) mk_in((v)->page, cls, text, style, id, font)

static void on_overlay(vidwin *v)
{
    unsigned m = 0;
    int i;
    for (i = 0; i < v->nov; i++)
        if (SendMessageW(v->ov[i], BM_GETCHECK, 0, 0) == BST_CHECKED) m |= v->ov_bit[i];
    a7800vid_tab_set_overlays(v->tab, m);
    refresh_view(v, A7800VID_TAB_PICTURE | A7800VID_TAB_TABLES);
}

static LRESULT CALLBACK page_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    vidwin *v = (vidwin *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!v) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_SIZE:
        layout(v);
        refresh_view(v, A7800VID_TAB_PICTURE);
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (v->updating) return 0;
        if (id == IDV_SOURCE && code == CBN_SELCHANGE) {
            a7800vid_tab_set_source(v->tab, (int)SendMessageW(v->source, CB_GETCURSEL, 0, 0));
            dbg_video_refresh(h, 1);
            v->scroll_x = v->scroll_y = 0;
            pic_update_scroll(v);
        } else if (id == IDV_BREAK_REG && code == BN_CLICKED) {
            break_on_cram(v);
        } else if (id == IDV_VIEW && code == CBN_SELCHANGE) {
            a7800vid_tab_set_view(v->tab, (int)SendMessageW(v->view, CB_GETCURSEL, 0, 0));
            EnableWindow(v->source, a7800vid_tab_view(v->tab) == A7800VID_V_DECODED);
            dbg_video_refresh(h, 1);
            v->scroll_x = v->scroll_y = 0;
            pic_update_scroll(v);
        } else if (id == IDV_ZOOM && code == CBN_SELCHANGE) {
            int i = (int)SendMessageW(v->zoom, CB_GETCURSEL, 0, 0);
            if (i >= 0 && i < v->nzoom) a7800vid_tab_set_zoom(v->tab, v->zoom_val[i]);
            refresh_view(v, A7800VID_TAB_PICTURE | R_RESET);
        } else if (id >= IDV_OV0 && id < IDV_OV0 + v->nov && code == BN_CLICKED) {
            on_overlay(v);
        } else if (id == IDV_BREAK && code == BN_CLICKED) {
            break_on_pixel(v);
        } else if (id == IDV_UNPIN && code == BN_CLICKED) {
            a7800vid_tab_unpin(v->tab);
            InvalidateRect(v->pic, NULL, FALSE);
            update_pixel(v);
        }
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->idFrom == IDV_SEC && nh->code == TCN_SELCHANGE) {
            show_section(v);
            refresh_view(v, A7800VID_TAB_TABLES);
        } else if ((nh->idFrom == IDV_REGS || nh->idFrom == IDV_LINES) && nh->code == NM_CUSTOMDRAW) {
            NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)lp;
            listfill *lf = nh->idFrom == IDV_REGS ? &v->rf : &v->lf;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                int i = (int)cd->nmcd.dwItemSpec;
                if (i >= 0 && i < lf->nhot && lf->hot[i]) {
                    if (nh->idFrom == IDV_REGS) {       /* a register that just changed: the accent */
                        cd->clrTextBk = ACCENT;
                        cd->clrText = RGB(255, 255, 255);
                    } else {                            /* a line where something changed */
                        cd->clrTextBk = RGB(0xf6, 0xd6, 0xd2);
                        cd->clrText = RGB(0, 0, 0);
                    }
                }
                return CDRF_DODEFAULT;
            }
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        /* the read-only labels sit on the page colour */
        HDC dc = (HDC)wp;
        SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    case WM_DESTROY:
        a7800vid_tab_free(v->tab);
        free(v->rf.hot);
        free(v->lf.hot);
        free(v);
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void register_classes(HINSTANCE inst)
{
    static int done;
    WNDCLASSW wc;
    if (done) return;
    done = 1;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = page_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = PAGE_CLASS;
    RegisterClassW(&wc);
    wc.lpfnWndProc = pic_proc;
    wc.lpszClassName = PIC_CLASS;
    wc.hbrBackground = NULL;
    wc.hCursor = LoadCursor(NULL, IDC_CROSS);
    wc.style = CS_DBLCLKS;
    RegisterClassW(&wc);
    wc.lpfnWndProc = col_proc;
    wc.lpszClassName = COL_CLASS;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.style = 0;
    RegisterClassW(&wc);
}

static HWND mk_list(vidwin *v, int id, const wchar_t *const *names, int n)
{
    HWND lv = mk(v, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP,
                 id, v->mono);
    LVCOLUMNW col;
    int i;
    SendMessageW(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER,
                 LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    memset(&col, 0, sizeof col);
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    for (i = 0; i < n; i++) {
        col.pszText = (LPWSTR)names[i];
        col.cx = 80;
        SendMessageW(lv, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&col);
    }
    return lv;
}

HWND dbg_video_create(HWND parent, a7800session *session, HFONT ui, HFONT mono, int dpi)
{
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    vidwin *v = calloc(1, sizeof *v);
    int n, i;
    const a7800vid_overlay_info *od;
    const int *zv;
    TCITEMW item;
    static const wchar_t *const regcols[3] = { L"Register", L"Value", L"Meaning" };
    static const wchar_t *const linecols[11] = { L"Row", L"Line", L"CTRL", L"CHAR", L"DPP", L"DLL", L"DL", L"Zone", L"Objs", L"Cycles", L"BG" };
    if (!v) return NULL;
    register_classes(inst);
    v->ui = ui;
    v->mono = mono;
    v->dpi = dpi > 0 ? dpi : 96;
    v->pal_hover = -1;
    v->reg_selected = -1;
    v->tab = a7800vid_tab_new(session);
    if (!v->tab) {
        free(v);
        return NULL;
    }
    memset(&v->bmi, 0, sizeof v->bmi);
    v->bmi.bmiHeader.biSize = sizeof v->bmi.bmiHeader;
    v->bmi.bmiHeader.biPlanes = 1;
    v->bmi.bmiHeader.biBitCount = 32;
    v->bmi.bmiHeader.biCompression = BI_RGB;
    v->page = CreateWindowExW(0, PAGE_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent,
                              NULL, inst, NULL);
    SetWindowLongPtrW(v->page, GWLP_USERDATA, (LONG_PTR)v);

    v->label_view = mk(v, L"STATIC", L"View", SS_LEFT, IDV_LABEL_VIEW, ui);
    v->view = mk(v, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDV_VIEW, ui);
    for (i = 0; i < A7800VID_V_COUNT; i++) {
        wchar_t *w = wide(a7800vid_view_name(i));
        SendMessageW(v->view, CB_ADDSTRING, 0, (LPARAM)w);
        free(w);
    }
    v->label_source = mk(v, L"STATIC", L"Source", SS_LEFT, IDV_LABEL_SOURCE, ui);
    v->source = mk(v, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDV_SOURCE, ui);
    for (i = 0; i < A7800VID_D_COUNT; i++) {
        wchar_t *w = wide(a7800vid_source_name(i));
        SendMessageW(v->source, CB_ADDSTRING, 0, (LPARAM)w);
        free(w);
    }
    v->label_zoom = mk(v, L"STATIC", L"Zoom", SS_LEFT, IDV_LABEL_ZOOM, ui);
    v->zoom = mk(v, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDV_ZOOM, ui);
    zv = a7800vid_zooms(&n);
    for (i = 0; i < n && i < MAX_ZOOMS; i++) {
        wchar_t lbl[16];
        v->zoom_val[v->nzoom++] = zv[i];
        if (zv[i] == 0) wcscpy(lbl, L"Fit");
        else swprintf(lbl, 16, L"%d×", zv[i]);
        SendMessageW(v->zoom, CB_ADDSTRING, 0, (LPARAM)lbl);
    }
    v->brk = mk(v, L"BUTTON", L"Break on writes", BS_PUSHBUTTON | WS_TABSTOP, IDV_BREAK, ui);
    od = a7800vid_overlays(&n);
    for (i = 0; i < n && i < MAX_OVERLAYS; i++) {
        wchar_t *w = wide(od[i].label);
        v->ov[i] = mk(v, L"BUTTON", w, BS_AUTOCHECKBOX | WS_TABSTOP, IDV_OV0 + i, ui);
        v->ov_bit[i] = od[i].bit;
        v->nov++;
        free(w);
    }
    v->status = mk(v, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDV_STATUS, mono);
    v->pic = CreateWindowExW(WS_EX_CLIENTEDGE, PIC_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_HSCROLL | WS_VSCROLL, 0, 0, 10, 10,
                             v->page, (HMENU)(INT_PTR)IDV_PIC, inst, NULL);
    SetWindowLongPtrW(v->pic, GWLP_USERDATA, (LONG_PTR)v);
    v->note = mk(v, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, IDV_NOTE, ui);

    v->sec = mk(v, WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | WS_TABSTOP, IDV_SEC, ui);
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    for (i = 0; i < A7800VID_S_COUNT; i++) {
        wchar_t *w = wide(a7800vid_section_name(i));
        item.pszText = w;
        SendMessageW(v->sec, TCM_INSERTITEMW, (WPARAM)i, (LPARAM)&item);
        free(w);
    }
    SetWindowPos(v->sec, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);   /* under the section controls */
    v->pixel = mk(v, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDV_PIXEL, mono);
    v->unpin = mk(v, L"BUTTON", L"Unpin", BS_PUSHBUTTON | WS_TABSTOP, IDV_UNPIN, ui);
    v->regs = mk_list(v, IDV_REGS, regcols, 3);
    v->lines = mk_list(v, IDV_LINES, linecols, 11);
    v->cols = CreateWindowExW(0, COL_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 10, 10, v->page,
                              (HMENU)(INT_PTR)IDV_COLS, inst, NULL);
    SetWindowLongPtrW(v->cols, GWLP_USERDATA, (LONG_PTR)v);
    v->brk_reg = mk(v, L"BUTTON", L"Break on register write", BS_PUSHBUTTON | WS_TABSTOP, IDV_BREAK_REG, ui);
    v->beam = mk(v, PROGRESS_CLASSW, L"", PBS_SMOOTH, IDV_BEAM, NULL);
    SendMessageW(v->beam, PBM_SETRANGE32, 0, 1000);
    v->timing = mk(v, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDV_TIMING, mono);

    /* start from the controller's state (the A7800_DEBUGGER_VIDEO_* variables) */
    v->updating = 1;
    SendMessageW(v->view, CB_SETCURSEL, (WPARAM)a7800vid_tab_view(v->tab), 0);
    SendMessageW(v->source, CB_SETCURSEL, (WPARAM)a7800vid_tab_source(v->tab), 0);
    EnableWindow(v->source, a7800vid_tab_view(v->tab) == A7800VID_V_DECODED);
    for (i = 0; i < v->nzoom; i++)
        if (v->zoom_val[i] == a7800vid_tab_zoom(v->tab)) SendMessageW(v->zoom, CB_SETCURSEL, (WPARAM)i, 0);
    for (i = 0; i < v->nov; i++)
        SendMessageW(v->ov[i], BM_SETCHECK, (a7800vid_tab_overlays(v->tab) & v->ov_bit[i]) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(v->sec, TCM_SETCURSEL, (WPARAM)a7800vid_tab_env_section(v->tab), 0);
    v->updating = 0;
    show_section(v);
    return v->page;
}

void dbg_video_place(HWND page, int x, int y, int w, int h)
{
    MoveWindow(page, x, y, w > 1 ? w : 1, h > 1 ? h : 1, TRUE);
}

void dbg_video_fonts(HWND page, HFONT ui, HFONT mono, int dpi)
{
    vidwin *v = (vidwin *)GetWindowLongPtrW(page, GWLP_USERDATA);
    if (!v) return;
    v->ui = ui;
    v->mono = mono;
    v->dpi = dpi > 0 ? dpi : 96;
    layout(v);
    InvalidateRect(v->page, NULL, TRUE);
}
