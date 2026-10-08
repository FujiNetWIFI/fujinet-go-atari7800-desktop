/*
 * FujiNet Go Atari 7800 -- the Windows (Win32 + GDI) frontend.
 *
 * No toolkit: a plain window, a menu bar, a status bar and StretchDIBits.
 * That is enough for MARIA's 320-pixel-wide picture (224 lines NTSC, 260
 * PAL), and it keeps the artifact a folder you copy rather than a runtime
 * hunt.
 *
 * Three Windows-specific things are load bearing:
 *
 *   DwmFlush() on a present thread is this platform's frame clock. There is
 *   no GdkFrameClock here, and a plain timer would beat against the panel.
 *
 *   WM_ACTIVATE releases every held key. Alt-tabbing away mid-jump and coming
 *   back to a character walking into a wall is the classic symptom of not
 *   doing this, and Windows is where it happens most, because the WM eats the
 *   key-up.
 *
 *   The light gun is the mouse. While a port has one, the cursor over the
 *   picture is a crosshair and every move and click is mapped back through
 *   the letterbox to frame pixels for the session.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a7800session.h"
#include "controllers/controller_window.h"
#include "debugger/dbg_window.h"
#include "key_forward.h"
#include "resource.h"

#define WIN_CLASS "FujiNetGoAtari7800"
#define APP_TITLE "FujiNet Go Atari 7800"

/* How long a gamepad connect/disconnect notice stays in the status bar. */
#define PAD_NOTICE_MS 4000
/* The status bar's right-hand part: the console and its BIOS. */
#define STATUS_RIGHT_W 200

static a7800session *g_session;
static HWND g_hwnd;
static HWND g_statusbar;
static uint32_t *g_fb;
static int g_fb_height;
static uint64_t g_serial;
static BITMAPINFO g_bmi;
static CRITICAL_SECTION g_fb_lock;
static volatile LONG g_running = 1;
static HANDLE g_present_thread;
static int g_aspect = 0, g_smooth = 0, g_fullscreen = 0;   /* aspect: 0 TV (4:3), 1 square pixels */
static WINDOWPLACEMENT g_placement;
static int g_sysact_down[A7800_SYSACT_COUNT];
static unsigned g_pad_generation;
static char g_pad_notice[160];
static DWORD g_pad_notice_until;
static int g_diff_shown[2] = { -1, -1 };    /* difficulty checks the menu last showed */

/* Where the picture was last drawn (client coordinates), for mapping the
 * pointer back to frame pixels. Written by paint, read by the mouse. */
static RECT g_pic_rect;
static int g_pic_fbh;
static unsigned g_mouse_buttons;       /* bit 0 left, bit 1 right */
static int g_pointer_sent;             /* the session has a pointer to clear */
static int g_tracking_leave;

/* The status bar's owner-drawn left part: the cartridge's status, after a
 * dot that wears the accent colour while its link to FujiNet is up. */
static char g_status_text[400];
static int g_status_link;

/* ---- the present thread: this platform's frame clock ---------------------- */

static DWORD WINAPI present_thread(LPVOID arg)
{
    (void)arg;
    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        LARGE_INTEGER t, f;
        int h = 0;
        /* Blocks until the compositor's next vblank; falls through at once
         * if the DWM is off, and the host's wall-clock pacing takes over --
         * which is the whole reason notify_vsync is advisory. */
        if (FAILED(DwmFlush())) Sleep(8);
        QueryPerformanceCounter(&t);
        QueryPerformanceFrequency(&f);
        a7800session_notify_vsync(g_session,
                                (int64_t)(t.QuadPart * 1000000000LL / f.QuadPart));
        EnterCriticalSection(&g_fb_lock);
        if (a7800session_copy_frame(g_session, g_fb, &h, &g_serial)) {
            g_fb_height = h;
            LeaveCriticalSection(&g_fb_lock);
            InvalidateRect(g_hwnd, NULL, FALSE);
        } else {
            LeaveCriticalSection(&g_fb_lock);
        }
    }
    return 0;
}

/* ---- painting ------------------------------------------------------------- */

static int statusbar_height(void)
{
    RECT r;
    if (!g_statusbar || !IsWindowVisible(g_statusbar)) return 0;
    GetWindowRect(g_statusbar, &r);
    return r.bottom - r.top;
}

static void paint(HDC dc)
{
    RECT rc;
    double want, w, h, sw, sh;
    int fbh;

    GetClientRect(g_hwnd, &rc);
    rc.bottom -= statusbar_height();
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    if (w <= 0 || h <= 0) return;

    EnterCriticalSection(&g_fb_lock);
    fbh = g_fb_height;
    if (fbh <= 0) { LeaveCriticalSection(&g_fb_lock); return; }

    /* A television showed the whole picture at 4:3, NTSC or PAL; "square"
     * shows the framebuffer's pixels exactly. */
    want = g_aspect == 0 ? 4.0 / 3.0 : (double)A7800SESSION_FB_WIDTH / (double)fbh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }

    /* The session's pixels are 0x00RRGGBB: exactly a 32-bit BI_RGB DIB. */
    g_bmi.bmiHeader.biHeight = -fbh;   /* top-down */
    SetStretchBltMode(dc, g_smooth ? HALFTONE : COLORONCOLOR);
    SetRect(&g_pic_rect, (int)((w - sw) / 2), (int)((h - sh) / 2),
            (int)((w - sw) / 2) + (int)sw, (int)((h - sh) / 2) + (int)sh);
    g_pic_fbh = fbh;
    StretchDIBits(dc, g_pic_rect.left, g_pic_rect.top, (int)sw, (int)sh,
                  0, 0, A7800SESSION_FB_WIDTH, fbh, g_fb, &g_bmi, DIB_RGB_COLORS, SRCCOPY);
    LeaveCriticalSection(&g_fb_lock);
}

/* ---- the light gun ----------------------------------------------------------- */

/* Client coordinates to frame pixels; 1 if over the picture. */
static int pointer_to_frame(int cx, int cy, int *x, int *y)
{
    const int pw = g_pic_rect.right - g_pic_rect.left;
    const int ph = g_pic_rect.bottom - g_pic_rect.top;
    if (pw <= 0 || ph <= 0 || g_pic_fbh <= 0) return 0;
    *x = (int)((long long)(cx - g_pic_rect.left) * A7800SESSION_FB_WIDTH / pw);
    *y = (int)((long long)(cy - g_pic_rect.top) * g_pic_fbh / ph);
    return *x >= 0 && *x < A7800SESSION_FB_WIDTH && *y >= 0 && *y < g_pic_fbh;
}

