/*
 * nessession's private state. Not installed; only the core/src sources
 * include it. Plain C so the C modules (settings, paths, media, audio,
 * gamepads, bindings) and the C++ session (session.cpp, which owns the
 * MesenHost) share one struct.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NES_SESSION_INTERNAL_H
#define NES_SESSION_INTERNAL_H

#include <pthread.h>
#include <stdint.h>

#include "nessession.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NES_PATH_MAX 1024

typedef struct setting_kv {
    char *key;
    char *val;
    struct setting_kv *next;
} setting_kv;

struct nessession {
    char config_dir[NES_PATH_MAX];
    char data_dir[NES_PATH_MAX];
    char carts_dir[NES_PATH_MAX];
    char mesen_dir[NES_PATH_MAX];    /* <data>/mesen/ -- Mesen's home folder */
    char settings_file[NES_PATH_MAX];

    setting_kv *settings;
    pthread_mutex_t settings_mtx;
    int settings_dirty;

    char last_error[256];

    /* ---- FujiNet runtime (fujinet_runtime.c) ---- */
    char fujinet_root[NES_PATH_MAX];    /* <data>/fujinet */
    char fujinet_config[NES_PATH_MAX];  /* .../fnconfig.ini */
    char fujinet_sd[NES_PATH_MAX];      /* .../SD */
    char fujinet_data[NES_PATH_MAX];    /* .../data */
    char fujinet_lib[NES_PATH_MAX];     /* resolved libfujinet path, "" until then */
    char fujinet_runtime_src[NES_PATH_MAX]; /* caller-given pristine tree, or "" */
    char webui_url[64];                   /* http://127.0.0.1:11507/ */
    int  fujinet_running;

    /* cross-thread system-action latch (see nessession_sysaction_post) */
    pthread_mutex_t sysact_mtx;
    unsigned sysact_pending;

    /* the running configuration */
    nessession_start_opts opts;
    char cart_path[NES_PATH_MAX];

    /* keys the keyboard currently holds, by target, so a release clears
     * exactly what its press asserted even if the binding changed meanwhile */
    uint32_t held_keysym[NES_TARGET_COUNT];

    void *host;               /* MesenHost*, session.cpp only */
    void *audio;              /* audio_sdl.c state, NULL until started */
    void *gamepad;            /* gamepad_sdl.c state, NULL until started */
    void *debugger;           /* nesdebug, lazily created */
    int running;

    /* the last gamepad hot-plug event, for a frontend toast */
    pthread_mutex_t pad_event_mtx;
    char pad_event[128];
};

void settings_init(struct nessession *s);
void settings_free_all(struct nessession *s);

int paths_init(struct nessession *s, const char *config_dir,
               const char *data_dir);
/* Locate libfujinet and provision the runtime tree (fnconfig.ini + data/ +
 * SD/) into <data>/fujinet on first run. Returns 0, or -1 if no runtime is
 * available (not fatal to the session -- see fujinet_start). */
int paths_provision_fujinet(struct nessession *s);

void session_set_error(struct nessession *s, const char *fmt, ...);

/* fujinet_runtime.c */
int  fujinet_start(struct nessession *s);
void fujinet_stop(struct nessession *s);
/* Block (up to timeout_ms) until the BoIP port accepts, so the emulator's
 * first dial-out finds the listener. Returns 0 once up, -1 on timeout. */
int  fujinet_wait_for_boip(struct nessession *s, int timeout_ms);

/* bindings.c */
void bindings_init(struct nessession *s);

/* audio_sdl.c */
int  audio_start(struct nessession *s);
void audio_stop(struct nessession *s);

/* gamepad_sdl.c */
int  gamepad_start(struct nessession *s);
void gamepad_stop(struct nessession *s);
/* Called by the gamepad thread with the button/axis state it resolved. */
void session_gamepad_apply(struct nessession *s, int port, int act, int down);
/* Called by the gamepad thread on a connect/disconnect. */
void session_gamepad_event(struct nessession *s, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* NES_SESSION_INTERNAL_H */
