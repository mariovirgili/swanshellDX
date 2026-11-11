/**
 * sfswan, PC tools: typing on the SF-7000's keyboard from the environment
 * (sftest, wssim_sf).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef GGSWAN_SF_HOST_H_
#define GGSWAN_SF_HOST_H_
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/sf7000.h"

/*
 * Typing: SF_TYPE="300:FILES\n|700:LOAD \"ASTRO.BAS\"\n|1500:RUN\n":
 * from each frame number the text, a key every 8 frames (down for 4), with
 * SHIFT where the character needs it. \n is CR; \b BREAK.
 */
static const char *type_text;
static int type_pos, type_until = -1;
static void type_frame(int fr, const char *script) {
    for (const char *k = script; k && *k; ) {
        int kf = atoi(k);
        const char *col = strchr(k, ':');
        if (!col) break;
        if (kf == fr) { type_text = col + 1; type_pos = 0; type_until = fr; }
        const char *bar = strchr(col, '|');
        if (!bar) break;
        k = bar + 1;
    }
    if (!type_text)
        return;
    int t = fr - type_until;
    if (t % 8 == 0) {
        memset(sf_keys, 0, sizeof(sf_keys));
        char c = type_text[type_pos];
        if (!c || c == '|') { type_text = NULL; return; }
        if (c == '\\' && type_text[type_pos + 1]) {
            c = type_text[++type_pos] == 'n' ? '\r' : type_text[type_pos] == 'b' ? 1 : type_text[type_pos];
        }
        type_pos++;
        uint8_t key = c == 1 ? SF_K_BREAK : sf_char_key(c);
        if (key == 0xFF) { fprintf(stderr, "no key for '%c'\n", c); return; }
        sf_key_set(key & 0x7F, 1);
        if (key & SF_KEY_SHIFT_BIT)
            sf_key_set(SF_K_SHIFT, 1);
    } else if (t % 8 == 4) {
        memset(sf_keys, 0, sizeof(sf_keys));
    }
}
#endif
