/*
 * nessession -- the toolkit-agnostic desktop session for FujiNet Go NES.
 *
 * Owns the emulator (MesenCE's core, on Mesen's own emulation thread -- see
 * core/mesen/MesenHost.h), the FujiNet cartridge, the SDL audio and gamepad
 * backends, the in-process FujiNet runtime, the shared settings store, the
 * remappable key/pad bindings and the media path layout. Frontends (GTK4,
 * Qt6, AppKit, Win32) drive this API and do only windowing, painting and
 * event translation. A frontend that needs something which is not one of
 * those three things belongs here instead.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NESSESSION_H
#define NESSESSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The PPU paints 256x240. Frontends show it at 4:3 (8:7 pixels) or with
 * square pixels, per the "aspect" setting. */
#define NESSESSION_FB_WIDTH      256
#define NESSESSION_FB_MAX_HEIGHT 240

/* FujiNet's BoIP listener and its web admin UI. High ports of this app's own
 * so a standalone fujinet-pc, or a sibling FujiNet Go app, never collides:
 * ADAM uses 65216/65214, Apple II 1985/8000, CoCo 65504, MSX 65505/64003,
 * Intellivision 65503/64003, Astrocade 11500/11501, ColecoVision
 * 11502/11503, Atari 2600 11504/11505, and the NES work's own MAME dev
 * harness uses 9995.
 *
 * Direction: FujiNet LISTENS and the NES FujiNet cartridge dials in, as on
 * every sibling. That decides startup ordering -- see nessession_start. */
#define NESSESSION_BOIP_PORT  11506
#define NESSESSION_WEBUI_PORT 11507

/* The host audio device rate; Mesen's mixer resamples the APU to this. */
#define NESSESSION_AUDIO_RATE 48000

/* The accent colour every frontend uses for its highlights (the Map target,
 * the debugger's current line, the FujiNet status dot): the NES logo's red.
 * (The icon is in the console's two greys; see tools/icons/make-icons.py.) */
#define NESSESSION_ACCENT_RGB 0xC4001E

typedef struct nessession nessession;
typedef struct nesdebug nesdebug;

/* All members optional (NULL = default).
 *  config_dir:  default $XDG_CONFIG_HOME/fujinet-go-nes
 *  data_dir:    default $XDG_DATA_HOME/fujinet-go-nes
 *  fujinet_lib: path to libfujinet.so/.dylib/.dll; default searches
 *               $FUJINET_LIB, the executable's directory, the install
 *               libdir, then tools/fujinet/work/out. "" disables FujiNet.
 *  fujinet_runtime_src: directory holding the pristine fnconfig.ini + data/
 *               + SD/ used to provision the user's runtime tree on first
 *               start (a macOS app passes its bundle's runtime dir). */
typedef struct {
    const char *config_dir;
    const char *data_dir;
    const char *fujinet_lib;
    const char *fujinet_runtime_src;
} nessession_paths;

nessession *nessession_new(const nessession_paths *paths);
void nessession_free(nessession *s);

/* ---- settings (shared INI; one store for every frontend of this target) --- */
int         nessession_get_int(nessession *s, const char *key, int def);
void        nessession_set_int(nessession *s, const char *key, int value);
const char *nessession_get_str(nessession *s, const char *key,
                               const char *def);
void        nessession_set_str(nessession *s, const char *key,
                               const char *value);
void        nessession_settings_flush(nessession *s);

/* ---- machine options ------------------------------------------------------ */

/* Console region. AUTO follows the image's header (NTSC unless it says PAL). */
typedef enum {
    NES_REGION_AUTO = 0, NES_REGION_NTSC, NES_REGION_PAL, NES_REGION_DENDY,
    NES_REGION_COUNT
} nes_region;

/* What is plugged into a controller port. */
typedef enum {
    NES_CTRL_STANDARD = 0, NES_CTRL_NONE, NES_CTRL_COUNT
} nes_ctrl_type;

/* Human-readable, NULL past the end (for filling combo boxes). */
const char *nes_region_name(int r);
const char *nes_ctrl_type_name(int t);

