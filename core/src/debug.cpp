/*
 * debug.cpp -- the debugger contract (core/include/a7800debug.h) over MAME's
 * debugger engine, reached through libmame_fngo's C API on the emulation
 * thread (MameHost::WithMame).
 *
 * MAME does the work: breakpoints, watchpoints, stepping, the expression
 * evaluator, the disassembler and its command console. What lives here is
 * what the family's debugger windows want that MAME does not keep: labels
 * from a symbol file (also fed to MAME's expressions), the stop reason,
 * MARIA's and the I/O chips' registers gathered into structs, and the
 * family's own prompt commands.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "MameHost.h"

extern "C" {
#include "a7800debug.h"
#include "session_internal.h"
}

MameHost* a7800session_host(a7800session* s);

namespace {

struct Label
{
	uint16_t address;
	std::string name;
};

constexpr int WatchpointBase = 1000;

} // namespace

struct a7800debug
{
	a7800session* session = nullptr;
	std::mutex mutex;
	unsigned generation = 1;
	bool attached = false;
	std::string stop_reason;
	int stop_address = -1;
	std::vector<Label> labels;
	uint32_t console_seq = 0;      // the console's lines already handed out
	uint32_t palette[256] = {};
	bool palette_valid = false;
};

namespace {

MameHost* host(a7800debug* d)
{
	return a7800session_host(d->session);
}

// Run fn on the emulation thread; false (fn not run) when nothing runs.
template <typename F>
bool with_mame(a7800debug* d, F&& fn)
{
	if(!d || !d->session->running) {
		return false;
	}
	return host(d)->WithMame([&](fngo_mame* m) { fn(m); });
}

int put(char* dst, int dstsz, const std::string& s)
{
	if(!dst || dstsz <= 0) return 0;
	return snprintf(dst, (size_t)dstsz, "%s", s.c_str());
}

std::string hex(uint32_t v, int digits)
{
	char buf[16];
	snprintf(buf, sizeof buf, "%0*X", digits, v);
	return buf;
}

void bump(a7800debug* d)
{
	std::lock_guard<std::mutex> lock(d->mutex);
	d->generation++;
}

} // namespace

// ---- lifecycle --------------------------------------------------------------

extern "C" a7800debug* a7800debug_get(a7800session* s)
{
	if(!s) return nullptr;
	if(!s->debugger) {
		a7800debug* d = new a7800debug();
		d->session = s;
		s->debugger = d;
	}
	return static_cast<a7800debug*>(s->debugger);
}

void a7800debug_destroy(a7800session* s)
{
	delete static_cast<a7800debug*>(s->debugger);
	s->debugger = nullptr;
}

// The emulation thread: the debugger has just stopped the machine. MAME
// says why in its console ("Stopped at breakpoint 1", "Stopped at
// watchpoint 2 writing ..."); peek at it without taking the lines from the
// prompt.
void a7800debug_note_stop(a7800session* s)
{
	a7800debug* d = static_cast<a7800debug*>(s->debugger);
	if(!d) {
		d = a7800debug_get(s);
	}
	fngo_mame* m = a7800session_host(s)->Mame();
	if(!m) return;

	fngo_mame_cpu cpu;
	fngo_mame_cpu_get(m, &cpu);

	uint32_t seq;
	{
		std::lock_guard<std::mutex> lock(d->mutex);
		seq = d->console_seq;
	}
	std::vector<char> text(8192);
	fngo_mame_console_text(m, &seq, text.data(), (int)text.size());
	std::string reason = "stopped";
	std::istringstream lines(text.data());
	std::string line;
	while(std::getline(lines, line)) {
		if(line.rfind("Stopped at", 0) == 0) {
			reason = line.substr(11);
			if(!reason.empty()) reason[0] = (char)tolower((unsigned char)reason[0]);
		}
	}

	std::lock_guard<std::mutex> lock(d->mutex);
	d->stop_reason = reason;
	d->stop_address = (int)cpu.pc;
	d->generation++;
}

extern "C" void a7800debug_attach(a7800debug* d)
{
	if(!d) return;
	d->attached = true;
	a7800debug_stop(d);
}

extern "C" void a7800debug_detach(a7800debug* d)
{
	if(!d) return;
	d->attached = false;
	a7800debug_resume(d);
}

extern "C" int a7800debug_is_attached(a7800debug* d)
{
	return d && d->attached ? 1 : 0;
}

extern "C" int a7800debug_is_stopped(a7800debug* d)
{
	return d && d->session->running && host(d)->DebugStopped() ? 1 : 0;
}

extern "C" void a7800debug_stop(a7800debug* d)
{
	with_mame(d, [](fngo_mame* m) { fngo_mame_debug_break(m); });
	bump(d);
}

extern "C" void a7800debug_resume(a7800debug* d)
{
	with_mame(d, [](fngo_mame* m) { fngo_mame_debug_go(m); });
	bump(d);
}

extern "C" int a7800debug_stop_reason(a7800debug* d, char* dst, int dstsz, int* address)
{
	if(!d) return 0;
	std::lock_guard<std::mutex> lock(d->mutex);
	if(address) *address = d->stop_address;
	return put(dst, dstsz, d->stop_reason);
}

extern "C" unsigned a7800debug_generation(a7800debug* d)
{
	if(!d) return 0;
	std::lock_guard<std::mutex> lock(d->mutex);
	return d->generation;
}

// ---- stepping -------------------------------------------------------------------

namespace {
void step(a7800debug* d, int kind)
{
	with_mame(d, [kind](fngo_mame* m) { fngo_mame_debug_step(m, kind); });
	std::lock_guard<std::mutex> lock(d->mutex);
	d->stop_reason = "step";
	d->generation++;
}
}

extern "C" void a7800debug_step(a7800debug* d) { if(d) step(d, FNGO_STEP_INTO); }
extern "C" void a7800debug_step_over(a7800debug* d) { if(d) step(d, FNGO_STEP_OVER); }
extern "C" void a7800debug_step_out(a7800debug* d) { if(d) step(d, FNGO_STEP_OUT); }

extern "C" void a7800debug_frame(a7800debug* d)
{
	if(!d) return;
	with_mame(d, [](fngo_mame* m) { fngo_mame_debug_frame(m); });
	bump(d);
}

extern "C" void a7800debug_run_to(a7800debug* d, uint16_t addr)
{
	if(!d) return;
	with_mame(d, [addr](fngo_mame* m) { fngo_mame_debug_run_to(m, addr); });
	bump(d);
}

// ---- CPU ------------------------------------------------------------------------

extern "C" void a7800debug_cpu_get(a7800debug* d, a7800debug_cpu* out)
{
	if(!out) return;
	memset(out, 0, sizeof *out);
	fngo_mame_cpu c {};
	if(!with_mame(d, [&](fngo_mame* m) { fngo_mame_cpu_get(m, &c); })) return;
	out->pc = (int)c.pc;
	out->sp = (int)(c.sp & 0xFF);        // MAME's 6502 reports it as $01xx
	out->a = (int)c.a;
	out->x = (int)c.x;
	out->y = (int)c.y;
	out->ps = (int)c.p;
	out->n = (c.p >> 7) & 1;
	out->v = (c.p >> 6) & 1;
	out->d = (c.p >> 3) & 1;
	out->i = (c.p >> 2) & 1;
	out->z = (c.p >> 1) & 1;
	out->c = c.p & 1;
	out->total_cycles = c.cycles;
	out->scanline = c.beam_y;
	out->dot = c.beam_x;
	out->frame = (uint32_t)c.frame;
}

extern "C" void a7800debug_cpu_set(a7800debug* d, int reg, int value)
{
	with_mame(d, [reg, value](fngo_mame* m) {
		static const int map[] = { FNGO_REG_PC, FNGO_REG_SP, FNGO_REG_A, FNGO_REG_X, FNGO_REG_Y, FNGO_REG_P };
		if(reg >= A7800_REG_PC && reg <= A7800_REG_PS) {
			// the stack lives in page 1, which MAME's SP includes
			const uint32_t v = reg == A7800_REG_SP ? 0x100u | ((uint32_t)value & 0xFFu) : (uint32_t)value;
			fngo_mame_cpu_set(m, map[reg], v);
			return;
		}
		static const int bit[] = { 7, 6, 3, 2, 1, 0 };      // N V D I Z C
		fngo_mame_cpu c;
		fngo_mame_cpu_get(m, &c);
		const int b = bit[reg - A7800_FLAG_N];
		uint32_t p = value ? (c.p | (1u << b)) : (c.p & ~(1u << b));
		fngo_mame_cpu_set(m, FNGO_REG_P, p);
	});
	bump(d);
}

// ---- MARIA ------------------------------------------------------------------------

namespace {

template <typename T>
T item(fngo_mame* m, const char* device, const char* name)
{
	T v {};
	fngo_mame_save_item(m, device, name, &v, (int)sizeof v);
	return v;
}

void load_palette(a7800debug* d, fngo_mame* m)
{
	if(!d->palette_valid) {
		fngo_mame_palette(m, d->palette, 256);
		d->palette_valid = true;
	}
}

} // namespace

extern "C" void a7800debug_maria_get(a7800debug* d, a7800debug_maria* out)
{
	if(!out) return;
	memset(out, 0, sizeof *out);
	with_mame(d, [&](fngo_mame* m) {
		const char* maria = ":maria";
		out->dma_on = item<uint8_t>(m, maria, "m_dmaon") ? 1 : 0;
		out->color_kill = item<uint8_t>(m, maria, "m_color_kill") ? 1 : 0;
		out->kangaroo = item<uint8_t>(m, maria, "m_kangaroo") ? 1 : 0;
		out->border_control = item<uint8_t>(m, maria, "m_bcntl") ? 1 : 0;
		out->char_width = item<uint8_t>(m, maria, "m_cwidth") ? 2 : 1;
		out->read_mode = item<uint8_t>(m, maria, "m_rm") & 3;
		out->charbase = (uint8_t)(item<uint32_t>(m, maria, "m_charbase") >> 8);
		out->dpp = item<uint16_t>(m, maria, "m_dpp");
		out->dll = (uint16_t)item<uint32_t>(m, maria, "m_dll");
		out->dl = (uint16_t)item<uint32_t>(m, maria, "m_dl");
		out->offset = item<int32_t>(m, maria, "m_offset");
		out->holey = item<uint8_t>(m, maria, "m_holey");
		out->dli = item<uint8_t>(m, maria, "m_dli") ? 1 : 0;
		out->vblank = item<uint8_t>(m, maria, "m_vblank") ? 1 : 0;
		fngo_mame_save_item(m, maria, "m_maria_palette", out->palette, (int)sizeof out->palette);
		out->ctrl = (uint8_t)((out->color_kill << 7) | (out->dma_on ? 0x40 : 0x60)
		                      | ((out->char_width == 2) << 4) | (out->border_control << 3)
		                      | (out->kangaroo << 2) | out->read_mode);
		fngo_mame_cpu c;
		fngo_mame_cpu_get(m, &c);
		out->scanline = c.beam_y;
	});
}

extern "C" int a7800debug_dll_text(a7800debug* d, char* dst, int dstsz)
{
	std::string text;
	with_mame(d, [&](fngo_mame* m) {
		uint16_t dpp = item<uint16_t>(m, ":maria", "m_dpp");
		int lines = 0;
		for(int zone = 0; zone < 64 && lines < 272; zone++) {
			uint8_t e[3];
			fngo_mame_read(m, (uint16_t)(dpp + zone * 3), e, 3);
			const int height = (e[0] & 0x0f) + 1;
			const uint16_t dl = (uint16_t)((e[1] << 8) | e[2]);
			char line[96];
			snprintf(line, sizeof line, "$%04X: %2d line%s, DL $%04X%s%s%s\n",
			         (unsigned)(dpp + zone * 3), height, height == 1 ? " " : "s", dl,
			         (e[0] & 0x80) ? ", DLI" : "", (e[0] & 0x40) ? ", holey 16" : "",
			         (e[0] & 0x20) ? ", holey 8" : "");
			text += line;
			lines += height;
		}
	});
	return put(dst, dstsz, text);
}

extern "C" uint32_t a7800debug_color(a7800debug* d, uint8_t index)
{
	if(!d) return 0;
	{
		std::lock_guard<std::mutex> lock(d->mutex);
		if(d->palette_valid) return d->palette[index];
	}
	with_mame(d, [&](fngo_mame* m) { load_palette(d, m); });
	return d->palette[index];
}

extern "C" int a7800debug_palette_image(a7800debug* d, uint32_t* dst)
{
	if(!dst) return 0;
	a7800debug_maria maria;
	a7800debug_maria_get(d, &maria);
	if(!with_mame(d, [&](fngo_mame* m) { load_palette(d, m); }))
		return 0;
	// 8 palettes across (32 px each), their 4 entries down (16 px each);
	// entry 0 of every palette is BACKGRND.
	for(int y = 0; y < A7800DEBUG_PALETTE_HEIGHT; y++) {
		for(int x = 0; x < A7800DEBUG_PALETTE_WIDTH; x++) {
			const int pal = x / 32, entry = y / 16;
			const uint8_t reg = entry == 0 ? maria.palette[0] : maria.palette[pal * 4 + entry];
			dst[y * A7800DEBUG_PALETTE_WIDTH + x] = d->palette[reg];
		}
	}
	return 1;
}

// ---- TIA, RIOT, POKEY, the controls -------------------------------------------------

extern "C" void a7800debug_io_get(a7800debug* d, a7800debug_io* out)
{
	if(!out) return;
	memset(out, 0, sizeof *out);
	fngo_mame_cart_status st;
	if(fngo_mame_fujinet_status(&st)) {
		out->inptctrl = st.inptctrl;
		out->inpt_locked = st.inpt_locked;
		out->pokey_present = strstr(st.live_kind, "pokey") != nullptr;
	}
	with_mame(d, [&](fngo_mame* m) {
		fngo_mame_save_item(m, ":tia", "AUDC", out->audc, 2);
		fngo_mame_save_item(m, ":tia", "AUDF", out->audf, 2);
		fngo_mame_save_item(m, ":tia", "AUDV", out->audv, 2);
		fngo_mame_read(m, 0x0280, &out->swcha, 1);
		fngo_mame_read(m, 0x0282, &out->swchb, 1);
		fngo_mame_read(m, 0x0008, out->inpt, 6);
		const char* pokey = ":cartslot:fujinet:pokey";
		fngo_mame_save_item(m, pokey, "m_AUDF", out->pokey_audf, 4);
		fngo_mame_save_item(m, pokey, "m_AUDC", out->pokey_audc, 4);
		fngo_mame_save_item(m, pokey, "m_AUDCTL", &out->pokey_audctl, 1);
	});
	for(int p = 0; p < 2; p++) {
		out->held[p] = a7800session_buttons_held(d->session, p);
		const int type = a7800session_port_type(d->session, p);
		out->port_type[p] = type == A7800_CTRL_AUTO ? a7800session_port_detected(d->session, p) : type;
	}
}

// ---- memory ------------------------------------------------------------------------

extern "C" int a7800debug_read(a7800debug* d, uint16_t addr, uint8_t* dst, int n)
{
	int got = 0;
	if(!dst || n <= 0) return 0;
	memset(dst, 0, (size_t)n);
	with_mame(d, [&](fngo_mame* m) { got = fngo_mame_read(m, addr, dst, n); });
	return got;
}

extern "C" void a7800debug_write(a7800debug* d, uint16_t addr, uint8_t value)
{
	with_mame(d, [&](fngo_mame* m) { fngo_mame_write(m, addr, value); });
	bump(d);
}

// ---- labels ------------------------------------------------------------------------

namespace {

std::string label_at(a7800debug* d, uint16_t addr)
{
	std::lock_guard<std::mutex> lock(d->mutex);
	for(const Label& l : d->labels)
		if(l.address == addr) return l.name;
	return std::string();
}

} // namespace

extern "C" int a7800debug_label_address(a7800debug* d, const char* label)
{
	if(!d || !label) return -1;
	std::lock_guard<std::mutex> lock(d->mutex);
	for(const Label& l : d->labels)
		if(l.name == label) return l.address;
	return -1;
}

extern "C" int a7800debug_address_label(a7800debug* d, uint16_t addr, char* dst, int dstsz)
{
	if(!d) return 0;
	return put(dst, dstsz, label_at(d, addr));
}

extern "C" int a7800debug_set_label(a7800debug* d, uint16_t addr, const char* label)
{
	if(!d) return -1;
	{
		std::lock_guard<std::mutex> lock(d->mutex);
		d->labels.erase(std::remove_if(d->labels.begin(), d->labels.end(),
		                               [addr](const Label& l) { return l.address == addr; }),
		                d->labels.end());
		if(label && *label) d->labels.push_back(Label{ addr, label });
	}
	if(label && *label)
		with_mame(d, [&](fngo_mame* m) { fngo_mame_symbol_add(m, label, addr); });
	bump(d);
	return 0;
}

namespace {

// ld65 -Ln / VICE: "al 00C123 .name" (or "al C:C123 .name")
// ca65 .dbg:       sym\tid=..,name="name",...,val=0xC123,...,type=lab
// plain:           name = $C123
int parse_symbols(std::istream& in, std::vector<Label>& out)
{
	int count = 0;
	std::string line;
	while(std::getline(in, line)) {
		while(!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
		if(line.rfind("al ", 0) == 0) {
			std::istringstream ls(line.substr(3));
			std::string addr, name;
			ls >> addr >> name;
			const size_t colon = addr.find(':');
			if(colon != std::string::npos) addr = addr.substr(colon + 1);
			if(!name.empty() && name[0] == '.') name = name.substr(1);
			const unsigned long a = strtoul(addr.c_str(), nullptr, 16);
			if(!name.empty() && a <= 0xFFFF) { out.push_back(Label{ (uint16_t)a, name }); count++; }
		} else if(line.rfind("sym", 0) == 0 && line.find("name=\"") != std::string::npos) {
			const size_t n0 = line.find("name=\"") + 6;
			const size_t n1 = line.find('"', n0);
			const size_t v = line.find("val=0x");
			if(n1 == std::string::npos || v == std::string::npos || line.find("type=lab") == std::string::npos)
				continue;
			const unsigned long a = strtoul(line.c_str() + v + 6, nullptr, 16);
			if(a <= 0xFFFF) { out.push_back(Label{ (uint16_t)a, line.substr(n0, n1 - n0) }); count++; }
		} else {
			const size_t eq = line.find('=');
			const size_t dollar = line.find('$');
			if(eq == std::string::npos || dollar == std::string::npos || dollar < eq) continue;
			std::string name = line.substr(0, eq);
			name.erase(name.find_last_not_of(" \t") + 1);
			name.erase(0, name.find_first_not_of(" \t"));
			const unsigned long a = strtoul(line.c_str() + dollar + 1, nullptr, 16);
			if(!name.empty() && a <= 0xFFFF && name.find(' ') == std::string::npos) {
				out.push_back(Label{ (uint16_t)a, name });
				count++;
			}
		}
	}
	return count;
}

// MAME's expression symbols must be identifiers.
bool symbol_ok(const std::string& name)
{
	if(name.empty() || !(isalpha((unsigned char)name[0]) || name[0] == '_')) return false;
	for(char c : name)
		if(!(isalnum((unsigned char)c) || c == '_')) return false;
	return true;
}

} // namespace

extern "C" int a7800debug_load_symbols(a7800debug* d, const char* path, char* msg, int msgsz)
{
	if(!d) return -1;
	std::vector<std::string> candidates;
	if(path && *path)
		candidates.push_back(path);
	else {
		std::string cart = d->session->cart_path;
		if(cart.empty()) {
			put(msg, msgsz, "No cartridge file is open; choose a symbol file.");
			return -1;
		}
		const size_t dot = cart.find_last_of('.');
		const std::string stem = dot == std::string::npos ? cart : cart.substr(0, dot);
		for(const char* ext : { ".lbl", ".dbg", ".sym", ".labels" })
			candidates.push_back(stem + ext);
	}

	for(const std::string& p : candidates) {
		std::ifstream in(p);
		if(!in) continue;
		std::vector<Label> got;
		const int n = parse_symbols(in, got);
		{
			std::lock_guard<std::mutex> lock(d->mutex);
			d->labels.insert(d->labels.end(), got.begin(), got.end());
		}
		with_mame(d, [&](fngo_mame* m) {
			for(const Label& l : got)
				if(symbol_ok(l.name)) fngo_mame_symbol_add(m, l.name.c_str(), l.address);
		});
		bump(d);
		put(msg, msgsz, std::to_string(n) + " labels loaded from " + p);
		return n;
	}
	put(msg, msgsz, candidates.size() == 1 ? "Cannot read " + candidates[0]
	                                       : std::string("No symbol file next to the cartridge"));
	return -1;
}

// ---- disassembly -----------------------------------------------------------------------

namespace {

std::vector<fngo_mame_bp> breakpoints(fngo_mame* m)
{
	std::vector<fngo_mame_bp> bps(64);
	const int n = fngo_mame_bp_list(m, bps.data(), (int)bps.size());
	bps.resize((size_t)std::min(n, 64));
	return bps;
}

int insn_length(fngo_mame* m, uint32_t addr)
{
	char text[64];
	uint8_t bytes[8];
	const int len = fngo_mame_disassemble(m, addr & 0xFFFF, text, sizeof text, bytes);
	return len > 0 ? len : 1;
}

} // namespace

extern "C" int a7800debug_disassemble(a7800debug* d, uint16_t addr, a7800debug_line* out,
                                      int max, int* pc_line)
{
	int n = 0;
	if(pc_line) *pc_line = -1;
	if(!out || max <= 0) return 0;
	with_mame(d, [&](fngo_mame* m) {
		fngo_mame_cpu c;
		fngo_mame_cpu_get(m, &c);
		const std::vector<fngo_mame_bp> bps = breakpoints(m);
		uint32_t a = addr;
		for(; n < max && a <= 0xFFFF; n++) {
			a7800debug_line& l = out[n];
			memset(&l, 0, sizeof l);
			char text[64];
			uint8_t bytes[8] = {};
			int len = fngo_mame_disassemble(m, a, text, sizeof text, bytes);
			if(len <= 0) { len = 1; snprintf(text, sizeof text, ".byte $%02X", bytes[0]); }
			l.address = (uint16_t)a;
			l.is_pc = a == (c.pc & 0xFFFF);
			l.is_code = 1;
			for(const fngo_mame_bp& bp : bps)
				if(bp.address == a) l.has_breakpoint = 1;
			int off = 0;
			for(int i = 0; i < len && i < 4; i++)
				off += snprintf(l.bytes + off, sizeof l.bytes - (size_t)off, i ? " %02X" : "%02X", bytes[i]);
			snprintf(l.disasm, sizeof l.disasm, "%s", text);
			snprintf(l.label, sizeof l.label, "%s", label_at(d, (uint16_t)a).c_str());
			if(l.is_pc && pc_line) *pc_line = n;
			a += (uint32_t)len;
		}
	});
	return n;
}

extern "C" int a7800debug_row_address(a7800debug* d, uint16_t addr, int rows)
{
	int result = addr;
	with_mame(d, [&](fngo_mame* m) {
		if(rows >= 0) {
			uint32_t a = addr;
			for(int i = 0; i < rows && a < 0xFFFF; i++)
				a += (uint32_t)insn_length(m, a);
			result = (int)std::min<uint32_t>(a, 0xFFFF);
			return;
		}
		// Backwards: disassemble forward from a little before, keeping the
		// boundaries, and take the start that lands exactly on addr.
		const int back = -rows;
		for(int lead = back * 3 + 8; lead >= back; lead--) {
			const int start = (int)addr - lead;
			if(start < 0) continue;
			std::vector<uint32_t> seen;
			uint32_t a = (uint32_t)start;
			while(a < addr) {
				seen.push_back(a);
				a += (uint32_t)insn_length(m, a);
			}
			if(a == addr && (int)seen.size() >= back) {
				result = (int)seen[seen.size() - (size_t)back];
				return;
			}
		}
		result = std::max(0, (int)addr - back);
	});
	return result;
}

// ---- the FujiNet cartridge ---------------------------------------------------------------

extern "C" void a7800debug_cart_get(a7800debug* d, a7800debug_cart* out)
{
	if(!out) return;
	memset(out, 0, sizeof *out);
	(void)d;
	fngo_mame_cart_status st;
	if(!fngo_mame_fujinet_status(&st)) return;
	static const char* const modes[] = { "boot block", "loading", "game", "FujiNet app" };
	out->present = st.present;
	out->link_up = st.link_up;
	out->worker = st.worker;
	out->booted_image = st.booted_image;
	out->mode = st.mode;
	snprintf(out->mode_name, sizeof out->mode_name, "%s", st.mode < 4 ? modes[st.mode] : "?");
	out->handover = st.handover;
	out->load_pct = st.load_pct;
	snprintf(out->kind, sizeof out->kind, "%s", st.live_kind);
	out->live_crc = st.live_crc;
	out->staged = st.staged;
	// the cartridge's mapper kinds (a78map.h), in order
	static const char* const kinds[] = { "a78_rom", "a78_pokey", "a78_sg", "a78_sg_pokey", "a78_sg_ram",
	                                     "a78_sg9", "a78_mram", "a78_abs", "a78_act", "a78_hsc" };
	if(st.staged)
		snprintf(out->staged_kind, sizeof out->staged_kind, "%s", st.staged_kind < 10 ? kinds[st.staged_kind] : "?");
	out->staged_crc = st.staged_crc;
	out->hsc = st.hsc;
	out->tv = st.tv;
	out->ackseq = st.ackseq;
	out->last_error = st.err;
	out->boot_state = st.boot_state;
	out->boot_pct = st.boot_pct;
	out->boot_err = st.boot_err;
	out->queue_depth = st.queue_depth;
	snprintf(out->link_error, sizeof out->link_error, "%s", st.link_error);
}

extern "C" int a7800debug_cart_info(a7800debug* d, char* dst, int dstsz)
{
	a7800debug_cart c;
	a7800debug_cart_get(d, &c);
	if(!c.present) return put(dst, dstsz, "FujiNet cartridge: not running");
	char buf[256];
	snprintf(buf, sizeof buf, "FujiNet cartridge: %s%s%s%s, %s, link %s",
	         c.booted_image ? "game" : c.mode_name,
	         c.kind[0] ? " (" : "", c.kind, c.kind[0] ? (std::string(", CRC ") + hex(c.live_crc, 8) + ")").c_str() : "",
	         c.tv ? "PAL" : "NTSC", c.link_up ? "up" : "down");
	return put(dst, dstsz, buf);
}

// ---- breakpoints and watchpoints -------------------------------------------------------------

extern "C" int a7800debug_breakpoint_check(a7800debug* d, uint16_t addr)
{
	int found = 0;
	with_mame(d, [&](fngo_mame* m) {
		for(const fngo_mame_bp& bp : breakpoints(m))
			if(bp.address == addr) found = 1;
	});
	return found;
}

extern "C" int a7800debug_breakpoint_toggle(a7800debug* d, uint16_t addr)
{
	int now = 0;
	with_mame(d, [&](fngo_mame* m) {
		bool removed = false;
		for(const fngo_mame_bp& bp : breakpoints(m))
			if(bp.address == addr) { fngo_mame_bp_clear(m, bp.index); removed = true; }
		if(!removed) now = fngo_mame_bp_set(m, addr, nullptr) >= 0;
	});
	bump(d);
	return now;
}

extern "C" int a7800debug_breakpoint_add(a7800debug* d, int type, uint16_t start, uint16_t end,
                                         const char* condition)
{
	int id = -1;
	with_mame(d, [&](fngo_mame* m) {
		if(type & A7800DEBUG_BP_EXEC) {
			id = fngo_mame_bp_set(m, start, condition);
			return;
		}
		int wp = 0;
		if(type & A7800DEBUG_BP_READ) wp |= FNGO_WP_READ;
		if(type & A7800DEBUG_BP_WRITE) wp |= FNGO_WP_WRITE;
		const uint32_t length = end >= start ? (uint32_t)(end - start) + 1 : 1;
		const int index = fngo_mame_wp_set(m, wp, start, length, condition);
		id = index >= 0 ? WatchpointBase + index : -1;
	});
	bump(d);
	return id;
}

extern "C" void a7800debug_breakpoint_remove(a7800debug* d, int id)
{
	with_mame(d, [id](fngo_mame* m) {
		if(id >= WatchpointBase) fngo_mame_wp_clear(m, id - WatchpointBase);
		else fngo_mame_bp_clear(m, id);
	});
	bump(d);
}

extern "C" void a7800debug_breakpoint_enable(a7800debug* d, int id, int enabled)
{
	with_mame(d, [id, enabled](fngo_mame* m) {
		if(id >= WatchpointBase) fngo_mame_wp_enable(m, id - WatchpointBase, enabled);
		else fngo_mame_bp_enable(m, id, enabled);
	});
	bump(d);
}

extern "C" int a7800debug_breakpoint_list(a7800debug* d, a7800debug_breakpoint* out, int max)
{
	int n = 0;
	with_mame(d, [&](fngo_mame* m) {
		for(const fngo_mame_bp& bp : breakpoints(m)) {
			if(out && n < max) {
				a7800debug_breakpoint& b = out[n];
				memset(&b, 0, sizeof b);
				b.id = bp.index;
				b.type = A7800DEBUG_BP_EXEC;
				b.start = b.end = (uint16_t)bp.address;
				b.enabled = bp.enabled;
				snprintf(b.condition, sizeof b.condition, "%s", strcmp(bp.condition, "1") ? bp.condition : "");
			}
			n++;
		}
		std::vector<fngo_mame_wp> wps(64);
		const int nw = std::min(fngo_mame_wp_list(m, wps.data(), (int)wps.size()), 64);
		for(int i = 0; i < nw; i++) {
			if(out && n < max) {
				a7800debug_breakpoint& b = out[n];
				memset(&b, 0, sizeof b);
				b.id = WatchpointBase + wps[i].index;
				b.type = ((wps[i].type & FNGO_WP_READ) ? A7800DEBUG_BP_READ : 0)
				         | ((wps[i].type & FNGO_WP_WRITE) ? A7800DEBUG_BP_WRITE : 0);
				b.start = (uint16_t)wps[i].address;
				b.end = (uint16_t)(wps[i].address + (wps[i].length ? wps[i].length - 1 : 0));
				b.enabled = wps[i].enabled;
				snprintf(b.condition, sizeof b.condition, "%s", strcmp(wps[i].condition, "1") ? wps[i].condition : "");
			}
			n++;
		}
	});
	return n;
}

extern "C" void a7800debug_breakpoint_clear(a7800debug* d)
{
	with_mame(d, [](fngo_mame* m) {
		for(const fngo_mame_bp& bp : breakpoints(m))
			fngo_mame_bp_clear(m, bp.index);
		std::vector<fngo_mame_wp> wps(64);
		const int nw = std::min(fngo_mame_wp_list(m, wps.data(), (int)wps.size()), 64);
		for(int i = 0; i < nw; i++)
			fngo_mame_wp_clear(m, wps[i].index);
	});
	bump(d);
}

// ---- files -----------------------------------------------------------------------------

extern "C" int a7800debug_save(a7800debug* d, const char* kind, const char* path,
                               char* msg, int msgsz)
{
	if(!d || !kind || !path) return -1;
	std::ofstream out(path, std::ios::binary);
	if(!out) {
		put(msg, msgsz, std::string("Cannot write ") + path);
		return -1;
	}
	if(!strcmp(kind, "dis")) {
		std::vector<a7800debug_line> lines(256);
		uint32_t a = 0x4000;
		while(a <= 0xFFFF) {
			const int n = a7800debug_disassemble(d, (uint16_t)a, lines.data(), (int)lines.size(), nullptr);
			if(n <= 0) break;
			for(int i = 0; i < n; i++) {
				const a7800debug_line& l = lines[i];
				if(l.label[0]) out << l.label << ":\n";
				char row[160];
				snprintf(row, sizeof row, "%04X  %-12s %s\n", l.address, l.bytes, l.disasm);
				out << row;
			}
			const uint32_t next = lines[n - 1].address + (uint32_t)std::max<size_t>(1, (strlen(lines[n - 1].bytes) + 1) / 3);
			if(next <= a) break;
			a = next;
		}
		put(msg, msgsz, std::string("Disassembly of $4000-$FFFF saved to ") + path);
		return 0;
	}
	uint32_t from = 0, len = 0x10000;
	if(!strcmp(kind, "ram")) { from = 0x1800; len = 0x1000; }
	else if(strcmp(kind, "mem") != 0) {
		put(msg, msgsz, std::string("Nothing to save called ") + kind);
		return -1;
	}
	std::vector<uint8_t> buf(len);
	for(uint32_t off = 0; off < len; off += 0x1000)
		a7800debug_read(d, (uint16_t)(from + off), buf.data() + off, (int)std::min<uint32_t>(0x1000, len - off));
	out.write((const char*)buf.data(), (std::streamsize)buf.size());
	put(msg, msgsz, std::to_string(len) + " bytes saved to " + path);
	return out ? 0 : -1;
}

// ---- the prompt ------------------------------------------------------------------------

namespace {

// MAME's own commands that would restore or replay the machine's past:
// fujinet-pc has already acted on every mailbox transaction in it.
const char* const Refused[] = { "ss", "sl", "statesave", "stateload", "rewind", "rw", "hardreset", "exit", "quit" };

const char* const Commands[] = {
	"help", "cart", "maria", "labels",
	"step", "s", "over", "o", "out", "go", "g", "gvblank", "gv", "gint", "gtime", "gt",
	"next", "n", "focus", "bpset", "bp", "bpclear", "bpc", "bpdisable", "bpd",
	"bpenable", "bpe", "bplist", "bpl", "wpset", "wp", "wpclear", "wpc", "wpdisable",
	"wpenable", "wplist", "wpl", "rpset", "rp", "rpclear", "rplist", "print", "printf",
	"dump", "d", "find", "f", "fill", "dasm", "trace", "tracelog", "history", "comadd",
	"comdelete", "comlist", "symlist", "cpulist", "softreset",
	"a", "x", "y", "p", "sp", "pc", "curpc", "cycles", "frame", "beamx", "beamy",
};

std::string help_text()
{
	return
		"FujiNet Go's own commands:\n"
		"  cart                the FujiNet cartridge's state\n"
		"  maria               MARIA's registers and display list list\n"
		"  labels <file>       load symbols (ld65 -Ln, ca65 .dbg, name = $1234)\n"
		"Everything else is MAME's debugger: \"help\" topics follow.\n";
}

std::string first_word(const std::string& s)
{
	size_t b = s.find_first_not_of(" \t");
	if(b == std::string::npos) return std::string();
	size_t e = s.find_first_of(" \t,", b);
	std::string w = s.substr(b, e == std::string::npos ? std::string::npos : e - b);
	std::transform(w.begin(), w.end(), w.begin(), [](unsigned char c) { return (char)tolower(c); });
	return w;
}

std::string maria_text(a7800debug* d)
{
	a7800debug_maria m;
	a7800debug_maria_get(d, &m);
	static const char* const modes[] = { "160A/160B", "?", "320B/320D", "320A/320C" };
	char buf[512];
	snprintf(buf, sizeof buf,
	         "CTRL $%02X: DMA %s, color %s, %d-byte characters, border %s, kangaroo %s, %s\n"
	         "DPP $%04X  CHARBASE $%02X00  DLL $%04X  DL $%04X  offset %d  holey %d  DLI %d  VBLANK %d  line %d\n"
	         "BACKGRND $%02X\n",
	         m.ctrl, m.dma_on ? "on" : "off", m.color_kill ? "killed" : "on", m.char_width,
	         m.border_control ? "black" : "background", m.kangaroo ? "on" : "off", modes[m.read_mode & 3],
	         m.dpp, m.charbase, m.dll, m.dl, m.offset, m.holey, m.dli, m.vblank, m.scanline, m.palette[0]);
	std::string out = buf;
	for(int p = 0; p < 8; p++) {
		snprintf(buf, sizeof buf, "P%d: $%02X $%02X $%02X\n", p, m.palette[p * 4 + 1], m.palette[p * 4 + 2], m.palette[p * 4 + 3]);
		out += buf;
	}
	std::vector<char> dll(4096);
	a7800debug_dll_text(d, dll.data(), (int)dll.size());
	out += "display list list:\n";
	out += dll.data();
	return out;
}

} // namespace

extern "C" int a7800debug_command(a7800debug* d, const char* command, char* dst, int dstsz)
{
	if(!d || !command) return put(dst, dstsz, "");
	const std::string cmd(command);
	const std::string word = first_word(cmd);
	if(word.empty()) return put(dst, dstsz, "");

	for(const char* r : Refused)
		if(word == r)
			return put(dst, dstsz, "\"" + word + "\" is not available here: it would replay the machine's past, "
			                       "and fujinet-pc has already acted on every transaction in it.\n");

	if(word == "cart") {
		char line[256];
		a7800debug_cart_info(d, line, sizeof line);
		return put(dst, dstsz, std::string(line) + "\n");
	}
	if(word == "maria")
		return put(dst, dstsz, maria_text(d));
	if(word == "labels") {
		std::string file = cmd.substr(cmd.find("labels") + 6);
		file.erase(0, file.find_first_not_of(" \t"));
		char msg[256];
		a7800debug_load_symbols(d, file.empty() ? nullptr : file.c_str(), msg, sizeof msg);
		return put(dst, dstsz, std::string(msg) + "\n");
	}

	std::string out;
	if(word == "help" && cmd.find_first_not_of(" \t", cmd.find("help") + 4) == std::string::npos)
		out = help_text();

	const bool ran = with_mame(d, [&](fngo_mame* m) {
		std::vector<char> text(65536);
		uint32_t seq;
		{
			std::lock_guard<std::mutex> lock(d->mutex);
			seq = d->console_seq;
		}
		// drop what was printed before (stop messages): the prompt shows the
		// command's own output
		fngo_mame_console_text(m, &seq, text.data(), (int)text.size());
		const int rc = fngo_mame_debug_command(m, cmd.c_str());
		fngo_mame_console_text(m, &seq, text.data(), (int)text.size());
		{
			std::lock_guard<std::mutex> lock(d->mutex);
			d->console_seq = seq;
		}
		std::istringstream lines(text.data());
		std::string line;
		while(std::getline(lines, line)) {
			if(!line.empty() && line[0] == '>') continue;      // the echo
			out += line + "\n";
		}
		if(rc > 0 && out.find("rror") == std::string::npos)
			out += std::string(cmd.size() >= (size_t)rc ? (size_t)rc - 1 : 0, ' ') + "^ error here\n";
	});
	if(!ran)
		out += "The machine is not running.\n";
	bump(d);
	return put(dst, dstsz, out);
}

extern "C" int a7800debug_completions(a7800debug* d, const char* prefix, char* dst, int dstsz)
{
	if(!prefix || !dst || dstsz <= 0) return 0;
	dst[0] = '\0';
	const std::string pre(prefix);
	int len = 0, n = 0;
	auto emit = [&](const std::string& s) {
		if(s.compare(0, pre.size(), pre) != 0) return;
		if(len + (int)s.size() + 2 >= dstsz) return;
		len += snprintf(dst + len, (size_t)(dstsz - len), "%s\n", s.c_str());
		n++;
	};
	for(const char* c : Commands) emit(c);
	if(d) {
		std::lock_guard<std::mutex> lock(d->mutex);
		for(const Label& l : d->labels) emit(l.name);
	}
	return n;
}
