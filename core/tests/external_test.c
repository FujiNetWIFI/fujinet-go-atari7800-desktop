/*
 * external_test -- "Use external FujiNet": with the switch on, the session
 * starts no runtime of its own and the cartridge dials the given address.
 * A stand-in listener on a spare port plays the FujiNet and must see the
 * connection. SKIPs (77) on Windows (no BSD sockets here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
int main(void)
{
    printf("SKIP: external_test uses BSD sockets\n");
    return 77;
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "a7800session.h"
#include "test_tmpdir.h"

static int failures;
static void check(int passed, const char *what)
{
    printf("%s: %s\n", passed ? "ok" : "FAIL", what);
    if (!passed) failures++;
}

int main(void)
{
    char cfg[512], data[512], addr[64];
    a7800session_paths paths;
    a7800session *session;
    a7800session_start_opts opts;
    struct sockaddr_in sin;
    socklen_t sinlen = sizeof sin;
    int listener, conn = -1;

    listener = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener < 0 || bind(listener, (struct sockaddr *)&sin, sizeof sin) ||
        listen(listener, 1) || getsockname(listener, (struct sockaddr *)&sin, &sinlen))
        return 1;
    snprintf(addr, sizeof addr, "localhost:%d", ntohs(sin.sin_port));

    test_tmpdir(cfg, sizeof cfg, "xcfg");
    test_tmpdir(data, sizeof data, "xdata");
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    session = a7800session_new(&paths);
    if (!session) return 1;

    a7800session_default_opts(session, &opts);
    opts.enable_audio = 0;
    opts.enable_gamepad = 0;
    opts.enable_fujinet = 1;
    opts.external_fujinet = 1;
    opts.fujinet_addr = "not an address";
    check(a7800session_start(session, &opts) != 0, "a bad address refuses to start");
    check(!a7800session_is_running(session), "...and nothing runs");

    opts.fujinet_addr = addr;
    check(a7800session_start(session, &opts) == 0, "starts with the external address");
    check(a7800session_fujinet_external(session) == 1, "the session reports external");
    check(a7800session_fujinet_running(session) == 0, "no built-in runtime was started");
    check(strcmp(a7800session_boip_endpoint(session), addr) == 0, "the endpoint is the address");
    check(a7800session_fujinet_webui_url(session)[0] == '\0', "no web UI url");

    {
        struct pollfd pfd = { listener, POLLIN, 0 };
        if (poll(&pfd, 1, 30000) > 0)
            conn = accept(listener, NULL, NULL);
    }
    check(conn >= 0, "the cartridge dialled the stand-in FujiNet");
    if (conn >= 0) close(conn);

    a7800session_stop(session);
    check(a7800session_fujinet_external(session) == 0, "stopping clears external");
    a7800session_free(session);
    close(listener);
    return failures ? 1 : 0;
}
#endif