/* ---- lifecycle ------------------------------------------------------------ */
typedef struct {
    const char *cart_path;    /* cartridge image; NULL boots the CONFIG client */
    int region;               /* nes_region */
    int port_type[2];         /* nes_ctrl_type per port (0 = player 1) */
    int analog_joystick;      /* gamepad sticks drive the D-pad too */
    int keyboard;             /* nes_keyboard on the expansion port */
    int enable_fujinet;       /* start the in-process FujiNet runtime */
    int enable_audio;         /* open the SDL audio device */
    int enable_gamepad;       /* start the SDL gamepad thread */
} nessession_start_opts;

/* Fills opts from the settings store (keys: cart region port0_type
 * port1_type analog_joystick keyboard enable_fujinet enable_audio
 * enable_gamepad). */
void nessession_default_opts(nessession *s, nessession_start_opts *opts);

/* Starts FujiNet (if enabled) and then the emulator.
 *
 * The order is not arbitrary: FujiNet listens and the cartridge dials in, so
 * the listener has to exist before the machine's first transaction or the
 * CONFIG client boots reporting no link. start() brings FujiNet up first and
 * waits briefly for the port.
 *
 * Returns 0, or -1 with nessession_last_error() set. FujiNet failing to
 * start is NOT fatal: the machine boots with the cartridge reporting the
 * link down, which is far more useful than refusing to run. */
int  nessession_start(nessession *s, const nessession_start_opts *opts);
void nessession_stop(nessession *s);
int  nessession_is_running(const nessession *s);
const char *nessession_last_error(const nessession *s);

/* ---- cartridges ------------------------------------------------------------
 * The FujiNet cartridge is always in the slot; CONFIG is its resident image.
 *
 * load_cart puts a local .nes image into the cartridge's SRAMs directly, in
 * place of CONFIG (the cartridge's own mapper engine runs it; an image whose
 * mapper it does not support is refused with a reason). The path is
 * remembered in the "cart" setting, so it boots again next start.
 * Returns 0 or -1 + error. */
int  nessession_load_cart(nessession *s, const char *path);
const char *nessession_cart_path(const nessession *s);
/* 1 if the image can run on the FujiNet cartridge; else 0 and why. */
int  nessession_check_cart(const char *path, char *why, int whysz);

/* Reset Game: the console's RESET button. The cartridge has no reset line,
 * so whatever it holds -- CONFIG, a network-booted game, an opened
 * cartridge -- restarts from its reset vector. */
int  nessession_reset_game(nessession *s);
/* Reset to CONFIG: a power cycle of the cartridge. Ejects an opened
 * cartridge (clears the "cart" setting) and boots CONFIG through the
 * cartridge's loader, exactly as at power-on. Escape by default. */
int  nessession_reset_to_config(nessession *s);
/* Eject: the same as Reset to CONFIG (there is no "no cartridge" state
 * worth having on a FujiNet cartridge). */
int  nessession_eject(nessession *s);

/* ---- video ---------------------------------------------------------------
 * copy_frame copies the latest frame into dst (FB_WIDTH*FB_MAX_HEIGHT uint32
 * XRGB8888 pixels) iff its serial differs from *serial_inout, updates it,
 * writes the frame's line count into *height and returns 1; returns 0 when
 * unchanged, leaving dst alone. Pass 0 to force a copy (e.g. the first paint
 * after a window map). */
int  nessession_copy_frame(nessession *s, uint32_t *dst, int *height,
                           uint64_t *serial_inout);
/* 60 or 50: the running region's refresh rate (rounded). */
int  nessession_refresh_rate(nessession *s);

/* Feed the UI's frame-clock ticks (CLOCK_MONOTONIC ns). While a steady
 * stream near the console's refresh rate arrives, the emulator phase-locks
 * one frame per tick; otherwise Mesen paces on the wall clock. A frontend
 * with no frame clock simply never calls this. */
void nessession_notify_vsync(nessession *s, int64_t frame_time_ns);

