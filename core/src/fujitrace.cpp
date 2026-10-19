/*
 * fujitrace.cpp -- see fujitrace.h. The decoding is fujinet-go-msdos-
 * desktop's FujiTrace: SLIP per direction, then the FujiBus header's
 * device, command and length, and the command's name on requests.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

extern "C" {
#include "fujitrace.h"
}

#include "fuji_names.h"

namespace {

const char* fuji_command_name(int id)
{
	for(const auto& c : fuji_command_names)
		if(c.id == id)
			return c.name;
	return "";
}

struct Entry
{
	a7800debug_fuji_packet p;
	uint64_t hash;              // FNV-1a over the whole packet
};

uint64_t hash_of(const std::vector<uint8_t>& bytes)
{
	uint64_t h = 1469598103934665603ull;
	for(uint8_t b : bytes) {
		h ^= b;
		h *= 1099511628211ull;
	}
	return h;
}

bool same(const Entry& a, const Entry& b)
{
	return a.hash == b.hash && a.p.to_host == b.p.to_host && a.p.nbytes == b.p.nbytes
	       && memcmp(a.p.data, b.p.data, sizeof a.p.data) == 0;
}

// "7800 → FujiNet 70 E8 GET ADAPTERCONFIG (6 bytes) ×3"
void describe(a7800debug_fuji_packet& p)
{
	const char* way = p.to_host ? "FujiNet \xe2\x86\x92 7800" : "7800 \xe2\x86\x92 FujiNet";
	char times[16] = "";
	if(p.repeat > 1)
		snprintf(times, sizeof times, " \xc3\x97%u", (unsigned)p.repeat);
	if(p.device >= 0) {
		const char* name = p.to_host ? "" : fuji_command_name(p.command);
		snprintf(p.text, sizeof p.text, "%s %02X %02X%s%s (%d bytes)%s", way, p.device, p.command,
		         name[0] ? " " : "", name, p.nbytes, times);
	} else {
		snprintf(p.text, sizeof p.text, "%s %d bytes%s", way, p.nbytes, times);
	}
}

} // namespace

struct fujitrace
{
	std::mutex mutex;
	std::deque<Entry> ring;
	uint64_t serial = 0;
	std::vector<uint8_t> slip[2];   // by to_host
	bool escaped[2] = { false, false };

	void Packet(int to_host);
};

// A whole packet in slip[to_host]: a new entry, or one more of the last.
void fujitrace::Packet(int to_host)
{
	const std::vector<uint8_t>& acc = slip[to_host];
	Entry e {};
	a7800debug_fuji_packet& p = e.p;
	p.to_host = to_host;
	p.repeat = 1;
	p.nbytes = (int)acc.size();
	memcpy(p.data, acc.data(), std::min(acc.size(), sizeof p.data));
	p.device = acc.size() >= 6 ? acc[0] : -1;
	p.command = acc.size() >= 6 ? acc[1] : -1;
	p.length = acc.size() >= 6 ? (acc[2] | (acc[3] << 8)) : -1;
	e.hash = hash_of(acc);

	// The same packet again (a push, a bare send)
	if(!ring.empty() && same(ring.back(), e)) {
		Entry& last = ring.back();
		last.p.repeat++;
		last.p.serial = ++serial;
		describe(last.p);
		return;
	}
	// The same exchange again: Q R Q' and now R'. Q' goes; Q and R count it.
	const size_t n = ring.size();
	if(n >= 3 && same(ring[n - 1], ring[n - 3]) && same(ring[n - 2], e)
	   && ring[n - 1].p.repeat == 1 && ring[n - 3].p.repeat == ring[n - 2].p.repeat) {
		ring.pop_back();
		Entry& q = ring[n - 3];
		Entry& r = ring[n - 2];
		q.p.repeat++;
		r.p.repeat++;
		r.p.serial = ++serial;
		describe(q.p);
		describe(r.p);
		return;
	}
	p.serial = ++serial;
	describe(p);
	ring.push_back(e);
	if(ring.size() > A7800DEBUG_FUJI_TRACE_MAX)
		ring.pop_front();
}

extern "C" fujitrace* fujitrace_new(void)
{
	return new fujitrace();
}

extern "C" void fujitrace_free(fujitrace* t)
{
	delete t;
}

extern "C" void fujitrace_feed(fujitrace* t, int dir, const uint8_t* raw, size_t len)
{
	if(!t || !raw) return;
	std::lock_guard<std::mutex> lock(t->mutex);
	const int to_host = dir == 0 ? 1 : 0;
	// The console sending ends whatever reply was arriving.
	if(!to_host) {
		t->slip[1].clear();
		t->escaped[1] = false;
	}
	std::vector<uint8_t>& acc = t->slip[to_host];
	for(size_t i = 0; i < len; i++) {
		const uint8_t b = raw[i];
		if(b == 0xC0) {
			if(!acc.empty()) {
				t->Packet(to_host);
				acc.clear();
			}
			t->escaped[to_host] = false;
			continue;
		}
		if(t->escaped[to_host]) {
			acc.push_back(b == 0xDC ? 0xC0 : b == 0xDD ? 0xDB : b);
			t->escaped[to_host] = false;
		} else if(b == 0xDB) {
			t->escaped[to_host] = true;
		} else if(acc.size() < 4096) {
			acc.push_back(b);
		}
	}
}

extern "C" int fujitrace_get(fujitrace* t, a7800debug_fuji_packet* out, int max)
{
	if(!t || !out || max <= 0) return 0;
	std::lock_guard<std::mutex> lock(t->mutex);
	int n = 0;
	const size_t start = t->ring.size() > (size_t)max ? t->ring.size() - (size_t)max : 0;
	for(size_t i = start; i < t->ring.size(); i++)
		out[n++] = t->ring[i].p;
	return n;
}

extern "C" void fujitrace_clear(fujitrace* t)
{
	if(!t) return;
	std::lock_guard<std::mutex> lock(t->mutex);
	t->ring.clear();
}
