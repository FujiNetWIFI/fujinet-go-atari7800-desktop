/*
 * fujitrace_test -- the debugger's FujiNet trace decoder (core/src/
 * fujitrace.h), with made-up bytes: SLIP frames both ways, whole and in
 * pieces, escapes, short and long packets, repeats folded into one entry,
 * the ring's limit, and an empty trace. No MAME, no session, no network.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "fujitrace.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static a7800debug_fuji_packet pk[A7800DEBUG_FUJI_TRACE_MAX];

/* A FujiBus packet (6-byte header, then the payload), SLIP-framed into
 * out. Returns the frame's length. */
static size_t frame(uint8_t *out, uint8_t device, uint8_t command, const uint8_t *payload, size_t n)
{
    uint8_t pkt[512];
    size_t len = 6 + n, i, w = 0;
    pkt[0] = device; pkt[1] = command;
    pkt[2] = (uint8_t)(len & 0xFF); pkt[3] = (uint8_t)(len >> 8);
    pkt[4] = 0; pkt[5] = 0;
    if (n) memcpy(pkt + 6, payload, n);
    out[w++] = 0xC0;
    for (i = 0; i < len; i++) {
        if (pkt[i] == 0xC0) { out[w++] = 0xDB; out[w++] = 0xDC; }
        else if (pkt[i] == 0xDB) { out[w++] = 0xDB; out[w++] = 0xDD; }
        else out[w++] = pkt[i];
    }
    out[w++] = 0xC0;
    return w;
}