/* ---- audio ---------------------------------------------------------------
 * Owned by the session (SDL) when opts.enable_audio was set. A frontend
 * that wants the device itself can pull interleaved stereo float frames at
 * NESSESSION_AUDIO_RATE instead. Returns the frames written (silence is
 * written for any shortfall). */
int  nessession_render_audio(nessession *s, float *out, int nframes);
void nessession_set_volume(nessession *s, int percent);

/* ---- input ---------------------------------------------------------------
 * Every control the machine has, flattened into one target index so the
 * bindings table, the settings store and the frontends can all name them:
 * per port the standard controller, then the console's RESET button, then
 * the session's own actions. */
typedef enum {
    NES_ACT_UP = 0, NES_ACT_DOWN, NES_ACT_LEFT, NES_ACT_RIGHT,
    NES_ACT_A, NES_ACT_B, NES_ACT_SELECT, NES_ACT_START,
    NES_ACT_TURBO_A, NES_ACT_TURBO_B,
    NES_ACT_PER_PORT
} nes_action;

typedef enum {
    NES_SW_RESET = 0,                 /* Backspace by default: Reset Game */
    NES_SW_COUNT
} nes_switch;

typedef enum {
    NES_SYSACT_RESET_CONFIG = 0,      /* Escape by default */
    NES_SYSACT_PAUSE,                 /* stop in the debugger */
    NES_SYSACT_COUNT
} nes_sysaction;

#define NES_TARGET_PORT(port, act) ((port) * NES_ACT_PER_PORT + (act))
#define NES_TARGET_SWITCH(sw)      (2 * NES_ACT_PER_PORT + (sw))
#define NES_TARGET_SYSACT(sa)      (2 * NES_ACT_PER_PORT + NES_SW_COUNT + (sa))
#define NES_TARGET_COUNT           (2 * NES_ACT_PER_PORT + NES_SW_COUNT + NES_SYSACT_COUNT)

/* Apply one control directly. `down` is press/release. The RESET button
 * resets on press. */
void nessession_press(nessession *s, int target, int down);
void nessession_sysaction(nessession *s, int sysact);
/* Release everything the keyboard holds (focus loss). */
void nessession_release_all(nessession *s);
/* What the console sees held on a port right now (keyboard, gamepads and
 * the on-screen controller together), one bit per nes_action: bit
 * NES_ACT_UP ... bit NES_ACT_TURBO_B. For a controller window's highlights. */
unsigned nessession_buttons_held(nessession *s, int port);

/* ---- keyboard translation ------------------------------------------------
 * keysym is an X11/xkb keysym (== a GDK keyval; Qt, Win32 and AppKit map
 * through the HID tables below), so one bindings table serves every
 * frontend. Returns 1 if the key drives a target (and has been applied /
 * released), 0 if it should be ignored. System actions are reported through
 * nessession_key_sysaction instead and left to the frontend. */
int  nessession_key(nessession *s, uint32_t keysym, int down);
/* The system action a keysym is bound to, or -1. */
int  nessession_key_sysaction(nessession *s, uint32_t keysym);

