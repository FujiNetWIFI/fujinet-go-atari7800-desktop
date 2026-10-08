/*
 * session.cpp -- the frontend contract (core/include/a7800session.h) over the
 * MAME host (core/mame/MameHost.h).
 *
 * C++ because it owns the MameHost; everything it exposes is the plain C
 * API, and the C modules (settings, paths, media, roms, audio, gamepads,
 * bindings) share the struct through session_internal.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "MameHost.h"

extern "C" {
#include "a78info.h"
#include "a7800debug.h"
#include "session_internal.h"
}

// debug.cpp
void a7800debug_note_stop(a7800session* s);
void a7800debug_destroy(a7800session* s);

namespace {

MameHost* host_of(struct a7800session* s)
{
	return static_cast<MameHost*>(s->host);
}

// What MAME's CONTROLLERS setting calls each a7800_ctrl_type.
int mame_controller(int type)
{
	switch(type) {
		case A7800_CTRL_JOY2600: return 1;
		case A7800_CTRL_LIGHTGUN: return 2;
		case A7800_CTRL_NONE: return 3;
		default: return 0;
	}
}

// The opened cartridge: its bytes (kept so a power cycle stages it again)
// and what its header and CRC say.
struct Cart
{
	std::vector<uint8_t> image;
	a78info info {};
};

Cart& cart_of(struct a7800session* s)
{
	// one session per process in practice; keyed by the session anyway
	static std::vector<std::pair<a7800session*, Cart>> carts;
	for(auto& c : carts) {
		if(c.first == s) {
			return c.second;
		}
	}
	carts.emplace_back(s, Cart());
	return carts.back().second;
}

const char* system_for(int region)
{
	return region == A7800_REGION_PAL ? "a7800p" : "a7800";
}

// The console an image wants: its header's TV byte, then whether only the
// PAL BIOS accepts it, else NTSC.
int region_for_image(const a78info& info, const std::vector<uint8_t>& image)
{
	if(info.tv == 1) {
		return A7800_REGION_PAL;
	}
	if(info.tv == 0) {
		return A7800_REGION_NTSC;
	}
	fngo_mame_plan_t plan;
	char why[64];
	if(!image.empty() && fngo_mame_plan(image.data(), (uint32_t)image.size(), nullptr, &plan, why, sizeof why) == 0
	   && plan.biosok == 2) {
		return A7800_REGION_PAL;
	}
	return A7800_REGION_NTSC;
}

// The console to run: the setting, or for AUTO what an opened cartridge
// wants (CONFIG runs on whichever console was last chosen).
int wanted_region(struct a7800session* s)
{
	const int setting = s->opts.region;
	if(setting == A7800_REGION_NTSC || setting == A7800_REGION_PAL) {
		return setting;
	}
	Cart& c = cart_of(s);
	if(s->cart_path[0] && !c.image.empty()) {
		return region_for_image(c.info, c.image);
	}
	return s->running_region ? s->running_region : A7800_REGION_NTSC;
}

// What each port has now: the setting, or for AUTO what the running image
// wants.
void apply_controllers(struct a7800session* s)
{
	if(!s->running) {
		return;
	}
	for(int p = 0; p < 2; p++) {
		const int type = s->opts.port_type[p] == A7800_CTRL_AUTO ? s->detected[p] : s->opts.port_type[p];
		host_of(s)->SetController(p, mame_controller(type));
	}
}

// A new image in the cartridge (the emulation thread tells us): what AUTO
// resolves to for it.
void detect_controllers(struct a7800session* s, uint32_t crc, bool game)
{
	int ctrl[2] = { A7800_CTRL_PROLINE, A7800_CTRL_PROLINE };
	Cart& c = cart_of(s);
	if(game) {
		if(s->cart_path[0] && crc == c.info.crc) {
			ctrl[0] = c.info.ctrl[0];
			ctrl[1] = c.info.ctrl[1];
		} else {
			a78info_known_controllers(crc, ctrl);
		}
	}
	s->detected[0] = ctrl[0];
	s->detected[1] = ctrl[1];
	s->live_crc = crc;
	apply_controllers(s);
}

bool load_image(struct a7800session* s, const char* path, std::vector<uint8_t>& out, a78info& info)
{
	uint8_t* data = nullptr;
	uint32_t size = 0;
	char why[256];
	if(media_read_image(path, &data, &size, why, sizeof why) != 0) {
		session_set_error(s, "%s", why);
		return false;
	}
	out.assign(data, data + size);
	fngo_mame_free(data);
	fngo_mame_plan_t plan;
	if(fngo_mame_plan(out.data(), (uint32_t)out.size(), nullptr, &plan, why, sizeof why) != 0) {
		session_set_error(s, "%s", why);
		out.clear();
		return false;
	}
	a78info_parse(out.data(), (uint32_t)out.size(), &info);
	return true;
}

std::vector<uint8_t> hsc_rom(struct a7800session* s)
{
	std::vector<uint8_t> rom(0x1000);
	if(roms_hsc_load(s, rom.data()) != 0) {
		rom.clear();
	}
	return rom;
}

// A new console (region or BIOS changed): hard reset; if the BIOS cannot be
// loaded, fall back to none.
int restart_machine(struct a7800session* s)
{
	const int region = wanted_region(s);
	const char* bios = roms_bios_name(s, region);
	std::string err;
	if(!host_of(s)->Reconfigure(system_for(region), bios, err)) {
		if(strcmp(bios, "none") != 0) {
			// MAME refused the BIOS (it ended the thread): start again without it
			host_of(s)->Stop();
			MameHost::Config cfg;
			cfg.mameDir = s->mame_dir;
			cfg.romPath = s->roms_dir;
			cfg.system = system_for(region);
			cfg.bios = "none";
			cfg.fujinetPort = A7800SESSION_BOIP_PORT;
			cfg.fujinetDebug = getenv("A7800_FUJINET_DEBUG") != nullptr;
			cfg.volume = a7800session_get_int(s, "volume", 100);
			Cart& c = cart_of(s);
			cfg.bootMode = c.image.empty() ? FNGO_BOOT_NONE : FNGO_BOOT_STAGED;
			cfg.bootImage = c.image;
			cfg.hscRom = hsc_rom(s);
			cfg.hscOn = s->opts.hsc && !cfg.hscRom.empty();
			std::string err2;
			if(!host_of(s)->Start(cfg, err2)) {
				session_set_error(s, "%s", err2.c_str());
				s->running = 0;
				return -1;
			}
			session_set_error(s, "The %s BIOS could not be loaded (%s); the console starts the cartridge directly.",
			                  region == A7800_REGION_PAL ? "PAL" : "NTSC", err.c_str());
		} else {
			session_set_error(s, "%s", err.c_str());
			return -1;
		}
	}
	s->running_region = region;
	host_of(s)->SetDifficulty(0, s->difficulty[0] != 0);
	host_of(s)->SetDifficulty(1, s->difficulty[1] != 0);
	apply_controllers(s);
	return 0;
}

} // namespace

// ---- helpers shared with the C modules -----------------------------------

extern "C" void session_set_error(struct a7800session* s, const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s->last_error, sizeof s->last_error, fmt, ap);
	va_end(ap);
}

extern "C" void session_gamepad_event(struct a7800session* s, const char* text)
{
	pthread_mutex_lock(&s->pad_event_mtx);
	snprintf(s->pad_event, sizeof s->pad_event, "%s", text ? text : "");
	pthread_mutex_unlock(&s->pad_event_mtx);
}

extern "C" int a7800session_gamepad_last_event(a7800session* s, char* dst, int dstsz)
{
	if(!dst || dstsz <= 0) return 0;
	pthread_mutex_lock(&s->pad_event_mtx);
	const int n = snprintf(dst, static_cast<size_t>(dstsz), "%s", s->pad_event);
	pthread_mutex_unlock(&s->pad_event_mtx);
	return n;
}

#ifdef A7800_GAMEPAD_STUB
extern "C" int gamepad_start(struct a7800session* s)
{
	session_set_error(s, "gamepad support not built");
	return -1;
}
extern "C" void gamepad_stop(struct a7800session*) { }
extern "C" int a7800session_gamepad_count(a7800session*) { return 0; }
extern "C" int a7800session_gamepad_name(a7800session*, int, char* dst, int dstsz)
{
	if(dst && dstsz > 0) dst[0] = '\0';
	return 0;
}
extern "C" void a7800session_gamepad_assign(a7800session*, int, int) { }
extern "C" int a7800session_gamepad_assignment(a7800session*, int) { return -1; }
extern "C" int a7800session_gamepad_effective_port(a7800session*, int) { return -1; }
extern "C" unsigned a7800session_gamepad_generation(a7800session*) { return 0; }
extern "C" void a7800session_gamepad_capture_begin(a7800session*) { }
extern "C" void a7800session_gamepad_capture_cancel(a7800session*) { }
extern "C" int a7800session_gamepad_capture_poll(a7800session*, int*) { return 0; }
#endif

// ---- names -----------------------------------------------------------------

extern "C" const char* a7800_region_name(int r)
{
	static const char* const names[A7800_REGION_COUNT + 1] =
		{ "Auto", "NTSC", "PAL", nullptr };
	return (r >= 0 && r < A7800_REGION_COUNT) ? names[r] : nullptr;
}

extern "C" const char* a7800_ctrl_type_name(int t)
{
	static const char* const names[A7800_CTRL_COUNT + 1] =
		{ "Auto", "ProLine Joystick", "2600 Joystick", "XG-1 Light Gun", "None", nullptr };
	return (t >= 0 && t < A7800_CTRL_COUNT) ? names[t] : nullptr;
}

// ---- lifecycle -------------------------------------------------------------

extern "C" a7800session* a7800session_new(const a7800session_paths* paths)
{
	auto* s = static_cast<struct a7800session*>(calloc(1, sizeof(struct a7800session)));
	if(!s) return nullptr;

	pthread_mutex_init(&s->settings_mtx, nullptr);
	pthread_mutex_init(&s->sysact_mtx, nullptr);
	pthread_mutex_init(&s->pad_event_mtx, nullptr);

	if(paths_init(s, paths ? paths->config_dir : nullptr,
	              paths ? paths->data_dir : nullptr) != 0)
	{
		a7800session_free(s);
		return nullptr;
	}
	settings_init(s);
	bindings_init(s);
	roms_provision_embedded(s);

	snprintf(s->webui_url, sizeof s->webui_url, "http://127.0.0.1:%d/",
	         A7800SESSION_WEBUI_PORT);
	if(paths && paths->fujinet_lib)
		snprintf(s->fujinet_lib, sizeof s->fujinet_lib, "%s", paths->fujinet_lib);
	if(paths && paths->fujinet_runtime_src)
		snprintf(s->fujinet_runtime_src, sizeof s->fujinet_runtime_src, "%s",
		         paths->fujinet_runtime_src);

	s->difficulty[0] = a7800session_get_int(s, "left_diff", 1);
	s->difficulty[1] = a7800session_get_int(s, "right_diff", 1);
	s->detected[0] = s->detected[1] = A7800_CTRL_PROLINE;
	s->host = new MameHost();
	return s;
}

extern "C" void a7800session_free(a7800session* s)
{
	if(!s) return;
	a7800session_stop(s);
	a7800session_settings_flush(s);
	settings_free_all(s);
	a7800debug_destroy(s);
	delete host_of(s);
	pthread_mutex_destroy(&s->sysact_mtx);
	pthread_mutex_destroy(&s->pad_event_mtx);
	free(s);
}

extern "C" void a7800session_default_opts(a7800session* s, a7800session_start_opts* opts)
{
	memset(opts, 0, sizeof *opts);
	opts->region = a7800session_get_int(s, "region", A7800_REGION_AUTO);
	opts->port_type[0] = a7800session_get_int(s, "port0_type", A7800_CTRL_AUTO);
	opts->port_type[1] = a7800session_get_int(s, "port1_type", A7800_CTRL_AUTO);
	opts->analog_joystick = a7800session_get_int(s, "analog_joystick", 1);
	opts->hsc = a7800session_get_int(s, "hsc", 0);
	opts->enable_fujinet = a7800session_get_int(s, "enable_fujinet", 1);
	opts->enable_audio = a7800session_get_int(s, "enable_audio", 1);
	opts->enable_gamepad = a7800session_get_int(s, "enable_gamepad", 1);
	opts->cart_path = a7800session_get_str(s, "cart", nullptr);
	if(opts->cart_path && !opts->cart_path[0])
		opts->cart_path = nullptr;
}

extern "C" int a7800session_start(a7800session* s, const a7800session_start_opts* opts)
{
	a7800session_start_opts local;

	if(s->running) return 0;
	s->last_error[0] = '\0';

	if(!opts)
	{
		a7800session_default_opts(s, &local);
		opts = &local;
	}
	s->opts = *opts;
	if(s->opts.region < 0 || s->opts.region >= A7800_REGION_COUNT)
		s->opts.region = A7800_REGION_AUTO;
	for(int p = 0; p < 2; p++)
		if(s->opts.port_type[p] < 0 || s->opts.port_type[p] >= A7800_CTRL_COUNT)
			s->opts.port_type[p] = A7800_CTRL_AUTO;

	// A remembered cartridge that no longer loads (moved, or a board the
	// cartridge cannot map) must not leave the app unbootable: fall back to
	// CONFIG and say why.
	Cart& c = cart_of(s);
	c.image.clear();
	s->cart_path[0] = '\0';
	if(opts->cart_path)
	{
		if(load_image(s, opts->cart_path, c.image, c.info))
			snprintf(s->cart_path, sizeof s->cart_path, "%s", opts->cart_path);
		else
		{
			fprintf(stderr, "a7800: %s: %s; booting CONFIG\n", opts->cart_path, s->last_error);
			a7800session_set_str(s, "cart", "");
		}
	}
	s->opts.cart_path = s->cart_path[0] ? s->cart_path : nullptr;
	s->detected[0] = s->cart_path[0] ? c.info.ctrl[0] : A7800_CTRL_PROLINE;
	s->detected[1] = s->cart_path[0] ? c.info.ctrl[1] : A7800_CTRL_PROLINE;

	// FujiNet FIRST: it listens and the cartridge dials in, so the listener
	// has to exist before the machine's first transaction or the CONFIG
	// client boots reporting no link. Failing to start is NOT fatal (the
	// cartridge also keeps redialling).
	if(opts->enable_fujinet)
	{
		if(fujinet_start(s) == 0)
			fujinet_wait_for_boip(s, 3000);
	}

	const int region = wanted_region(s);
	MameHost::Config cfg;
	cfg.mameDir = s->mame_dir;
	cfg.romPath = s->roms_dir;
	cfg.system = system_for(region);
	cfg.bios = roms_bios_name(s, region);
	cfg.fujinetHost = "127.0.0.1";
	cfg.fujinetPort = A7800SESSION_BOIP_PORT;
	cfg.fujinetDebug = getenv("A7800_FUJINET_DEBUG") != nullptr;
	cfg.volume = a7800session_get_int(s, "volume", 100);
	cfg.startStopped = getenv("A7800_START_STOPPED") != nullptr;
	cfg.bootMode = c.image.empty() ? FNGO_BOOT_NONE : FNGO_BOOT_STAGED;
	cfg.bootImage = c.image;
	cfg.hscRom = hsc_rom(s);
	cfg.hscOn = s->opts.hsc && !cfg.hscRom.empty();

	MameHost::Callbacks cb;
	cb.onStopped = [s] { a7800debug_note_stop(s); };
	cb.onImage = [s](uint32_t crc, bool game) { detect_controllers(s, crc, game); };
	host_of(s)->SetCallbacks(cb);

	std::string err;
	if(!host_of(s)->Start(cfg, err))
	{
		if(cfg.bios != "none")
		{
			// The chosen BIOS will not load: the console runs without one, as
			// it does by default, rather than not at all.
			fprintf(stderr, "a7800: %s; starting without a BIOS\n", err.c_str());
			cfg.bios = "none";
			std::string err2;
			if(!host_of(s)->Start(cfg, err2))
			{
				session_set_error(s, "%s", err2.c_str());
				fujinet_stop(s);
				return -1;
			}
		}
		else
		{
			session_set_error(s, "%s", err.c_str());
			fujinet_stop(s);
			return -1;
		}
	}
	s->running_region = region;
	s->running = 1;

	host_of(s)->SetDifficulty(0, s->difficulty[0] != 0);
	host_of(s)->SetDifficulty(1, s->difficulty[1] != 0);
	apply_controllers(s);

	if(opts->enable_gamepad && gamepad_start(s) != 0)
	{
		fprintf(stderr, "a7800: gamepads unavailable (%s)\n", s->last_error);
		s->last_error[0] = '\0';
	}
	if(opts->enable_audio && audio_start(s) != 0)
	{
		fprintf(stderr, "a7800: audio unavailable (%s); continuing silent\n", s->last_error);
		s->last_error[0] = '\0';
	}
	return 0;
}

extern "C" void a7800session_stop(a7800session* s)
{
	if(!s->running) return;
	audio_stop(s);
	gamepad_stop(s);
	host_of(s)->Stop();
	fujinet_stop(s);
	s->running = 0;
}

extern "C" int a7800session_is_running(const a7800session* s) { return s->running; }
extern "C" const char* a7800session_last_error(const a7800session* s) { return s->last_error; }

// ---- cartridges ------------------------------------------------------------

extern "C" int a7800session_check_cart(const char* path, char* why, int whysz)
{
	char buf[256];
	uint8_t* data = nullptr;
	uint32_t size = 0;
	if(why && whysz > 0) why[0] = '\0';
	if(!path || !*path) return 0;
	if(media_read_image(path, &data, &size, buf, sizeof buf) != 0)
	{
		if(why && whysz > 0) snprintf(why, static_cast<size_t>(whysz), "%s", buf);
		return 0;
	}
	fngo_mame_plan_t plan;
	const int rc = fngo_mame_plan(data, size, nullptr, &plan, buf, sizeof buf);
	fngo_mame_free(data);
	if(rc == 0) return 1;
	if(why && whysz > 0) snprintf(why, static_cast<size_t>(whysz), "%s", buf);
	return 0;
}

extern "C" int a7800session_load_cart(a7800session* s, const char* path)
{
	if(!s->running || !path || !*path) return -1;
	Cart& c = cart_of(s);
	std::vector<uint8_t> image;
	a78info info;
	if(!load_image(s, path, image, info))
		return -1;

	c.image = std::move(image);
	c.info = info;
	snprintf(s->cart_path, sizeof s->cart_path, "%s", path);
	s->opts.cart_path = s->cart_path;
	a7800session_set_str(s, "cart", path);
	s->detected[0] = info.ctrl[0];
	s->detected[1] = info.ctrl[1];

	host_of(s)->SetBoot(FNGO_BOOT_STAGED, c.image, std::string());
	// A PAL cartridge on an NTSC console (or the reverse) with the region on
	// AUTO: another console. Otherwise the power switch.
	if(wanted_region(s) != s->running_region)
		return restart_machine(s);
	host_of(s)->PowerCycle();
	apply_controllers(s);
	return 0;
}

extern "C" const char* a7800session_cart_path(const a7800session* s)
{
	return s->cart_path;
}

extern "C" int a7800session_power_cycle(a7800session* s)
{
	if(!s->running) return -1;
	host_of(s)->PowerCycle();
	return 0;
}

extern "C" int a7800session_reboot_to_config(a7800session* s)
{
	if(!s->running) return -1;
	Cart& c = cart_of(s);
	c.image.clear();
	s->cart_path[0] = '\0';
	s->opts.cart_path = nullptr;
	a7800session_set_str(s, "cart", "");
	host_of(s)->SetBoot(FNGO_BOOT_NONE, std::vector<uint8_t>(), std::string());
	host_of(s)->PowerCycle();
	return 0;
}

extern "C" int a7800session_eject(a7800session* s)
{
	return a7800session_reboot_to_config(s);
}

// ---- video / audio ---------------------------------------------------------

extern "C" int a7800session_copy_frame(a7800session* s, uint32_t* dst, int* height,
                                       uint64_t* serial_inout)
{
	if(!s->host) return 0;
	uint32_t w = 0, h = 0;
	if(!host_of(s)->CopyFrame(dst, static_cast<size_t>(A7800SESSION_FB_WIDTH) * A7800SESSION_FB_MAX_HEIGHT,
	                          w, h, serial_inout))
		return 0;
	if(height) *height = static_cast<int>(std::min<uint32_t>(h, A7800SESSION_FB_MAX_HEIGHT));
	return 1;
}

extern "C" int a7800session_refresh_rate(a7800session* s)
{
	if(!s->host) return 60;
	return host_of(s)->GetFps() < 55.0 ? 50 : 60;
}

extern "C" void a7800session_notify_vsync(a7800session* s, int64_t frame_time_ns)
{
	if(s->host) host_of(s)->NotifyVsync(frame_time_ns);
}

extern "C" int a7800session_render_audio(a7800session* s, float* out, int nframes)
{
	if(nframes <= 0) return 0;
	if(!s->host || !s->running)
	{
		memset(out, 0, sizeof(float) * 2 * static_cast<size_t>(nframes));
		return nframes;
	}
	host_of(s)->FillAudio(out, static_cast<uint32_t>(nframes));
	return nframes;
}

extern "C" void a7800session_set_volume(a7800session* s, int percent)
{
	if(percent < 0) percent = 0;
	if(percent > 100) percent = 100;
	a7800session_set_int(s, "volume", percent);
	if(s->host) host_of(s)->SetVolume(percent);
}

// ---- input -----------------------------------------------------------------

extern "C" void a7800session_press(a7800session* s, int target, int down)
{
	if(target < 0 || target >= A7800_TARGET_COUNT) return;

	if(target < 2 * A7800_ACT_PER_PORT)
	{
		host_of(s)->SetAction(target / A7800_ACT_PER_PORT, target % A7800_ACT_PER_PORT, down != 0);
		return;
	}
	target -= 2 * A7800_ACT_PER_PORT;
	if(target < A7800_SW_COUNT)
	{
		if(target == A7800_SW_LEFT_DIFF || target == A7800_SW_RIGHT_DIFF)
		{
			if(down)
				a7800session_switch_set(s, target, !a7800session_switch_get(s, target));
			return;
		}
		host_of(s)->SetSwitch(target, down != 0);
		return;
	}
	if(down)
		a7800session_sysaction(s, target - A7800_SW_COUNT);
}

extern "C" void a7800session_switch_pulse(a7800session* s, int sw)
{
	if(sw == A7800_SW_LEFT_DIFF || sw == A7800_SW_RIGHT_DIFF)
	{
		a7800session_switch_set(s, sw, !a7800session_switch_get(s, sw));
		return;
	}
	// long enough for a game that reads the switches once a frame or less
	host_of(s)->PulseSwitch(sw, 8);
}

extern "C" int a7800session_switch_get(a7800session* s, int sw)
{
	if(sw == A7800_SW_LEFT_DIFF) return s->difficulty[0];
	if(sw == A7800_SW_RIGHT_DIFF) return s->difficulty[1];
	return host_of(s)->SwitchHeld(sw) ? 1 : 0;
}

extern "C" void a7800session_switch_set(a7800session* s, int sw, int on)
{
	if(sw == A7800_SW_LEFT_DIFF || sw == A7800_SW_RIGHT_DIFF)
	{
		const int which = sw == A7800_SW_RIGHT_DIFF;
		s->difficulty[which] = on ? 1 : 0;
		a7800session_set_int(s, which ? "right_diff" : "left_diff", s->difficulty[which]);
		host_of(s)->SetDifficulty(which, on != 0);
		return;
	}
	if(sw >= 0 && sw < A7800_SW_COUNT)
		host_of(s)->SetSwitch(sw, on != 0);
}

extern "C" void a7800session_sysaction(a7800session* s, int sysact)
{
	switch(sysact)
	{
		case A7800_SYSACT_REBOOT_CONFIG: a7800session_reboot_to_config(s); break;
		case A7800_SYSACT_PAUSE:
			if(s->running)
			{
				a7800debug* d = a7800debug_get(s);
				a7800debug_attach(d);
				a7800debug_stop(d);
			}
			break;
		default: break;
	}
}

extern "C" void a7800session_sysaction_post(a7800session* s, int sysact)
{
	if(sysact < 0 || sysact >= A7800_SYSACT_COUNT) return;
	pthread_mutex_lock(&s->sysact_mtx);
	s->sysact_pending |= 1u << sysact;
	pthread_mutex_unlock(&s->sysact_mtx);
}

extern "C" int a7800session_sysaction_take(a7800session* s, int* out)
{
	int found = 0;
	pthread_mutex_lock(&s->sysact_mtx);
	for(int i = 0; i < A7800_SYSACT_COUNT; i++)
		if(s->sysact_pending & (1u << i))
		{
			s->sysact_pending &= ~(1u << i);
			if(out) *out = i;
			found = 1;
			break;
		}
	pthread_mutex_unlock(&s->sysact_mtx);
	return found;
}

extern "C" unsigned a7800session_buttons_held(a7800session* s, int port)
{
	if(port < 0 || port > 1) return 0;
	return host_of(s)->GetActions(port);
}

extern "C" void a7800session_pointer(a7800session* s, int x, int y, int inside, unsigned buttons)
{
	host_of(s)->SetPointer(x, y, inside != 0, buttons);
}

extern "C" int a7800session_lightgun_active(a7800session* s)
{
	if(!s->running) return 0;
	return host_of(s)->Controller(0) == 2 || host_of(s)->Controller(1) == 2;
}

// The gamepad thread's entry point: the same routing as press, but never
// touching the settings or the debugger from that thread.
extern "C" void session_gamepad_apply(struct a7800session* s, int port, int act, int down)
{
	if(port < 0 || port > 1 || act < 0 || act >= A7800_ACT_PER_PORT) return;
	host_of(s)->SetAction(port, act, down != 0);
}

// ---- controller types / region ----------------------------------------------

extern "C" void a7800session_set_port_type(a7800session* s, int port, int type)
{
	if(port < 0 || port > 1 || type < 0 || type >= A7800_CTRL_COUNT) return;
	s->opts.port_type[port] = type;
	a7800session_set_int(s, port ? "port1_type" : "port0_type", type);
	apply_controllers(s);
}

extern "C" int a7800session_port_type(a7800session* s, int port)
{
	if(port < 0 || port > 1) return A7800_CTRL_AUTO;
	if(s->running) return s->opts.port_type[port];
	return a7800session_get_int(s, port ? "port1_type" : "port0_type", A7800_CTRL_AUTO);
}

extern "C" int a7800session_port_detected(a7800session* s, int port)
{
	if(port < 0 || port > 1) return A7800_CTRL_PROLINE;
	return s->detected[port];
}

extern "C" void a7800session_set_analog(a7800session* s, int joystick)
{
	s->opts.analog_joystick = joystick ? 1 : 0;
	a7800session_set_int(s, "analog_joystick", s->opts.analog_joystick);
}

extern "C" void a7800session_set_region(a7800session* s, int region)
{
	if(region < 0 || region >= A7800_REGION_COUNT) return;
	s->opts.region = region;
	a7800session_set_int(s, "region", region);
	if(s->running && wanted_region(s) != s->running_region)
		restart_machine(s);
}

extern "C" int a7800session_region(a7800session* s)
{
	if(s->running) return s->opts.region;
	return a7800session_get_int(s, "region", A7800_REGION_AUTO);
}

extern "C" int a7800session_running_region(a7800session* s)
{
	return s->running_region ? s->running_region : A7800_REGION_NTSC;
}

extern "C" void session_machine_changed(struct a7800session* s)
{
	if(s->running)
		restart_machine(s);
}

extern "C" void session_hsc_changed(struct a7800session* s)
{
	if(!s->running) return;
	// The cartridge takes its HSC when a console is built: a new one, which
	// boots whatever the cartridge boots at power-on.
	const std::vector<uint8_t> rom = hsc_rom(s);
	host_of(s)->SetHsc(rom, s->opts.hsc && !rom.empty());
	restart_machine(s);
}

// ---- FujiNet ---------------------------------------------------------------

extern "C" int a7800session_fujinet_running(const a7800session* s)
{
	return s->fujinet_running;
}

extern "C" const char* a7800session_fujinet_webui_url(const a7800session* s)
{
	return s->webui_url;
}

extern "C" int a7800session_cart_link_up(a7800session* s)
{
	if(!s->running) return -1;
	fngo_mame_cart_status st;
	if(!fngo_mame_fujinet_status(&st)) return 0;
	return st.link_up ? 1 : 0;
}

extern "C" int a7800session_cart_status(a7800session* s, char* dst, int dstsz)
{
	if(!dst || dstsz <= 0) return 0;
	dst[0] = '\0';
	if(!s->running) return snprintf(dst, static_cast<size_t>(dstsz), "stopped");
	fngo_mame_cart_status st;
	if(!fngo_mame_fujinet_status(&st))
		return snprintf(dst, static_cast<size_t>(dstsz), "starting");
	if(st.mode == 1)
		return snprintf(dst, static_cast<size_t>(dstsz), "loading %d%%", st.load_pct);
	if(st.mode == 2)
		return snprintf(dst, static_cast<size_t>(dstsz), "game running; mailbox closed");
	if(st.link_up)
		return snprintf(dst, static_cast<size_t>(dstsz), "connected");
	if(st.link_error[0])
		return snprintf(dst, static_cast<size_t>(dstsz), "link down: %s", st.link_error);
	return snprintf(dst, static_cast<size_t>(dstsz), "link down");
}

extern "C" int a7800session_cart_booted_game(a7800session* s)
{
	if(!s->running) return 0;
	fngo_mame_cart_status st;
	return fngo_mame_fujinet_status(&st) && st.booted_image ? 1 : 0;
}

// ---- paths -----------------------------------------------------------------

extern "C" const char* a7800session_config_path(const a7800session* s) { return s->config_dir; }
extern "C" const char* a7800session_data_path(const a7800session* s) { return s->data_dir; }
extern "C" const char* a7800session_carts_path(const a7800session* s) { return s->carts_dir; }
extern "C" const char* a7800session_roms_path(const a7800session* s) { return s->roms_dir; }
extern "C" const char* a7800session_sd_path(const a7800session* s) { return s->fujinet_sd; }

// ---- debugger --------------------------------------------------------------

extern "C" a7800debug* a7800session_debugger(a7800session* s)
{
	return a7800debug_get(s);
}

// Reachable from the debugger module (core/src/debug.cpp) without a public
// C++ header: the host behind a session.
MameHost* a7800session_host(a7800session* s)
{
	return host_of(s);
}
