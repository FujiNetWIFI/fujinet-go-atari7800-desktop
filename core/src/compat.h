/*
 * Small cross-platform helpers shared by fujinet_runtime.c.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef A7800_COMPAT_H
#define A7800_COMPAT_H

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE /* setenv() on glibc under a strict -std=c11 */
#endif

#include <stdlib.h>
#include <time.h>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <unistd.h>
#endif

static inline int a7800_setenv(const char *name, const char *value)
{
#if defined(_WIN32)
    return _putenv_s(name, value);
#else
    return setenv(name, value, 1);
#endif
}

static inline void a7800_sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static inline void a7800_closesocket(int fd)
{
#if defined(_WIN32)
    closesocket((SOCKET)fd);
#else
    close(fd);
#endif
}

#endif /* A7800_COMPAT_H */