/* Non-printing keys in the keysym space the frontends translate to. */
enum {
    NES_KEYSYM_NONE = 0,
    NES_KEYSYM_UP = 0xff52, NES_KEYSYM_DOWN = 0xff54,
    NES_KEYSYM_LEFT = 0xff51, NES_KEYSYM_RIGHT = 0xff53,
    NES_KEYSYM_ESCAPE = 0xff1b, NES_KEYSYM_RETURN = 0xff0d,
    NES_KEYSYM_BACKSPACE = 0xff08, NES_KEYSYM_TAB = 0xff09,
    NES_KEYSYM_SPACE = 0x20,
    NES_KEYSYM_F1 = 0xffbe, NES_KEYSYM_F2, NES_KEYSYM_F3, NES_KEYSYM_F4,
    NES_KEYSYM_F5, NES_KEYSYM_F6, NES_KEYSYM_F7, NES_KEYSYM_F8,
    NES_KEYSYM_F9, NES_KEYSYM_F10, NES_KEYSYM_F11, NES_KEYSYM_F12,
    NES_KEYSYM_LSHIFT = 0xffe1, NES_KEYSYM_RSHIFT = 0xffe2,
    NES_KEYSYM_LCTRL = 0xffe3, NES_KEYSYM_RCTRL = 0xffe4,
    NES_KEYSYM_LALT = 0xffe9, NES_KEYSYM_RALT = 0xffea,
    NES_KEYSYM_KP_0 = 0xffb0, NES_KEYSYM_KP_1, NES_KEYSYM_KP_2,
    NES_KEYSYM_KP_3, NES_KEYSYM_KP_4, NES_KEYSYM_KP_5, NES_KEYSYM_KP_6,
    NES_KEYSYM_KP_7, NES_KEYSYM_KP_8, NES_KEYSYM_KP_9,
    NES_KEYSYM_KP_ENTER = 0xff8d, NES_KEYSYM_KP_MULTIPLY = 0xffaa,
    NES_KEYSYM_KP_DIVIDE = 0xffaf, NES_KEYSYM_KP_PERIOD = 0xffae,
    NES_KEYSYM_SCROLL_LOCK = 0xff14
};
/* Native key codes for the platforms whose toolkits do not deliver keysyms:
 * Windows scan code (set 1, with the E0 flag), Linux evdev code (GTK/Qt
 * keycode minus 8) and macOS virtual key code, each mapped to a keysym. 0
 * when unknown. */
uint32_t nessession_keysym_from_win_scancode(unsigned scancode, int extended);
uint32_t nessession_keysym_from_evdev(unsigned code);
uint32_t nessession_keysym_from_macos_keycode(unsigned keycode);
/* Name for a keysym ("F1", "Space", "a", "Keypad 5"); returns length. */
int nessession_keysym_name(uint32_t keysym, char *dst, int dstsz);

/* ---- remappable bindings --------------------------------------------------
 * Every target can be driven by one keyboard key and one gamepad button.
 * Rebinding STEALS: a key drives exactly one target, because one keystroke
 * doing two things is worse than losing the old binding. Persisted in the
 * settings store under "bindings" as only the entries that differ from the
 * defaults. */
typedef enum {
    NES_PAD_BTN_NONE = -1,
    NES_PAD_BTN_SOUTH = 0, NES_PAD_BTN_EAST, NES_PAD_BTN_WEST,
    NES_PAD_BTN_NORTH, NES_PAD_BTN_BACK, NES_PAD_BTN_GUIDE,
    NES_PAD_BTN_START, NES_PAD_BTN_LEFT_STICK, NES_PAD_BTN_RIGHT_STICK,
    NES_PAD_BTN_LEFT_SHOULDER, NES_PAD_BTN_RIGHT_SHOULDER,
    NES_PAD_BTN_DPAD_UP, NES_PAD_BTN_DPAD_DOWN, NES_PAD_BTN_DPAD_LEFT,
    NES_PAD_BTN_DPAD_RIGHT,
    NES_PAD_BTN_LEFT_TRIGGER, NES_PAD_BTN_RIGHT_TRIGGER,
    NES_PAD_BTN_COUNT,
    /* Raw joystick bands for devices SDL has no gamepad mapping for (a
     * plain HID adapter): button index, and hat directions (hat*8 + dir). */
    NES_PAD_BTN_RAW_BASE = 64, NES_PAD_BTN_RAW_LAST = 127,
    NES_PAD_HAT_BASE = 192, NES_PAD_HAT_LAST = 255
} nes_pad_button;
#define NES_PAD_HAT_DIRS 8
#define NES_PAD_BTN_IS_NAMED(b) ((b) >= 0 && (b) < NES_PAD_BTN_COUNT)
#define NES_PAD_BTN_IS_RAW(b)   ((b) >= NES_PAD_BTN_RAW_BASE && (b) <= NES_PAD_BTN_RAW_LAST)
#define NES_PAD_BTN_IS_HAT(b)   ((b) >= NES_PAD_HAT_BASE && (b) <= NES_PAD_HAT_LAST)

