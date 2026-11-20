/*
 * controls_test -- the layout every toolkit draws the Controllers window and
 * the expansion keyboards from, and its hit tests, pure: the extent never
 * depends on the port types, every part is inside it, the targets are the
 * binding targets, round parts are ellipses, POWER is no target, the cross
 * drag's dead zone and sectors, and the keyboard skins cover every key.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "nessession.h"

static int failures;
static void check(int pass, const char *what)
{
    printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

static int find_target(const nes_control *ctl, int num, int target, int kind)
{
    int idx;
    for (idx = 0; idx < num; idx++)
        if (ctl[idx].target == target && (kind < 0 || ctl[idx].kind == kind)) return idx;
    return -1;
}

static int find_label(const nes_control *ctl, int num, const char *label, int port)
{
    int idx;
    for (idx = 0; idx < num; idx++)
        if (!strcmp(ctl[idx].label, label) && ctl[idx].port == port) return idx;
    return -1;
}

static int count_kind(const nes_control *ctl, int num, int kind, int port)
{
    int idx, total = 0;
    for (idx = 0; idx < num; idx++)
        if (ctl[idx].kind == kind && ctl[idx].port == port) total++;
    return total;
}

static void centre(const nes_control *ctl, float *xpos, float *ypos)
{
    *xpos = ctl->x + ctl->w / 2;
    *ypos = ctl->y + ctl->h / 2;
}

#define STD NES_CTRL_STANDARD
#define NONE NES_CTRL_NONE

int main(void)
{
    static const int types[] = { NES_CTRL_STANDARD, NES_CTRL_NONE };
    const nes_control *ctl;
    float width, height, w2, h2, xpos, ypos;
    int num, n2, left, right, idx, port, hit, other;

    /* every type pair: the same extent, every part inside it */
    ctl = nessession_controls_layout(STD, STD, &num, &width, &height);
    check(num > 0 && fabsf(width - 33.0f) < 0.01f, "two 16 u pads and a 1 u gap are 33 u wide");
    check(fabsf(height - 12.8f) < 0.01f, "and 12.8 u tall");
    check(width == NESSESSION_CONTROLS_W && height == NESSESSION_CONTROLS_H, "the defines say the same");
    for (left = 0; left < 2; left++)
        for (right = 0; right < 2; right++) {
            ctl = nessession_controls_layout(types[left], types[right], &n2, &w2, &h2);
            check(n2 > 0 && w2 == width && h2 == height, "the extent never depends on the types");
            for (idx = 0; idx < n2; idx++)
                if (ctl[idx].x < -0.001f || ctl[idx].y < -0.001f ||
                    ctl[idx].x + ctl[idx].w > width + 0.001f || ctl[idx].y + ctl[idx].h > height + 0.001f) {
                    printf("part %d (%s) outside the extent\n", idx, ctl[idx].label);
                    check(0, "every part inside the extent");
                    break;
                }
        }
    ctl = nessession_controls_layout(-1, 99, &n2, &w2, &h2);
    check(n2 > 0 && count_kind(ctl, n2, NES_CTL_ROUND_BUTTON, 0) == 2 && count_kind(ctl, n2, NES_CTL_ROUND_BUTTON, 1) == 2,
          "an unknown type is drawn as a pad");

    /* a pad: the cross, four arms, B and A, SELECT / START, two turbo pills */
    ctl = nessession_controls_layout(STD, STD, &num, &width, &height);
    for (port = 0; port < 2; port++) {
        static const int arms[4] = { NES_ACT_UP, NES_ACT_DOWN, NES_ACT_LEFT, NES_ACT_RIGHT };
        int arm;
        check(count_kind(ctl, num, NES_CTL_DPAD, port) == 1, "one cross per pad");
        check(count_kind(ctl, num, NES_CTL_DPAD_ARM, port) == 4, "with four arms");
        check(count_kind(ctl, num, NES_CTL_ROUND_BUTTON, port) == 2, "two round buttons");
        check(count_kind(ctl, num, NES_CTL_PILL, port) == 4, "four pills: SELECT, START and two turbos");
        for (arm = 0; arm < 4; arm++)
            check(find_target(ctl, num, NES_TARGET_PORT(port, arms[arm]), NES_CTL_DPAD_ARM) >= 0, "an arm for each direction");
        idx = find_target(ctl, num, NES_TARGET_PORT(port, NES_ACT_B), -1);
        hit = find_target(ctl, num, NES_TARGET_PORT(port, NES_ACT_A), -1);
        check(idx >= 0 && hit >= 0 && ctl[idx].kind == NES_CTL_ROUND_BUTTON && ctl[idx].x < ctl[hit].x,
              "B is the left round button, A the right");
        check(ctl[idx].device == NES_CTRL_STANDARD && ctl[idx].port == port, "tagged as the port's pad");
        idx = find_target(ctl, num, NES_TARGET_PORT(port, NES_ACT_SELECT), NES_CTL_PILL);
        hit = find_target(ctl, num, NES_TARGET_PORT(port, NES_ACT_START), NES_CTL_PILL);
        check(idx >= 0 && hit >= 0 && ctl[idx].x < ctl[hit].x, "SELECT is left of START");
        idx = find_target(ctl, num, NES_TARGET_PORT(port, NES_ACT_TURBO_B), NES_CTL_PILL);
        hit = find_target(ctl, num, NES_TARGET_PORT(port, NES_ACT_TURBO_A), NES_CTL_PILL);
        check(idx >= 0 && hit >= 0 && ctl[idx].x < ctl[hit].x &&
              ctl[idx].y < ctl[find_target(ctl, num, NES_TARGET_PORT(port, NES_ACT_B), -1)].y,
              "turbo B is above B, turbo A above A");
    }
    /* every pad action is on the picture exactly once per port */
    for (port = 0; port < 2; port++) {
        int act;
        for (act = 0; act < NES_ACT_PER_PORT; act++) {
            int tgt = NES_TARGET_PORT(port, act), found = 0;
            for (idx = 0; idx < num; idx++) if (ctl[idx].target == tgt) found++;
            check(found == 1, "each pad action is listed once");
        }
    }
    /* the second pad is the first, shifted by one pad pitch */
    idx = find_target(ctl, num, NES_TARGET_PORT(0, NES_ACT_A), -1);
    hit = find_target(ctl, num, NES_TARGET_PORT(1, NES_ACT_A), -1);
    check(fabsf((ctl[hit].x - ctl[idx].x) - 17.0f) < 0.01f && ctl[hit].y == ctl[idx].y, "pad 2 is pad 1 plus 17 u");

    /* the console strip: RESET is the target; POWER is not; nothing is a system action */
    check(find_target(ctl, num, NES_TARGET_SWITCH(NES_SW_RESET), NES_CTL_BUTTON) >= 0, "RESET is a target");
    for (idx = 0; idx < num; idx++)
        if (ctl[idx].target >= NES_TARGET_SYSACT(0)) check(0, "no system action on the picture");
    check(find_target(ctl, num, NES_TARGET_SYSACT(NES_SYSACT_RESET_CONFIG), -1) < 0 &&
          find_target(ctl, num, NES_TARGET_SYSACT(NES_SYSACT_PAUSE), -1) < 0, "Reset to CONFIG and Stop are not drawn");
    idx = find_label(ctl, num, "POWER", -1);
    check(idx >= 0 && ctl[idx].target == -1 && ctl[idx].kind == NES_CTL_BODY, "POWER is drawn, no target");
    hit = find_label(ctl, num, "RESET", -1);
    check(hit >= 0 && ctl[hit].x > ctl[idx].x, "RESET is right of POWER");

    /* a port set to None: a socket and its words, nothing to press */
    ctl = nessession_controls_layout(NONE, STD, &num, &width, &height);
    check(count_kind(ctl, num, NES_CTL_DPAD, 0) == 0 && count_kind(ctl, num, NES_CTL_ROUND_BUTTON, 0) == 0 &&
          count_kind(ctl, num, NES_CTL_PILL, 0) == 0, "an empty port has nothing to press");
    check(find_target(ctl, num, NES_TARGET_PORT(0, NES_ACT_A), -1) < 0 &&
          find_target(ctl, num, NES_TARGET_PORT(0, NES_ACT_UP), -1) < 0, "and no port action is listed");
    idx = find_label(ctl, num, "No controller", 0);
    check(idx >= 0 && ctl[idx].kind == NES_CTL_CAPTION && ctl[idx].device == NES_CTRL_NONE, "the socket's words");
    check(find_label(ctl, num, "Socket", 0) >= 0, "the socket outline");
    check(count_kind(ctl, num, NES_CTL_ROUND_BUTTON, 1) == 2, "while port 2 is still a pad");
    check(nessession_controls_hit(NONE, STD, 8.0f, 4.0f) == -1, "the empty socket is no target");

    /* hit test: round parts are ellipses; pills are a little generous; bodies, captions, arms never hit */
    ctl = nessession_controls_layout(STD, STD, &num, &width, &height);
    idx = find_target(ctl, num, NES_TARGET_PORT(1, NES_ACT_A), -1);
    centre(&ctl[idx], &xpos, &ypos);
    check(nessession_controls_hit(STD, STD, xpos, ypos) == idx, "A's centre hits it");
    check(nessession_controls_hit(STD, STD, ctl[idx].x + 0.05f, ctl[idx].y + 0.05f) != idx,
          "its box corner is outside the circle");
    idx = find_target(ctl, num, NES_TARGET_PORT(0, NES_ACT_START), -1);
    centre(&ctl[idx], &xpos, &ypos);
    check(nessession_controls_hit(STD, STD, xpos, ypos) == idx, "START's centre hits it");
    check(nessession_controls_hit(STD, STD, ctl[idx].x - 0.2f, ypos) == idx, "a pill is hit a little outside the rubber");
    check(nessession_controls_hit(STD, STD, ctl[idx].x - 0.5f, ypos) != idx, "but not far outside it");
    idx = find_target(ctl, num, NES_TARGET_PORT(0, NES_ACT_UP), NES_CTL_DPAD_ARM);
    centre(&ctl[idx], &xpos, &ypos);
    other = nessession_controls_hit(STD, STD, xpos, ypos);
    check(other >= 0 && ctl[other].kind == NES_CTL_DPAD && ctl[other].port == 0, "an arm hits the cross, not the arm");
    check(nessession_controls_hit(STD, STD, 8.0f, 0.4f) == -1, "a caption is not a target");
    check(nessession_controls_hit(STD, STD, -1.0f, -1.0f) == -1, "outside is nothing");
    idx = find_target(ctl, num, NES_TARGET_SWITCH(NES_SW_RESET), -1);
    centre(&ctl[idx], &xpos, &ypos);
    check(nessession_controls_hit(STD, STD, xpos, ypos) == idx, "RESET is hit");
    idx = find_label(ctl, num, "POWER", -1);
    centre(&ctl[idx], &xpos, &ypos);
    check(nessession_controls_hit(STD, STD, xpos, ypos) == -1, "POWER is not pressable");
    idx = find_label(ctl, num, "Face", 0);
    centre(&ctl[idx], &xpos, &ypos);
    check(nessession_controls_hit(STD, STD, xpos, ypos) == -1 || ctl[nessession_controls_hit(STD, STD, xpos, ypos)].kind != NES_CTL_BODY,
          "a body is never hit");

    /* the cross drag: 0.4 u of nothing, then eight sectors, never opposed bits */
    check(nessession_dpad_from_vector(0.0f, 0.0f) == 0, "centre is nothing");
    check(nessession_dpad_from_vector(0.3f, 0.0f) == 0, "inside the dead zone");
    check(nessession_dpad_from_vector(0.5f, 0.0f) == (1u << NES_ACT_RIGHT), "just outside it: right");
    check(nessession_dpad_from_vector(-1.5f, 0.1f) == (1u << NES_ACT_LEFT), "left");
    check(nessession_dpad_from_vector(0.1f, -1.5f) == (1u << NES_ACT_UP), "up (y is down)");
    check(nessession_dpad_from_vector(0.0f, 1.5f) == (1u << NES_ACT_DOWN), "down");
    check(nessession_dpad_from_vector(1.5f, -1.5f) == ((1u << NES_ACT_RIGHT) | (1u << NES_ACT_UP)), "up-right");
    check(nessession_dpad_from_vector(-1.5f, 1.5f) == ((1u << NES_ACT_LEFT) | (1u << NES_ACT_DOWN)), "down-left");
    check(nessession_dpad_from_vector(1.5f, 0.4f) == (1u << NES_ACT_RIGHT), "a shallow angle is still just right");
    check(nessession_dpad_from_vector(1.5f, 0.9f) == ((1u << NES_ACT_RIGHT) | (1u << NES_ACT_DOWN)), "a steep one is a diagonal");
    check(nessession_dpad_from_vector(100.0f, 0.0f) == (1u << NES_ACT_RIGHT), "far away is the same");

    /* the colours the frontends share */
    check(NESSESSION_SHELL_RGB == 0xD8D6D0 && NESSESSION_BUTTON_RGB == 0xE0281C &&
          NESSESSION_HELD_RGB == 0xFFB04A && NESSESSION_FACE_RGB == 0x3A3A3C, "the shared colour defines");

    /* the keyboard skins: every key of the layout, once, inside the case */
    {
        int type;
        for (type = NES_KBD_FAMILY_BASIC; type <= NES_KBD_SUBOR; type++) {
            const nes_kbd_key *keys;
            const nes_skin_item *sk;
            int nkeys = nessession_keyboard_layout(type, &keys), nsk, key, caps = 0, ok = 1;
            int class_count[4] = { 0, 0, 0, 0 };
            sk = nessession_keyboard_skin_layout(type, &nsk, &width, &height);
            check(sk != NULL && nsk > nkeys && width > 15.0f && height > 7.0f, "the skin has a case and plates besides the keys");
            check(sk[0].kind == NES_SKIN_BODY && !strcmp(sk[0].label, "Case") && sk[0].w == width && sk[0].h == height,
                  "the first item is the case, filling the extent");
            for (idx = 0; idx < nsk; idx++) {
                if (sk[idx].x < -0.001f || sk[idx].y < -0.001f || sk[idx].x + sk[idx].w > width + 0.001f ||
                    sk[idx].y + sk[idx].h > height + 0.001f) ok = 0;
                if (sk[idx].kind == NES_SKIN_KEYCAP) { caps++; class_count[sk[idx].cap_class & 3]++; }
            }
            check(ok, "every item inside the case");
            check(caps == nkeys, "one cap per key of the layout");
            for (key = 0; key < nkeys; key++) {
                int found = 0;
                for (idx = 0; idx < nsk; idx++)
                    if (sk[idx].kind == NES_SKIN_KEYCAP && sk[idx].key == keys[key].index &&
                        !strcmp(sk[idx].label, keys[key].label)) found++;
                if (found < 1) ok = 0;
            }
            check(ok, "each layout key is on the skin with its legend");
            if (type == NES_KBD_FAMILY_BASIC)
                check(class_count[NES_CAP_RED] > 8 && class_count[NES_CAP_GREY] > 40 && class_count[NES_CAP_DARK] == 0,
                      "Family BASIC: red and grey caps");
            else
                check(class_count[NES_CAP_DARK] > 20 && class_count[NES_CAP_CREAM] > 40 && class_count[NES_CAP_RED] == 0,
                      "Subor: dark and cream caps");
            /* a key's centre hits it */
            for (idx = 0; idx < nsk; idx++)
                if (sk[idx].kind == NES_SKIN_KEYCAP) {
                    hit = nessession_keyboard_skin_hit(type, sk[idx].x + sk[idx].w / 2, sk[idx].y + sk[idx].h / 2);
                    if (hit != idx) { ok = 0; break; }
                }
            check(ok, "every cap's centre hits that cap");
            check(nessession_keyboard_skin_hit(type, 0.1f, 0.1f) == -1 &&
                  nessession_keyboard_skin_hit(type, -1.0f, 5.0f) == -1, "the case itself is no key");
        }
        check(nessession_keyboard_skin_layout(NES_KBD_NONE, &num, &width, &height) == NULL && num == 0,
              "no keyboard, no skin");
        check(nessession_keyboard_skin_hit(NES_KBD_NONE, 3.0f, 3.0f) == -1, "and nothing to hit");
    }

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
