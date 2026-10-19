/*
 * fujitrace_test -- the FujiNet tab's packet decoder with synthetic bytes:
 * SLIP frames both ways, escapes, reads split anywhere, a send dropping a
 * half-read reply, repeats folding into xN, short packets, the ring's cap
 * and the empty state. No emulator, no ROM, no network.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <cstdio>
#include <cstring>
#include <vector>

#include "fuji_trace.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static nesdebug_fuji_packet p[FujiTrace::Max];

/* READ HOST SLOTS: device 70, command F4, length 6, checksum, no params */
static const uint8_t kRequest[] = { 0xC0, 0x70, 0xF4, 0x06, 0x00, 0x7A, 0x00, 0xC0 };
/* its ACK, with a C0 and a DB in the payload (escaped on the wire) */
static const uint8_t kReply[] = { 0xC0, 0x70, 0x06, 0x0A, 0x00, 0x11, 0x00,
                                  0x41, 0xDB, 0xDC, 0xDB, 0xDD, 0x42, 0xC0 };

int main(void)
{
    FujiTrace t;
    int n;

    check(t.Copy(p, FujiTrace::Max) == 0, "empty: no packets");
    check(t.Total() == 0, "empty: nothing decoded");

    /* a request, whole */
    t.Feed(1, kRequest, sizeof kRequest);
    n = t.Copy(p, FujiTrace::Max);
    check(n == 1, "a request is one packet");
    check(p[0].to_host == 0 && p[0].device == 0x70 && p[0].command == 0xF4 && p[0].length == 6,
          "its direction and FujiBus header");
    check(p[0].nbytes == 6 && p[0].repeat == 1, "six bytes, once");
    check(strstr(p[0].text, "NES \xe2\x86\x92 FujiNet 70 F4 READ HOST SLOTS (6 bytes)") != NULL,
          "named from the firmware's command table");

    /* the reply, split mid-escape and mid-frame */
    t.Feed(0, kReply, 3);
    t.Feed(0, kReply + 3, 6);        /* ends on the DB of DB DC */
    check(t.Copy(p, FujiTrace::Max) == 1, "a partial reply is not a packet yet");
    t.Feed(0, kReply + 9, sizeof kReply - 9);
    n = t.Copy(p, FujiTrace::Max);
    check(n == 2 && p[1].to_host == 1, "the reply arrives whole across three reads");
    check(p[1].nbytes == 10 && p[1].data[6] == 0x41 && p[1].data[7] == 0xC0 && p[1].data[8] == 0xDB &&
          p[1].data[9] == 0x42, "SLIP escapes decode");
    check(strstr(p[1].text, "FujiNet \xe2\x86\x92 NES 70 06 ACK (10 bytes)") != NULL, "the reply's line");
    check(p[1].serial > p[0].serial, "serials rise");

    /* the same exchange twice more: still two entries, x3 each */
    {
        uint64_t last = p[1].serial;
        t.Feed(1, kRequest, sizeof kRequest);
        t.Feed(0, kReply, sizeof kReply);
        t.Feed(1, kRequest, sizeof kRequest);
        t.Feed(0, kReply, sizeof kReply);
        n = t.Copy(p, FujiTrace::Max);
        check(n == 2, "a repeated exchange folds into the one before");
        check(p[0].repeat == 3 && p[1].repeat == 3, "and counts it");
        check(strstr(p[0].text, "\xc3\x97" "3") != NULL && strstr(p[1].text, "\xc3\x97" "3") != NULL,
              "the lines say x3");
        check(p[1].serial > last, "a fold changes the last serial");
        check(t.Total() == 6, "every packet is still counted");
    }

    /* an exchange repeats request first: the reply before it is not part of it */
    {
        static const uint8_t ssid[] = { 0xC0, 0x70, 0xFE, 0x06, 0x00, 0x75, 0x00, 0xC0 };
        t.Clear();
        t.Feed(1, kRequest, sizeof kRequest);
        t.Feed(0, kReply, sizeof kReply);
        t.Feed(1, ssid, sizeof ssid);
        t.Feed(0, kReply, sizeof kReply);
        t.Feed(1, ssid, sizeof ssid);
        t.Feed(0, kReply, sizeof kReply);
        n = t.Copy(p, FujiTrace::Max);
        check(n == 4 && p[0].repeat == 1 && p[1].repeat == 1 && p[2].command == 0xFE &&
              p[2].repeat == 2 && p[3].to_host && p[3].repeat == 2,
              "request, reply, then (request, reply) x2");
        t.Feed(0, kReply, sizeof kReply);
        n = t.Copy(p, FujiTrace::Max);
        check(n == 5 && p[3].repeat == 2 && p[4].repeat == 1, "a stray reply does not join a counted exchange");
    }

    /* a lone repeat of one packet folds too; a different one does not */
    t.Clear();
    t.Feed(1, kRequest, sizeof kRequest);
    t.Feed(0, kReply, sizeof kReply);
    t.Feed(1, kRequest, sizeof kRequest);
    t.Feed(0, kReply, sizeof kReply);
    t.Feed(1, kRequest, sizeof kRequest);
    t.Feed(0, kReply, sizeof kReply);
    {
        static const uint8_t other[] = { 0xC0, 0x70, 0xE8, 0x06, 0x00, 0x5E, 0x00, 0xC0 };
        t.Feed(1, other, sizeof other);
        t.Feed(1, other, sizeof other);
        n = t.Copy(p, FujiTrace::Max);
        check(n == 3 && p[2].command == 0xE8 && p[2].repeat == 2, "one packet twice is x2");
        t.Feed(0, kReply, sizeof kReply);
        n = t.Copy(p, FujiTrace::Max);
        check(n == 4 && p[3].repeat == 1, "the same reply after another request is a new entry");
    }

    /* a send drops a half-read reply rather than gluing it to the next */
    {
        static const uint8_t junk[] = { 0xC0, 0x99, 0x98, 0x97 };
        t.Clear();
        t.Feed(0, junk, sizeof junk);
        t.Feed(1, kRequest, sizeof kRequest);
        t.Feed(0, kReply + 1, sizeof kReply - 1);   /* no leading C0 this time */
        n = t.Copy(p, FujiTrace::Max);
        check(n == 2 && p[1].to_host == 1 && p[1].nbytes == 10 && p[1].data[0] == 0x70,
              "the reply after a send starts afresh");
    }

    /* a packet too short for a header */
    {
        static const uint8_t shortp[] = { 0xC0, 0x01, 0x02, 0xC0 };
        t.Clear();
        t.Feed(0, shortp, sizeof shortp);
        n = t.Copy(p, FujiTrace::Max);
        check(n == 1 && p[0].device == -1 && p[0].command == -1 && p[0].length == -1, "short: no header");
        check(strstr(p[0].text, "FujiNet \xe2\x86\x92 NES 2 bytes") != NULL, "short: just a byte count");
    }

    /* long packets keep their first 64 bytes */
    {
        std::vector<uint8_t> big = { 0xC0, 0xFF, 0x57, 0x08, 0x02, 0x00, 0x00 };
        for (int i = 0; i < 514; i++) big.push_back((uint8_t)(i & 0x7F));
        big.push_back(0xC0);
        t.Clear();
        t.Feed(0, big.data(), big.size());
        n = t.Copy(p, FujiTrace::Max);
        check(n == 1 && p[0].nbytes == 520 && p[0].length == 0x208, "a push frame decodes whole");
        check(p[0].data[63] == 57, "its first 64 bytes are kept");
        check(strstr(p[0].text, "FF 57 WRITE (520 bytes)") != NULL, "an inbound push is named too");
    }

    /* the ring keeps the newest 512 (7-bit numbering: no C0 or DB inside) */
    {
        t.Clear();
        for (int i = 0; i < 600; i++) {
            const uint8_t f[] = { 0xC0, 0x70, 0xF4, 0x06, 0x00, (uint8_t)(i & 0x7F), (uint8_t)(i >> 7), 0xC0 };
            t.Feed(1, f, sizeof f);
        }
        n = t.Copy(p, FujiTrace::Max);
        check(n == (int)FujiTrace::Max, "the ring holds 512");
        check(p[0].data[4] == (88 & 0x7F) && p[0].data[5] == 88 >> 7 &&
              p[n - 1].data[4] == (599 & 0x7F) && p[n - 1].data[5] == 599 >> 7, "oldest first, the newest kept");
        n = t.Copy(p, 10);
        check(n == 10 && p[0].data[4] == (590 & 0x7F) && p[9].data[4] == (599 & 0x7F), "max takes the newest");
    }

    t.Clear();
    check(t.Copy(p, FujiTrace::Max) == 0, "cleared");

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
