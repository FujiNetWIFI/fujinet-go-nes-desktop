/*
 * MesenHost.cpp -- see MesenHost.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "pch.h"
#include <chrono>
#include <cmath>
#include <cstring>

#include "MesenHost.h"

#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/KeyManager.h"
#include "Shared/MessageManager.h"
#include "Shared/NotificationManager.h"
#include "Shared/RenderedFrame.h"
#include "Shared/SettingTypes.h"
#include "Shared/Audio/BaseSoundManager.h"
#include "Shared/Audio/SoundMixer.h"
#include "Shared/Interfaces/IKeyManager.h"
#include "Shared/Interfaces/IMessageManager.h"
#include "Shared/Interfaces/INotificationListener.h"
#include "Shared/Interfaces/IRenderingDevice.h"
#include "Shared/Video/VideoRenderer.h"
#include "Debugger/DebugTypes.h"
#include "NES/Mappers/Homebrew/FujiNetCart.h"
#include "Shared/BaseControlManager.h"
#include "Shared/BaseControlDevice.h"
#include "Shared/Interfaces/IConsole.h"
#include "Shared/Interfaces/ITapeRecorder.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/VirtualFile.h"

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

// Mesen's own default NES palette (its UI's NesConfig.UserPalette default):
// the core starts from an all-zero UserPalette and relies on the UI to push
// this.
const uint32_t DefaultPalette[64] = {
	0xFF666666, 0xFF002A88, 0xFF1412A7, 0xFF3B00A4, 0xFF5C007E, 0xFF6E0040, 0xFF6C0600, 0xFF561D00,
	0xFF333500, 0xFF0B4800, 0xFF005200, 0xFF004F08, 0xFF00404D, 0xFF000000, 0xFF000000, 0xFF000000,
	0xFFADADAD, 0xFF155FD9, 0xFF4240FF, 0xFF7527FE, 0xFFA01ACC, 0xFFB71E7B, 0xFFB53120, 0xFF994E00,
	0xFF6B6D00, 0xFF388700, 0xFF0C9300, 0xFF008F32, 0xFF007C8D, 0xFF000000, 0xFF000000, 0xFF000000,
	0xFFFFFEFF, 0xFF64B0FF, 0xFF9290FF, 0xFFC676FF, 0xFFF36AFF, 0xFFFE6ECC, 0xFFFE8170, 0xFFEA9E22,
	0xFFBCBE00, 0xFF88D800, 0xFF5CE430, 0xFF45E082, 0xFF48CDDE, 0xFF4F4F4F, 0xFF000000, 0xFF000000,
	0xFFFFFEFF, 0xFFC0DFFF, 0xFFD3D2FF, 0xFFE8C8FF, 0xFFFBC2FF, 0xFFFEC4EA, 0xFFFECCC5, 0xFFF7D8A5,
	0xFFE4E594, 0xFFCFEF96, 0xFFBDF4AB, 0xFFB3F3CC, 0xFFB5EBF2, 0xFFB8B8B8, 0xFF000000, 0xFF000000,
};

// Virtual key codes: 1 + port * KeysPerPort + nes_action for the two
// controllers (0 means "unbound" to Mesen, so codes start at 1), then
// KeyboardBase + key index for the expansion-port keyboard.
constexpr int KeysPerPort = 16;
constexpr int KeyboardBase = 64;
constexpr int KeyboardKeys = 128;
constexpr int KeyCount = KeyboardBase + KeyboardKeys;

uint16_t KeyCode(int port, int action)
{
	return (uint16_t)(1 + port * KeysPerPort + action);
}

uint16_t KeyboardCode(int index)
{
	return (uint16_t)(KeyboardBase + index);
}

} // namespace

// ---------------------------------------------------------------------------
// the platform classes
// ---------------------------------------------------------------------------

class FngoRenderer : public IRenderingDevice
{
	MesenHost* _host;
	Emulator* _emu;

public:
	FngoRenderer(MesenHost* host, Emulator* emu) : _host(host), _emu(emu)
	{
		_emu->GetVideoRenderer()->RegisterRenderingDevice(this);
	}

	void UpdateFrame(RenderedFrame& frame) override
	{
		_host->PublishFrame((const uint32_t*)frame.FrameBuffer, frame.Width, frame.Height);
	}

	void ClearFrame() override
	{
		std::vector<uint32_t> black(256 * 240, 0xFF000000);
		_host->PublishFrame(black.data(), 256, 240);
	}

	void Render(RenderSurfaceInfo&, RenderSurfaceInfo&) override {}
	void Reset() override {}
	void SetFullscreenMode(FullscreenSettings) override {}
};

class FngoAudioDevice : public BaseSoundManager
{
	MesenHost* _host;
	Emulator* _emu;

public:
	FngoAudioDevice(MesenHost* host, Emulator* emu) : _host(host), _emu(emu)
	{
		_emu->GetSoundMixer()->RegisterAudioDevice(this);
	}

	void PlayBuffer(int16_t* soundBuffer, uint32_t frames, uint32_t sampleRate, bool isStereo) override
	{
		_sampleRate = sampleRate;
		_isStereo = true;
		if(isStereo) {
			_host->PushAudio(soundBuffer, frames);
		} else {
			std::vector<int16_t> st(frames * 2);
			for(uint32_t i = 0; i < frames; i++) {
				st[i * 2] = st[i * 2 + 1] = soundBuffer[i];
			}
			_host->PushAudio(st.data(), frames);
		}
	}

	void ProcessEndOfFrame() override
	{
		// Read/write gaps in bytes, once a frame: Mesen's resampler nudges its
		// rate by up to 0.5% to hold AudioConfig.AudioLatency.
		uint32_t readPos, writePos, size;
		_host->AudioLatency(readPos, writePos, size);
		_bufferSize = size;
		ProcessLatency(readPos, writePos);
	}

	void Stop() override {}
	void Pause() override {}
	string GetAvailableDevices() override { return ""; }
	void SetAudioDevice(string) override {}
};

class FngoKeyManager : public IKeyManager
{
	std::atomic<bool> _keys[KeyCount] = {};

public:
	void Set(uint16_t code, bool down)
	{
		if(code < KeyCount) {
			_keys[code].store(down, std::memory_order_relaxed);
		}
	}

	void Clear()
	{
		for(auto& k : _keys) {
			k.store(false, std::memory_order_relaxed);
		}
	}

	void RefreshState() override {}
	void UpdateDevices() override {}
	bool IsMouseButtonPressed(MouseButton) override { return false; }
	bool IsKeyPressed(uint16_t keyCode) override
	{
		return keyCode < KeyCount && _keys[keyCode].load(std::memory_order_relaxed);
	}
	vector<uint16_t> GetPressedKeys() override
	{
		vector<uint16_t> out;
		for(uint16_t i = 1; i < KeyCount; i++) {
			if(_keys[i].load(std::memory_order_relaxed)) {
				out.push_back(i);
			}
		}
		return out;
	}
	string GetKeyName(uint16_t keyCode) override { return "Key" + std::to_string(keyCode); }
	uint16_t GetKeyCode(string) override { return 0; }
	bool SetKeyState(uint16_t scanCode, bool state) override
	{
		Set(scanCode, state);
		return true;
	}
	void ResetKeyState() override { Clear(); }
	void SetDisabled(bool) override {}
};

class FngoListener : public INotificationListener
{
	MesenHost* _host;

public:
	explicit FngoListener(MesenHost* host) : _host(host) {}

	void ProcessNotification(ConsoleNotificationType type, void* parameter) override
	{
		if(type == ConsoleNotificationType::PpuFrameDone) {
			_host->OnFrameDone();
		} else {
			_host->OnNotification((int)type, parameter);
		}
	}
};

class FngoMessages : public IMessageManager
{
	MesenHost* _host;

public:
	explicit FngoMessages(MesenHost* host) : _host(host) {}

	void DisplayMessage(string title, string message) override
	{
		_host->OnMessage(title, message);
	}
};

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

MesenHost::MesenHost()
{
	_frame.assign(256 * 240, 0xFF000000);
	_ring.assign(AudioRingFrames * 2, 0);
}

MesenHost::~MesenHost()
{
	Stop();
}

bool MesenHost::Start(const Config& config, std::string& error)
{
	if(_started) {
		return true;
	}
	_config = config;
	_volume.store(config.volume);

	if(!config.homeDir.empty()) {
		FolderUtilities::SetHomeFolder(config.homeDir);
	}

	// Messages go to the session, not to an OSD we do not draw; Mesen's log
	// (which carries the cartridge's own) goes to stdout when debugging it.
	MessageManager::SetOptions(false, config.fujinetDebug);
	_messages.reset(new FngoMessages(this));
	MessageManager::RegisterMessageManager(_messages.get());

	FujiNetCartHostConfig cart;
	cart.Enabled = true;
	cart.Host = config.fujinetHost;
	cart.Port = config.fujinetPort;
	cart.Debug = config.fujinetDebug;
	FujiNetCart::SetHostConfig(cart);

	_emu.reset(new Emulator());
	// No ShortcutKeyHandler: every shortcut is the frontend's.
	_emu->Initialize(false);

	_keys.reset(new FngoKeyManager());
	KeyManager::SetSettings(_emu->GetSettings());
	KeyManager::RegisterKeyManager(_keys.get());

	_listener = std::make_shared<FngoListener>(this);
	_emu->GetNotificationManager()->RegisterNotificationListener(_listener);

	ApplySettings();

	Emulator* emu = _emu.get();
	_emu->SetAudioVideoInitCallback(
		[this, emu]() -> IAudioDevice* { return new FngoAudioDevice(this, emu); },
		[this, emu]() -> IRenderingDevice* { return new FngoRenderer(this, emu); });

	_started = true;
	(void)error;
	return true;
}

void MesenHost::Stop()
{
	if(!_started) {
		return;
	}
	_started = false;
	{
		// A frame thread parked waiting for a tick must not hold up the stop
		std::lock_guard<std::mutex> lock(_vsyncLock);
		_vsyncLastNs = 0;
		_vsyncSerial++;
	}
	_vsyncCv.notify_all();

	_emu->Stop(false, true, false);
	_emu->Release();
	KeyManager::RegisterKeyManager(nullptr);
	MessageManager::UnregisterMessageManager(_messages.get());
	_emu.reset();
	_listener.reset();
	_keys.reset();
	_messages.reset();
}

void MesenHost::ApplySettings()
{
	EmuSettings* settings = _emu->GetSettings();

	// SetPreferences re-applies MessageManager's options from this flag
	settings->SetFlagState(EmulationFlags::OutputToStdout, _config.fujinetDebug);

	PreferencesConfig prefs = settings->GetPreferences();
	prefs.DisableGameSelectionScreen = true;
	prefs.DisableOsd = true;
	prefs.AllowBackgroundInput = true;
	prefs.AutoSaveStateDelay = 0;     // a save state cannot hold the mailbox
	prefs.RewindBufferSize = 0;       // nor can rewinding undo fujinet-pc
	prefs.ShowFps = false;
	prefs.ShowDebugInfo = false;
	settings->SetPreferences(prefs);

	EmulationConfig emuCfg = settings->GetEmulationConfig();
	emuCfg.EmulationSpeed = 100;
	emuCfg.RunAheadFrames = 0;        // run-ahead re-executes frames: it would re-send transactions
	settings->SetEmulationConfig(emuCfg);

	AudioConfig audio = settings->GetAudioConfig();
	audio.EnableAudio = true;
	audio.SampleRate = _config.audioRate;
	audio.AudioLatency = 60;
	audio.MasterVolume = 100;         // the session's volume is applied at the ring
	audio.MuteSoundInBackground = false;
	audio.ReduceSoundInBackground = false;
	settings->SetAudioConfig(audio);

	VideoConfig video = settings->GetVideoConfig();
	video.VideoFilter = VideoFilterType::None;
	video.AspectRatio = VideoAspectRatio::NoStretching;
	settings->SetVideoConfig(video);

	NesConfig nes = settings->GetNesConfig();
	nes.EnableHdPacks = false;
	nes.AutoConfigureInput = false;
	nes.DisableGameDatabase = true;   // the cartridge maps every image itself
	nes.RamPowerOnState = RamState::AllZeros;
	nes.NtscOverscan = {};
	nes.PalOverscan = {};
	nes.IsFullColorPalette = false;
	memcpy(nes.UserPalette, DefaultPalette, sizeof(DefaultPalette));
	for(int i = 0; i < 11; i++) {
		nes.ChannelVolumes[i] = 100;
		nes.ChannelPanning[i] = 0;
	}
	nes.EpsmVolume = 100;
	switch(_config.region) {
		case 1: nes.Region = ConsoleRegion::Ntsc; break;
		case 2: nes.Region = ConsoleRegion::Pal; break;
		case 3: nes.Region = ConsoleRegion::Dendy; break;
		default: nes.Region = ConsoleRegion::Auto; break;
	}

	auto setPort = [](ControllerConfig& port, int index, int type) {
		port = {};
		port.Type = type == 0 ? ControllerType::NesController : ControllerType::None;
		KeyMapping& m = port.Keys.Mapping1;
		m.Up = KeyCode(index, 0);
		m.Down = KeyCode(index, 1);
		m.Left = KeyCode(index, 2);
		m.Right = KeyCode(index, 3);
		m.A = KeyCode(index, 4);
		m.B = KeyCode(index, 5);
		m.Select = KeyCode(index, 6);
		m.Start = KeyCode(index, 7);
		m.TurboA = KeyCode(index, 8);
		m.TurboB = KeyCode(index, 9);
		port.Keys.TurboSpeed = 2;
	};
	setPort(nes.Port1, 0, _config.portType[0]);
	setPort(nes.Port2, 1, _config.portType[1]);
	// The expansion port: a keyboard, its every key on its own virtual code
	// (Mesen reads KeyMapping.CustomKeys[key index]). The Family BASIC
	// keyboard brings its Data Recorder with it.
	nes.ExpPort = {};
	switch(_config.keyboard) {
		case 1: nes.ExpPort.Type = ControllerType::FamilyBasicKeyboard; break;
		case 2: nes.ExpPort.Type = ControllerType::SuborKeyboard; break;
		default: nes.ExpPort.Type = ControllerType::None; break;
	}
	for(int i = 0; i < KeyboardKeys && i < 100; i++) {
		nes.ExpPort.Keys.Mapping1.CustomKeys[i] = KeyboardCode(i);
	}
	nes.MapperInput = {};
	settings->SetNesConfig(nes);
}

bool MesenHost::Load(const std::string& path, const uint8_t* data, uint32_t size, std::string& error)
{
	if(!_started) {
		error = "the emulator is not running";
		return false;
	}

	VirtualFile file = data ? VirtualFile(data, size, path) : VirtualFile(path);
	if(!file.IsValid()) {
		error = "cannot read " + path;
		return false;
	}

	if(!data) {
		vector<uint8_t> bytes;
		file.ReadFile(bytes);
		string why;
		if(!FujiNetCart::CheckImage(bytes.data(), (uint32_t)bytes.size(), why)) {
			error = why;
			return false;
		}
	}

	// A fresh LoadRom (stopRom = true) is a power cycle of the cartridge: the
	// old console and its cartridge are destroyed, SRAMs and all, and the new
	// cartridge takes over the mailbox on its first clock.
	if(!_emu->LoadRom(file, VirtualFile())) {
		error = "the image could not be loaded";
		return false;
	}

	// The new cartridge takes the mailbox over on its first clock; until then
	// the status a frontend reads would still be the old cartridge's. Wait
	// for the hand-over (briefly: a debugger that breaks at once never clocks).
	const uint32_t want = FujiNetCart::LatestInstance();
	for(int waited = 0; waited < 500; waited += 2) {
		FujiNetCartStatus st;
		if(FujiNetCart::GetStatus(st) && st.Instance == want) {
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return true;
}

bool MesenHost::LoadConfig(std::string& error)
{
	uint32_t size = 0;
	const uint8_t* rom = FujiNetCart::GetConfigRom(size);
	return Load("FujiNet CONFIG.nes", rom, size, error);
}

bool MesenHost::LoadCart(const std::string& path, std::string& error)
{
	return Load(path, nullptr, 0, error);
}

void MesenHost::Reset()
{
	if(_started && _emu->IsRunning()) {
		_emu->Reset();
	}
}

std::string MesenHost::GetLog()
{
	return MessageManager::GetLog();
}

// ---------------------------------------------------------------------------
// video
// ---------------------------------------------------------------------------

void MesenHost::PublishFrame(const uint32_t* argb, uint32_t width, uint32_t height)
{
	if(!argb || width == 0 || height == 0) {
		return;
	}
	std::lock_guard<std::mutex> lock(_frameLock);
	size_t n = (size_t)width * height;
	if(_frame.size() < n) {
		_frame.resize(n);
	}
	memcpy(_frame.data(), argb, n * sizeof(uint32_t));
	_frameWidth = width;
	_frameHeight = height;
	_frameSerial++;
}

bool MesenHost::CopyFrame(uint32_t* dst, size_t maxPixels, uint32_t& width, uint32_t& height, uint64_t* serialInOut)
{
	std::lock_guard<std::mutex> lock(_frameLock);
	if(serialInOut && *serialInOut == _frameSerial && *serialInOut != 0) {
		return false;
	}
	size_t n = std::min((size_t)_frameWidth * _frameHeight, maxPixels);
	if(dst) {
		for(size_t i = 0; i < n; i++) {
			dst[i] = _frame[i] & 0x00FFFFFF;     // XRGB
		}
	}
	width = _frameWidth;
	height = _frameHeight;
	if(serialInOut) {
		*serialInOut = _frameSerial;
	}
	return true;
}

double MesenHost::GetFps()
{
	if(!_started || !_emu->IsRunning()) {
		return 60.0988;
	}
	return _emu->GetFps();
}

void MesenHost::NotifyVsync(int64_t)
{
	int64_t now = MonoNs();
	{
		std::lock_guard<std::mutex> lock(_vsyncLock);
		if(_vsyncLastNs) {
			int64_t period = now - _vsyncLastNs;
			// A smoothed tick period, so a single late tick does not drop the lock
			_vsyncPeriodNs = _vsyncPeriodNs ? (_vsyncPeriodNs * 7 + period) / 8 : period;
		}
		_vsyncLastNs = now;
		_vsyncSerial++;
	}
	_vsyncCv.notify_all();
}

// Called on the emulation thread as each frame completes. Phase-lock to the
// frontend's frame clock only when that clock runs at the console's own rate
// (within 5%): a 144 Hz display must not make the NES run at 144 fps, and a
// 50 Hz PAL game on a 60 Hz display stays on Mesen's own wall-clock pacing.
void MesenHost::OnFrameDone()
{
	std::unique_lock<std::mutex> lock(_vsyncLock);
	int64_t now = MonoNs();
	if(!_vsyncLastNs || now - _vsyncLastNs > VsyncRecentNs || !_vsyncPeriodNs) {
		return;
	}
	double fps = _emu ? _emu->GetFps() : 60.0988;
	double want = 1e9 / fps;
	if(std::fabs((double)_vsyncPeriodNs - want) > want * 0.05) {
		return;
	}
	uint64_t serial = _vsyncSerial;
	_vsyncCv.wait_for(lock, std::chrono::nanoseconds((int64_t)(want * 2)), [&] {
		return _vsyncSerial != serial || !_started;
	});
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------

void MesenHost::PushAudio(const int16_t* samples, uint32_t frames)
{
	uint32_t w = _ringWrite.load(std::memory_order_relaxed);
	uint32_t r = _ringRead.load(std::memory_order_acquire);
	for(uint32_t i = 0; i < frames; i++) {
		uint32_t next = (w + 1) % AudioRingFrames;
		if(next == r) {
			break;                        // full: drop the rest of this burst
		}
		_ring[w * 2] = samples[i * 2];
		_ring[w * 2 + 1] = samples[i * 2 + 1];
		w = next;
	}
	_ringWrite.store(w, std::memory_order_release);
}

void MesenHost::AudioLatency(uint32_t& readPos, uint32_t& writePos, uint32_t& bufferBytes)
{
	readPos = _ringRead.load(std::memory_order_acquire) * 4;
	writePos = _ringWrite.load(std::memory_order_acquire) * 4;
	bufferBytes = AudioRingFrames * 4;
}

void MesenHost::FillAudio(float* out, uint32_t frames)
{
	uint32_t r = _ringRead.load(std::memory_order_relaxed);
	uint32_t w = _ringWrite.load(std::memory_order_acquire);
	float gain = (float)_volume.load(std::memory_order_relaxed) / (100.0f * 32768.0f);
	uint32_t i = 0;
	for(; i < frames && r != w; i++) {
		out[i * 2] = _ring[r * 2] * gain;
		out[i * 2 + 1] = _ring[r * 2 + 1] * gain;
		r = (r + 1) % AudioRingFrames;
	}
	for(; i < frames; i++) {
		out[i * 2] = out[i * 2 + 1] = 0.0f;
	}
	_ringRead.store(r, std::memory_order_release);
}

void MesenHost::SetVolume(int percent)
{
	_volume.store(std::max(0, std::min(100, percent)));
}

// ---------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------

void MesenHost::SetButton(int port, int action, bool down)
{
	if(_keys && port >= 0 && port < 2 && action >= 0 && action < KeysPerPort) {
		_keys->Set(KeyCode(port, action), down);
	}
}

void MesenHost::ReleaseAll()
{
	if(_keys) {
		_keys->Clear();
	}
}

uint8_t MesenHost::GetButtons(int port)
{
	if(!_keys || port < 0 || port > 1) {
		return 0;
	}
	// nes_action order: Up Down Left Right A B Select Start
	static const int bit[8] = { 4, 5, 6, 7, 0, 1, 2, 3 };
	uint8_t out = 0;
	for(int a = 0; a < 8; a++) {
		if(_keys->IsKeyPressed(KeyCode(port, a))) {
			out |= (uint8_t)(1 << bit[a]);
		}
	}
	return out;
}

unsigned MesenHost::GetActions(int port)
{
	if(!_keys || port < 0 || port > 1) {
		return 0;
	}
	unsigned out = 0;
	for(int a = 0; a < KeysPerPort; a++) {
		if(_keys->IsKeyPressed(KeyCode(port, a))) {
			out |= 1u << a;
		}
	}
	return out;
}

uint32_t MesenHost::PaletteColor(uint8_t index)
{
	if(_emu) {
		return _emu->GetSettings()->GetNesConfig().UserPalette[index & 0x3F] & 0x00FFFFFF;
	}
	return DefaultPalette[index & 0x3F] & 0x00FFFFFF;
}

void MesenHost::SetPortType(int port, int type)
{
	if(port < 0 || port > 1) {
		return;
	}
	_config.portType[port] = type;
	if(_started) {
		// Mesen re-reads the controller types between frames
		auto lock = _emu->AcquireLock();
		ApplySettings();
	}
}

void MesenHost::SetKeyboard(int type)
{
	_config.keyboard = type;
	if(_keys) {
		for(int i = 0; i < KeyboardKeys; i++) {
			_keys->Set(KeyboardCode(i), false);
		}
	}
	if(_started) {
		// Mesen re-reads the devices between frames
		auto lock = _emu->AcquireLock();
		ApplySettings();
	}
}

void MesenHost::SetKeyboardKey(int index, bool down)
{
	if(_keys && index >= 0 && index < KeyboardKeys) {
		_keys->Set(KeyboardCode(index), down);
	}
}

bool MesenHost::KeyboardKeyHeld(int index)
{
	return _keys && index >= 0 && index < KeyboardKeys && _keys->IsKeyPressed(KeyboardCode(index));
}

bool MesenHost::Tape(int action, const std::string& path)
{
	if(!_started || !_emu->IsRunning()) {
		return false;
	}
	shared_ptr<IConsole> console = _emu->GetConsole();
	if(!console || !console->GetControlManager()->GetControlDevice<ITapeRecorder>()) {
		return false;
	}
	_emu->ProcessTapeRecorderAction((TapeRecorderAction)action, path);
	return true;
}

int MesenHost::TapeState()
{
	if(!_started || !_emu->IsRunning()) {
		return 0;
	}
	shared_ptr<IConsole> console = _emu->GetConsole();
	shared_ptr<ITapeRecorder> recorder = console ? console->GetControlManager()->GetControlDevice<ITapeRecorder>() : nullptr;
	if(!recorder) {
		return 0;
	}
	if(recorder->IsRecording()) {
		return 2;
	}
	return recorder->IsPlaying() ? 1 : 0;
}

void MesenHost::SetRegion(int region)
{
	_config.region = region;
	if(_started) {
		ApplySettings();
	}
}

// ---------------------------------------------------------------------------
// notifications
// ---------------------------------------------------------------------------

void MesenHost::OnNotification(int type, void* parameter)
{
	switch((ConsoleNotificationType)type) {
		case ConsoleNotificationType::CodeBreak: {
			if(!_callbacks.onStopped) {
				break;
			}
			BreakEvent* evt = (BreakEvent*)parameter;
			StopReason reason = StopReason::Other;
			std::string text = "stopped";
			int pc = -1;
			if(evt) {
				switch(evt->Source) {
					case BreakSource::Pause: reason = StopReason::Pause; text = "stopped"; break;
					case BreakSource::CpuStep:
					case BreakSource::PpuStep: reason = StopReason::Step; text = "step"; break;
					case BreakSource::Breakpoint: {
						reason = StopReason::Breakpoint;
						char buf[64];
						const char* kind = "exec";
						switch(evt->Operation.Type) {
							case MemoryOperationType::Read: kind = "read"; break;
							case MemoryOperationType::Write: kind = "write"; break;
							default: break;
						}
						snprintf(buf, sizeof buf, "breakpoint: %s $%04X", kind, evt->Operation.Address & 0xFFFF);
						text = buf;
						break;
					}
					case BreakSource::BreakOnBrk: text = "BRK"; break;
					case BreakSource::NesBreakOnCpuCrash: text = "CPU crash (illegal opcode)"; break;
					case BreakSource::BreakOnUnofficialOpCode: text = "unofficial opcode"; break;
					case BreakSource::BreakOnUnstableOpCode: text = "unstable opcode"; break;
					case BreakSource::BreakOnUninitMemoryRead: text = "uninitialized memory read"; break;
					default: break;
				}
			}
			_callbacks.onStopped(reason, text, pc);
			break;
		}
		case ConsoleNotificationType::DebuggerResumed:
			if(_callbacks.onResumed) {
				_callbacks.onResumed();
			}
			break;
		default:
			break;
	}
}

void MesenHost::OnMessage(const std::string& title, const std::string& message)
{
	if(_callbacks.onMessage) {
		_callbacks.onMessage(title, message);
	}
}