typedef struct {
    uint32_t keysym;     /* 0 = no key */
    int      button;     /* nes_pad_button, NONE = no gamepad button */
} nes_binding;

const char *nes_target_name(int target);           /* "Player 1: Up" */
const char *nes_target_short_name(int target);     /* "Up" */
nes_binding nessession_binding_get(nessession *s, int target);
/* Bind; the previous holder of the key/button (if any) is described into
 * `stolen` (may be NULL). keysym 0 / button NONE unbinds. */
void nessession_binding_set_key(nessession *s, int target, uint32_t keysym,
                                char *stolen, int stolensz);
void nessession_binding_set_button(nessession *s, int target, int button,
                                   char *stolen, int stolensz);
void nessession_bindings_reset(nessession *s);
const char *nes_pad_button_name(int button);
/* The target a keysym / pad button drives, or -1. */
int nessession_target_for_key(nessession *s, uint32_t keysym);
int nessession_target_for_button(nessession *s, int port, int button);

/* Map mode: after begin(), the next gamepad button pressed on any pad is
 * reported by poll() (returns 1 and the button). cancel() disarms. */
void nessession_gamepad_capture_begin(nessession *s);
void nessession_gamepad_capture_cancel(nessession *s);
int  nessession_gamepad_capture_poll(nessession *s, int *button);

/* ---- controller types (live) ---------------------------------------------
 * Change what is plugged into a port without restarting -- a booted game
 * survives. Persisted as port0_type / port1_type. */
void nessession_set_port_type(nessession *s, int port, int type);
int  nessession_port_type(nessession *s, int port);
void nessession_set_analog(nessession *s, int joystick);

/* ---- region (live; takes effect at the next power cycle) ------------------*/
void nessession_set_region(nessession *s, int region);
int  nessession_region(nessession *s);

/* ---- the expansion port: keyboards and the Data Recorder ----------------
 * The Famicom's expansion port takes a keyboard: Nintendo's Family BASIC
 * Keyboard (HVC-007, 72 keys; its Data Recorder comes with it) or the Subor
 * keyboard (the Famiclone educational keyboard, 99 keys). Both are scanned
 * by the program through $4016/$4017. Persisted as "keyboard"; changing it
 * is live. */
typedef enum {
    NES_KBD_NONE = 0, NES_KBD_FAMILY_BASIC, NES_KBD_SUBOR, NES_KBD_COUNT
} nes_keyboard;
const char *nes_keyboard_name(int type);    /* NULL past the end */
void nessession_set_keyboard(nessession *s, int type);
int  nessession_keyboard(nessession *s);

/* Keyboard mode: while a keyboard is attached and the mode is on (it turns
 * on when one is attached), every key typed on the host goes to the
 * emulated keyboard -- by position, not by legend, so Shift and the NES
 * software decide what a key means -- instead of to the controller
 * bindings. Scroll Lock toggles the mode (nessession_key handles it, so
 * every frontend behaves alike); macOS, which has no Scroll Lock, uses a
 * menu item. Gamepads keep driving the controllers either way. */
void nessession_set_keyboard_mode(nessession *s, int on);
int  nessession_keyboard_mode(nessession *s);
/* 1 while keys go to the emulated keyboard: a frontend then forwards every
 * key (Escape, Backspace, Tab, F-keys included) to nessession_key and keeps
 * only its Ctrl/Cmd menu accelerators. */
int  nessession_keyboard_captures(nessession *s);

/* The on-screen keyboard. A layout is the keyboard's keys in key units
 * (x, y of the top-left corner; w, h), so every frontend draws the same
 * picture. `index` is what keyboard_press / keyboard_held take. Returns the
 * count; 0 for NES_KBD_NONE. */
typedef struct {
    const char *label;
    int index;
    float x, y, w, h;
} nes_kbd_key;
int  nessession_keyboard_layout(int type, const nes_kbd_key **keys);
/* Press/release a key on the attached keyboard (the mouse on the on-screen
 * keyboard). Held keys from the host keyboard and from here combine. */