static void forward_pointer(int cx, int cy)
{
    int x = 0, y = 0, inside;
    if (!a7800session_lightgun_active(g_session)) {
        /* The gun went away: the session must not keep aiming the last spot. */
        if (g_pointer_sent) { a7800session_pointer(g_session, 0, 0, 0, 0); g_pointer_sent = 0; }
        return;
    }
    inside = pointer_to_frame(cx, cy, &x, &y);
    a7800session_pointer(g_session, x, y, inside, g_mouse_buttons);
    g_pointer_sent = 1;
}

static void pointer_left(void)
{
    g_tracking_leave = 0;
    if (g_pointer_sent) { a7800session_pointer(g_session, 0, 0, 0, 0); g_pointer_sent = 0; }
}

static void mouse_button(HWND hwnd, unsigned bit, int down, LPARAM lp)
{
    if (down) {
        if (!g_mouse_buttons) SetCapture(hwnd);
        g_mouse_buttons |= bit;
    } else {
        g_mouse_buttons &= ~bit;
        if (!g_mouse_buttons) ReleaseCapture();
    }
    forward_pointer(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
}

/* ---- helpers -------------------------------------------------------------- */

static void restart_session(void)
{
    a7800session_start_opts o;
    a7800session_settings_flush(g_session);
    a7800session_default_opts(g_session, &o);
    a7800session_stop(g_session);
    if (a7800session_start(g_session, &o) != 0)
        MessageBoxA(g_hwnd, a7800session_last_error(g_session), "Could not start",
                    MB_ICONWARNING | MB_OK);
}

static void run_sysaction(int sa)
{
    switch (sa) {
    case A7800_SYSACT_REBOOT_CONFIG: a7800session_reboot_to_config(g_session); break;
    case A7800_SYSACT_PAUSE:
        /* Showing the debugger attaches it, and attaching stops the machine. */
        a7800_debugger_show(g_hwnd, g_session);
        break;
    default: break;
    }
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '\\');
    const char *t = strrchr(p, '/');
    if (t && (!s || t > s)) s = t;
    return s ? s + 1 : p;
}

static void set_text_utf8(HWND h, const char *text)
{
    wchar_t w[512];
    MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 512);
    SetWindowTextW(h, w);
}

static void layout_statusbar(void)
{
    RECT rc;
    int parts[2];
    if (!g_statusbar) return;
    SendMessageA(g_statusbar, WM_SIZE, 0, 0);
    GetClientRect(g_hwnd, &rc);
    parts[0] = rc.right - STATUS_RIGHT_W > 100 ? rc.right - STATUS_RIGHT_W : 100;
    parts[1] = -1;
    SendMessageA(g_statusbar, SB_SETPARTS, 2, (LPARAM)parts);
}

static void update_status(void)
{
    char title[512], st[160], console[120];
    const char *cart = a7800session_cart_path(g_session);
    const int running = a7800session_is_running(g_session);

    if (!running) {
        snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 stopped");
        snprintf(st, sizeof st, "stopped");
    } else {
        a7800session_cart_status(g_session, st, sizeof st);
        if (cart && cart[0])
            snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 %s", base_name(cart));
        else if (a7800session_cart_booted_game(g_session))
            snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 booted from FujiNet");
        else
            snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 CONFIG");
    }
    set_text_utf8(g_hwnd, title);

    if (!g_statusbar) return;
    if (g_pad_notice[0] && GetTickCount() < g_pad_notice_until) {
        snprintf(g_status_text, sizeof g_status_text, "%s", g_pad_notice);
    } else {
        g_pad_notice[0] = '\0';
        snprintf(g_status_text, sizeof g_status_text, "FujiNet: %s%s", st,
                 a7800session_fujinet_running(g_session) ? "" : " (runtime not running)");
    }
    g_status_link = a7800session_cart_link_up(g_session) == 1;
    SendMessageA(g_statusbar, SB_SETTEXTA, 0 | SBT_OWNERDRAW, (LPARAM)g_status_text);

    /* The console actually running, and whether a BIOS starts it. */
    {
        const int region = a7800session_running_region(g_session);
        const int bios = a7800session_bios(g_session, region);
        snprintf(console, sizeof console, "%s console, %s", region == A7800_REGION_PAL ? "PAL" : "NTSC",
                 bios >= 0 && a7800session_bios_available(g_session, bios) ? "BIOS" : "no BIOS");
        SendMessageA(g_statusbar, SB_SETTEXTA, 1, (LPARAM)console);
    }
}

