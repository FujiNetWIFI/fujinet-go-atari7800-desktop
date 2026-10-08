/*
 * fujinet_runtime.c -- dlopen libfujinet and drive the fujinet_desktop_*
 * entry points (the desktop build of fujinet-pc-rs232 plus the in-process
 * entry wrapper, tools/fujinet/support/fujinet_desktop_entry.cpp). Transposed
 * from the Intv port's fujinet_runtime.c.
 *
 * FujiNet listens on the BoIP port (11500) and the emulator's cart device
 * dials into it; the web admin UI binds 11501 (via FUJINET_WEBUI_BIND).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "compat.h"
#include "dynlib.h"
#include "session_internal.h"

typedef int (*start_runtime_fn)(const char *root, const char *config,
                                const char *sd, const char *data,
                                int listen_port);
typedef void (*stop_runtime_fn)(void);
typedef const char *(*last_error_fn)(void);
typedef int (*copy_log_fn)(char *out, int max_bytes);

/* One runtime per process: the library owns background threads (web admin,
 * network listeners) inside its mapping, so it is loaded once and NEVER
 * dlclose'd -- unmapping it while such a thread runs executes freed code.
 * A stopped runtime is restarted through the same handle. */
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static a7800_dynlib g_handle;
static start_runtime_fn g_start;
static stop_runtime_fn g_stop;
static last_error_fn g_last_error;
static copy_log_fn g_copy_log;

static int load_library_locked(struct a7800session *s)
{
    char errbuf[256];
    if (g_handle) return 0;

    g_handle = a7800_dynlib_open(s->fujinet_lib);
    if (!g_handle) {
        session_set_error(s, "FujiNet library load failed: %s",
                          a7800_dynlib_error(errbuf, sizeof errbuf));
        return -1;
    }
    g_start = (start_runtime_fn)a7800_dynlib_sym(g_handle, "fujinet_desktop_start_runtime");
    g_stop = (stop_runtime_fn)a7800_dynlib_sym(g_handle, "fujinet_desktop_stop_runtime");
    g_last_error = (last_error_fn)a7800_dynlib_sym(g_handle, "fujinet_desktop_last_error_message");
    g_copy_log = (copy_log_fn)a7800_dynlib_sym(g_handle, "fujinet_desktop_copy_recent_log");

    if (!g_start || !g_stop || !g_last_error) {
        session_set_error(s, "%s is missing the desktop runtime contract",
                          s->fujinet_lib);
        g_start = NULL;
        return -1;
    }
    return 0;
}

int fujinet_start(struct a7800session *s)
{
    pthread_mutex_lock(&g_mtx);
    if (s->fujinet_running) {
        pthread_mutex_unlock(&g_mtx);
        return 0;
    }
    if (paths_provision_fujinet(s) != 0) {
        pthread_mutex_unlock(&g_mtx);
        fprintf(stderr, "a7800session: FujiNet runtime unavailable; "
                        "continuing without it\n");
        return -1;
    }
    if (load_library_locked(s) != 0) {
        pthread_mutex_unlock(&g_mtx);
        return -1;
    }
    /* The web admin UI binds here (the entry wrapper reads this env). */
    {
        char bind[64];
        snprintf(bind, sizeof bind, "127.0.0.1:%d", A7800SESSION_WEBUI_PORT);
        a7800_setenv("FUJINET_WEBUI_BIND", bind);
    }
    /* fnFsSPIFFS's flash base and mgHttpClient's CA bundle root from this
     * rather than the CWD; leaving it unset fails quietly (the CA store loads
     * empty and https fetches look like a network fault). */
    a7800_setenv("FUJINET_RUNTIME_ROOT", s->fujinet_root);

    if (!g_start(s->fujinet_root, s->fujinet_config, s->fujinet_sd,
                 s->fujinet_data, A7800SESSION_BOIP_PORT)) {
        const char *err = g_last_error ? g_last_error() : NULL;
        session_set_error(s, "FujiNet runtime failed to start: %s",
                          err && *err ? err : "(unknown)");
        pthread_mutex_unlock(&g_mtx);
        return -1;
    }
    s->fujinet_running = 1;
    pthread_mutex_unlock(&g_mtx);
    return 0;
}

int fujinet_wait_for_boip(struct a7800session *s, int timeout_ms)
{
    int waited = 0;
    if (!s->fujinet_running)
        return -1;

    for (;;) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof addr);
            addr.sin_family = AF_INET;
            addr.sin_port = htons(A7800SESSION_BOIP_PORT);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            int ok = connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0;
            a7800_closesocket(fd);
            if (ok)
                return 0;
        }
        if (waited >= timeout_ms)
            return -1;
        a7800_sleep_ms(25);
        waited += 25;
    }
}

void fujinet_stop(struct a7800session *s)
{
    stop_runtime_fn stop = NULL;
    pthread_mutex_lock(&g_mtx);
    if (s->fujinet_running) {
        stop = g_stop;
        s->fujinet_running = 0;
    }
    pthread_mutex_unlock(&g_mtx);
    if (stop) stop();
}

int a7800session_fujinet_copy_log(a7800session *s, char *dst, int max)
{
    (void)s;
    if (!dst || max <= 0) return 0;
    dst[0] = '\0';
    pthread_mutex_lock(&g_mtx);
    copy_log_fn fn = g_copy_log;
    pthread_mutex_unlock(&g_mtx);
    return fn ? fn(dst, max) : 0;
}
