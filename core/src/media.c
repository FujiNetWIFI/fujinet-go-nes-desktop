/*
 * media -- routing a file the user dropped on the window to the right place.
 *
 * Two destinations, and which one a file wants is not a matter of taste:
 *
 *   Cartridges (.nes) go to the cartridge directory and are opened onto
 *   the FujiNet cartridge directly, whose own mapper engine maps them.
 *
 *   Disk and tape images go to the FujiNet SD folder, because FujiNet is
 *   what serves them. Copying one into the cartridge directory would look
 *   like it worked and then fail to boot.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "session_internal.h"

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    return slash ? slash + 1 : path;
}

static int ext_is(const char *path, const char *const *exts)
{
    const char *dot = strrchr(base_name(path), '.');
    int i;
    if (!dot) return 0;
    for (i = 0; exts[i]; i++) {
        const char *a = dot + 1, *b = exts[i];
        while (*a && *b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
            if (ca != *b) break;
            a++; b++;
        }
        if (!*a && !*b) return 1;
    }
    return 0;
}

/* iNES / NES 2.0 images only: the FujiNet cartridge parses the header
 * itself, and a headerless .bin or .rom cannot be mapped. .fuji is a FujiNet
 * client image (the CONFIG ROM's own naming on some siblings; on the NES it
 * is an ordinary .nes file). */
static const char *const cart_exts[] = { "nes", "fuji", NULL };
static const char *const disk_exts[] = { "dsk", "ddp", "img", "atr", "po",
                                         "do", "d64", "cas", NULL };

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out;
    char buf[16384];
    size_t n;

    if (!in) return -1;
    out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            return -1;
        }
    }
    fclose(in);
    if (fclose(out) != 0) return -1;
    return 0;
}

int nessession_import_media(nessession *s, const char *src_path,
                               char *dest_out, int dest_sz)
{
    const char *name = base_name(src_path);
    const char *dir;

    if (!src_path || !*src_path) {
        session_set_error(s, "No file to import");
        return -1;
    }

    if (ext_is(src_path, cart_exts)) {
        dir = s->carts_dir;
    } else if (ext_is(src_path, disk_exts)) {
        /* The SD tree only exists once the FujiNet runtime has been
         * provisioned. Test the DIRECTORY, not just the path string: the
         * path is always computed, so a string check passes and the copy
         * then fails with "could not copy", which tells the user nothing
         * about the actual problem. */
        struct stat st;
        if (!s->fujinet_sd[0] ||
            stat(s->fujinet_sd, &st) != 0 || !S_ISDIR(st.st_mode)) {
            session_set_error(s,
                "%s is a disk image, which FujiNet serves -- but the FujiNet "
                "runtime is not available, so there is nowhere to put it.",
                name);
            return -1;
        }
        dir = s->fujinet_sd;
    } else {
        session_set_error(s,
            "Don't know what %s is. Cartridges are .nes images; disk "
            "images go to FujiNet's SD folder.", name);
        return -1;
    }

    snprintf(dest_out, (size_t)dest_sz, "%s/%s", dir, name);
    if (copy_file(src_path, dest_out) != 0) {
        session_set_error(s, "Could not copy %s to %s", name, dir);
        return -1;
    }
    return 0;
}

int nessession_media_is_cartridge(const char *path)
{
    return path ? ext_is(path, cart_exts) : 0;
}

/* Into the SD root, so CONFIG shows it at the top of the SD host with no
 * navigation. Any cartridge image goes, whatever its extension: FujiNet
 * serves it as bytes and the cartridge boots whatever its nesmap can map
 * (the runtime pushes .nes, .rom and .bin files to the cartridge). */
int nessession_import_cart_to_sd(nessession *s, const char *src_path,
                                   char *dest_out, int dest_sz)
{
    const char *name;
    struct stat st;

    if (!src_path || !*src_path) {
        session_set_error(s, "No file to import");
        return -1;
    }
    name = base_name(src_path);
    if (!s->fujinet_sd[0] ||
        stat(s->fujinet_sd, &st) != 0 || !S_ISDIR(st.st_mode)) {
        session_set_error(s,
            "The FujiNet runtime is not available, so there is no SD folder "
            "to import %s into.", name);
        return -1;
    }
    snprintf(dest_out, (size_t)dest_sz, "%s/%s", s->fujinet_sd, name);
    if (copy_file(src_path, dest_out) != 0) {
        session_set_error(s, "Could not copy %s to %s", name, s->fujinet_sd);
        return -1;
    }
    return 0;
}