int main(void)
{
    fujitrace *t = fujitrace_new();
    uint8_t req[1100], rep[1100], other[1100];
    size_t nreq, nrep, nother, i;
    static const uint8_t answer[] = { 0x11, 0xC0, 0x22, 0xDB, 0x33 };
    int n;
    uint64_t last;

    /* empty */
    check(fujitrace_get(t, pk, A7800DEBUG_FUJI_TRACE_MAX) == 0, "a new trace is empty");

    /* a request, whole */
    nreq = frame(req, 0x70, 0xE8, NULL, 0);
    fujitrace_feed(t, 1, req, nreq);
    n = fujitrace_get(t, pk, A7800DEBUG_FUJI_TRACE_MAX);
    check(n == 1, "one request, one packet");
    check(pk[0].to_host == 0 && pk[0].device == 0x70 && pk[0].command == 0xE8 && pk[0].length == 6
          && pk[0].nbytes == 6 && pk[0].repeat == 1, "the request's header");
    check(strcmp(pk[0].text, "7800 \xe2\x86\x92 FujiNet 70 E8 GET ADAPTERCONFIG (6 bytes)") == 0,
          "the request is named");
    printf("     %s\n", pk[0].text);

    /* the reply, a byte at a time, with escapes */
    nrep = frame(rep, 0x70, 0x01, answer, sizeof answer);
    for (i = 0; i < nrep; i++) fujitrace_feed(t, 0, rep + i, 1);
    n = fujitrace_get(t, pk, A7800DEBUG_FUJI_TRACE_MAX);
    check(n == 2, "a reply in pieces is one packet");
    check(pk[1].to_host == 1 && pk[1].nbytes == 11 && pk[1].length == 11, "the reply's header");
    check(memcmp(pk[1].data + 6, answer, sizeof answer) == 0, "SLIP escapes are undone");
    check(strcmp(pk[1].text, "FujiNet \xe2\x86\x92 7800 70 01 (11 bytes)") == 0, "the reply's line");
    check(pk[1].serial > pk[0].serial, "serials rise");
    printf("     %s\n", pk[1].text);

    /* the same exchange three more times: one entry each, x4 */
    last = pk[1].serial;
    for (i = 0; i < 3; i++) {
        fujitrace_feed(t, 1, req, nreq);
        fujitrace_feed(t, 0, rep, nrep / 2);
        fujitrace_feed(t, 0, rep + nrep / 2, nrep - nrep / 2);
    }
    n = fujitrace_get(t, pk, A7800DEBUG_FUJI_TRACE_MAX);
    check(n == 2, "repeated exchanges fold into the first");
    check(pk[0].repeat == 4 && pk[1].repeat == 4, "both halves count four");
    check(strstr(pk[0].text, " \xc3\x97" "4") != NULL && strstr(pk[1].text, " \xc3\x97" "4") != NULL,
          "the lines say x4");
    check(pk[1].serial > last, "a fold moves the last serial");
    printf("     %s\n     %s\n", pk[0].text, pk[1].text);

    /* a request without its reply yet shows; a different exchange starts anew */
    fujitrace_feed(t, 1, req, nreq);
    check(fujitrace_get(t, pk, A7800DEBUG_FUJI_TRACE_MAX) == 3, "a request waiting for its reply shows");
    nother = frame(other, 0x70, 0x02, NULL, 0);
    fujitrace_feed(t, 0, other, nother);
    n = fujitrace_get(t, pk, A7800DEBUG_FUJI_TRACE_MAX);
    check(n == 4 && pk[2].repeat == 1 && pk[3].repeat == 1, "a different reply is not folded");

    /* a packet that repeats on its own */
    fujitrace_feed(t, 0, other, nother);
    fujitrace_feed(t, 0, other, nother);
    n = fujitrace_get(t, pk, A7800DEBUG_FUJI_TRACE_MAX);
    check(n == 4 && pk[3].repeat == 3, "the same packet in a row folds");

    /* the console sending ends a reply that was arriving */
    fujitrace_clear(t);
    check(fujitrace_get(t, pk, A7800DEBUG_FUJI_TRACE_MAX) == 0, "clear empties it");
    fujitrace_feed(t, 0, rep, 5);
    fujitrace_feed(t, 1, req, nreq);
    fujitrace_feed(t, 0, rep, nrep);
    n = fujitrace_get(t, pk, A7800DEBUG_FUJI_TRACE_MAX);
    check(n == 2 && pk[1].to_host == 1 && pk[1].nbytes == 11, "a half reply is dropped when the console sends");

    /* short and long */
    fujitrace_clear(t);
    {
        static const uint8_t junk[] = { 0xC0, 0x41, 0x42, 0x43, 0xC0 };
        uint8_t big[300];
        for (i = 0; i < sizeof big; i++) big[i] = (uint8_t)i;
        fujitrace_feed(t, 0, junk, sizeof junk);
        nother = frame(other, 0x71, 0x52, big, sizeof big);
        fujitrace_feed(t, 0, other, nother);
    }
    n = fujitrace_get(t, pk, A7800DEBUG_FUJI_TRACE_MAX);
    check(n == 2 && pk[0].device == -1 && pk[0].nbytes == 3, "a short packet has no header");
    check(strcmp(pk[0].text, "FujiNet \xe2\x86\x92 7800 3 bytes") == 0, "and says its size");
    check(pk[1].nbytes == 306 && pk[1].length == 306 && pk[1].data[63] == 57, "a long one keeps its first 64 bytes");

    /* the ring keeps the newest */
    fujitrace_clear(t);
    for (i = 0; i < 600; i++) {
        uint8_t b[2] = { (uint8_t)(i & 0xFF), (uint8_t)(i >> 8) };
        nother = frame(other, 0x70, 0x52, b, 2);
        fujitrace_feed(t, (int)(i & 1), other, nother);
    }
    n = fujitrace_get(t, pk, A7800DEBUG_FUJI_TRACE_MAX);
    check(n == A7800DEBUG_FUJI_TRACE_MAX, "the ring holds 512");
    check(pk[0].data[6] == (600 - 512) % 256 && pk[n - 1].data[6] == 599 % 256 && pk[n - 1].data[7] == 2,
          "oldest first, newest last");
    n = fujitrace_get(t, pk, 10);
    check(n == 10 && pk[9].data[6] == 599 % 256, "a smaller ask gets the newest");

    fujitrace_free(t);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
