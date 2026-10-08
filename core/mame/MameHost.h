/*
 * MameHost -- MAME's Atari 7800 (libmame_fngo, from the tschak909/mame
 * fork's fujinet-go-a7800 branch) with FujiNet Go's own platform layer
 * around it.
 *
 * libmame_fngo runs a machine on a thread it is lent and hands back, through
 * callbacks on that thread, every finished frame, every block of mixed audio
 * and a turn once per frame (and over and over while the debugger holds the
 * machine). This class lends it the thread and turns those into the
 * family's contracts:
 *
 *   frames      a serial-stamped XRGB slot (copy_frame), and the pacing:
 *               MAME's throttle is off, so each frame waits for the
 *               frontend's next vsync tick when they run at the console's
 *               rate, or for a wall-clock deadline otherwise;
 *   audio       a stereo ring the session's SDL device pulls, fed through a
 *               resampler that nudges its rate by up to 0.5% to hold the
 *               ring's fill (MAME has no rate hook of its own);
 *   input       the joystick, button, switch and light-gun state staged
 *               from any thread and written to MAME's input ports on the
 *               emulation thread's turn;
 *   jobs        anything else that must touch the machine (the debugger,
 *               a reset, a reconfiguration) queued from any thread and run
 *               on the same turn.
 *
 * Save states, rewind and the like do not exist here: they would replay
 * or roll back mailbox transactions fujinet-pc has already acted on.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAME_HOST_H
#define MAME_HOST_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <pthread.h>

#include "fngo_mame.h"

class MameHost
{
public:
	struct Config
	{
		std::string mameDir;        // MAME's own files (cfg, comments)
		std::string romPath;        // where imported BIOS images live
		std::string system = "a7800";   // or "a7800p"
		std::string bios = "none";
		std::string fujinetHost = "127.0.0.1";
		int fujinetPort = 11510;
		bool fujinetDebug = false;
		int volume = 100;
		bool startStopped = false;
		// what the first power-on boots, and the High Score Cart
		int bootMode = FNGO_BOOT_NONE;
		std::vector<uint8_t> bootImage;
		std::string bootMapper;
		std::vector<uint8_t> hscRom;
		bool hscOn = false;
	};

	struct Callbacks
	{
		// the debugger stopped the machine (emulation thread)
		std::function<void()> onStopped;
		// a new image is running in the cartridge (emulation thread)
		std::function<void(uint32_t crc, bool game)> onImage;
		// every frame's turn, after input (emulation thread)
		std::function<void()> onService;
	};

	MameHost();
	~MameHost();

	MameHost(const MameHost&) = delete;
	MameHost& operator=(const MameHost&) = delete;

	void SetCallbacks(Callbacks cb) { _callbacks = std::move(cb); }

	// Start the emulation thread and the first machine. Fails (with the
	// reason) if libmame_fngo is missing or the machine cannot start (a
	// BIOS whose files are not there).
	bool Start(const Config& config, std::string& error);
	void Stop();
	bool IsRunning() const { return _running.load(); }

	// What the cartridge boots at the next power-on (FNGO_BOOT_*). The image
	// is copied.
	void SetBoot(int mode, const std::vector<uint8_t>& image, const std::string& mapper);
	void SetHsc(const std::vector<uint8_t>& rom, bool on);
	// The console's power switch: a soft reset, the cartridge boots again.
	void PowerCycle();
	// A new machine: another console (NTSC/PAL) or BIOS. Waits until the new
	// machine runs; false and the reason if it cannot.
	bool Reconfigure(const std::string& system, const std::string& bios, std::string& error);
	const std::string& System() const { return _config.system; }

	// ---- video ----
	bool CopyFrame(uint32_t* dst, size_t maxPixels, uint32_t& width, uint32_t& height, uint64_t* serialInOut);
	void NotifyVsync(int64_t frameTimeNs);
	double GetFps() const;

	// ---- audio ----
	void FillAudio(float* out, uint32_t frames);
	void SetVolume(int percent);

	// ---- input (any thread) ----
	// a7800_action on port 0/1
	void SetAction(int port, int action, bool down);
	unsigned GetActions(int port) const;
	// a momentary console switch (a7800_switch: Select, Reset, Pause)
	void SetSwitch(int sw, bool down);
	// press it, hold it for `frames`, let go (menu items, on-screen buttons)
	void PulseSwitch(int sw, int frames);
	bool SwitchHeld(int sw) const;
	// the difficulty switches: true = A (pro)
	void SetDifficulty(int which, bool a);
	// what each port has, as MAME's CONTROLLERS setting: 0 ProLine,
	// 1 2600 joystick, 2 light gun, 3 none
	void SetController(int port, int kind);
	int Controller(int port) const { return _controller[port & 1].load(); }
	// the light guns' aim, in frame pixels, and the pointer's buttons
	void SetPointer(int x, int y, bool inside, unsigned buttons);
	void ReleaseAll();

	// ---- the machine (any thread) ----
	// Run fn on the emulation thread with the machine and wait for it.
	// Returns false (fn not run) when no machine is running or it did not
	// take its turn within the timeout.
	bool WithMame(const std::function<void(fngo_mame*)>& fn, int timeoutMs = 2000);
	// The same, fire and forget.
	void Post(std::function<void(fngo_mame*)> fn);
	fngo_mame* Mame() const { return _mame; }
	// Whether the debugger holds the machine, as of the emulation thread's
	// last turn (cheap: no round trip).
	bool DebugStopped() const { return _debugStopped.load(); }

	std::string RecentLog();

	// ---- called from libmame_fngo's callbacks (emulation thread) ----
	void OnFrame(const uint32_t* xrgb, int width, int height, uint64_t frameNo, bool paced);
	void OnAudio(const int16_t* stereo, int frames);
	void OnService();
	void OnLog(int channel, const char* text);
	void OnStopped();

private:
	static void* ThreadMain(void* self);
	void ThreadBody();
	void Pace();
	void ApplyInput();
	void RunJobs();
	void WatchCartridge();

	Config _config;
	Callbacks _callbacks;
	fngo_mame* _mame = nullptr;

	pthread_t _thread {};
	bool _threadStarted = false;
	std::atomic<bool> _running { false };
	std::atomic<bool> _quit { false };
	std::atomic<bool> _debugStopped { false };
	std::mutex _startLock;
	std::condition_variable _startCv;
	int _startState = 0;            // 0 starting, 1 running, -1 failed
	int _runResult = 0;

	// ---- video ----
	mutable std::mutex _frameLock;
	std::vector<uint32_t> _frame;
	uint32_t _frameWidth = 320;
	uint32_t _frameHeight = 224;
	uint64_t _frameSerial = 0;
	std::atomic<int> _frameRateMilli { 59958 };

	// ---- pacing ----
	std::mutex _vsyncLock;
	std::condition_variable _vsyncCv;
	int64_t _vsyncLastNs = 0;
	int64_t _vsyncPeriodNs = 0;
	uint64_t _vsyncSerial = 0;
	int64_t _nextFrameNs = 0;

	// ---- audio: int16 stereo ring, single producer / single consumer ----
	static constexpr uint32_t AudioRingFrames = 16384;
	static constexpr uint32_t AudioTargetFrames = 2400;   // 50 ms
	std::vector<int16_t> _ring;
	std::atomic<uint32_t> _ringRead { 0 };
	std::atomic<uint32_t> _ringWrite { 0 };
	std::atomic<int> _volume { 100 };
	double _resamplePos = 0.0;
	int16_t _lastSample[2] = { 0, 0 };

	// ---- input, staged ----
	std::atomic<unsigned> _actions[2] = { { 0 }, { 0 } };
	std::atomic<unsigned> _switches { 0 };
	std::atomic<unsigned> _difficulty { 3 };      // bit0 left A, bit1 right A
	std::atomic<int> _controller[2] = { { 0 }, { 0 } };
	std::atomic<int> _pointerX { 160 };
	std::atomic<int> _pointerY { 112 };
	std::atomic<unsigned> _pointerButtons { 0 };
	std::atomic<bool> _inputDirty { true };
	std::atomic<int> _pulse[3] = { { 0 }, { 0 }, { 0 } };
	struct Applied { unsigned actions[2]; unsigned switches; unsigned difficulty; int controller[2]; int px, py; unsigned buttons; bool valid; };
	Applied _applied {};

	// ---- jobs ----
	std::mutex _jobLock;
	std::condition_variable _jobCv;
	std::deque<std::function<void(fngo_mame*)>> _jobs;
	std::atomic<bool> _jobsPending { false };
	uint64_t _jobsDone = 0;

	// ---- the cartridge, watched ----
	uint32_t _seenInstance = 0;
	uint32_t _seenCrc = 0;
	int _watchCountdown = 0;

	// ---- log ----
	std::mutex _logLock;
	std::string _log;
};

#endif // MAME_HOST_H
