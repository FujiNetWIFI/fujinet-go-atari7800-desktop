/*
 * The Win32 Controllers window: both joysticks side by side -- the stick's
 * four directions and the ProLine's two buttons -- each with its port's
 * controller type and the gamepad driving it, then the console's switches
 * (Select, Reset, Pause, both difficulty switches) and Reboot to CONFIG,
 * then the Map row.
 *
 * A button lights in the accent colour whenever the console sees it held,
 * from whatever source (keyboard, gamepad, or a click here), so the window
 * doubles as an input tester.
 *
 * Buttons are driven by WM_LBUTTONDOWN/WM_LBUTTONUP on the window rather
 * than by BN_CLICKED: a joystick button is HELD, and a game polls it, so a
 * value present only for the instant of a click falls between frames. The
 * mouse is captured on press and released on button-up wherever that
 * happens, so dragging off a button cannot strand the machine with a button
 * held forever.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controller_window.h"

/* GET_X_LPARAM / GET_Y_LPARAM live here, not in windows.h. */
#include <windowsx.h>

#include <stdio.h>
#include <string.h>

#include "../key_forward.h"

#define PAD_CLASS "FujiNetGoAtari7800Controllers"

#define BTN      40         /* a stick direction */
#define FACE_W   72         /* Button 1, Button 2 */
#define FACE_H   40
#define GAP       6
#define PAD_W   (2 * MARGIN + 3 * BTN + 2 * GAP + 24 + 2 * FACE_W + GAP)
#define MARGIN   12
#define HEAD_H   22
#define TYPE_H   26
#define PADS_H   20
#define SW_W    120         /* Select, Reset, Pause */
#define WIDE_W  170         /* the difficulty switches, Reboot to CONFIG */

#define IDT_CAPTURE 1
#define IDT_HELD    2

typedef struct {
    RECT rc;
    int target;
    char face[32];
} pad_button;

static HWND g_panel;
static a7800session *g_session;
static pad_button g_btn[A7800_TARGET_COUNT];
static int g_nbtn;
static int g_held = -1;          /* button index under the captured mouse */
static int g_map_state = -2;     /* -2 idle, -1 armed, >=0 awaiting a key/button */
static RECT g_map_rc, g_defaults_rc, g_hint_rc, g_console_rc;
static RECT g_head_rc[2], g_type_rc[2], g_pads_rc[2], g_box_rc[2];
static unsigned g_shown_held[2];
static unsigned g_shown_switches;
static int g_shown_type[2] = { -1, -1 };
static char g_hint[200];
static HBRUSH g_accent_brush;
static HFONT g_bold;
static int g_total_w, g_total_h;

static void add_button(const char *face, int target, int x, int y, int w, int h)
{
    pad_button *b;
    if (g_nbtn >= A7800_TARGET_COUNT) return;
    b = &g_btn[g_nbtn++];
    SetRect(&b->rc, x, y, x + w, y + h);
    b->target = target;
    snprintf(b->face, sizeof b->face, "%s", face);
}

/* One joystick: the stick's directions left as a cross, the two buttons
 * right. Returns the y below it. */