void nessession_keyboard_press(nessession *s, int index, int down);
int  nessession_keyboard_held(nessession *s, int index);
/* The attached keyboard's key index for a host keysym, or -1: what
 * keyboard mode does with a key. */
int  nessession_keyboard_index_for_keysym(int type, uint32_t keysym);

/* The Family BASIC Data Recorder (with the Family BASIC keyboard only):
 * Play feeds a tape file to the program reading $4016; Record captures
 * what the program writes until Stop, which saves it. Tapes are raw bit
 * streams, by default in nessession_tapes_path(). Return 0 or -1 + error. */
typedef enum { NES_TAPE_IDLE = 0, NES_TAPE_PLAYING, NES_TAPE_RECORDING } nes_tape_state;
int  nessession_tape_play(nessession *s, const char *path);
int  nessession_tape_record(nessession *s, const char *path);
int  nessession_tape_stop(nessession *s);
int  nessession_tape_state(nessession *s);
const char *nessession_tapes_path(const nessession *s);

/* ---- gamepads (SDL, hotplugged; started by nessession_start) -------------
 * Pads are assigned to ports in connection order unless assigned
 * explicitly. A pad that disconnects and reconnects gets its port back. */
int  nessession_gamepad_count(nessession *s);
int  nessession_gamepad_name(nessession *s, int idx, char *dst, int dstsz);
void nessession_gamepad_assign(nessession *s, int idx, int port); /* -1 = auto */
int  nessession_gamepad_assignment(nessession *s, int idx);
int  nessession_gamepad_effective_port(nessession *s, int idx);
/* Bumped on every add/remove so a frontend can refresh its lists cheaply. */
unsigned nessession_gamepad_generation(nessession *s);
/* The last hot-plug event as text ("Connected: 8BitDo SN30 Pro (player 1)",
 * "Disconnected: ..."), for a toast; returns length, 0 if none yet. */
int  nessession_gamepad_last_event(nessession *s, char *dst, int dstsz);

/* ---- cross-thread system actions ------------------------------------------
 * The gamepad thread cannot call into a UI toolkit, so a system action it
 * resolves is posted here and a frontend's timer takes it. */
void nessession_sysaction_post(nessession *s, int sysact);
int  nessession_sysaction_take(nessession *s, int *out);

/* ---- FujiNet -------------------------------------------------------------*/
int         nessession_fujinet_running(const nessession *s);
const char *nessession_fujinet_webui_url(const nessession *s);
int         nessession_fujinet_copy_log(nessession *s, char *dst, int max);
/* The cartridge's link to FujiNet: 1 up, 0 down, -1 not running. */
int         nessession_cart_link_up(nessession *s);
/* Status text from the cartridge ("connected", "link down: ...", "loading
 * 42%", "game running; mailbox closed", ...). Returns length. */
int         nessession_cart_status(nessession *s, char *dst, int dstsz);
/* 1 once anything other than CONFIG is in the cartridge (a network boot or
 * an opened cartridge file). */
int         nessession_cart_booted_game(nessession *s);

/* ---- media ---------------------------------------------------------------
 * Import a cartridge into FujiNet's SD folder so CONFIG can boot it through
 * the cartridge. Returns 0 and the destination path, or -1 with the error. */
int  nessession_import_cart_to_sd(nessession *s, const char *src_path,
                                  char *dest_out, int dest_sz);
/* Routes a dropped file: cartridge images (.nes) to the cartridge directory
 * (the returned path is usable with load_cart), anything else to the
 * FujiNet SD folder. */
int  nessession_import_media(nessession *s, const char *src_path,
                             char *dest_out, int dest_sz);
int  nessession_media_is_cartridge(const char *path);

const char *nessession_config_path(const nessession *s);
const char *nessession_data_path(const nessession *s);
const char *nessession_carts_path(const nessession *s);
const char *nessession_sd_path(const nessession *s);

/* ---- debugger ------------------------------------------------------------*/
nesdebug *nessession_debugger(nessession *s);

#ifdef __cplusplus
}
#endif

#endif /* NESSESSION_H */
