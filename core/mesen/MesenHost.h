/*
 * MesenHost -- MesenCE's core with FujiNet Go's own platform layer under it.
 *
 * Mesen's C++ core already runs itself: Emulator::LoadRom starts its
 * emulation thread, a video decoder and a renderer thread, and every public
 * call is made for use from another thread (AcquireLock, DebuggerRequest).
 * What it leaves to a platform is a handful of small interfaces, and this
 * file implements them in place of Mesen's Linux/, MacOS/, Windows/ and
 * Sdl/ layers and its C# UI:
 *
 *   IRenderingDevice    the decoded 256x240 ARGB frame goes into a
 *                       serial-stamped slot -- the family's copy_frame
 *                       contract;
 *   IAudioDevice        Mesen's mixed, resampled 48 kHz stereo goes into a
 *                       ring the session's SDL device pulls; read/write gaps
 *                       feed Mesen's dynamic-rate control, so the audio
 *                       latency holds;
 *   IKeyManager         one virtual key per NES controller button, set by
 *                       the session's keyboard and gamepad translators;
 *   INotificationListener  breaks and resumes for the debugger, and the
 *                       per-frame hook where the emulator phase-locks to
 *                       the frontend's vsync ticks;
 *   IMessageManager     Mesen's on-screen messages, routed to the session.
 *
 * And the settings a C# UI would have pushed: palette, channel volumes,
 * controllers, and everything that must stay OFF here -- rewind, run-ahead
 * and save states would replay or roll back mailbox transactions that
 * fujinet-pc has already acted on.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MESEN_HOST_H
#define MESEN_HOST_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Emulator;
class FngoRenderer;
class FngoAudioDevice;
class FngoKeyManager;
class FngoListener;
class FngoMessages;

class MesenHost
{
public:
	// What stopped the machine, for the debugger's status line.
	enum class StopReason { Pause, Step, Breakpoint, Other };

	struct Callbacks
	{
		std::function<void(StopReason, const std::string& text, int pc)> onStopped;
		std::function<void()> onResumed;
		std::function<void(const std::string& title, const std::string& message)> onMessage;
	};

	struct Config
	{
		std::string homeDir;        // Mesen's home folder (saves, debugger workspaces)
		std::string fujinetHost = "127.0.0.1";
		int fujinetPort = 11506;
		bool fujinetDebug = false;
		uint32_t audioRate = 48000;
		int volume = 100;
		int region = 0;             // nes_region
		int portType[2] = { 0, 0 }; // nes_ctrl_type
		int keyboard = 0;           // nes_keyboard on the expansion port
	};

	MesenHost();
	~MesenHost();

	void SetCallbacks(Callbacks cb) { _callbacks = std::move(cb); }

	bool Start(const Config& config, std::string& error);
	void Stop();
	bool IsRunning() const { return _started; }

	// Power on with the cartridge's resident CONFIG, through its loader.
	bool LoadConfig(std::string& error);
	// Power on with a cartridge image put straight into the SRAMs.
	bool LoadCart(const std::string& path, std::string& error);
	// The console's RESET button.
	void Reset();

	bool CopyFrame(uint32_t* dst, size_t maxPixels, uint32_t& width, uint32_t& height, uint64_t* serialInOut);
	void NotifyVsync(int64_t frameTimeNs);
	double GetFps();

	void FillAudio(float* out, uint32_t frames);
	void SetVolume(int percent);

	// One NES controller button (nes_action) on port 0/1.
	void SetButton(int port, int action, bool down);
	void ReleaseAll();
	// Buttons held on a port, in the controller's shift order: bit 0 A, 1 B,
	// 2 Select, 3 Start, 4 Up, 5 Down, 6 Left, 7 Right.
	uint8_t GetButtons(int port);
	// The same, one bit per nes_action (bit 0 Up ... bit 9 Turbo B).
	unsigned GetActions(int port);
	// A palette index (0-63) as XRGB, through the palette the video uses.
	uint32_t PaletteColor(uint8_t index);
	void SetPortType(int port, int type);
	void SetRegion(int region);

	// The expansion-port keyboard (nes_keyboard) and its keys, by MesenCE
	// key index.
	void SetKeyboard(int type);
	void SetKeyboardKey(int index, bool down);
	bool KeyboardKeyHeld(int index);

	// The Family BASIC Data Recorder: 0 play, 1 record, 2 stop (MesenCE's
	// TapeRecorderAction). False when no recorder is attached.
	bool Tape(int action, const std::string& path);
	// nes_tape_state.
	int TapeState();

	Emulator* GetEmulator() { return _emu.get(); }
	std::string GetLog();

	// ---- called from the host classes ----
	void PublishFrame(const uint32_t* argb, uint32_t width, uint32_t height);
	void PushAudio(const int16_t* samples, uint32_t frames);
	void AudioLatency(uint32_t& readPos, uint32_t& writePos, uint32_t& bufferBytes);
	void OnFrameDone();
	void OnNotification(int type, void* parameter);
	void OnMessage(const std::string& title, const std::string& message);

private:
	void ApplySettings();
	bool Load(const std::string& path, const uint8_t* data, uint32_t size, std::string& error);

	std::unique_ptr<Emulator> _emu;
	Config _config;
	Callbacks _callbacks;
	bool _started = false;

	std::unique_ptr<FngoKeyManager> _keys;
	std::shared_ptr<FngoListener> _listener;
	std::unique_ptr<FngoMessages> _messages;

	// ---- video: the published frame ----
	std::mutex _frameLock;
	std::vector<uint32_t> _frame;
	uint32_t _frameWidth = 256;
	uint32_t _frameHeight = 240;
	uint64_t _frameSerial = 0;

	// ---- vsync phase lock ----
	std::mutex _vsyncLock;
	std::condition_variable _vsyncCv;
	int64_t _vsyncLastNs = 0;
	int64_t _vsyncPeriodNs = 0;
	uint64_t _vsyncSerial = 0;

	// ---- audio: int16 stereo ring, single producer / single consumer ----
	static constexpr uint32_t AudioRingFrames = 16384;
	std::vector<int16_t> _ring;
	std::atomic<uint32_t> _ringRead { 0 };
	std::atomic<uint32_t> _ringWrite { 0 };
	std::atomic<int> _volume { 100 };
};

#endif // MESEN_HOST_H