static int build_controller(int port, int x0, int y0)
{
    int y = y0, cy, cx;

    SetRect(&g_head_rc[port], x0, y, x0 + PAD_W, y + HEAD_H);
    y += HEAD_H;
    SetRect(&g_type_rc[port], x0, y, x0 + PAD_W, y + TYPE_H);
    y += TYPE_H;
    SetRect(&g_pads_rc[port], x0, y, x0 + PAD_W, y + PADS_H);
    y += PADS_H + GAP;

    SetRect(&g_box_rc[port], x0, y, x0 + PAD_W, y + 3 * BTN + 2 * GAP + 2 * MARGIN);
    y += MARGIN;
    cx = x0 + MARGIN;
    cy = y;
    add_button("Up", A7800_TARGET_PORT(port, A7800_ACT_UP), cx + BTN + GAP, cy, BTN, BTN);
    add_button("Left", A7800_TARGET_PORT(port, A7800_ACT_LEFT), cx, cy + BTN + GAP, BTN, BTN);
    add_button("Right", A7800_TARGET_PORT(port, A7800_ACT_RIGHT), cx + 2 * (BTN + GAP), cy + BTN + GAP, BTN, BTN);
    add_button("Down", A7800_TARGET_PORT(port, A7800_ACT_DOWN), cx + BTN + GAP, cy + 2 * (BTN + GAP), BTN, BTN);

    /* The ProLine's buttons sit either side of the stick's base; here they
     * are side by side, level with the cross's middle. */
    cx += 3 * BTN + 2 * GAP + 24;
    add_button("Button 1", A7800_TARGET_PORT(port, A7800_ACT_BUTTON1), cx, cy + BTN + GAP, FACE_W, FACE_H);
    add_button("Button 2", A7800_TARGET_PORT(port, A7800_ACT_BUTTON2), cx + FACE_W + GAP, cy + BTN + GAP, FACE_W, FACE_H);

    return g_box_rc[port].bottom;
}

static void layout(void)
{
    const int x1 = MARGIN + PAD_W + 2 * MARGIN;
    int y, cx;

    g_nbtn = 0;
    y = build_controller(0, MARGIN, MARGIN);
    build_controller(1, x1, MARGIN);
    g_total_w = x1 + PAD_W + MARGIN;

    /* The console's switches, centred: the momentary three, then the two
     * difficulty switches and Reboot to CONFIG. */
    y += MARGIN + 4;
    SetRect(&g_console_rc, MARGIN, y, g_total_w - MARGIN, y + HEAD_H);
    y += HEAD_H;
    cx = (g_total_w - (3 * SW_W + 2 * GAP)) / 2;
    add_button("Select", A7800_TARGET_SWITCH(A7800_SW_SELECT), cx, y, SW_W, FACE_H);
    add_button("Reset", A7800_TARGET_SWITCH(A7800_SW_RESET), cx + SW_W + GAP, y, SW_W, FACE_H);
    add_button("Pause", A7800_TARGET_SWITCH(A7800_SW_PAUSE), cx + 2 * (SW_W + GAP), y, SW_W, FACE_H);
    y += FACE_H + GAP;
    cx = (g_total_w - (3 * WIDE_W + 2 * GAP)) / 2;
    add_button("Left Difficulty", A7800_TARGET_SWITCH(A7800_SW_LEFT_DIFF), cx, y, WIDE_W, FACE_H);
    add_button("Right Difficulty", A7800_TARGET_SWITCH(A7800_SW_RIGHT_DIFF), cx + WIDE_W + GAP, y, WIDE_W, FACE_H);
    add_button("Reboot to CONFIG", A7800_TARGET_SYSACT(A7800_SYSACT_REBOOT_CONFIG),
               cx + 2 * (WIDE_W + GAP), y, WIDE_W, FACE_H);
    y += FACE_H + MARGIN;

    SetRect(&g_map_rc, MARGIN, y, MARGIN + 70, y + 30);
    SetRect(&g_defaults_rc, MARGIN + 76, y, MARGIN + 76 + 84, y + 30);
    SetRect(&g_hint_rc, MARGIN + 76 + 84 + 10, y, g_total_w - MARGIN, y + 30);
    y += 30;
    g_total_h = y + MARGIN;
}

static void press_target(int target, int down)
{
    if (target >= A7800_TARGET_SYSACT(0)) {
        /* System actions fire on release, like a real button: pressing and
         * dragging off must not power-cycle the machine. */
        if (!down) a7800session_sysaction(g_session, target - A7800_TARGET_SYSACT(0));
        return;
    }
    /* The difficulty switches toggle on the press, as from a bound key. */
    a7800session_press(g_session, target, down);
}

static int hit(int x, int y)
{
    POINT p = { x, y };
    int i;
    for (i = 0; i < g_nbtn; i++)
        if (PtInRect(&g_btn[i].rc, p)) return i;
    return -1;
}