static void draw_status_part(const DRAWITEMSTRUCT *di)
{
    const char *text = (const char *)di->itemData;
    RECT r = di->rcItem, dot;
    const int size = 8, cy = (r.top + r.bottom) / 2;
    HBRUSH brush = CreateSolidBrush(g_status_link
        ? RGB((A7800SESSION_ACCENT_RGB >> 16) & 0xff, (A7800SESSION_ACCENT_RGB >> 8) & 0xff, A7800SESSION_ACCENT_RGB & 0xff)
        : GetSysColor(COLOR_GRAYTEXT));
    HGDIOBJ oldb = SelectObject(di->hDC, brush);
    HGDIOBJ oldp = SelectObject(di->hDC, GetStockObject(NULL_PEN));
    wchar_t w[400];

    SetRect(&dot, r.left + 4, cy - size / 2, r.left + 4 + size, cy - size / 2 + size);
    Ellipse(di->hDC, dot.left, dot.top, dot.right + 1, dot.bottom + 1);
    SelectObject(di->hDC, oldp);
    SelectObject(di->hDC, oldb);
    DeleteObject(brush);

    r.left = dot.right + 6;
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, GetSysColor(COLOR_BTNTEXT));
    MultiByteToWideChar(CP_UTF8, 0, text ? text : "", -1, w, 400);
    DrawTextW(di->hDC, w, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

/* Gamepad hot-plug: the session bumps a generation on every add/remove and
 * keeps the last event as text; show it for a few seconds. */
static void poll_gamepads(void)
{
    const unsigned gen = a7800session_gamepad_generation(g_session);
    if (gen == g_pad_generation) return;
    g_pad_generation = gen;
    if (a7800session_gamepad_last_event(g_session, g_pad_notice, sizeof g_pad_notice) > 0) {
        g_pad_notice_until = GetTickCount() + PAD_NOTICE_MS;
        update_status();
    }
    a7800_controller_window_gamepads_changed();
}

/* ---- menu ----------------------------------------------------------------- */

/* Keep the difficulty checks in step with the session: the Controllers
 * window and a bound key toggle them too. Cheap enough for the 100 ms
 * timer; only touches the menu when something changed. */
static void sync_switch_ui(int force)
{
    static const int sw[2] = { A7800_SW_LEFT_DIFF, A7800_SW_RIGHT_DIFF };
    static const int id[2] = { IDM_LEFT_DIFF, IDM_RIGHT_DIFF };
    HMENU m = GetMenu(g_hwnd);
    int i;
    for (i = 0; i < 2; i++) {
        const int on = a7800session_switch_get(g_session, sw[i]);
        if (!force && on == g_diff_shown[i]) continue;
        g_diff_shown[i] = on;
        if (m) CheckMenuItem(m, (UINT)id[i], MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
    }
}

static void toggle_difficulty(int sw)
{
    a7800session_switch_set(g_session, sw, !a7800session_switch_get(g_session, sw));
    sync_switch_ui(0);
}

static void build_menu(HWND hwnd)
{
    HMENU bar = CreateMenu();
    HMENU machine = CreatePopupMenu();
    HMENU console = CreatePopupMenu();
    HMENU view = CreatePopupMenu();
    HMENU fuji = CreatePopupMenu();
    HMENU help = CreatePopupMenu();

    AppendMenuA(machine, MF_STRING, IDM_OPEN, "&Open Cartridge...\tCtrl+O");
    AppendMenuA(machine, MF_STRING, IDM_EJECT, "&Eject Cartridge");
    AppendMenuA(machine, MF_STRING, IDM_IMPORT_SD, "&Import Cartridge to SD...");
    AppendMenuA(machine, MF_STRING, IDM_IMPORT_BIOS, "Import &BIOS...");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_POWER_CYCLE, "&Power Cycle");
    AppendMenuA(machine, MF_STRING, IDM_REBOOT_CONFIG, "Reboot to &CONFIG\tCtrl+R");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_SETTINGS, "&Settings...\tCtrl+,");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_EXIT, "E&xit");

    /* F1-F3 are the switches' key bindings (remappable in the Controllers
     * window), not accelerators: the labels only show the defaults. The
     * difficulty switches have no default key, so Alt+L / Alt+R are. */
    AppendMenuA(console, MF_STRING, IDM_SELECT, "&Select\tF1");
    AppendMenuA(console, MF_STRING, IDM_RESET, "&Reset\tF2");
    AppendMenuA(console, MF_STRING, IDM_PAUSE, "&Pause\tF3");
    AppendMenuA(console, MF_SEPARATOR, 0, NULL);
    AppendMenuA(console, MF_STRING, IDM_LEFT_DIFF, "&Left Difficulty A\tAlt+L");
    AppendMenuA(console, MF_STRING, IDM_RIGHT_DIFF, "Ri&ght Difficulty A\tAlt+R");

    AppendMenuA(view, MF_STRING, IDM_CONTROLLERS, "&Controllers\tF9");
    AppendMenuA(view, MF_STRING, IDM_DEBUGGER, "&Debugger\tF12");
    AppendMenuA(view, MF_SEPARATOR, 0, NULL);
    AppendMenuA(view, MF_STRING | (g_aspect == 0 ? MF_CHECKED : 0), IDM_TV_ASPECT, "&TV Aspect (4:3)");
    AppendMenuA(view, MF_STRING | (g_smooth ? MF_CHECKED : 0), IDM_SMOOTH, "&Smooth Scaling");
    AppendMenuA(view, MF_STRING, IDM_FULLSCREEN, "&Fullscreen\tF11");

    AppendMenuA(fuji, MF_STRING, IDM_FUJINET_CONFIG, "FujiNet &Web UI");
    AppendMenuA(fuji, MF_STRING, IDM_FUJINET_LOG, "Console &Log");

    AppendMenuA(help, MF_STRING, IDM_ABOUT, "&About " APP_TITLE);

    AppendMenuA(bar, MF_POPUP, (UINT_PTR)machine, "&Machine");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)console, "&Console");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)view, "&View");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)fuji, "&FujiNet");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)help, "&Help");
    SetMenu(hwnd, bar);
    sync_switch_ui(1);
}

static void open_cart(const char *path)
{
    if (a7800session_load_cart(g_session, path) != 0)
        MessageBoxA(g_hwnd, a7800session_last_error(g_session), "Could not open",
                    MB_ICONWARNING | MB_OK);
    update_status();
}

static void import_rom(const char *path)
{
    char what[160], msg[400];
    if (a7800session_import_rom(g_session, path, what, sizeof what) != 0) {
        MessageBoxA(g_hwnd, a7800session_last_error(g_session), "Import failed", MB_ICONWARNING | MB_OK);
        return;
    }
    snprintf(msg, sizeof msg, "Imported the %s.\n\nNo BIOS is needed to play; choose it in Settings "
             "to start games through it.", what);
    MessageBoxA(g_hwnd, msg, "Imported", MB_ICONINFORMATION | MB_OK);
    update_status();
}

/* A dropped file: a cartridge opens, a BIOS or High Score Cart ROM is
 * imported, anything else goes to FujiNet's SD folder. */
static void load_media(const char *path)
{
    char dest[1024];
    if (a7800session_media_is_rom(path)) { import_rom(path); return; }
    if (a7800session_import_media(g_session, path, dest, sizeof dest) != 0) {
        MessageBoxA(g_hwnd, a7800session_last_error(g_session), "Import failed",
                    MB_ICONWARNING | MB_OK);
        return;
    }
    if (a7800session_media_is_cartridge(path)) { open_cart(dest); return; }
    MessageBoxA(g_hwnd, "Copied to FujiNet's SD folder. Mount it from the CONFIG client.",
                "Imported", MB_ICONINFORMATION | MB_OK);
}

static int pick_file(const char *title, const char *filter, char *path, DWORD pathsz)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    path[0] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = pathsz;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

static int pick_cart(const char *title, char *path, DWORD pathsz)
{
    return pick_file(title, "Atari 7800 cartridges (*.a78;*.bin;*.zip;*.7z)\0*.a78;*.bin;*.zip;*.7z\0"
                            "All files\0*.*\0\0", path, pathsz);
}

/* ---- settings window -------------------------------------------------------
 *
 * Same keys and defaults as the other frontends' Preferences, so a machine
 * configured in one comes up the same in another. The TV system, the BIOS,
 * the High Score Cart, controller types, the stick and the picture apply
 * live; the host options restart the session when the window closes.
 */

static HWND g_settings_window;
static int g_settings_dirty;
static HWND g_pad_list, g_pad_port, g_port_combo[2], g_bios_combo[2], g_hsc_check;
static int g_port_detected_shown[2];
/* Which BIOS each row of a BIOS combo is: -1 none, else a bios_info index. */
static int g_bios_rows[2][8];

