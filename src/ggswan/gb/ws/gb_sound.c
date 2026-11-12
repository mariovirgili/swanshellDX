/**
 * gbswan - the Game Boy's sound with the WonderSwan's four channels, once
 * a frame, from the sound registers as the program left them:
 *
 * - channels 1 and 2 (squares) are WS channels 1 and 2 with a wave of the
 *   square's duty; channel 3 is WS channel 3 with the Game Boy's own 32
 *   samples (the same size and depth); channel 4 (noise) is WS channel 4's
 *   noise;
 * - lengths, envelopes and channel 1's sweep are stepped here, at the
 *   Game Boy's 512 Hz in the mean (8.57 steps a frame);
 * - a channel is started when bit 7 of its fourth register is found set
 *   (the bit is then cleared: it reads 1 anyway).
 *
 * So everything is a frame late at most and no finer than a frame: two
 * starts of a channel inside a frame are one, a wave changed while it
 * plays is heard from the next frame, and what programs do by writing the
 * volume many times a frame (samples) is not heard. The noise's lowest
 * rates are out of the WS's reach (it goes down to 1.5 kHz) and its short
 * mode is 254 steps long for the Game Boy's 127.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include <string.h>
#include <wonderful.h>
#include <ws.h>
#include "gb.h"
#include "gb_ws.h"

#define WAVES ((uint8_t*) 0x1B00)       // the WS's four waves, 16 bytes each (GB-MEMORY-MAP.md)
#define NR(x) gb_io[x]                   // FF10h + ...: 10h-14h, 16h-19h, 1Ah-1Eh, 20h-23h, 24h-26h

typedef struct {
    uint8_t on;
    uint8_t vol;
    uint8_t env_timer;
    uint16_t length;
} chan_t;
static chan_t ch[4];
static uint8_t duty_set[2];             // the duty WS channels 1 and 2 have the wave of (FFh: none)
static uint8_t seq_step;
static uint16_t seq_acc;
static uint8_t sweep_timer, sweep_on;

static const uint8_t __far reg4[4] = { 0x14, 0x19, 0x1E, 0x23 };
static const uint8_t __far reg2[4] = { 0x12, 0x17, 0x1C, 0x21 };
static const uint8_t __far reg1[4] = { 0x11, 0x16, 0x1B, 0x20 };

static void set_duty(uint8_t c, uint8_t duty) {
    static const uint8_t __far high[4] = { 4, 8, 16, 24 };     // of 32: 12.5%, 25%, 50%, 75%
    if (duty_set[c] == duty) return;
    duty_set[c] = duty;
    uint8_t *w = WAVES + c * 16;
    for (uint8_t i = 0; i < 16; i++)
        w[i] = (i * 2 < high[duty] ? 0x0F : 0) | (i * 2 + 1 < high[duty] ? 0xF0 : 0);
}

static uint16_t sweep_next(void) {
    uint16_t f = NR(0x13) | (uint16_t) (NR(0x14) & 7) << 8;
    uint16_t d = f >> (NR(0x10) & 7);
    return (NR(0x10) & 8) ? f - d : f + d;
}

static void trigger(uint8_t c) {
    chan_t *k = &ch[c];
    uint8_t dac = c == 2 ? NR(0x1A) & 0x80 : NR(reg2[c]) & 0xF8;
    k->on = dac != 0;
    k->vol = NR(reg2[c]) >> 4;
    k->env_timer = NR(reg2[c]) & 7;
    k->length = c == 2 ? 256 - NR(0x1B) : 64 - (NR(reg1[c]) & 63);
    if (c == 0) {
        sweep_timer = (NR(0x10) >> 4) & 7;
        sweep_on = (NR(0x10) & 0x77) != 0;
        if ((NR(0x10) & 7) && sweep_next() > 2047) k->on = 0;
    }
}

static void step(void) {
    uint8_t s = seq_step++ & 7;
    if (!(s & 1)) {
        // lengths: 256 a second
        for (uint8_t c = 0; c < 4; c++)
            if ((NR(reg4[c]) & 0x40) && ch[c].length && --ch[c].length == 0) ch[c].on = 0;
    }
    if (s == 2 || s == 6) {
        // channel 1's sweep: 128 a second
        if (sweep_on && ch[0].on && ((NR(0x10) >> 4) & 7)) {
            if (sweep_timer) sweep_timer--;
            if (!sweep_timer) {
                sweep_timer = (NR(0x10) >> 4) & 7;
                uint16_t f = sweep_next();
                if (f > 2047) {
                    ch[0].on = 0;
                } else if (NR(0x10) & 7) {
                    NR(0x13) = f & 0xFF;
                    NR(0x14) = (NR(0x14) & 0xF8) | f >> 8;
                }
            }
        }
    }
    if (s == 7) {
        // envelopes: 64 a second
        for (uint8_t c = 0; c < 4; c++) {
            if (c == 2) continue;
            uint8_t e = NR(reg2[c]);
            if (!(e & 7)) continue;
            if (ch[c].env_timer) ch[c].env_timer--;
            if (!ch[c].env_timer) {
                ch[c].env_timer = e & 7;
                if ((e & 8) && ch[c].vol < 15) ch[c].vol++;
                else if (!(e & 8) && ch[c].vol) ch[c].vol--;
            }
        }
    }
}

// a volume (0-15) to the WS's left and right, by NR51's bits for the channel
static uint8_t pan(uint8_t c, uint8_t v) {
    uint8_t p = NR(0x25);
    return ((p >> (4 + c) & 1) ? v << 4 : 0) | ((p >> c & 1) ? v : 0);
}

void gb_sound_frame(void) {
    if (!(NR(0x26) & 0x80)) {
        // the sound off
        outportb(WS_SOUND_CH_CTRL_PORT, 0);
        for (uint8_t c = 0; c < 4; c++) ch[c].on = 0;
        NR(0x26) &= 0x80;
        return;
    }
    for (uint8_t c = 0; c < 4; c++) {
        if (NR(reg4[c]) & 0x80) {
            NR(reg4[c]) &= 0x7F;
            trigger(c);
        }
        // (the DAC turned off turns the channel off)
        if (c == 2 ? !(NR(0x1A) & 0x80) : !(NR(reg2[c]) & 0xF8)) ch[c].on = 0;
    }
    seq_acc += 8572;
    while (seq_acc >= 1000) {
        seq_acc -= 1000;
        step();
    }

    uint8_t ctrl = 0;
    // the squares: the Game Boy's 131072 / (2048 - x) Hz with the WS's
    // 96000 / (2048 - n): 2048 - n = (2048 - x) * 375 / 512
    for (uint8_t c = 0; c < 2; c++) {
        if (!ch[c].on) continue;
        uint8_t r3 = c ? 0x18 : 0x13;
        uint16_t x = NR(r3) | (uint16_t) (NR(r3 + 1) & 7) << 8;
        uint16_t per = (uint32_t) (2048 - x) * 375 >> 9;
        if (per < 2) continue;                  // (above what can be heard)
        set_duty(c, NR(reg1[c]) >> 6);
        outportw(c ? WS_SOUND_FREQ_CH2_PORT : WS_SOUND_FREQ_CH1_PORT, 2048 - per);
        outportb(c ? WS_SOUND_VOL_CH2_PORT : WS_SOUND_VOL_CH1_PORT, pan(c, ch[c].vol));
        ctrl |= 1 << c;
    }
    // the wave: 65536 / (2048 - x) Hz: 2048 - n = (2048 - x) * 375 / 256
    if (ch[2].on) {
        uint16_t x = NR(0x1D) | (uint16_t) (NR(0x1E) & 7) << 8;
        uint32_t per = (uint32_t) (2048 - x) * 375 >> 8;
        uint8_t lvl = (NR(0x1C) >> 5) & 3;
        if (per >= 2 && per <= 2047 && lvl) {
            // (the Game Boy's first sample is the high half of a byte, the WS's the low)
            for (uint8_t i = 0; i < 16; i++) {
                uint8_t b = NR(0x30 + i);
                WAVES[32 + i] = b << 4 | b >> 4;
            }
            outportw(WS_SOUND_FREQ_CH3_PORT, 2048 - (uint16_t) per);
            outportb(WS_SOUND_VOL_CH3_PORT, pan(2, lvl == 1 ? 15 : lvl == 2 ? 7 : 3));
            ctrl |= 4;
        }
    }
    // the noise: 524288 / d / 2^(s + 1) Hz, d = 0.5, 1, 2, ... 7: the WS's
    // noise steps at 3072000 / (2048 - n)
    if (ch[3].on) {
        uint8_t p = NR(0x22);
        uint8_t d = (p & 7) ? (p & 7) * 2 : 1, s = p >> 4;
        uint32_t per = s < 14 ? ((uint32_t) d << s) * 375 >> 6 : 2047;
        if (per < 1) per = 1;
        if (per > 2047) per = 2047;
        static uint8_t short_mode = 0xFF;
        if (short_mode != (p & 8)) {
            short_mode = p & 8;
            outportb(WS_SOUND_NOISE_CTRL_PORT, (short_mode ? WS_SOUND_NOISE_CTRL_LENGTH_254 : WS_SOUND_NOISE_CTRL_LENGTH_32767)
                | WS_SOUND_NOISE_CTRL_ENABLE | WS_SOUND_NOISE_CTRL_RESET);
        }
        outportw(WS_SOUND_FREQ_CH4_PORT, 2048 - (uint16_t) per);
        outportb(WS_SOUND_VOL_CH4_PORT, pan(3, ch[3].vol));
        ctrl |= 0x88;
    }
    outportb(WS_SOUND_CH_CTRL_PORT, ctrl);
    // NR52's low bits: the channels playing
    NR(0x26) = (NR(0x26) & 0x80) | ch[0].on | ch[1].on << 1 | ch[2].on << 2 | ch[3].on << 3;
}

void gb_sound_init(void) {
    memset(ch, 0, sizeof(ch));
    memset(WAVES, 0, 64);
    duty_set[0] = duty_set[1] = 0xFF;
    seq_step = 0;
    seq_acc = 0;
    outportb(WS_SOUND_CH_CTRL_PORT, 0);
    outportb(WS_SOUND_WAVE_BASE_PORT, 0x1B00 >> 6);
    outportb(WS_SOUND_OUT_CTRL_PORT, WS_SOUND_OUT_CTRL_HEADPHONE_ENABLE
        | WS_SOUND_OUT_CTRL_SPEAKER_ENABLE | WS_SOUND_OUT_CTRL_SPEAKER_VOLUME_100);
    outportb(WS_SOUND_NOISE_CTRL_PORT, WS_SOUND_NOISE_CTRL_ENABLE | WS_SOUND_NOISE_CTRL_RESET);
}
