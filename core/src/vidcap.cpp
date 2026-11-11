/*
 * vidcap.cpp -- the C face of the host's Video tab capture, for
 * core/src/vidtab.c (session_internal.h).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "MameHost.h"
#include "a7800vidcap.h"
#include "session_internal.h"

MameHost* a7800session_host(a7800session* s);

namespace {

MameHost* host_of_running(a7800session* s)
{
	if(!s || !s->running) {
		return nullptr;
	}
	MameHost* host = a7800session_host(s);
	return host && host->IsRunning() ? host : nullptr;
}

}

extern "C" int session_capture_running(a7800session* s)
{
	return host_of_running(s) != nullptr;
}

extern "C" void session_capture_enable(a7800session* s, int on)
{
	if(s && s->host) {
		a7800session_host(s)->CaptureEnable(on != 0);
	}
}

extern "C" int session_capture_get(a7800session* s, a7800vid_capture* dst, uint64_t* serial)
{
	MameHost* host = host_of_running(s);
	return host && host->CaptureGet(dst, serial) ? 1 : 0;
}

extern "C" int session_capture_fill(a7800session* s, a7800vid_capture* dst)
{
	MameHost* host = host_of_running(s);
	return host && host->CaptureFill(dst) ? 1 : 0;
}
