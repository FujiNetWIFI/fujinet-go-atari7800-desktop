/*
 * The external-FujiNet address field: "host:port", a bare port, empty text
 * for the default, and everything that must be refused.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "a7800session.h"

static int failures;

static void good(const char *text, const char *want_host, int want_port)
{
    char host[64] = "";
    int port = 0;
    int result = a7800session_parse_hostport(text, host, sizeof(host), &port);
    if (result != 0 || strcmp(host, want_host) || port != want_port) {
        fprintf(stderr, "FAIL \"%s\": result %d, got %s:%d, want %s:%d\n", text,
                result, host, port, want_host, want_port);
        failures++;
    }
}

static void bad(const char *text)
{
    char host[64] = "keep";
    int port = 7;
    if (a7800session_parse_hostport(text, host, sizeof(host), &port) == 0 ||
        strcmp(host, "keep") || port != 7) {
        fprintf(stderr, "FAIL \"%s\" should be refused untouched\n", text);
        failures++;
    }
}

int main(void)
{
    good("", "127.0.0.1", 11510);
    good("   ", "127.0.0.1", 11510);
    good(NULL, "127.0.0.1", 11510);
    good("11600", "127.0.0.1", 11600);
    good("192.168.1.20:11510", "192.168.1.20", 11510);
    good("  127.0.0.1:1  ", "127.0.0.1", 1);
    good("localhost:65535", "localhost", 65535);
    good("fujinet.local:11510", "fujinet.local", 11510);
    good("my-fuji_2.example.com:80", "my-fuji_2.example.com", 80);

    bad("0");
    bad("65536");
    bad("-5");
    bad("abc");
    bad("127.0.0.1:");
    bad(":11510");
    bad("127.0.0.1:abc");
    bad("256.1.1.1:11510");
    bad("1.2.3:11510");
    bad("1.2.3.4.5:11510");
    bad("-bad.example:11510");
    bad(".example:11510");
    bad("a..b:11510");
    bad("exa mple:11510");
    bad("::1:11510");
    bad("1.2.3.4:11510x");
    return failures ? 1 : 0;
}