static const char *aspect_name(int i)
{
    static const char *const names[] = { "4:3 TV", "Square pixels", NULL };
    return (i >= 0 && i < 2) ? names[i] : NULL;
}

static void settings_apply_checkbox(HWND hwnd, int id, const char *key, int def)
{
    int on = SendMessageA(GetDlgItem(hwnd, id), BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (a7800session_get_int(g_session, key, def) != on) {
        a7800session_set_int(g_session, key, on);
        g_settings_dirty = 1;
    }
}

static void refresh_pad_list(void)
{
    int i, n, sel;
    if (!g_pad_list) return;
    sel = (int)SendMessageA(g_pad_list, LB_GETCURSEL, 0, 0);
    SendMessageA(g_pad_list, LB_RESETCONTENT, 0, 0);
    n = a7800session_gamepad_count(g_session);
    if (n == 0) {
        SendMessageA(g_pad_list, LB_ADDSTRING, 0, (LPARAM)"(no gamepads connected)");
    }
    for (i = 0; i < n; i++) {
        char name[128], line[200];
        int eff = a7800session_gamepad_effective_port(g_session, i);
        a7800session_gamepad_name(g_session, i, name, sizeof name);
        if (eff >= 0)
            snprintf(line, sizeof line, "%s  [player %d%s]", name, eff + 1,
                     a7800session_gamepad_assignment(g_session, i) < 0 ? ", automatic" : "");
        else
            snprintf(line, sizeof line, "%s  [unused]", name);
        SendMessageA(g_pad_list, LB_ADDSTRING, 0, (LPARAM)line);
    }
    if (sel >= 0 && sel < n) SendMessageA(g_pad_list, LB_SETCURSEL, (WPARAM)sel, 0);
}

/* "Auto (now: XG-1 Light Gun)": AUTO names what it resolved to for the
 * running game, which changes as games boot. */
static void port_auto_text(int port, char *out, int outsz)
{
    const char *now = a7800_ctrl_type_name(a7800session_port_detected(g_session, port));
    snprintf(out, (size_t)outsz, "Auto (now: %s)", now ? now : "?");
}

static void refresh_port_combos(int force)
{
    int port;
    for (port = 0; port < 2; port++) {
        const int det = a7800session_port_detected(g_session, port);
        char text[96];
        int sel;
        if (!g_port_combo[port] || (!force && det == g_port_detected_shown[port])) continue;
        g_port_detected_shown[port] = det;
        sel = (int)SendMessageA(g_port_combo[port], CB_GETCURSEL, 0, 0);
        port_auto_text(port, text, sizeof text);
        SendMessageA(g_port_combo[port], CB_DELETESTRING, 0, 0);
        SendMessageA(g_port_combo[port], CB_INSERTSTRING, 0, (LPARAM)text);
        SendMessageA(g_port_combo[port], CB_SETCURSEL, (WPARAM)sel, 0);
    }
}

/* The BIOS choices for one console: None, then each image of that region
 * that has been imported. */
static void fill_bios_combo(int which)
{
    const int region = which ? A7800_REGION_PAL : A7800_REGION_NTSC;
    const int chosen = a7800session_bios(g_session, region);
    HWND c = g_bios_combo[which];
    int i, row = 0, sel = 0;
    if (!c) return;
    SendMessageA(c, CB_RESETCONTENT, 0, 0);
    SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)"None (start the cartridge directly)");
    g_bios_rows[which][row++] = -1;
    for (i = 0; i < a7800session_bios_count() && row < 8; i++) {
        const a7800_bios_info *b = a7800session_bios_info(i);
        if (!b || b->region != region || !a7800session_bios_available(g_session, i)) continue;
        SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)b->desc);
        if (i == chosen) sel = row;
        g_bios_rows[which][row++] = i;
    }
    SendMessageA(c, CB_SETCURSEL, (WPARAM)sel, 0);
}

