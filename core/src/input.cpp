/*
 * input.cpp -- the 7800's controllers: each port's stick and two buttons and
 * the console's Select / Reset / Pause, as the frontends' targets, the
 * keyboard's bindings, the gamepads and the Controllers window's pointer.
 *
 * Every way of holding an input is a source of its own (input_merge.h) and the
 * console sees their merge, under the session's input mutex (a leaf lock).
 * So a key release, a window drag letting go or an unplugged pad can only ever
 * release what that source itself held. The merged state is handed on to
 * MameHost (atomics read by the emulation thread; nothing here takes a lock
 * the MAME thread takes).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <cstring>

#include "MameHost.h"

extern "C" {
#include "session_internal.h"
}

namespace {

MameHost* host_of(struct a7800session* s)
{
	return static_cast<MameHost*>(s->host);
}

const unsigned kVert = (1u << A7800_ACT_UP) | (1u << A7800_ACT_DOWN);
const unsigned kHorz = (1u << A7800_ACT_LEFT) | (1u << A7800_ACT_RIGHT);
const unsigned kMomentary = (1u << A7800_SW_SELECT) | (1u << A7800_SW_RESET) | (1u << A7800_SW_PAUSE);

// Every source's bits on a port, opposed pairs cancelled (a joystick's
// switches cannot close both ways). input_mtx held.
unsigned merged_joy(const struct a7800session* s, int port)
{
	unsigned bits = 0;
	for(int src = 0; src < A7800_SRC_COUNT; src++)
		bits |= s->joy_src[port][src];
	if((bits & kVert) == kVert) bits &= ~kVert;
	if((bits & kHorz) == kHorz) bits &= ~kHorz;
	return bits;
}

unsigned merged_switches(const struct a7800session* s)
{
	unsigned bits = 0;
	for(int src = 0; src < A7800_SRC_COUNT; src++)
		bits |= s->sw_src[src];
	return bits & kMomentary;
}

// Tell MameHost what changed. input_mtx held, so the order the sources
// changed in is the order the machine hears it.
void forward_locked(struct a7800session* s)
{
	MameHost* host = host_of(s);
	if(!host) return;
	for(int port = 0; port < A7800_PORTS; port++)
	{
		const unsigned now = merged_joy(s, port);
		const unsigned diff = now ^ s->fwd_joy[port];
		for(int act = 0; act < A7800_ACT_PER_PORT; act++)
			if(diff & (1u << act))
				host->SetAction(port, act, (now >> act) & 1u);
		s->fwd_joy[port] = static_cast<uint8_t>(now);
	}
	const unsigned sw_now = merged_switches(s);
	const unsigned sw_diff = sw_now ^ s->fwd_sw;
	for(int sw = 0; sw < A7800_SW_COUNT; sw++)
		if(sw_diff & (1u << sw))
			host->SetSwitch(sw, (sw_now >> sw) & 1u);
	s->fwd_sw = static_cast<uint8_t>(sw_now);
}

void clear_source_locked(struct a7800session* s, int src)
{
	for(int port = 0; port < A7800_PORTS; port++)
		s->joy_src[port][src] = 0;
	s->sw_src[src] = 0;
	forward_locked(s);
}

} // namespace

extern "C" void input_press(struct a7800session* s, int src, int target, int down)
{
	if(!s || src < 0 || src >= A7800_SRC_COUNT || target < 0 || target >= A7800_TARGET_SYSACT(0))
		return;
	if(target >= A7800_TARGET_SWITCH(A7800_SW_LEFT_DIFF))
	{
		// a latched toggle: a press from any source flips it; the release is nothing
		const int sw = target - 2 * A7800_ACT_PER_PORT;
		if(down) a7800session_switch_set(s, sw, !a7800session_switch_get(s, sw));
		return;
	}
	pthread_mutex_lock(&s->input_mtx);
	if(target < 2 * A7800_ACT_PER_PORT)
	{
		const int port = target / A7800_ACT_PER_PORT;
		const unsigned bit = 1u << (target % A7800_ACT_PER_PORT);
		if(down) s->joy_src[port][src] |= static_cast<uint8_t>(bit);
		else s->joy_src[port][src] &= static_cast<uint8_t>(~bit);
	}
	else
	{
		const unsigned bit = 1u << (target - 2 * A7800_ACT_PER_PORT);
		if(down) s->sw_src[src] |= static_cast<uint8_t>(bit);
		else s->sw_src[src] &= static_cast<uint8_t>(~bit);
	}
	forward_locked(s);
	pthread_mutex_unlock(&s->input_mtx);
}

extern "C" void input_set_port(struct a7800session* s, int src, int port, unsigned mask)
{
	if(!s || src < 0 || src >= A7800_SRC_COUNT || port < 0 || port >= A7800_PORTS)
		return;
	pthread_mutex_lock(&s->input_mtx);
	s->joy_src[port][src] = static_cast<uint8_t>(mask & ((1u << A7800_ACT_PER_PORT) - 1));
	forward_locked(s);
	pthread_mutex_unlock(&s->input_mtx);
}

extern "C" void input_pad_clear(struct a7800session* s, int owner)
{
	if(!s || owner < 0 || owner >= A7800_MAX_PADS) return;
	pthread_mutex_lock(&s->input_mtx);
	clear_source_locked(s, A7800_SRC_PAD(owner));
	clear_source_locked(s, A7800_SRC_STICK(owner));
	pthread_mutex_unlock(&s->input_mtx);
}

// ---- public: the Controllers window's pointer ----------------------------------------

extern "C" void a7800session_press(a7800session* s, int target, int down)
{
	if(target < 0 || target >= A7800_TARGET_COUNT) return;
	if(target < A7800_TARGET_SYSACT(0))
		input_press(s, A7800_SRC_POINTER, target, down);
	else if(down)
		a7800session_sysaction(s, target - A7800_TARGET_SYSACT(0));
}

extern "C" void a7800session_pointer_set(a7800session* s, int port, unsigned mask)
{
	input_set_port(s, A7800_SRC_POINTER, port, mask);
}

// Lets go of the window's pointer: every part and the stick it holds. The
// keyboard and the gamepads are left alone.
extern "C" void a7800session_release_pointer(a7800session* s)
{
	pthread_mutex_lock(&s->input_mtx);
	clear_source_locked(s, A7800_SRC_POINTER);
	pthread_mutex_unlock(&s->input_mtx);
}

// Lets go of the keyboard's holds only.
extern "C" void a7800session_release_keyboard(a7800session* s)
{
	pthread_mutex_lock(&s->input_mtx);
	memset(s->held_keysym, 0, sizeof s->held_keysym);
	clear_source_locked(s, A7800_SRC_KEYBOARD);
	pthread_mutex_unlock(&s->input_mtx);
}

// Focus loss: the keyboard's and the pointer's holds. The gamepads' stay: each
// knows when it lets go.
extern "C" void a7800session_release_all(a7800session* s)
{
	if(!s) return;
	a7800session_release_keyboard(s);
	a7800session_release_pointer(s);
}

extern "C" void a7800session_clear_held(a7800session* s)
{
	a7800session_release_all(s);
}

extern "C" unsigned a7800session_buttons_held(a7800session* s, int port)
{
	if(port < 0 || port >= A7800_PORTS) return 0;
	pthread_mutex_lock(&s->input_mtx);
	const unsigned bits = merged_joy(s, port);
	pthread_mutex_unlock(&s->input_mtx);
	return bits;
}

extern "C" int a7800session_switch_held(a7800session* s, int sw)
{
	if(sw < 0 || sw >= A7800_SW_COUNT) return 0;
	if(sw == A7800_SW_LEFT_DIFF || sw == A7800_SW_RIGHT_DIFF)
		return a7800session_switch_get(s, sw);
	pthread_mutex_lock(&s->input_mtx);
	const unsigned bits = merged_switches(s);
	pthread_mutex_unlock(&s->input_mtx);
	// a menu's pulse is held in the host, not in a source
	return ((bits >> sw) & 1u) || host_of(s)->SwitchHeld(sw);
}

extern "C" int a7800session_target_held(a7800session* s, int target)
{
	if(target >= 0 && target < 2 * A7800_ACT_PER_PORT)
		return (a7800session_buttons_held(s, target / A7800_ACT_PER_PORT)
		        >> (target % A7800_ACT_PER_PORT)) & 1;
	if(target >= A7800_TARGET_SWITCH(0) && target < A7800_TARGET_SYSACT(0))
		return a7800session_switch_held(s, target - A7800_TARGET_SWITCH(0));
	return 0;
}
