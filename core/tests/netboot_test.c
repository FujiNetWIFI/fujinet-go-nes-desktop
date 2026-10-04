/*
 * netboot_test -- a network boot end to end, through the real in-process
 * FujiNet: the cartridge bring-up's fujiboot client (MOUNT_HOST ->
 * SET_DEVICE_FULLPATH -> MOUNT_IMAGE on host slot 0, "/hello.nes") runs as
 * the open cartridge, FujiNet pushes hello.nes back over the DBC stream, the
 * loader ROM copies it into the SRAMs slice by slice, and the new image
 * runs (hello.nes claims the mailbox, so it stays open). Then Reset to
 * CONFIG brings CONFIG back.
 *
 * Needs fujiboot.nes and hello.nes from fujinet-firmware's pico/nes/build
 * (./build.sh there): NES_TESTROM_DIR=/path/to/pico/nes/build. SKIPs (77)
 * without them or without the FujiNet runtime.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nessession.h"
#include "nesdebug.h"
#include "test_tmpdir.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int wait_status(nessession *s, const char *want, int timeout_ms)
{
    char st[160] = "";
    int waited = 0;
    while (waited < timeout_ms) {
        nessession_cart_status(s, st, sizeof st);
        if (strstr(st, want)) return 1;
        sleep_ms(50); waited += 50;
    }
    printf("  (status: %s)\n", st);
    return 0;
}

static long file_size(const char *p)
{
    FILE *f = fopen(p, "rb");
    long n;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fclose(f);
    return n;
}

int main(void)
{
    const char *dir = getenv("NES_TESTROM_DIR");
    char cfg[512], data[512], boot[1024], hello[1024], dest[1200];
    nessession_paths p;
    nessession *s;
    nessession_start_opts o;

    if (!dir) {
        printf("SKIP: set NES_TESTROM_DIR to fujinet-firmware's pico/nes/build\n");
        return 77;
    }
    snprintf(boot, sizeof boot, "%s/fujiboot.nes", dir);
    snprintf(hello, sizeof hello, "%s/hello.nes", dir);
    if (file_size(boot) <= 0 || file_size(hello) <= 0) {
        printf("SKIP: no fujiboot.nes / hello.nes in %s\n", dir);
        return 77;
    }

    test_tmpdir(cfg, sizeof cfg, "ncfg");
    test_tmpdir(data, sizeof data, "ndata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data;
    s = nessession_new(&p);
    if (!s) return 1;

    nessession_default_opts(s, &o);
    o.enable_audio = 0; o.enable_gamepad = 0; o.enable_fujinet = 1;
    check(nessession_start(s, &o) == 0, "session starts with FujiNet");
    if (!nessession_fujinet_running(s)) {
        printf("SKIP: no FujiNet runtime available\n");
        nessession_free(s);
        return 77;
    }

    /* the image to boot goes in the root of the SD host (host slot 0) */
    check(nessession_import_cart_to_sd(s, hello, dest, sizeof dest) == 0, "hello.nes imported to SD");

    check(nessession_load_cart(s, boot) == 0, "fujiboot opened as the cartridge");
    check(wait_status(s, "connected", 10000), "fujiboot keeps the mailbox and the link is up");
    {
        /* fujiboot and hello share their start-up code; watch a byte where
         * the two PRG images differ until the pushed one is in place */
        nesdebug *d = nessession_debugger(s);
        static uint8_t a[32784], b[32784];
        FILE *fa = fopen(boot, "rb"), *fb = fopen(hello, "rb");
        size_t na = fa ? fread(a, 1, sizeof a, fa) : 0, nb = fb ? fread(b, 1, sizeof b, fb) : 0;
        long off = -1, i;
        int waited = 0, done = 0;
        nesdebug_cart c;
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        for (i = 16; i < (long)na && i < (long)nb && i < 16 + 32768; i++)
            if (a[i] != b[i]) { off = i; break; }
        check(off >= 0, "the two test images differ");
        while (off >= 0 && waited < 60000 && !done) {
            uint8_t v = 0;
            nesdebug_cart_get(d, &c);
            if (c.booted_image && !c.loading) {
                nesdebug_attach(d);
                nesdebug_read(d, (uint16_t)(0x8000 + off - 16), &v, 1);
                nesdebug_detach(d);
                done = v == b[off];
            }
            if (!done) { sleep_ms(100); waited += 100; }
        }
        check(done, "the pushed image was loaded into the PRG SRAM and runs");
        nesdebug_cart_get(d, &c);
        check(c.booted_image && !c.loading && c.mapper == 0, "the cartridge holds the new image");
        check(c.mailbox_live, "hello.nes claims the mailbox, so it stays open");
    }

    check(nessession_reset_to_config(s) == 0, "reset to CONFIG");
    check(wait_status(s, "connected", 10000), "CONFIG is back, link up");
    check(!nessession_cart_booted_game(s), "and nothing booted");

    nessession_stop(s);
    nessession_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
