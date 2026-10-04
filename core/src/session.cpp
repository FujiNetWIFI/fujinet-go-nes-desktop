/*
 * session.cpp -- the frontend contract (core/include/nessession.h) over the
 * Mesen host (core/mesen/MesenHost.h).
 *
 * C++ because it owns the MesenHost and talks to Mesen's objects; everything
 * it exposes is the plain C API, and the C modules (settings, paths, media,
 * audio, gamepads, bindings) share the struct through session_internal.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "MesenHost.h"
#include "NES/Mappers/Homebrew/FujiNetCart.h"

extern "C" {
#include "session_internal.h"
#include "nesdebug.h"
}

// debug.cpp
void nesdebug_note_stop(nessession* s, const char* msg, int addr);
void nesdebug_note_resume(nessession* s);
void nesdebug_destroy(nessession* s);
void nesdebug_before_load(nessession* s);
void nesdebug_after_load(nessession* s);

namespace {

MesenHost* host_of(struct nessession* s)
{
  return static_cast<MesenHost*>(s->host);
}

// The debugger attaches to whatever console is running; a power cycle
// replaces the console, so the debugger is told before and after.
template<typename F>
bool with_reload(struct nessession* s, F&& fn)
{
  nesdebug_before_load(s);
  const bool ok = fn();
  nesdebug_after_load(s);
  return ok;
}

} // namespace

// ---- helpers shared with the C modules -----------------------------------

extern "C" void session_set_error(struct nessession* s, const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s->last_error, sizeof s->last_error, fmt, ap);
  va_end(ap);
}

extern "C" void session_gamepad_event(struct nessession* s, const char* text)
{
  pthread_mutex_lock(&s->pad_event_mtx);
  snprintf(s->pad_event, sizeof s->pad_event, "%s", text ? text : "");
  pthread_mutex_unlock(&s->pad_event_mtx);
}

extern "C" int nessession_gamepad_last_event(nessession* s, char* dst, int dstsz)
{
  if(!dst || dstsz <= 0) return 0;
  pthread_mutex_lock(&s->pad_event_mtx);
  const int n = snprintf(dst, static_cast<size_t>(dstsz), "%s", s->pad_event);
  pthread_mutex_unlock(&s->pad_event_mtx);
  return n;
}

#ifdef NES_GAMEPAD_STUB
extern "C" int gamepad_start(struct nessession* s)
{
  session_set_error(s, "gamepad support not built");
  return -1;
}
extern "C" void gamepad_stop(struct nessession*) { }
extern "C" int nessession_gamepad_count(nessession*) { return 0; }
extern "C" int nessession_gamepad_name(nessession*, int, char* dst, int dstsz)
{
  if(dst && dstsz > 0) dst[0] = '\0';
  return 0;
}
extern "C" void nessession_gamepad_assign(nessession*, int, int) { }
extern "C" int nessession_gamepad_assignment(nessession*, int) { return -1; }
extern "C" int nessession_gamepad_effective_port(nessession*, int) { return -1; }
extern "C" unsigned nessession_gamepad_generation(nessession*) { return 0; }
extern "C" void nessession_gamepad_capture_begin(nessession*) { }
extern "C" void nessession_gamepad_capture_cancel(nessession*) { }
extern "C" int nessession_gamepad_capture_poll(nessession*, int*) { return 0; }
#endif

// ---- names -----------------------------------------------------------------

extern "C" const char* nes_region_name(int r)
{
  static const char* const names[NES_REGION_COUNT + 1] =
    { "Auto", "NTSC", "PAL", "Dendy", nullptr };
  return (r >= 0 && r < NES_REGION_COUNT) ? names[r] : nullptr;
}

extern "C" const char* nes_ctrl_type_name(int t)
{
  static const char* const names[NES_CTRL_COUNT + 1] =
    { "NES Controller", "None", nullptr };
  return (t >= 0 && t < NES_CTRL_COUNT) ? names[t] : nullptr;
}

// ---- lifecycle -------------------------------------------------------------

extern "C" nessession* nessession_new(const nessession_paths* paths)
{
  auto* s = static_cast<struct nessession*>(calloc(1, sizeof(struct nessession)));
  if(!s) return nullptr;

  pthread_mutex_init(&s->settings_mtx, nullptr);
  pthread_mutex_init(&s->sysact_mtx, nullptr);
  pthread_mutex_init(&s->pad_event_mtx, nullptr);

  if(paths_init(s, paths ? paths->config_dir : nullptr,
                paths ? paths->data_dir : nullptr) != 0)
  {
    nessession_free(s);
    return nullptr;
  }
  settings_init(s);
  bindings_init(s);

  snprintf(s->webui_url, sizeof s->webui_url, "http://127.0.0.1:%d/",
           NESSESSION_WEBUI_PORT);
  if(paths && paths->fujinet_lib)
    snprintf(s->fujinet_lib, sizeof s->fujinet_lib, "%s", paths->fujinet_lib);
  if(paths && paths->fujinet_runtime_src)
    snprintf(s->fujinet_runtime_src, sizeof s->fujinet_runtime_src, "%s",
             paths->fujinet_runtime_src);

  s->host = new MesenHost();
  return s;
}

extern "C" void nessession_free(nessession* s)
{
  if(!s) return;
  nessession_stop(s);
  nessession_settings_flush(s);
  settings_free_all(s);
  nesdebug_destroy(s);
  delete host_of(s);
  pthread_mutex_destroy(&s->sysact_mtx);
  pthread_mutex_destroy(&s->pad_event_mtx);
  free(s);
}

extern "C" void nessession_default_opts(nessession* s, nessession_start_opts* opts)
{
  memset(opts, 0, sizeof *opts);
  opts->region = nessession_get_int(s, "region", NES_REGION_AUTO);
  opts->port_type[0] = nessession_get_int(s, "port0_type", NES_CTRL_STANDARD);
  opts->port_type[1] = nessession_get_int(s, "port1_type", NES_CTRL_STANDARD);
  opts->analog_joystick = nessession_get_int(s, "analog_joystick", 1);
  opts->enable_fujinet = nessession_get_int(s, "enable_fujinet", 1);
  opts->enable_audio = nessession_get_int(s, "enable_audio", 1);
  opts->enable_gamepad = nessession_get_int(s, "enable_gamepad", 1);
  opts->cart_path = nessession_get_str(s, "cart", nullptr);
  if(opts->cart_path && !opts->cart_path[0])
    opts->cart_path = nullptr;
}

extern "C" int nessession_start(nessession* s, const nessession_start_opts* opts)
{
  nessession_start_opts local;

  if(s->running) return 0;
  s->last_error[0] = '\0';

  if(!opts)
  {
    nessession_default_opts(s, &local);
    opts = &local;
  }
  s->opts = *opts;
  if(opts->cart_path)
    snprintf(s->cart_path, sizeof s->cart_path, "%s", opts->cart_path);
  else
    s->cart_path[0] = '\0';
  s->opts.cart_path = s->cart_path[0] ? s->cart_path : nullptr;
  for(int p = 0; p < 2; p++)
    if(s->opts.port_type[p] < 0 || s->opts.port_type[p] >= NES_CTRL_COUNT)
      s->opts.port_type[p] = NES_CTRL_STANDARD;

  // FujiNet FIRST: it listens and the cartridge dials in, so the listener
  // has to exist before the machine's first transaction or the CONFIG
  // client boots reporting no link. Failing to start is NOT fatal (the
  // cartridge also keeps redialling).
  if(opts->enable_fujinet)
  {
    if(fujinet_start(s) == 0)
      fujinet_wait_for_boip(s, 3000);
  }

  MesenHost::Config cfg;
  cfg.homeDir = s->mesen_dir;
  cfg.fujinetHost = "127.0.0.1";
  cfg.fujinetPort = NESSESSION_BOIP_PORT;
  cfg.fujinetDebug = getenv("NES_FUJINET_DEBUG") != nullptr;
  cfg.audioRate = NESSESSION_AUDIO_RATE;
  cfg.volume = nessession_get_int(s, "volume", 100);
  cfg.region = s->opts.region;
  cfg.portType[0] = s->opts.port_type[0];
  cfg.portType[1] = s->opts.port_type[1];

  MesenHost::Callbacks cb;
  cb.onStopped = [s](MesenHost::StopReason, const std::string& msg, int addr) {
    nesdebug_note_stop(s, msg.c_str(), addr);
  };
  cb.onResumed = [s] { nesdebug_note_resume(s); };
  host_of(s)->SetCallbacks(cb);

  std::string err;
  if(!host_of(s)->Start(cfg, err))
  {
    session_set_error(s, "%s", err.c_str());
    fujinet_stop(s);
    return -1;
  }

  // A remembered cartridge that no longer loads (moved, or a mapper the
  // cartridge cannot run) must not leave the app unbootable: fall back to
  // CONFIG and say why.
  bool booted = false;
  if(s->cart_path[0])
  {
    if(host_of(s)->LoadCart(s->cart_path, err))
      booted = true;
    else
    {
      fprintf(stderr, "nes: %s: %s; booting CONFIG\n", s->cart_path, err.c_str());
      s->cart_path[0] = '\0';
      s->opts.cart_path = nullptr;
      nessession_set_str(s, "cart", "");
    }
  }
  if(!booted && !host_of(s)->LoadConfig(err))
  {
    session_set_error(s, "%s", err.c_str());
    host_of(s)->Stop();
    fujinet_stop(s);
    return -1;
  }

  s->running = 1;

  if(opts->enable_gamepad && gamepad_start(s) != 0)
  {
    fprintf(stderr, "nes: gamepads unavailable (%s)\n", s->last_error);
    s->last_error[0] = '\0';
  }
  if(opts->enable_audio && audio_start(s) != 0)
  {
    fprintf(stderr, "nes: audio unavailable (%s); continuing silent\n", s->last_error);
    s->last_error[0] = '\0';
  }
  return 0;
}

extern "C" void nessession_stop(nessession* s)
{
  if(!s->running) return;
  audio_stop(s);
  gamepad_stop(s);
  nesdebug_before_load(s);
  host_of(s)->Stop();
  fujinet_stop(s);
  s->running = 0;
}

extern "C" int nessession_is_running(const nessession* s) { return s->running; }
extern "C" const char* nessession_last_error(const nessession* s) { return s->last_error; }

// ---- cartridges ------------------------------------------------------------

extern "C" int nessession_check_cart(const char* path, char* why, int whysz)
{
  if(why && whysz > 0) why[0] = '\0';
  if(!path || !*path) return 0;
  std::ifstream in(path, std::ios::binary);
  if(!in)
  {
    if(why && whysz > 0) snprintf(why, static_cast<size_t>(whysz), "Cannot open %s", path);
    return 0;
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::string err;
  if(FujiNetCart::CheckImage(bytes.data(), static_cast<uint32_t>(bytes.size()), err))
    return 1;
  if(why && whysz > 0) snprintf(why, static_cast<size_t>(whysz), "%s", err.c_str());
  return 0;
}

extern "C" int nessession_load_cart(nessession* s, const char* path)
{
  if(!s->running || !path || !*path) return -1;
  char why[256];
  if(!nessession_check_cart(path, why, sizeof why))
  {
    session_set_error(s, "%s", why);
    return -1;
  }
  std::string err;
  const bool ok = with_reload(s, [&] { return host_of(s)->LoadCart(path, err); });
  if(!ok)
  {
    session_set_error(s, "%s", err.c_str());
    // whatever was running may be gone with the old console: put CONFIG back
    std::string err2;
    with_reload(s, [&] { return host_of(s)->LoadConfig(err2); });
    s->cart_path[0] = '\0';
    s->opts.cart_path = nullptr;
    return -1;
  }
  snprintf(s->cart_path, sizeof s->cart_path, "%s", path);
  s->opts.cart_path = s->cart_path;
  nessession_set_str(s, "cart", path);
  return 0;
}

extern "C" const char* nessession_cart_path(const nessession* s)
{
  return s->cart_path;
}

extern "C" int nessession_reset_game(nessession* s)
{
  if(!s->running) return -1;
  host_of(s)->Reset();
  return 0;
}

extern "C" int nessession_reset_to_config(nessession* s)
{
  if(!s->running) return -1;
  std::string err;
  const bool ok = with_reload(s, [&] { return host_of(s)->LoadConfig(err); });
  s->cart_path[0] = '\0';
  s->opts.cart_path = nullptr;
  nessession_set_str(s, "cart", "");
  if(!ok)
  {
    session_set_error(s, "%s", err.c_str());
    return -1;
  }
  return 0;
}

extern "C" int nessession_eject(nessession* s)
{
  return nessession_reset_to_config(s);
}

// ---- video / audio ---------------------------------------------------------

extern "C" int nessession_copy_frame(nessession* s, uint32_t* dst, int* height,
                                     uint64_t* serial_inout)
{
  if(!s->host) return 0;
  uint32_t w = 0, h = 0;
  if(!host_of(s)->CopyFrame(dst, static_cast<size_t>(NESSESSION_FB_WIDTH) * NESSESSION_FB_MAX_HEIGHT,
                            w, h, serial_inout))
    return 0;
  if(height) *height = static_cast<int>(std::min<uint32_t>(h, NESSESSION_FB_MAX_HEIGHT));
  return 1;
}

extern "C" int nessession_refresh_rate(nessession* s)
{
  if(!s->host) return 60;
  const double fps = host_of(s)->GetFps();
  return fps < 55.0 ? 50 : 60;
}

extern "C" void nessession_notify_vsync(nessession* s, int64_t frame_time_ns)
{
  if(s->host) host_of(s)->NotifyVsync(frame_time_ns);
}

extern "C" int nessession_render_audio(nessession* s, float* out, int nframes)
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

extern "C" void nessession_set_volume(nessession* s, int percent)
{
  if(percent < 0) percent = 0;
  if(percent > 100) percent = 100;
  nessession_set_int(s, "volume", percent);
  if(s->host) host_of(s)->SetVolume(percent);
}

// ---- input -----------------------------------------------------------------

extern "C" void nessession_press(nessession* s, int target, int down)
{
  if(!s->running || target < 0 || target >= NES_TARGET_COUNT) return;

  if(target < 2 * NES_ACT_PER_PORT)
  {
    host_of(s)->SetButton(target / NES_ACT_PER_PORT, target % NES_ACT_PER_PORT, down != 0);
    return;
  }
  target -= 2 * NES_ACT_PER_PORT;
  if(target < NES_SW_COUNT)
  {
    if(target == NES_SW_RESET && down)
      nessession_reset_game(s);
    return;
  }
  if(down)
    nessession_sysaction(s, target - NES_SW_COUNT);
}

extern "C" void nessession_sysaction(nessession* s, int sysact)
{
  switch(sysact)
  {
    case NES_SYSACT_RESET_CONFIG: nessession_reset_to_config(s); break;
    case NES_SYSACT_PAUSE:
      if(s->running)
      {
        nesdebug* d = nesdebug_get(s);
        nesdebug_attach(d);
        nesdebug_stop(d);
      }
      break;
    default: break;
  }
}

extern "C" void nessession_sysaction_post(nessession* s, int sysact)
{
  if(sysact < 0 || sysact >= NES_SYSACT_COUNT) return;
  pthread_mutex_lock(&s->sysact_mtx);
  s->sysact_pending |= 1u << sysact;
  pthread_mutex_unlock(&s->sysact_mtx);
}

extern "C" int nessession_sysaction_take(nessession* s, int* out)
{
  int found = 0;
  pthread_mutex_lock(&s->sysact_mtx);
  for(int i = 0; i < NES_SYSACT_COUNT; i++)
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

extern "C" unsigned nessession_buttons_held(nessession* s, int port)
{
  if(!s->running || port < 0 || port > 1) return 0;
  return host_of(s)->GetActions(port);
}

// The gamepad thread's entry point: the same routing as press, but never
// touching the settings or the debugger from that thread.
extern "C" void session_gamepad_apply(struct nessession* s, int port, int act, int down)
{
  if(!s->running || port < 0 || port > 1 || act < 0 || act >= NES_ACT_PER_PORT) return;
  host_of(s)->SetButton(port, act, down != 0);
}

// ---- controller types / region (live) --------------------------------------

extern "C" void nessession_set_port_type(nessession* s, int port, int type)
{
  if(port < 0 || port > 1 || type < 0 || type >= NES_CTRL_COUNT) return;
  s->opts.port_type[port] = type;
  nessession_set_int(s, port ? "port1_type" : "port0_type", type);
  if(s->running) host_of(s)->SetPortType(port, type);
}

extern "C" int nessession_port_type(nessession* s, int port)
{
  if(port < 0 || port > 1) return NES_CTRL_STANDARD;
  if(s->running) return s->opts.port_type[port];
  return nessession_get_int(s, port ? "port1_type" : "port0_type", NES_CTRL_STANDARD);
}

extern "C" void nessession_set_analog(nessession* s, int joystick)
{
  s->opts.analog_joystick = joystick ? 1 : 0;
  nessession_set_int(s, "analog_joystick", s->opts.analog_joystick);
}

extern "C" void nessession_set_region(nessession* s, int region)
{
  if(region < 0 || region >= NES_REGION_COUNT) return;
  s->opts.region = region;
  nessession_set_int(s, "region", region);
  if(s->running) host_of(s)->SetRegion(region);
}

extern "C" int nessession_region(nessession* s)
{
  if(s->running) return s->opts.region;
  return nessession_get_int(s, "region", NES_REGION_AUTO);
}

// ---- FujiNet ---------------------------------------------------------------

extern "C" int nessession_fujinet_running(const nessession* s)
{
  return s->fujinet_running;
}

extern "C" const char* nessession_fujinet_webui_url(const nessession* s)
{
  return s->webui_url;
}

extern "C" int nessession_cart_link_up(nessession* s)
{
  if(!s->running) return -1;
  FujiNetCartStatus st;
  if(!FujiNetCart::GetStatus(st)) return 0;
  return st.LinkUp ? 1 : 0;
}

extern "C" int nessession_cart_status(nessession* s, char* dst, int dstsz)
{
  if(!dst || dstsz <= 0) return 0;
  dst[0] = '\0';
  if(!s->running) return snprintf(dst, static_cast<size_t>(dstsz), "stopped");
  FujiNetCartStatus st;
  if(!FujiNetCart::GetStatus(st))
    return snprintf(dst, static_cast<size_t>(dstsz), "starting");
  if(st.Loading)
    return snprintf(dst, static_cast<size_t>(dstsz), "loading %d%%", st.LoadPct);
  if(st.BootedImage && !st.MailboxLive)
    return snprintf(dst, static_cast<size_t>(dstsz), "game running; mailbox closed");
  if(st.LinkUp)
    return snprintf(dst, static_cast<size_t>(dstsz), st.Busy ? "connected (busy)" : "connected");
  if(st.LinkError[0])
    return snprintf(dst, static_cast<size_t>(dstsz), "link down: %s", st.LinkError);
  return snprintf(dst, static_cast<size_t>(dstsz), "link down");
}

extern "C" int nessession_cart_booted_game(nessession* s)
{
  if(!s->running) return 0;
  FujiNetCartStatus st;
  return FujiNetCart::GetStatus(st) && st.BootedImage ? 1 : 0;
}

// ---- paths -----------------------------------------------------------------

extern "C" const char* nessession_config_path(const nessession* s) { return s->config_dir; }
extern "C" const char* nessession_data_path(const nessession* s) { return s->data_dir; }
extern "C" const char* nessession_carts_path(const nessession* s) { return s->carts_dir; }
extern "C" const char* nessession_sd_path(const nessession* s) { return s->fujinet_sd; }

// ---- debugger --------------------------------------------------------------

extern "C" nesdebug* nessession_debugger(nessession* s)
{
  return nesdebug_get(s);
}

// Reachable from the debugger module (core/src/debug.cpp) without a public
// C++ header: the host behind a session.
MesenHost* nessession_host(nessession* s)
{
  return host_of(s);
}