static LRESULT CALLBACK settings_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SET_REGION:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                a7800session_set_region(g_session,
                    (int)SendMessageA(GetDlgItem(hwnd, IDC_SET_REGION), CB_GETCURSEL, 0, 0));
                update_status();
            }
            return 0;
        case IDC_SET_BIOS_NTSC:
        case IDC_SET_BIOS_PAL:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                const int which = LOWORD(wp) == IDC_SET_BIOS_PAL;
                const int row = (int)SendMessageA(g_bios_combo[which], CB_GETCURSEL, 0, 0);
                if (row >= 0 && row < 8)
                    a7800session_set_bios(g_session, which ? A7800_REGION_PAL : A7800_REGION_NTSC,
                                          g_bios_rows[which][row]);
                update_status();
            }
            return 0;
        case IDC_SET_HSC:
            a7800session_set_hsc(g_session,
                SendMessageA(g_hsc_check, BM_GETCHECK, 0, 0) == BST_CHECKED);
            return 0;
        case IDC_SET_ASPECT:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                HMENU m = GetMenu(g_hwnd);
                g_aspect = (int)SendMessageA(GetDlgItem(hwnd, IDC_SET_ASPECT), CB_GETCURSEL, 0, 0);
                a7800session_set_int(g_session, "aspect", g_aspect);
                if (m) CheckMenuItem(m, IDM_TV_ASPECT, MF_BYCOMMAND | (g_aspect == 0 ? MF_CHECKED : MF_UNCHECKED));
                InvalidateRect(g_hwnd, NULL, TRUE);
            }
            return 0;
        case IDC_SET_PORT0:
        case IDC_SET_PORT1:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int port = LOWORD(wp) == IDC_SET_PORT1;
                int sel = (int)SendMessageA(GetDlgItem(hwnd, LOWORD(wp)), CB_GETCURSEL, 0, 0);
                a7800session_set_port_type(g_session, port, sel);
                a7800_controller_window_gamepads_changed();
            }
            return 0;
        case IDC_SET_AN_JOY:
            a7800session_set_analog(g_session,
                SendMessageA(GetDlgItem(hwnd, IDC_SET_AN_JOY), BM_GETCHECK, 0, 0) == BST_CHECKED);
            return 0;
        case IDC_SET_PAD_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE) {
                int sel = (int)SendMessageA(g_pad_list, LB_GETCURSEL, 0, 0);
                SendMessageA(g_pad_port, CB_SETCURSEL,
                             (WPARAM)(a7800session_gamepad_assignment(g_session, sel) + 1), 0);
            }
            return 0;
        case IDC_SET_PAD_PORT:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int sel = (int)SendMessageA(g_pad_list, LB_GETCURSEL, 0, 0);
                int choice = (int)SendMessageA(g_pad_port, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < a7800session_gamepad_count(g_session))
                    a7800session_gamepad_assign(g_session, sel, choice - 1);
                refresh_pad_list();
                a7800_controller_window_gamepads_changed();
            }
            return 0;
        case IDC_SET_FUJINET: settings_apply_checkbox(hwnd, IDC_SET_FUJINET, "enable_fujinet", 1); return 0;
        case IDC_SET_AUDIO: settings_apply_checkbox(hwnd, IDC_SET_AUDIO, "enable_audio", 1); return 0;
        case IDC_SET_GAMEPAD: settings_apply_checkbox(hwnd, IDC_SET_GAMEPAD, "enable_gamepad", 1); return 0;
        default: break;
        }
        break;
    case WM_HSCROLL:
        if ((HWND)lp == GetDlgItem(hwnd, IDC_SET_VOLUME))
            a7800session_set_volume(g_session, (int)SendMessageA((HWND)lp, TBM_GETPOS, 0, 0));
        return 0;
    case WM_TIMER:
        if (wp == IDT_SETTINGS_PADS) {
            static unsigned seen;
            unsigned gen = a7800session_gamepad_generation(g_session);
            if (gen != seen) { seen = gen; refresh_pad_list(); }
            refresh_port_combos(0);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, IDT_SETTINGS_PADS);
        g_settings_window = NULL;
        g_pad_list = g_pad_port = NULL;
        g_port_combo[0] = g_port_combo[1] = NULL;
        g_bios_combo[0] = g_bios_combo[1] = NULL;
        g_hsc_check = NULL;
        if (g_settings_dirty) {
            g_settings_dirty = 0;
            restart_session();
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static HWND settings_checkbox(HWND parent, HINSTANCE inst, const char *text, int id, int x, int y, int w, int checked)
{
    HWND h = CreateWindowExA(0, "BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                             x, y, w, 22, parent, (HMENU)(INT_PTR)id, inst, NULL);
    SendMessageA(h, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(h, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return h;
}

static HWND settings_label(HWND parent, HINSTANCE inst, const char *text, int x, int y, int w, int h)
{
    HWND l = CreateWindowExA(0, "STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, parent,
                             NULL, inst, NULL);
    SendMessageA(l, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return l;
}

static HWND settings_combo(HWND parent, HINSTANCE inst, int id, int x, int y, int w,
                           const char *(*names)(int), int sel)
{
    HWND c = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                             x, y, w, 200, parent, (HMENU)(INT_PTR)id, inst, NULL);
    int i;
    if (names)
        for (i = 0; names(i); i++) SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)names(i));
    SendMessageA(c, CB_SETCURSEL, (WPARAM)sel, 0);
    SendMessageA(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}

static void show_settings(HINSTANCE inst)
{
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND h;
    int y = 12, port;

    if (g_settings_window) { SetForegroundWindow(g_settings_window); return; }
    {
        static int registered;
        if (!registered) {
            WNDCLASSA wc;
            memset(&wc, 0, sizeof wc);
            wc.lpfnWndProc = settings_proc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            wc.lpszClassName = "A7800SettingsWindow";
            RegisterClassA(&wc);
            registered = 1;
        }
    }
    g_settings_window = CreateWindowA("A7800SettingsWindow", "Settings",
        WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME),
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 720, NULL, NULL, inst, NULL);

    settings_label(g_settings_window, inst, "Console (applied immediately)", 16, y, 460, 18); y += 22;
    settings_label(g_settings_window, inst, "TV system:", 16, y + 3, 100, 18);
    settings_combo(g_settings_window, inst, IDC_SET_REGION, 120, y, 220, a7800_region_name,
                   a7800session_region(g_session));
    y += 30;
    settings_label(g_settings_window, inst, "NTSC BIOS:", 16, y + 3, 100, 18);
    g_bios_combo[0] = settings_combo(g_settings_window, inst, IDC_SET_BIOS_NTSC, 120, y, 340, NULL, 0);
    fill_bios_combo(0);
    y += 28;
    settings_label(g_settings_window, inst, "PAL BIOS:", 16, y + 3, 100, 18);
    g_bios_combo[1] = settings_combo(g_settings_window, inst, IDC_SET_BIOS_PAL, 120, y, 340, NULL, 0);
    fill_bios_combo(1);
    y += 28;
    settings_label(g_settings_window, inst,
                   "No BIOS is needed. Machine > Import BIOS... adds your own.", 120, y, 360, 18);
    y += 24;
    g_hsc_check = settings_checkbox(g_settings_window, inst, "High Score Cart (games keep their high scores)",
                                    IDC_SET_HSC, 16, y, 400, a7800session_hsc(g_session));
    if (!a7800session_hsc_available(g_session)) {
        EnableWindow(g_hsc_check, FALSE);
        y += 22;
        settings_label(g_settings_window, inst, "Import the High Score Cart's ROM to use it.", 36, y, 400, 18);
    }
    y += 26;
    settings_label(g_settings_window, inst, "Picture:", 16, y + 3, 100, 18);
    settings_combo(g_settings_window, inst, IDC_SET_ASPECT, 120, y, 220, aspect_name, g_aspect);
    y += 36;

    settings_label(g_settings_window, inst, "Controllers (applied immediately)", 16, y, 460, 18); y += 22;
    for (port = 0; port < 2; port++) {
        settings_label(g_settings_window, inst, port ? "Player 2:" : "Player 1:", 16, y + 3, 100, 18);
        g_port_combo[port] = settings_combo(g_settings_window, inst, port ? IDC_SET_PORT1 : IDC_SET_PORT0,
                                            120, y, 260, a7800_ctrl_type_name,
                                            a7800session_port_type(g_session, port));
        y += 28;
    }
    refresh_port_combos(1);   /* "Auto" names what it resolved to */
    y += 4;
    settings_checkbox(g_settings_window, inst, "The left stick drives the joystick too", IDC_SET_AN_JOY, 16, y, 340,
                      a7800session_get_int(g_session, "analog_joystick", 1));
    y += 32;

    settings_label(g_settings_window, inst, "Gamepads (select one, then choose its player):", 16, y, 460, 18); y += 22;
    g_pad_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY, 16, y, 320, 70,
        g_settings_window, (HMENU)(INT_PTR)IDC_SET_PAD_LIST, inst, NULL);
    SendMessageA(g_pad_list, WM_SETFONT, (WPARAM)font, TRUE);
    g_pad_port = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        346, y, 128, 200, g_settings_window, (HMENU)(INT_PTR)IDC_SET_PAD_PORT, inst, NULL);
    SendMessageA(g_pad_port, CB_ADDSTRING, 0, (LPARAM)"Automatic");
    SendMessageA(g_pad_port, CB_ADDSTRING, 0, (LPARAM)"Player 1");
    SendMessageA(g_pad_port, CB_ADDSTRING, 0, (LPARAM)"Player 2");
    SendMessageA(g_pad_port, CB_SETCURSEL, 0, 0);
    SendMessageA(g_pad_port, WM_SETFONT, (WPARAM)font, TRUE);
    refresh_pad_list();
    y += 80;

    settings_label(g_settings_window, inst, "Volume:", 16, y + 3, 100, 18);
    h = CreateWindowExA(0, TRACKBAR_CLASSA, "", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
                        120, y, 320, 28, g_settings_window, (HMENU)(INT_PTR)IDC_SET_VOLUME, inst, NULL);
    SendMessageA(h, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageA(h, TBM_SETTICFREQ, 10, 0);
    SendMessageA(h, TBM_SETPOS, TRUE, (LPARAM)a7800session_get_int(g_session, "volume", 100));
    y += 40;

    settings_label(g_settings_window, inst, "Host (applied by restarting the session)", 16, y, 460, 18); y += 22;
    settings_checkbox(g_settings_window, inst, "Enable FujiNet", IDC_SET_FUJINET, 16, y, 200,
                      a7800session_get_int(g_session, "enable_fujinet", 1)); y += 26;
    settings_checkbox(g_settings_window, inst, "Audio", IDC_SET_AUDIO, 16, y, 200,
                      a7800session_get_int(g_session, "enable_audio", 1)); y += 26;
    settings_checkbox(g_settings_window, inst, "Gamepads", IDC_SET_GAMEPAD, 16, y, 200,
                      a7800session_get_int(g_session, "enable_gamepad", 1));
    y += 40;

    /* Fit the window to what went into it. */
    {
        RECT want = { 0, 0, 490, y };
        AdjustWindowRect(&want, WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME), FALSE);
        SetWindowPos(g_settings_window, NULL, 0, 0, want.right - want.left, want.bottom - want.top,
                     SWP_NOMOVE | SWP_NOZORDER);
    }

    SetTimer(g_settings_window, IDT_SETTINGS_PADS, 500, NULL);
    ShowWindow(g_settings_window, SW_SHOW);
}

