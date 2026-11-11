/*
 * memtools -- see core/include/a7800memtools.h. Also the trace ring's pure
 * formatting and filters, which the Trace tab shares with its tests.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "a7800memtools.h"
#include "m6502dasm.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- range parsing ------------------------------------------------------------ */

static int hexdigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int eval_addr(a7800debug *d, const char *text, size_t len, long *out)
{
    char buf[128], err[64];

    while (len && isspace((unsigned char)text[0])) { text++; len--; }
    while (len && isspace((unsigned char)text[len - 1])) len--;
    if (!len || len >= sizeof buf) return 0;
    memcpy(buf, text, len);
    buf[len] = '\0';
    /* A bare 1-4 digit token is hex here (memory fields have always taken
     * "C000"), unless it names a label. */
    if (len <= 4) {
        size_t i;
        for (i = 0; i < len && hexdigit(buf[i]); i++) ;
        if (i == len && a7800debug_label_address(d, buf) < 0) {
            *out = strtol(buf, NULL, 16);
            return 1;
        }
    }
    if (a7800debug_eval(d, buf, out, err, sizeof err) != 0)
        return 0;
    *out &= 0xFFFF;
    return 1;
}

/* No operator characters: a number or a label. */
static int plain(const char *t, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++)
        if (strchr("+-*/%&|^<>=!~()[]{}", t[i])) return 0;
    return 1;
}

int a7800sym_parse_range(a7800debug *d, const char *text, uint16_t *lo,
                       uint16_t *hi)
{
    const char *dots, *p;
    size_t len;
    long a, b;

    if (!d || !text) return 0;
    while (isspace((unsigned char)*text)) text++;
    len = strlen(text);
    if (!len) return 0;

    dots = strstr(text, "..");
    if (dots) {
        if (!eval_addr(d, text, (size_t)(dots - text), &a) ||
            !eval_addr(d, dots + 2, strlen(dots + 2), &b))
            return 0;
    } else {
        int split = 0;
        for (p = text + 1; *p; p++) {
            if (*p != '-') continue;
            if (plain(text, (size_t)(p - text)) && plain(p + 1, strlen(p + 1)) &&
                eval_addr(d, text, (size_t)(p - text), &a) &&
                eval_addr(d, p + 1, strlen(p + 1), &b)) {
                split = 1;
                break;
            }
        }
        if (!split) {
            if (!eval_addr(d, text, len, &a)) return 0;
            b = a;
        }
    }
    if (a > b) { long x = a; a = b; b = x; }
    *lo = (uint16_t)a;
    *hi = (uint16_t)b;
    return 1;
}

/* ---- watches ------------------------------------------------------------------ */

static struct {
    pthread_mutex_t mtx;
    char text[A7800WATCH_MAX][96];
    int count;
    unsigned generation;
} g_watch = { .mtx = PTHREAD_MUTEX_INITIALIZER };

int a7800watch_count(void)
{
    int n;
    pthread_mutex_lock(&g_watch.mtx);
    n = g_watch.count;
    pthread_mutex_unlock(&g_watch.mtx);
    return n;
}

int a7800watch_add(const char *text)
{
    int idx = -1;

    if (!text) return -1;
    while (isspace((unsigned char)*text)) text++;
    if (!*text) return -1;
    pthread_mutex_lock(&g_watch.mtx);
    if (g_watch.count < A7800WATCH_MAX) {
        size_t n;
        idx = g_watch.count++;
        snprintf(g_watch.text[idx], sizeof g_watch.text[idx], "%s", text);
        n = strlen(g_watch.text[idx]);
        while (n && isspace((unsigned char)g_watch.text[idx][n - 1]))
            g_watch.text[idx][--n] = '\0';
        g_watch.generation++;
    }
    pthread_mutex_unlock(&g_watch.mtx);
    return idx;
}

void a7800watch_remove(int index)
{
    pthread_mutex_lock(&g_watch.mtx);
    if (index >= 0 && index < g_watch.count) {
        memmove(g_watch.text[index], g_watch.text[index + 1],
                (size_t)(g_watch.count - index - 1) * sizeof g_watch.text[0]);
        g_watch.count--;
        g_watch.generation++;
    }
    pthread_mutex_unlock(&g_watch.mtx);
}

void a7800watch_clear(void)
{
    pthread_mutex_lock(&g_watch.mtx);
    g_watch.count = 0;
    g_watch.generation++;
    pthread_mutex_unlock(&g_watch.mtx);
}

/* Valid until the next change; the windows are single-threaded users. */
const char *a7800watch_text(int index)
{
    const char *t = NULL;
    pthread_mutex_lock(&g_watch.mtx);
    if (index >= 0 && index < g_watch.count) t = g_watch.text[index];
    pthread_mutex_unlock(&g_watch.mtx);
    return t;
}

unsigned a7800watch_generation(void)
{
    unsigned g;
    pthread_mutex_lock(&g_watch.mtx);
    g = g_watch.generation;
    pthread_mutex_unlock(&g_watch.mtx);
    return g;
}

int a7800watch_range(a7800debug *d, int index, uint16_t *lo, uint16_t *hi)
{
    char text[96];
    const char *t = a7800watch_text(index);

    if (!t) return 0;
    snprintf(text, sizeof text, "%s", t);
    return a7800sym_parse_range(d, text, lo, hi);
}

