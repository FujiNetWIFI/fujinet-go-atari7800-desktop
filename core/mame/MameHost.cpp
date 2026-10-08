/*
 * MameHost.cpp -- see MameHost.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

#if defined(_WIN32)
  // timeBeginPeriod: Windows sleeps in 15.6 ms steps by default, which turns
  // the per-frame sleep in Pace() into ~30 fps. Asking for 1 ms granularity
  // while the emulator runs is what every emulator on Windows does; it is
  // process-scoped and undone in Stop().
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <timeapi.h>
#endif

#include "MameHost.h"

namespace {

int64_t MonoNs()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A vsync stream counts as live if a tick arrived within this window. Asked
// BEFORE blocking on a tick: waiting for one that never comes would cost the
// timeout on every frame and halve the frame rate of a frontend with no frame
// clock -- headless tests included.
constexpr int64_t VsyncRecentNs = 250'000'000;

// MAME's own thread needs what a main thread gets: macOS gives a secondary
// thread 512K, which MAME's deeper paths (the debugger's expression parser,
// the cartridge's mapper planning) can outgrow.
constexpr size_t EmulationThreadStack = 8u << 20;

// The consoles' frame rates: MARIA's dot clock over 454 dots and 263 (NTSC)
// or 313 (PAL) lines, as MAME's a7800 screens are configured.
constexpr double NtscFps = 7159090.0 / (454.0 * 263.0);     // 59.958
constexpr double PalFps = 7093788.0 / (454.0 * 313.0);      // 49.919

// MAME's a7800 input ports (src/mame/atari/a7800.cpp in the fork).
constexpr const char* PortJoysticks = ":JOYSTICKS";
constexpr const char* PortButtons = ":BUTTONS";
constexpr const char* PortConsole = ":CONSOLE";
constexpr const char* PortControllers = ":CONTROLLERS";
const char* const PortLightgun[4] = { ":LIGHTGUN1_X", ":LIGHTGUN1_Y", ":LIGHTGUN2_X", ":LIGHTGUN2_Y" };

// a7800_action -> JOYSTICKS / BUTTONS masks, per port
const uint32_t JoyMask[2][4] = {
	{ 0x10, 0x20, 0x40, 0x80 },     // player 1: up down left right
	{ 0x01, 0x02, 0x04, 0x08 },     // player 2
};
const uint32_t ButtonMask[2][2] = {
	{ 0x08, 0x02 },                 // player 1: button 1 (left), button 2 (right)
	{ 0x04, 0x01 },                 // player 2
};
// a7800_switch (Select, Reset, Pause) -> CONSOLE masks
const uint32_t SwitchMask[3] = { 0x02, 0x01, 0x08 };

// The controller kinds MAME's CONTROLLERS setting takes.
constexpr int KindLightgun = 2;

struct HostRef
{
	static MameHost* Of(void* user) { return static_cast<MameHost*>(user); }
};

void CbFrame(void* user, const uint32_t* xrgb, int width, int height, uint64_t frameNo, int paced)
{
	HostRef::Of(user)->OnFrame(xrgb, width, height, frameNo, paced != 0);
}
void CbAudio(void* user, const int16_t* stereo, int frames) { HostRef::Of(user)->OnAudio(stereo, frames); }
void CbService(void* user) { HostRef::Of(user)->OnService(); }
void CbLog(void* user, int channel, const char* text) { HostRef::Of(user)->OnLog(channel, text); }
void CbStopped(void* user) { HostRef::Of(user)->OnStopped(); }

thread_local bool tOnEmulationThread = false;

} // namespace

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

MameHost::MameHost()
{
	_frame.assign((size_t)320 * 272, 0);
	_ring.assign((size_t)AudioRingFrames * 2, 0);
}

MameHost::~MameHost()
{
	Stop();
}

bool MameHost::Start(const Config& config, std::string& error)
{
	if(_running.load()) {
		return true;
	}
	_config = config;
	_volume.store(config.volume);
	_quit.store(false);
	_ringRead.store(0);
	_ringWrite.store(0);
	_resamplePos = 0.0;
	_applied.valid = false;
	_inputDirty.store(true);
	_seenInstance = 0;
	_seenCrc = 0;
	_frameRateMilli.store(config.system == "a7800p" ? (int)(PalFps * 1000) : (int)(NtscFps * 1000));

	if(fngo_mame_api_version() != FNGO_MAME_API_VERSION) {
		error = "libmame_fngo is a different version than this program was built for";
		return false;
	}

	fngo_mame_callbacks cb = {};
	cb.frame = CbFrame;
	cb.audio = CbAudio;
	cb.service = CbService;
	cb.log = CbLog;
	cb.debug_stopped = CbStopped;
	cb.user = this;
	_mame = fngo_mame_create(&cb, config.mameDir.c_str());
	if(!_mame) {
		error = "MAME is already running in this process";
		return false;
	}
	fngo_mame_configure(_mame, config.system.c_str(), config.bios.c_str(), config.romPath.c_str());
	fngo_mame_fujinet_link(_mame, config.fujinetHost.c_str(), config.fujinetPort, config.fujinetDebug ? 1 : 0);
	fngo_mame_start_stopped(_mame, config.startStopped ? 1 : 0);
	fngo_mame_fujinet_boot(_mame, config.bootImage.empty() ? FNGO_BOOT_NONE : config.bootMode,
	                       config.bootImage.empty() ? nullptr : config.bootImage.data(),
	                       (uint32_t)config.bootImage.size(),
	                       config.bootMapper.empty() ? nullptr : config.bootMapper.c_str());
	fngo_mame_fujinet_hsc(_mame, config.hscRom.empty() ? nullptr : config.hscRom.data(),
	                      (uint32_t)config.hscRom.size(), config.hscOn ? 1 : 0);

#if defined(_WIN32)
	timeBeginPeriod(1);
#endif

	{
		std::lock_guard<std::mutex> lock(_startLock);
		_startState = 0;
		_runResult = 0;
	}

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, EmulationThreadStack);
	const int rc = pthread_create(&_thread, &attr, &MameHost::ThreadMain, this);
	pthread_attr_destroy(&attr);
	if(rc != 0) {
		fngo_mame_destroy(_mame);
		_mame = nullptr;
		error = "cannot start the emulation thread";
		return false;
	}
	_threadStarted = true;

	// The first frame says the machine runs; MAME refusing it (a BIOS whose
	// files are missing) ends the thread instead.
	std::unique_lock<std::mutex> lock(_startLock);
	_startCv.wait_for(lock, std::chrono::seconds(30), [this] { return _startState != 0; });
	if(_startState != 1) {
		const int result = _runResult;
		lock.unlock();
		Stop();
		char buf[96];
		snprintf(buf, sizeof buf, "MAME could not start the console (error %d)", result);
		error = buf;
		std::string log = RecentLog();
		size_t pos = log.rfind("required files are missing");
		if(pos != std::string::npos || log.find("NOT FOUND") != std::string::npos) {
			error += ": the BIOS files are missing";
		}
		return false;
	}
	_running.store(true);
	return true;
}

void MameHost::Stop()
{
	if(!_threadStarted) {
		return;
	}
	_quit.store(true);
	if(_mame) {
		fngo_mame_stop(_mame);
	}
	_vsyncCv.notify_all();
	pthread_join(_thread, nullptr);
	_threadStarted = false;
	_running.store(false);

	{
		// jobs nobody will run now
		std::lock_guard<std::mutex> lock(_jobLock);
		_jobs.clear();
		_jobsPending.store(false);
		_jobsDone++;
	}
	_jobCv.notify_all();

	if(_mame) {
		fngo_mame_destroy(_mame);
		_mame = nullptr;
	}
#if defined(_WIN32)
	timeEndPeriod(1);
#endif
}

void* MameHost::ThreadMain(void* self)
{
	static_cast<MameHost*>(self)->ThreadBody();
	return nullptr;
}

void MameHost::ThreadBody()
{
	tOnEmulationThread = true;
	const int result = fngo_mame_run(_mame);

	_running.store(false);
	{
		std::lock_guard<std::mutex> lock(_startLock);
		_runResult = result;
		if(_startState == 0) {
			_startState = -1;
		}
	}
	_startCv.notify_all();

	// Nothing will take queued jobs any more: release whoever waits on them.
	{
		std::lock_guard<std::mutex> lock(_jobLock);
		_jobs.clear();
		_jobsPending.store(false);
		_jobsDone++;
	}
	_jobCv.notify_all();
	if(result != 0) {
		fprintf(stderr, "a7800: MAME stopped with error %d\n", result);
	}
}

void MameHost::SetBoot(int mode, const std::vector<uint8_t>& image, const std::string& mapper)
{
	if(_mame) {
		fngo_mame_fujinet_boot(_mame, mode, image.empty() ? nullptr : image.data(), (uint32_t)image.size(),
		                       mapper.empty() ? nullptr : mapper.c_str());
	}
}

void MameHost::SetHsc(const std::vector<uint8_t>& rom, bool on)
{
	if(_mame) {
		fngo_mame_fujinet_hsc(_mame, rom.empty() ? nullptr : rom.data(), (uint32_t)rom.size(), on ? 1 : 0);
	}
}

void MameHost::PowerCycle()
{
	if(_mame && _running.load()) {
		fngo_mame_reset(_mame, FNGO_RESET_SOFT);
	}
}

bool MameHost::Reconfigure(const std::string& system, const std::string& bios, std::string& error)
{
	if(!_mame || !_running.load()) {
		error = "the emulator is not running";
		return false;
	}
	_config.system = system;
	_config.bios = bios;
	_frameRateMilli.store(system == "a7800p" ? (int)(PalFps * 1000) : (int)(NtscFps * 1000));
	const uint32_t before = fngo_mame_fujinet_latest();
	fngo_mame_configure(_mame, system.c_str(), bios.c_str(), nullptr);
	fngo_mame_reset(_mame, FNGO_RESET_HARD);

	// The new machine builds a new cartridge; wait for it (or for MAME to
	// give up on the machine and end the thread).
	for(int waited = 0; waited < 15000; waited += 5) {
		if(!_running.load()) {
			error = "MAME could not start the console";
			if(RecentLog().find("NOT FOUND") != std::string::npos) {
				error += ": the BIOS files are missing";
			}
			return false;
		}
		fngo_mame_cart_status st;
		if(fngo_mame_fujinet_latest() != before && fngo_mame_fujinet_status(&st) && st.instance == fngo_mame_fujinet_latest()) {
			_applied.valid = false;
			_inputDirty.store(true);
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	error = "the console did not restart";
	return false;
}

// ---------------------------------------------------------------------------
// video and pacing
// ---------------------------------------------------------------------------

void MameHost::OnFrame(const uint32_t* xrgb, int width, int height, uint64_t frameNo, bool paced)
{
	if(xrgb && width > 0 && height > 0) {
		std::lock_guard<std::mutex> lock(_frameLock);
		const size_t n = (size_t)width * (size_t)height;
		if(_frame.size() < n) {
			_frame.resize(n);
		}
		memcpy(_frame.data(), xrgb, n * sizeof(uint32_t));
		_frameWidth = (uint32_t)width;
		_frameHeight = (uint32_t)height;
		_frameSerial++;
	}
	(void)frameNo;

	if(_startState == 0) {
		{
			std::lock_guard<std::mutex> lock(_startLock);
			_startState = 1;
		}
		_startCv.notify_all();
	}

	if(paced && !_quit.load()) {
		Pace();
	}
}

bool MameHost::CopyFrame(uint32_t* dst, size_t maxPixels, uint32_t& width, uint32_t& height, uint64_t* serialInOut)
{
	std::lock_guard<std::mutex> lock(_frameLock);
	if(serialInOut && *serialInOut == _frameSerial && *serialInOut != 0) {
		return false;
	}
	const size_t n = std::min((size_t)_frameWidth * _frameHeight, maxPixels);
	if(dst) {
		memcpy(dst, _frame.data(), n * sizeof(uint32_t));
	}
	width = _frameWidth;
	height = _frameHeight;
	if(serialInOut) {
		*serialInOut = _frameSerial;
	}
	return true;
}

double MameHost::GetFps() const
{
	return _frameRateMilli.load() / 1000.0;
}

void MameHost::NotifyVsync(int64_t)
{
	const int64_t now = MonoNs();
	{
		std::lock_guard<std::mutex> lock(_vsyncLock);
		if(_vsyncLastNs) {
			const int64_t period = now - _vsyncLastNs;
			// A smoothed tick period, so a single late tick does not drop the lock
			_vsyncPeriodNs = _vsyncPeriodNs ? (_vsyncPeriodNs * 7 + period) / 8 : period;
		}
		_vsyncLastNs = now;
		_vsyncSerial++;
	}
	_vsyncCv.notify_all();
}

// MAME's throttle is off: every frame waits here. Phase-lock to the
// frontend's frame clock when it runs at the console's own rate (within 5%;
// a 144 Hz display must not run the 7800 at 144 fps, and a PAL game on a
// 60 Hz display paces on the wall clock); otherwise an absolute deadline
// ladder that resyncs, rather than fast-forwards, when badly behind (a laptop
// resume, a debugger stop). Queued jobs run while waiting, so the UI thread
// never waits a frame for one.
void MameHost::Pace()
{
	const int64_t frameNs = (int64_t)(1e9 / GetFps());

	{
		std::unique_lock<std::mutex> lock(_vsyncLock);
		const int64_t now = MonoNs();
		const bool locked = _vsyncLastNs && now - _vsyncLastNs < VsyncRecentNs && _vsyncPeriodNs
		                    && std::fabs((double)_vsyncPeriodNs - (double)frameNs) < frameNs * 0.05;
		if(locked) {
			const uint64_t seen = _vsyncSerial;
			const auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(2 * frameNs);
			while(_vsyncSerial == seen && !_quit.load()) {
				if(_jobsPending.load()) {
					lock.unlock();
					RunJobs();
					lock.lock();
					continue;
				}
				if(_vsyncCv.wait_until(lock, until) == std::cv_status::timeout) {
					break;
				}
			}
			_nextFrameNs = MonoNs();
			if(_vsyncSerial != seen) {
				return;
			}
		}
	}

	_nextFrameNs += frameNs;
	int64_t behind = MonoNs() - _nextFrameNs;
	if(behind > 4 * frameNs) {
		_nextFrameNs = MonoNs();
		return;
	}
	while(behind < 0 && !_quit.load()) {
		{
			std::unique_lock<std::mutex> lock(_vsyncLock);
			_vsyncCv.wait_for(lock, std::chrono::nanoseconds(-behind),
			                  [this] { return _jobsPending.load() || _quit.load(); });
		}
		if(_jobsPending.load()) {
			RunJobs();
		}
		behind = MonoNs() - _nextFrameNs;
	}
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------

// MAME mixes at exactly 48 kHz of emulated time, and emulated time follows
// the display (or the wall clock); the audio device runs on its own crystal.
// A linear resampler steps through MAME's samples up to 0.5% faster or
// slower to keep the ring near its target fill, so neither side drifts into
// an underrun or an ever-growing latency.
void MameHost::OnAudio(const int16_t* in, int frames)
{
	if(!in || frames <= 0) {
		return;
	}
	uint32_t w = _ringWrite.load(std::memory_order_relaxed);
	const uint32_t r = _ringRead.load(std::memory_order_acquire);
	const uint32_t fill = (w + AudioRingFrames - r) % AudioRingFrames;
	const double err = ((double)fill - (double)AudioTargetFrames) / (double)AudioTargetFrames;
	const double step = 1.0 + std::max(-0.005, std::min(0.005, err * 0.005));

	auto sample = [&](int index, int channel) -> int {
		return index == 0 ? _lastSample[channel] : in[(index - 1) * 2 + channel];
	};

	double pos = _resamplePos;
	while(pos < (double)frames) {
		const int i = (int)pos;
		const double frac = pos - i;
		const uint32_t next = (w + 1) % AudioRingFrames;
		if(next != r) {
			for(int ch = 0; ch < 2; ch++) {
				const double v = sample(i, ch) + (sample(i + 1, ch) - sample(i, ch)) * frac;
				_ring[(size_t)w * 2 + ch] = (int16_t)std::lround(v);
			}
			w = next;
		}
		pos += step;
	}
	_resamplePos = pos - (double)frames;
	_lastSample[0] = in[(frames - 1) * 2];
	_lastSample[1] = in[(frames - 1) * 2 + 1];
	_ringWrite.store(w, std::memory_order_release);
}

void MameHost::FillAudio(float* out, uint32_t frames)
{
	uint32_t r = _ringRead.load(std::memory_order_relaxed);
	const uint32_t w = _ringWrite.load(std::memory_order_acquire);
	const float gain = (float)_volume.load(std::memory_order_relaxed) / (100.0f * 32768.0f);
	uint32_t i = 0;
	for(; i < frames && r != w; i++) {
		out[i * 2] = _ring[(size_t)r * 2] * gain;
		out[i * 2 + 1] = _ring[(size_t)r * 2 + 1] * gain;
		r = (r + 1) % AudioRingFrames;
	}
	for(; i < frames; i++) {
		out[i * 2] = out[i * 2 + 1] = 0.0f;
	}
	_ringRead.store(r, std::memory_order_release);
}

void MameHost::SetVolume(int percent)
{
	_volume.store(std::max(0, std::min(100, percent)));
}

// ---------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------

void MameHost::SetAction(int port, int action, bool down)
{
	if(port < 0 || port > 1 || action < 0 || action > 5) {
		return;
	}
	if(down) {
		_actions[port].fetch_or(1u << action);
	} else {
		_actions[port].fetch_and(~(1u << action));
	}
	_inputDirty.store(true);
}

unsigned MameHost::GetActions(int port) const
{
	return (port < 0 || port > 1) ? 0 : _actions[port].load();
}

void MameHost::SetSwitch(int sw, bool down)
{
	if(sw < 0 || sw > 2) {
		return;
	}
	if(down) {
		_switches.fetch_or(1u << sw);
	} else {
		_switches.fetch_and(~(1u << sw));
	}
	_inputDirty.store(true);
}

void MameHost::PulseSwitch(int sw, int frames)
{
	if(sw < 0 || sw > 2) {
		return;
	}
	_pulse[sw].store(std::max(1, frames));
	SetSwitch(sw, true);
}

bool MameHost::SwitchHeld(int sw) const
{
	return sw >= 0 && sw <= 2 && (_switches.load() & (1u << sw));
}

void MameHost::SetDifficulty(int which, bool a)
{
	if(which < 0 || which > 1) {
		return;
	}
	if(a) {
		_difficulty.fetch_or(1u << which);
	} else {
		_difficulty.fetch_and(~(1u << which));
	}
	_inputDirty.store(true);
}

void MameHost::SetController(int port, int kind)
{
	if(port < 0 || port > 1 || kind < 0 || kind > 3) {
		return;
	}
	_controller[port].store(kind);
	_inputDirty.store(true);
}

void MameHost::SetPointer(int x, int y, bool inside, unsigned buttons)
{
	// Off the picture (or the secondary button: some games reload on a shot
	// at nothing) the gun aims where no beam ever passes.
	if(!inside || (buttons & 2)) {
		x = 319;
		y = 299;
	}
	_pointerX.store(std::max(0, std::min(319, x)));
	_pointerY.store(std::max(0, std::min(299, y)));
	_pointerButtons.store(buttons);
	_inputDirty.store(true);
}

void MameHost::ReleaseAll()
{
	_actions[0].store(0);
	_actions[1].store(0);
	_switches.store(0);
	_pointerButtons.store(0);
	_inputDirty.store(true);
}

// On the emulation thread: what changed since the last turn, into MAME's
// input ports.
void MameHost::ApplyInput()
{
	if(!_inputDirty.exchange(false) && _applied.valid) {
		return;
	}
	Applied want {};
	want.actions[0] = _actions[0].load();
	want.actions[1] = _actions[1].load();
	want.switches = _switches.load();
	want.difficulty = _difficulty.load();
	want.controller[0] = _controller[0].load();
	want.controller[1] = _controller[1].load();
	want.px = _pointerX.load();
	want.py = _pointerY.load();
	want.buttons = _pointerButtons.load();
	const bool all = !_applied.valid;

	for(int port = 0; port < 2; port++) {
		if(all || want.controller[port] != _applied.controller[port]) {
			fngo_mame_ioport_setting(_mame, PortControllers, port ? 0x0c : 0x03, (uint32_t)want.controller[port] << (port ? 2 : 0));
		}
		const bool gun = want.controller[port] == KindLightgun;
		unsigned actions = want.actions[port];
		if(gun && (want.buttons & 3)) {
			actions |= 1u << 4;                // the pointer pulls the trigger (Button 1)
		}
		const unsigned before = all ? ~actions : _applied.actions[port];
		for(int a = 0; a < 4; a++) {
			if(((actions ^ before) >> a) & 1) {
				fngo_mame_ioport_set(_mame, PortJoysticks, JoyMask[port][a], (actions >> a) & 1);
			}
		}
		for(int b = 0; b < 2; b++) {
			if(((actions ^ before) >> (4 + b)) & 1) {
				fngo_mame_ioport_set(_mame, PortButtons, ButtonMask[port][b], (actions >> (4 + b)) & 1);
			}
		}
		want.actions[port] = actions;
		if(gun && (all || want.px != _applied.px || want.py != _applied.py)) {
			fngo_mame_ioport_set(_mame, PortLightgun[port * 2], 0x1ff, want.px);
			fngo_mame_ioport_set(_mame, PortLightgun[port * 2 + 1], 0x1ff, want.py);
		}
	}
	for(int sw = 0; sw < 3; sw++) {
		if(all || (((want.switches ^ _applied.switches) >> sw) & 1)) {
			fngo_mame_ioport_set(_mame, PortConsole, SwitchMask[sw], (want.switches >> sw) & 1);
		}
	}
	if(all || want.difficulty != _applied.difficulty) {
		fngo_mame_ioport_setting(_mame, PortConsole, 0x40, (want.difficulty & 1) ? 0x40 : 0x00);
		fngo_mame_ioport_setting(_mame, PortConsole, 0x80, (want.difficulty & 2) ? 0x80 : 0x00);
	}
	want.valid = true;
	_applied = want;
}

// ---------------------------------------------------------------------------
// the emulation thread's turn
// ---------------------------------------------------------------------------

void MameHost::OnService()
{
	for(int sw = 0; sw < 3; sw++) {
		int left = _pulse[sw].load();
		if(left > 0) {
			_pulse[sw].store(--left);
			if(left == 0) {
				SetSwitch(sw, false);
			}
		}
	}
	ApplyInput();
	_debugStopped.store(fngo_mame_debug_stopped(_mame) != 0);
	RunJobs();
	WatchCartridge();
	if(_callbacks.onService) {
		_callbacks.onService();
	}
}

void MameHost::OnStopped()
{
	if(_callbacks.onStopped) {
		_callbacks.onStopped();
	}
}

// Every quarter second: has a new image started in the cartridge? (A game
// booted over the network, CONFIG again after a power cycle.) The session
// re-detects the controllers from it.
void MameHost::WatchCartridge()
{
	if(--_watchCountdown > 0) {
		return;
	}
	_watchCountdown = 15;
	fngo_mame_cart_status st;
	if(!fngo_mame_fujinet_status(&st)) {
		return;
	}
	if(st.instance != _seenInstance) {
		_seenInstance = st.instance;
		_seenCrc = 0;
	}
	if((st.mode == 2 || st.mode == 3) && st.live_crc && st.live_crc != _seenCrc) {
		_seenCrc = st.live_crc;
		if(_callbacks.onImage) {
			_callbacks.onImage(st.live_crc, st.booted_image != 0);
		}
	}
}

namespace {
struct Job
{
	std::function<void(fngo_mame*)> fn;
	bool done = false;
};
}

bool MameHost::WithMame(const std::function<void(fngo_mame*)>& fn, int timeoutMs)
{
	if(!_mame || !_running.load()) {
		return false;
	}
	if(tOnEmulationThread) {
		fn(_mame);
		return true;
	}

	auto job = std::make_shared<Job>();
	job->fn = fn;
	{
		std::lock_guard<std::mutex> lock(_jobLock);
		_jobs.push_back([job](fngo_mame* m) { job->fn(m); job->done = true; });
		_jobsPending.store(true);
	}
	_vsyncCv.notify_all();
	fngo_mame_wake(_mame);

	// The job may hold references into the caller's frame, so the caller
	// waits for it to run -- or for the thread to end and drop it.
	std::unique_lock<std::mutex> lock(_jobLock);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	while(!job->done && _running.load()) {
		if(_jobCv.wait_until(lock, deadline) == std::cv_status::timeout && !job->done) {
			// Still queued (a stall): keep waiting rather than return with the
			// job holding the caller's references.
			if(std::chrono::steady_clock::now() > deadline + std::chrono::seconds(30)) {
				fprintf(stderr, "a7800: the emulation thread has not taken a job for 30 s\n");
			}
		}
	}
	return job->done;
}

void MameHost::Post(std::function<void(fngo_mame*)> fn)
{
	if(!_mame || !_running.load()) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(_jobLock);
		_jobs.push_back(std::move(fn));
		_jobsPending.store(true);
	}
	_vsyncCv.notify_all();
	fngo_mame_wake(_mame);
}

void MameHost::RunJobs()
{
	if(!_jobsPending.load()) {
		return;
	}
	for(;;) {
		std::function<void(fngo_mame*)> fn;
		{
			std::lock_guard<std::mutex> lock(_jobLock);
			if(_jobs.empty()) {
				_jobsPending.store(false);
				break;
			}
			fn = std::move(_jobs.front());
			_jobs.pop_front();
		}
		fn(_mame);
		// a job may have let the machine go (step, run to ...): the caller
		// must not see the old stop once WithMame returns
		_debugStopped.store(fngo_mame_debug_stopped(_mame) != 0);
		{
			std::lock_guard<std::mutex> lock(_jobLock);
			_jobsDone++;
		}
		_jobCv.notify_all();
	}
}

// ---------------------------------------------------------------------------
// log
// ---------------------------------------------------------------------------

void MameHost::OnLog(int channel, const char* text)
{
	if(!text || !*text) {
		return;
	}
	static const bool verbose = getenv("A7800_MAME_VERBOSE") != nullptr;
	if(channel == FNGO_LOG_VERBOSE && !verbose) {
		return;
	}
	if(channel == FNGO_LOG_DEBUG || channel == FNGO_LOG_LOG) {
		return;
	}
	if(channel == FNGO_LOG_ERROR || verbose) {
		fputs(text, stderr);
	}
	std::lock_guard<std::mutex> lock(_logLock);
	_log += text;
	if(_log.size() > 256 * 1024) {
		_log.erase(0, _log.size() - 192 * 1024);
	}
}

std::string MameHost::RecentLog()
{
	std::lock_guard<std::mutex> lock(_logLock);
	return _log;
}