/* ---- FujiNet console log --------------------------------------------------- */

static HWND g_log_window;
static HWND g_log_edit;

static void log_refresh(void)
{
    static char buf[128 * 1024];
    int n;
    DWORD first, last, lines;
    if (!g_log_edit) return;

    first = (DWORD)SendMessageA(g_log_edit, EM_GETFIRSTVISIBLELINE, 0, 0);
    lines = (DWORD)SendMessageA(g_log_edit, EM_GETLINECOUNT, 0, 0);
    {
        RECT rc;
        HDC dc = GetDC(g_log_edit);
        TEXTMETRICA tm;
        int visible = 1;
        GetClientRect(g_log_edit, &rc);
        if (dc) {
            HFONT of = (HFONT)SelectObject(dc, (HGDIOBJ)SendMessageA(g_log_edit, WM_GETFONT, 0, 0));
            if (GetTextMetricsA(dc, &tm) && tm.tmHeight > 0)
                visible = (rc.bottom - rc.top) / tm.tmHeight;
            SelectObject(dc, of);
            ReleaseDC(g_log_edit, dc);
        }
        last = first + (DWORD)(visible > 0 ? visible : 1);
    }
    n = a7800session_fujinet_copy_log(g_session, buf, sizeof buf);
    SetWindowTextA(g_log_edit, n > 0 ? buf : "(no FujiNet output yet)");
    if (last >= lines) {
        int len = GetWindowTextLengthA(g_log_edit);
        SendMessageA(g_log_edit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
        SendMessageA(g_log_edit, EM_SCROLLCARET, 0, 0);
    }
}

static LRESULT CALLBACK log_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (g_log_edit) MoveWindow(g_log_edit, 0, 0, rc.right - rc.left, rc.bottom - rc.top, TRUE);
        return 0;
    }
    case WM_TIMER:
        if (wp == IDT_LOG_REFRESH) log_refresh();
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, IDT_LOG_REFRESH);
        g_log_window = NULL;
        g_log_edit = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void show_fujinet_log(HINSTANCE inst)
{
    RECT rc;
    if (g_log_window) { SetForegroundWindow(g_log_window); return; }
    {
        static int registered;
        if (!registered) {
            WNDCLASSA wc;
            memset(&wc, 0, sizeof wc);
            wc.lpfnWndProc = log_proc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            wc.lpszClassName = "A7800FujiNetLogWindow";
            RegisterClassA(&wc);
            registered = 1;
        }
    }
    g_log_window = CreateWindowA("A7800FujiNetLogWindow", "FujiNet Console Log", WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 860, 600, NULL, NULL, inst, NULL);
    GetClientRect(g_log_window, &rc);
    g_log_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        0, 0, rc.right - rc.left, rc.bottom - rc.top, g_log_window, (HMENU)(INT_PTR)IDC_LOG_EDIT, inst, NULL);
    SendMessageA(g_log_edit, WM_SETFONT, (WPARAM)GetStockObject(ANSI_FIXED_FONT), TRUE);
    SetTimer(g_log_window, IDT_LOG_REFRESH, 1000, NULL);
    log_refresh();
    ShowWindow(g_log_window, SW_SHOW);
}

/* ---- window --------------------------------------------------------------- */