int a7800watch_format(a7800debug *d, int index, char *dst, int dstsz)
{
    char text[96], name[48] = "";
    uint16_t lo, hi;
    const char *t = a7800watch_text(index);
    int n = 0, i;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!t) return 0;
    snprintf(text, sizeof text, "%s", t);
    if (!a7800sym_parse_range(d, text, &lo, &hi))
        return snprintf(dst, (size_t)dstsz, "%-14s ?", text);
    a7800debug_address_label(d, lo, name, sizeof name);
    n = snprintf(dst, (size_t)dstsz, "%-14s $%04X", text, lo);
    if (n >= dstsz) return dstsz - 1;
    if (!a7800debug_is_stopped(d))
        return n + snprintf(dst + n, (size_t)(dstsz - n), "  -");
    if (lo == hi) {
        uint8_t b = 0;
        a7800debug_read(d, lo, &b, 1);
        return n + snprintf(dst + n, (size_t)(dstsz - n), "  $%02X (%u)", b, b);
    }
    n += snprintf(dst + n, (size_t)(dstsz - n), "-$%04X ", hi);
    for (i = 0; i < 8 && lo + i <= hi && n < dstsz - 4; i++) {
        uint8_t b = 0;
        a7800debug_read(d, (uint16_t)(lo + i), &b, 1);
        n += snprintf(dst + n, (size_t)(dstsz - n), " %02X", b);
    }
    if (hi - lo >= 8 && n < dstsz - 4)
        n += snprintf(dst + n, (size_t)(dstsz - n), " ...");
    return n < dstsz ? n : dstsz - 1;
}

/* ---- search ------------------------------------------------------------------- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int a7800search_parse(int kind, const char *text, uint8_t *pat, int *n,
                    uint8_t *mask, int *flags)
{
    int cnt = 0;

    if (!text || !pat || !n || !mask || !flags) return 0;
    *mask = 0xFF;
    *flags = 0;
    if (kind == A7800SEARCH_TEXT) {
        const char *p;
        for (p = text; *p && cnt < A7800SEARCH_MAX; p++)
            pat[cnt++] = (uint8_t)*p;
        *flags = A7800DEBUG_SEARCH_NOCASE;
    } else {
        const char *p = text;
        while (*p) {
            int hi, lo;
            if (isspace((unsigned char)*p) || *p == ',') { p++; continue; }
            if (*p == '$') { p++; continue; }
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; continue; }
            hi = hexval(*p);
            if (hi < 0) return 0;
            lo = hexval(p[1]);
            if (lo < 0) { lo = hi; hi = 0; p += 1; }   /* a lone digit is a byte */
            else p += 2;
            if (cnt >= A7800SEARCH_MAX) break;
            pat[cnt++] = (uint8_t)(hi << 4 | lo);
        }
    }
    *n = cnt;
    return cnt > 0;
}

/* ---- breakpoint description ----------------------------------------------------- */

int a7800bp_describe(a7800debug *d, const a7800debug_breakpoint *b, char *dst,
                     int dstsz)
{
    char type[16], label[48] = "";
    int n;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!b) return 0;
    /* read + write together is one "Access" entry */
    if ((b->type & A7800DEBUG_BP_ACCESS) == A7800DEBUG_BP_ACCESS)
        snprintf(type, sizeof type, "Access");
    else
        snprintf(type, sizeof type, "%s%s%s",
                 (b->type & A7800DEBUG_BP_EXEC) ? "Exec" : "",
                 (b->type & A7800DEBUG_BP_READ) ? "Read" : "",
                 (b->type & A7800DEBUG_BP_WRITE) ? "Write" : "");
    a7800debug_address_label(d, b->start, label, sizeof label);
    if (b->start == b->end)
        n = snprintf(dst, (size_t)dstsz, "%s  $%04X", type, b->start);
    else
        n = snprintf(dst, (size_t)dstsz, "%s  $%04X-$%04X", type, b->start, b->end);
    if (n < dstsz && label[0])
        n += snprintf(dst + n, (size_t)(dstsz - n), " (%s)", label);
    if (n < dstsz && b->condition[0])
        n += snprintf(dst + n, (size_t)(dstsz - n), "  if %s", b->condition);
    return n < dstsz ? n : dstsz - 1;
}

/* ---- memory map labels ---------------------------------------------------------- */

int a7800mem_region_name(const a7800debug_region *map, int count, uint16_t addr,
                       char *dst, int dstsz)
{
    int i;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    for (i = 0; map && i < count; i++)
        if (addr >= map[i].lo && addr <= map[i].hi)
            return snprintf(dst, (size_t)dstsz, "%s", map[i].name);
    return 0;
}

/* ---- the trace ring's text and filters --------------------------------------------- */

int a7800debug_trace_format(const a7800debug_trace *t, char *dst, int dstsz)
{
    a7800dasm_line ins;

    if (!t || !dst || dstsz <= 0) return 0;
    a7800dasm_decode(t->pc, t->bytes, &ins);
    return snprintf(dst, (size_t)dstsz,
                    "%04X  %-14s A=%02X X=%02X Y=%02X P=%02X SP=%02X",
                    t->pc, ins.text, t->a, t->x, t->y, t->p, t->sp);
}

int a7800debug_trace_match(const a7800debug_trace *t, int filter, uint16_t lo,
                           uint16_t hi)
{
    a7800dasm_line ins;

    if (!t) return 0;
    if (filter == A7800DEBUG_TRACE_RANGE) {
        if (lo > hi) { uint16_t swap = lo; lo = hi; hi = swap; }
        return t->pc >= lo && t->pc <= hi;
    }
    if (filter == A7800DEBUG_TRACE_BRANCHES) {
        a7800dasm_decode(t->pc, t->bytes, &ins);
        return (ins.flags & (A7800DASM_JUMP | A7800DASM_CALL | A7800DASM_RET)) != 0;
    }
    return 1;
}
