/*
 * media_test -- where a dropped file goes, and the SD import.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "nessession.h"
#include "test_tmpdir.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static void write_file(const char *path, size_t n)
{
    FILE *f = fopen(path, "wb");
    size_t i;
    for (i = 0; i < n; i++) fputc((int)(i & 0xff), f);
    fclose(f);
}

int main(void)
{
    char cfg[512], data[512], src[700], dest[1200], sd[1200];
    nessession_paths p;
    nessession *s;

    test_tmpdir(cfg, sizeof cfg, "mcfg");
    test_tmpdir(data, sizeof data, "mdata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    s = nessession_new(&p);
    if (!s) return 1;

    check(nessession_media_is_cartridge("game.nes"), ".nes is a cartridge");
    check(nessession_media_is_cartridge("GAME.NES"), ".NES is a cartridge (case-insensitive)");
    check(nessession_media_is_cartridge("client.fuji"), ".fuji is a cartridge");
    check(!nessession_media_is_cartridge("game.bin"), "a headerless .bin is not");
    check(!nessession_media_is_cartridge("disk.atr"), ".atr is not");
    check(!nessession_media_is_cartridge("notes.txt"), ".txt is nothing");

    snprintf(src, sizeof src, "%s/smb.nes", cfg);
    write_file(src, 2048);
    check(nessession_import_media(s, src, dest, sizeof dest) == 0, "a cartridge imports");
    check(strncmp(dest, nessession_carts_path(s), strlen(nessession_carts_path(s))) == 0, "into the cartridge directory");
    check(exists(dest), "and the copy exists");

    snprintf(src, sizeof src, "%s/notes.txt", cfg);
    write_file(src, 10);
    check(nessession_import_media(s, src, dest, sizeof dest) == -1, "an unknown file is refused");
    check(strstr(nessession_last_error(s), "Don't know") != NULL, "with a useful message");

    /* the SD import needs the SD folder to exist -- there is no runtime
     * here, so make it by hand the way provisioning would */
    snprintf(src, sizeof src, "%s/zelda.nes", cfg);
    write_file(src, 4096);
    check(nessession_import_cart_to_sd(s, src, dest, sizeof dest) == -1, "SD import refuses when there is no SD folder");
    snprintf(sd, sizeof sd, "%s/fujinet", data); test_mkdir(sd);
    snprintf(sd, sizeof sd, "%s/fujinet/SD", data); test_mkdir(sd);
    check(nessession_import_cart_to_sd(s, src, dest, sizeof dest) == 0, "SD import copies into the SD root");
    snprintf(sd, sizeof sd, "%s/fujinet/SD/zelda.nes", data);
    check(strcmp(dest, sd) == 0 && exists(dest), "at the top of the SD tree");

    nessession_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