static void toggle_fullscreen(HWND hwnd)
{
    DWORD style = GetWindowLong(hwnd, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi;
        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        g_placement.length = sizeof g_placement;
        GetWindowPlacement(hwnd, &g_placement);
        if (GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLong(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetMenu(hwnd, NULL);
            ShowWindow(g_statusbar, SW_HIDE);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_fullscreen = 1;
        }
    } else {
        SetWindowLong(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        build_menu(hwnd);
        ShowWindow(g_statusbar, SW_SHOW);
        SetWindowPlacement(hwnd, &g_placement);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = 0;
    }
    InvalidateRect(hwnd, NULL, TRUE);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        layout_statusbar();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_DRAWITEM:
        if (wp == IDC_STATUSBAR) { draw_status_part((const DRAWITEMSTRUCT *)lp); return TRUE; }
        break;

    /* The light gun. */
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && a7800session_lightgun_active(g_session)) {
            POINT p;
            int x, y;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            if (pointer_to_frame(p.x, p.y, &x, &y)) { SetCursor(LoadCursor(NULL, IDC_CROSS)); return TRUE; }
        }
        break;
    case WM_MOUSEMOVE:
        if (!g_tracking_leave) {
            TRACKMOUSEEVENT tme;
            memset(&tme, 0, sizeof tme);
            tme.cbSize = sizeof tme;
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            g_tracking_leave = TrackMouseEvent(&tme) ? 1 : 0;
        }
        forward_pointer(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSELEAVE:
        pointer_left();
        return 0;
    case WM_LBUTTONDOWN: mouse_button(hwnd, 1, 1, lp); return 0;
    case WM_LBUTTONUP:   mouse_button(hwnd, 1, 0, lp); return 0;
    case WM_RBUTTONDOWN: mouse_button(hwnd, 2, 1, lp); return 0;
    case WM_RBUTTONUP:   mouse_button(hwnd, 2, 0, lp); return 0;
    case WM_CAPTURECHANGED:
        /* Lost the mouse mid-shot (Alt+Tab): the trigger is released. */
        if ((HWND)lp != hwnd && g_mouse_buttons) {
            g_mouse_buttons = 0;
            if (g_pointer_sent) a7800session_pointer(g_session, 0, 0, 0, 0);
            g_pointer_sent = 0;
        }
        break;

    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        uint32_t ks;
        int sa;
        if (wp == VK_F9) { a7800_controller_window_toggle(hwnd, g_session); return 0; }
        if (wp == VK_F12) { a7800_debugger_toggle(hwnd, g_session); return 0; }
        if (wp == VK_F11) { toggle_fullscreen(hwnd); return 0; }
        if (msg == WM_SYSKEYDOWN) {
            if (wp == 'L') { toggle_difficulty(A7800_SW_LEFT_DIFF); return 0; }
            if (wp == 'R') { toggle_difficulty(A7800_SW_RIGHT_DIFF); return 0; }
            break;
        }
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            if (wp == 'O') { PostMessage(hwnd, WM_COMMAND, IDM_OPEN, 0); return 0; }
            if (wp == 'R') { PostMessage(hwnd, WM_COMMAND, IDM_REBOOT_CONFIG, 0); return 0; }
            if (wp == VK_OEM_COMMA) { PostMessage(hwnd, WM_COMMAND, IDM_SETTINGS, 0); return 0; }
            break;
        }
        if (lp & (1 << 30)) return 0;  /* auto-repeat: the key is already held */
        ks = a7800_keysym_from_msg(wp, lp);
        if (!ks) break;
        sa = a7800session_key_sysaction(g_session, ks);
        if (sa >= 0) {
            if (!g_sysact_down[sa]) { g_sysact_down[sa] = 1; run_sysaction(sa); update_status(); }
            return 0;
        }
        /* A bound difficulty switch toggles inside the session; the menu
         * follows. */
        if (a7800session_key(g_session, ks, 1)) { sync_switch_ui(0); return 0; }
        break;
    }
    case WM_KEYUP: case WM_SYSKEYUP: {
        uint32_t ks = a7800_keysym_from_msg(wp, lp);
        int sa;
        if (!ks) break;
        sa = a7800session_key_sysaction(g_session, ks);
        if (sa >= 0) { g_sysact_down[sa] = 0; return 0; }
        if (a7800session_key(g_session, ks, 0)) return 0;
        break;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            a7800session_release_all(g_session);
            memset(g_sysact_down, 0, sizeof g_sysact_down);
        }
        return 0;

    case WM_TIMER:
        if (wp == IDT_STATUS) update_status();
        else if (wp == IDT_SYSACT) {
            int sa;
            while (a7800session_sysaction_take(g_session, &sa)) { run_sysaction(sa); update_status(); }
            poll_gamepads();
            sync_switch_ui(0);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_OPEN: {
            char path[MAX_PATH];
            if (pick_cart("Open Cartridge", path, sizeof path)) open_cart(path);
            return 0;
        }
        case IDM_EJECT: a7800session_eject(g_session); update_status(); return 0;
        case IDM_IMPORT_SD: {
            char path[MAX_PATH], dest[1024], msg[1200];
            if (!pick_cart("Import Cartridge to SD", path, sizeof path)) return 0;
            if (a7800session_import_cart_to_sd(g_session, path, dest, sizeof dest) != 0) {
                MessageBoxA(hwnd, a7800session_last_error(g_session), "Import failed", MB_ICONWARNING | MB_OK);
                return 0;
            }
            snprintf(msg, sizeof msg, "%s is on the SD host. Boot it from the CONFIG client.", base_name(dest));
            MessageBoxA(hwnd, msg, "Imported", MB_ICONINFORMATION | MB_OK);
            return 0;
        }
        case IDM_IMPORT_BIOS: {
            char path[MAX_PATH];
            if (pick_file("Import BIOS",
                          "BIOS and High Score Cart images (*.bin;*.rom;*.u7;*.a78;*.zip)\0*.bin;*.rom;*.u7;*.a78;*.zip\0"
                          "All files\0*.*\0\0", path, sizeof path))
                import_rom(path);
            return 0;
        }
        case IDM_POWER_CYCLE: a7800session_power_cycle(g_session); update_status(); return 0;
        case IDM_REBOOT_CONFIG: run_sysaction(A7800_SYSACT_REBOOT_CONFIG); update_status(); return 0;
        case IDM_SELECT: a7800session_switch_pulse(g_session, A7800_SW_SELECT); return 0;
        case IDM_RESET: a7800session_switch_pulse(g_session, A7800_SW_RESET); return 0;
        case IDM_PAUSE: a7800session_switch_pulse(g_session, A7800_SW_PAUSE); return 0;
        case IDM_LEFT_DIFF: toggle_difficulty(A7800_SW_LEFT_DIFF); return 0;
        case IDM_RIGHT_DIFF: toggle_difficulty(A7800_SW_RIGHT_DIFF); return 0;
        case IDM_CONTROLLERS: a7800_controller_window_toggle(hwnd, g_session); return 0;
        case IDM_DEBUGGER: a7800_debugger_toggle(hwnd, g_session); return 0;
        case IDM_FULLSCREEN: toggle_fullscreen(hwnd); return 0;
        case IDM_TV_ASPECT: {
            HMENU m = GetMenu(hwnd);
            g_aspect = g_aspect ? 0 : 1;
            if (m) CheckMenuItem(m, IDM_TV_ASPECT, MF_BYCOMMAND | (g_aspect == 0 ? MF_CHECKED : MF_UNCHECKED));
            a7800session_set_int(g_session, "aspect", g_aspect);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        case IDM_SMOOTH: {
            HMENU m = GetMenu(hwnd);
            g_smooth = !g_smooth;
            if (m) CheckMenuItem(m, IDM_SMOOTH, MF_BYCOMMAND | (g_smooth ? MF_CHECKED : MF_UNCHECKED));
            a7800session_set_int(g_session, "smooth", g_smooth);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        case IDM_SETTINGS: show_settings((HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE)); return 0;
        case IDM_FUJINET_LOG: show_fujinet_log((HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE)); return 0;
        case IDM_FUJINET_CONFIG:
            if (!a7800session_fujinet_running(g_session)) {
                MessageBoxA(hwnd, "FujiNet is not running.", "FujiNet", MB_ICONINFORMATION | MB_OK);
                return 0;
            }
            ShellExecuteA(hwnd, "open", a7800session_fujinet_webui_url(g_session), NULL, NULL, SW_SHOWNORMAL);
            return 0;
        case IDM_ABOUT:
            MessageBoxA(hwnd,
                APP_TITLE " " A7800_VERSION_STRING "\n\n"
                "An Atari 7800 with a built-in FujiNet. No BIOS is needed.\n"
                "The emulator is MAME's Atari 7800 (GPL-2.0-or-later; the a7800\n"
                "driver BSD-3-Clause) by the MAME team, with the FujiNet cartridge.\n\n"
                "Copyright (C) 2026 Thomas Cherryhomes -- GPL-3.0-or-later\n"
                "https://fujinet.online/",
                "About " APP_TITLE, MB_ICONINFORMATION | MB_OK);
            return 0;
        case IDM_EXIT: PostMessage(hwnd, WM_CLOSE, 0, 0); return 0;
        default: break;
        }
        break;

    case WM_DROPFILES: {
        char path[MAX_PATH];
        HDROP drop = (HDROP)wp;
        if (DragQueryFileA(drop, 0, path, sizeof path)) load_media(path);
        DragFinish(drop);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, IDT_STATUS);
        KillTimer(hwnd, IDT_SYSACT);
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static int env_on(const char *name)
{
    const char *env = getenv(name);
    return env && *env && *env != '0';
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    WNDCLASSEX wc;
    MSG msg;
    a7800session_start_opts opts;
    RECT want = { 0, 0, 224 * 3 * 4 / 3, 224 * 3 };
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES };
    char cart[MAX_PATH];
    (void)prev;

    InitCommonControlsEx(&icc);
    InitializeCriticalSection(&g_fb_lock);

    g_session = a7800session_new(NULL);
    if (!g_session) {
        MessageBoxA(NULL, "Could not create the session (unusable config or data directories?)",
                    APP_TITLE, MB_ICONERROR | MB_OK);
        return 1;
    }
    g_fb = calloc((size_t)A7800SESSION_FB_WIDTH * A7800SESSION_FB_MAX_HEIGHT, sizeof *g_fb);
    if (!g_fb) return 1;

    memset(&g_bmi, 0, sizeof g_bmi);
    g_bmi.bmiHeader.biSize = sizeof g_bmi.bmiHeader;
    g_bmi.bmiHeader.biWidth = A7800SESSION_FB_WIDTH;
    g_bmi.bmiHeader.biHeight = -A7800SESSION_FB_MAX_HEIGHT;
    g_bmi.bmiHeader.biPlanes = 1;
    g_bmi.bmiHeader.biBitCount = 32;
    g_bmi.bmiHeader.biCompression = BI_RGB;

    g_aspect = a7800session_get_int(g_session, "aspect", 0);
    g_smooth = a7800session_get_int(g_session, "smooth", 0);

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WIN_CLASS;
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(IDI_APPICON));
    wc.hIconSm = wc.hIcon;
    RegisterClassEx(&wc);

    /* Three times an NTSC picture's height, at 4:3, plus chrome. */
    AdjustWindowRectEx(&want, WS_OVERLAPPEDWINDOW, TRUE, WS_EX_ACCEPTFILES);
    g_hwnd = CreateWindowEx(WS_EX_ACCEPTFILES, WIN_CLASS, APP_TITLE, WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left,
                            want.bottom - want.top + 24, NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;
    build_menu(g_hwnd);
    g_statusbar = CreateWindowExA(0, STATUSCLASSNAMEA, "", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                  0, 0, 0, 0, g_hwnd, (HMENU)(INT_PTR)IDC_STATUSBAR, inst, NULL);
    layout_statusbar();
    ShowWindow(g_hwnd, show);

    a7800session_default_opts(g_session, &opts);
    if (cmdline && *cmdline) {
        /* a cartridge path on the command line, quotes and all */
        const char *p = cmdline;
        size_t n;
        if (*p == '"') p++;
        snprintf(cart, sizeof cart, "%s", p);
        n = strlen(cart);
        while (n && (cart[n - 1] == '"' || cart[n - 1] == ' ')) cart[--n] = '\0';
        if (cart[0]) opts.cart_path = cart;
    }
    if (a7800session_start(g_session, &opts) != 0)
        MessageBoxA(g_hwnd, a7800session_last_error(g_session), APP_TITLE, MB_ICONWARNING | MB_OK);

    g_pad_generation = a7800session_gamepad_generation(g_session);
    SetTimer(g_hwnd, IDT_STATUS, 1000, NULL);
    SetTimer(g_hwnd, IDT_SYSACT, 100, NULL);
    sync_switch_ui(1);
    update_status();

    if (env_on("A7800_OPEN_CONTROLLERS")) a7800_controller_window_toggle(g_hwnd, g_session);
    if (env_on("A7800_OPEN_DEBUGGER")) a7800_debugger_show(g_hwnd, g_session);
    if (env_on("A7800_OPEN_SETTINGS")) show_settings(inst);

    g_present_thread = CreateThread(NULL, 0, present_thread, NULL, 0, NULL);

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        /* Before TranslateMessage, so the sub-windows' keys reach them
         * regardless of which child control has the focus. */
        if (a7800_debugger_pretranslate(&msg)) continue;
        if (a7800_controller_pretranslate(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    InterlockedExchange(&g_running, 0);
    if (g_present_thread) {
        WaitForSingleObject(g_present_thread, 2000);
        CloseHandle(g_present_thread);
    }
    a7800session_stop(g_session);
    a7800session_free(g_session);
    free(g_fb);
    DeleteCriticalSection(&g_fb_lock);
    return (int)msg.wParam;
}