static void set_map_state(int state)
{
    g_map_state = state;
    if (state == -2) {
        KillTimer(g_panel, IDT_CAPTURE);
        a7800session_gamepad_capture_cancel(g_session);
        g_hint[0] = '\0';
    } else if (state == -1) {
        KillTimer(g_panel, IDT_CAPTURE);
        a7800session_gamepad_capture_cancel(g_session);
        snprintf(g_hint, sizeof g_hint, "Click a button to remap it");
    } else {
        snprintf(g_hint, sizeof g_hint, "Press a key or gamepad button for %s", a7800_target_name(state));
        a7800session_gamepad_capture_begin(g_session);
        SetTimer(g_panel, IDT_CAPTURE, 50, NULL);
    }
    InvalidateRect(g_panel, NULL, TRUE);
}

/* What a port has in it now: AUTO resolved to the running game's choice. */
static int effective_type(int port)
{
    const int t = a7800session_port_type(g_session, port);
    return t == A7800_CTRL_AUTO ? a7800session_port_detected(g_session, port) : t;
}

/* A button's face, which follows what is plugged in: a 2600 joystick's one
 * button is its fire button, a light gun's is its trigger. The difficulty
 * switches show their position. */
static void button_face(const pad_button *b, char *out, int outsz)
{
    const int t = b->target;
    if (t < 2 * A7800_ACT_PER_PORT && t % A7800_ACT_PER_PORT == A7800_ACT_BUTTON1) {
        const int type = effective_type(t / A7800_ACT_PER_PORT);
        snprintf(out, (size_t)outsz, "%s", type == A7800_CTRL_LIGHTGUN ? "Trigger"
                                          : type == A7800_CTRL_JOY2600 ? "Fire" : b->face);
        return;
    }
    if (t == A7800_TARGET_SWITCH(A7800_SW_LEFT_DIFF) || t == A7800_TARGET_SWITCH(A7800_SW_RIGHT_DIFF)) {
        const int sw = t - A7800_TARGET_SWITCH(0);
        snprintf(out, (size_t)outsz, "%s\n%s", b->face,
                 a7800session_switch_get(g_session, sw) ? "A (pro)" : "B (novice)");
        return;
    }
    snprintf(out, (size_t)outsz, "%s", b->face);
}

static void button_label(const pad_button *b, char *out, int outsz)
{
    if (g_map_state != -2) {
        const a7800_binding bind = a7800session_binding_get(g_session, b->target);
        char key[32];
        a7800session_keysym_name(bind.keysym, key, sizeof key);
        if (bind.button != A7800_PAD_BTN_NONE)
            snprintf(out, (size_t)outsz, "%s\n%s", key[0] ? key : "-", a7800_pad_button_name(bind.button));
        else
            snprintf(out, (size_t)outsz, "%s", key[0] ? key : "-");
    } else {
        button_face(b, out, outsz);
    }
}

static void draw_button(HDC dc, const RECT *rc, const char *label, int pushed, int accent)
{
    RECT r = *rc, text;
    int h;

    if (accent) {
        FillRect(dc, &r, g_accent_brush);
        FrameRect(dc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(dc, RGB(255, 255, 255));
    } else {
        DrawFrameControl(dc, &r, DFC_BUTTON, DFCS_BUTTONPUSH | (pushed ? DFCS_PUSHED : 0));
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    }
    /* Vertically centred, word-broken on the newline a Map-mode label
     * carries between its key and its pad button. */
    text = r;
    InflateRect(&text, -2, 0);
    h = DrawTextA(dc, label, -1, &text, DT_CENTER | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    text.left = r.left + 2;
    text.right = r.right - 2;
    text.top = r.top + ((r.bottom - r.top) - h) / 2 + (pushed ? 1 : 0);
    text.bottom = r.bottom;
    DrawTextA(dc, label, -1, &text, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
}

/* The gamepads driving a port, as one line. */
static void pads_line(int port, char *out, int outsz)
{
    int i, n = a7800session_gamepad_count(g_session), len = 0;
    out[0] = '\0';
    for (i = 0; i < n; i++) {
        char name[96];
        if (a7800session_gamepad_effective_port(g_session, i) != port) continue;
        a7800session_gamepad_name(g_session, i, name, sizeof name);
        len += snprintf(out + len, (size_t)(outsz - len), "%s%s", len ? ", " : "Gamepad: ", name);
        if (len >= outsz) break;
    }
    if (!out[0]) snprintf(out, (size_t)outsz, "No gamepad (keyboard only)");
}

/* The switches the console sees held, and the difficulty positions, as
 * bits in switch order. */
static unsigned switch_bits(void)
{
    unsigned bits = 0;
    int sw;
    for (sw = 0; sw < A7800_SW_COUNT; sw++)
        if (a7800session_switch_get(g_session, sw)) bits |= 1u << sw;
    return bits;
}

static void paint_panel(HDC dc)
{
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HGDIOBJ old = SelectObject(dc, font);
    RECT client;
    int i, port;

    GetClientRect(g_panel, &client);
    FillRect(dc, &client, (HBRUSH)(COLOR_BTNFACE + 1));
    SetBkMode(dc, TRANSPARENT);

    for (port = 0; port < 2; port++) {
        char line[200];
        RECT box = g_box_rc[port];
        const int type = a7800session_port_type(g_session, port);
        const int eff = effective_type(port);
        SelectObject(dc, g_bold);
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        DrawTextA(dc, port ? "Player 2" : "Player 1", -1, &g_head_rc[port],
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, font);
        /* The type line is itself a button: click to cycle what is plugged in. */
        if (type == A7800_CTRL_AUTO)
            snprintf(line, sizeof line, "Plugged in: Auto (%s)", a7800_ctrl_type_name(eff));
        else
            snprintf(line, sizeof line, "Plugged in: %s", a7800_ctrl_type_name(type));
        {
            RECT tr = g_type_rc[port];
            InflateRect(&tr, -16, -2);
            DrawFrameControl(dc, &tr, DFC_BUTTON, DFCS_BUTTONPUSH);
            SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
            DrawTextA(dc, line, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        pads_line(port, line, sizeof line);
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        DrawTextA(dc, line, -1, &g_pads_rc[port], DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        /* the joystick's body */
        FillRect(dc, &box, (HBRUSH)GetStockObject(eff == A7800_CTRL_NONE ? LTGRAY_BRUSH : GRAY_BRUSH));
        FrameRect(dc, &box, (HBRUSH)GetStockObject(DKGRAY_BRUSH));
    }

    SelectObject(dc, g_bold);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    DrawTextA(dc, "Console", -1, &g_console_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, font);

    for (i = 0; i < g_nbtn; i++) {
        char label[64];
        const int t = g_btn[i].target;
        int lit = 0;
        const int clicked = (i == g_held) && g_map_state == -2;
        if (t < 2 * A7800_ACT_PER_PORT)
            lit = (g_shown_held[t / A7800_ACT_PER_PORT] >> (t % A7800_ACT_PER_PORT)) & 1;
        else if (t >= A7800_TARGET_SWITCH(A7800_SW_SELECT) && t <= A7800_TARGET_SWITCH(A7800_SW_PAUSE))
            lit = (g_shown_switches >> (t - A7800_TARGET_SWITCH(0))) & 1;
        button_label(&g_btn[i], label, sizeof label);
        draw_button(dc, &g_btn[i].rc, label, clicked,
                    (g_map_state == -2 && (lit || clicked)) || (g_map_state >= 0 && t == g_map_state));
    }

    draw_button(dc, &g_map_rc, g_map_state == -2 ? "Map" : "Done", 0, g_map_state != -2);
    draw_button(dc, &g_defaults_rc, "Defaults", 0, 0);

    if (g_hint[0]) {
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        DrawTextA(dc, g_hint, -1, &g_hint_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(dc, old);
}

static void refresh_held(void)
{
    int port, changed = 0;
    unsigned sw;
    for (port = 0; port < 2; port++) {
        unsigned now = a7800session_buttons_held(g_session, port);
        if (now != g_shown_held[port]) { g_shown_held[port] = now; changed = 1; }
    }
    sw = switch_bits();
    if (sw != g_shown_switches) { g_shown_switches = sw; changed = 1; }
    /* AUTO follows the running game, so a boot can change a port's line */
    for (port = 0; port < 2; port++) {
        const int t = effective_type(port) * A7800_CTRL_COUNT + a7800session_port_type(g_session, port);
        if (t != g_shown_type[port]) { g_shown_type[port] = t; changed = 1; }
    }
    if (changed) InvalidateRect(g_panel, NULL, FALSE);
}

static LRESULT CALLBACK pad_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        /* Off-screen first: the held highlights repaint at 20 Hz. */
        RECT c;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ oldbmp;
        GetClientRect(hwnd, &c);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, c.right, c.bottom);
        oldbmp = SelectObject(mem, bmp);
        paint_panel(mem);
        BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        POINT p = { x, y };
        int i = hit(x, y), port;

        if (PtInRect(&g_map_rc, p)) { set_map_state(g_map_state == -2 ? -1 : -2); return 0; }
        if (PtInRect(&g_defaults_rc, p)) {
            a7800session_bindings_reset(g_session);
            snprintf(g_hint, sizeof g_hint, "Every key and button is back to its default");
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        for (port = 0; port < 2; port++) {
            if (PtInRect(&g_type_rc[port], p)) {
                int t = (a7800session_port_type(g_session, port) + 1) % A7800_CTRL_COUNT;
                a7800session_set_port_type(g_session, port, t);
                InvalidateRect(hwnd, NULL, TRUE);
                return 0;
            }
        }
        if (i < 0) return 0;

        if (g_map_state == -1) { set_map_state(g_btn[i].target); return 0; }
        if (g_map_state >= 0) return 0;

        g_held = i;
        SetCapture(hwnd);
        press_target(g_btn[i].target, 1);
        refresh_held();
        InvalidateRect(hwnd, &g_btn[i].rc, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
        /* Release wherever the mouse ended up: capture means this arrives
         * even if the pointer left the button. */
        if (g_held >= 0) {
            RECT r = g_btn[g_held].rc;
            int t = g_btn[g_held].target;
            g_held = -1;
            ReleaseCapture();
            press_target(t, 0);
            InvalidateRect(hwnd, &r, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        /* Lost the mouse some other way (Alt+Tab mid-press): let go. */
        if (g_held >= 0 && (HWND)lp != hwnd) {
            const int t = g_btn[g_held].target;
            g_held = -1;
            press_target(t, 0);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;

    case WM_TIMER:
        if (wp == IDT_CAPTURE && g_map_state >= 0) {
            int button;
            if (a7800session_gamepad_capture_poll(g_session, &button)) {
                char stolen[128];
                const int target = g_map_state;
                a7800session_binding_set_button(g_session, target, button, stolen, sizeof stolen);
                set_map_state(-1);   /* stay armed: remapping several in a row is normal */
                if (stolen[0])
                    snprintf(g_hint, sizeof g_hint, "%s: %s (taken from %s)", a7800_target_name(target),
                             a7800_pad_button_name(button), stolen);
                InvalidateRect(hwnd, NULL, TRUE);
            }
        } else if (wp == IDT_HELD) {
            refresh_held();
        }
        return 0;

    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        const uint32_t ks = a7800_keysym_from_msg(wp, lp);
        int sa;
        if (lp & (1 << 30)) return 0;   /* auto-repeat */
        if (g_map_state >= 0) {
            if (wp == VK_ESCAPE && !(GetKeyState(VK_SHIFT) & 0x8000)) { set_map_state(-1); return 0; }
            if (ks) {
                char stolen[128], name[32];
                const int target = g_map_state;
                a7800session_binding_set_key(g_session, target, ks, stolen, sizeof stolen);
                a7800session_keysym_name(ks, name, sizeof name);
                set_map_state(-1);   /* stay armed: remapping several in a row is normal */
                if (stolen[0])
                    snprintf(g_hint, sizeof g_hint, "%s: %s (taken from %s)", a7800_target_name(target), name, stolen);
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;
        }
        if (g_map_state == -1) {
            if (wp == VK_ESCAPE) set_map_state(-2);
            return 0;
        }
        if (wp == VK_F9) { ShowWindow(hwnd, SW_HIDE); return 0; }
        if (msg == WM_SYSKEYDOWN) break;   /* Alt+F4 and the system menu */
        if (!ks) break;
        sa = a7800session_key_sysaction(g_session, ks);
        if (sa >= 0) { a7800session_sysaction(g_session, sa); return 0; }
        if (a7800session_key(g_session, ks, 1)) { refresh_held(); return 0; }
        break;
    }
    case WM_KEYUP: case WM_SYSKEYUP: {
        const uint32_t ks = a7800_keysym_from_msg(wp, lp);
        if (g_map_state != -2) return 0;
        if (ks && a7800session_key(g_session, ks, 0)) return 0;
        break;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) a7800session_release_all(g_session);
        return 0;
    case WM_SHOWWINDOW:
        if (wp) { refresh_held(); SetTimer(hwnd, IDT_HELD, 50, NULL); }
        else KillTimer(hwnd, IDT_HELD);
        break;
    case WM_CLOSE:
        /* Hide, do not destroy: the window's position survives closing it. */
        set_map_state(-2);
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void a7800_controller_window_toggle(HWND parent, a7800session *session)
{
    g_session = session;

    if (!g_panel) {
        WNDCLASSEXA wc;
        RECT want;
        LOGFONTA lf;

        layout();
        g_accent_brush = CreateSolidBrush(RGB((A7800SESSION_ACCENT_RGB >> 16) & 0xff,
                                              (A7800SESSION_ACCENT_RGB >> 8) & 0xff,
                                              A7800SESSION_ACCENT_RGB & 0xff));
        GetObjectA(GetStockObject(DEFAULT_GUI_FONT), sizeof lf, &lf);
        lf.lfWeight = FW_BOLD;
        g_bold = CreateFontIndirectA(&lf);

        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = pad_proc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = PAD_CLASS;
        RegisterClassExA(&wc);

        SetRect(&want, 0, 0, g_total_w, g_total_h);
        /* A tool window with a caption and no thick frame or maximize box:
         * it is exactly the size of its controls. WS_EX_TOOLWINDOW also
         * keeps it off the taskbar. */
        AdjustWindowRectEx(&want, WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, FALSE, WS_EX_TOOLWINDOW);
        g_panel = CreateWindowExA(WS_EX_TOOLWINDOW, PAD_CLASS, "Controllers",
                                  WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                  want.right - want.left, want.bottom - want.top,
                                  parent, NULL, wc.hInstance, NULL);
        if (!g_panel) return;
        g_shown_held[0] = g_shown_held[1] = 0;
        g_shown_switches = 0;
    }

    if (IsWindowVisible(g_panel)) {
        set_map_state(-2);
        ShowWindow(g_panel, SW_HIDE);
    } else {
        ShowWindow(g_panel, SW_SHOW);
        SetForegroundWindow(g_panel);
    }
}

void a7800_controller_window_gamepads_changed(void)
{
    if (g_panel && IsWindowVisible(g_panel)) InvalidateRect(g_panel, NULL, FALSE);
}

int a7800_controller_pretranslate(MSG *msg)
{
    /* The window has no child controls, so its own window proc sees every
     * key; nothing to steal here. Kept for symmetry with the debugger. */
    (void)msg;
    return 0;
}
