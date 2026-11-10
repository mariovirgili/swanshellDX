// wssim: runs ws/main.c's translation layer on the PC, renders the WS
// screen it programs, and compares it with the reference renderer.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wsshim.h"
#include "../core/gg.h"
#include "../core/z80.h"

uint8_t iram[65536];
uint8_t ports[256];
static uint8_t psram[16 * 65536];
static uint8_t sram[8 * 65536];
static uint8_t fb[GG_VIEW_H][GG_VIEW_W][3];
static uint8_t wsfb[144][224][3];

void outportb(uint8_t p, uint8_t v) { ports[p] = v; }
void outportw(uint8_t p, uint16_t v) { ports[p] = v; ports[(uint8_t) (p + 1)] = v >> 8; }
uint8_t inportb(uint8_t p) { return ports[p]; }
uint16_t inportw(uint8_t p) { return ports[p] | (ports[(uint8_t) (p + 1)] << 8); }
static uint16_t bank(uint8_t p) { return inportw(p); }
void *MK_FP(uint16_t seg, uint16_t off) {
    switch (seg) {
    case 0x1000:
        if (ports[0xCE] & 1) return psram + (uint32_t) (bank(0xC1) & 15) * 65536 + off;
        return sram + (uint32_t) (bank(0xC1) & 7) * 65536 + off;
    case 0x2000: return psram + (uint32_t) (bank(0xC2) & 15) * 65536 + off;
    case 0x3000: return psram + (uint32_t) (bank(0xC3) & 15) * 65536 + off;
    default: return iram + seg * 16 + off;
    }
}
uint16_t ws_keypad_scan(void) { return 0; }

/* === Reference renderer (one line of the 256-pixel picture) === */

static inline uint8_t tile_pixel(uint16_t tile, uint8_t row, uint8_t col) {
    const uint8_t *p = gg_vram + ((tile * 32 + row * 4) & 0x3FFF);
    uint8_t bit = 7 - col;
    return ((p[0] >> bit) & 1) | (((p[1] >> bit) & 1) << 1)
        | (((p[2] >> bit) & 1) << 2) | (((p[3] >> bit) & 1) << 3);
}

static void render_line(uint16_t line) {
    uint8_t color[256];      // CRAM index 0..31
    uint8_t bg_prio[256];
    uint8_t *reg = gg.vdp.reg;
    uint8_t backdrop = 16 + (reg[7] & 0x0F);

    if (!gg_display_enabled()) {
        memset(color, backdrop, sizeof(color));
    } else {
        // background
        uint16_t nt = gg_name_table_base();
        uint8_t hscroll = (line < 16 && (reg[0] & 0x40)) ? 0 : reg[8];
        for (uint16_t x = 0; x < 256; x++) {
            uint8_t col = x >> 3;
            uint16_t vs = (col >= 24 && (reg[0] & 0x80)) ? 0 : gg.vdp.vscroll_frame;
            uint16_t y = (line + vs) % 224;
            uint8_t sx = (uint8_t) (x - hscroll);
            uint16_t e_addr = nt + ((y >> 3) * 32 + (sx >> 3)) * 2;
            uint16_t e = gg_vram[e_addr & 0x3FFF] | (gg_vram[(e_addr + 1) & 0x3FFF] << 8);
            uint16_t tile = e & 0x1FF;
            uint8_t row = y & 7, c = sx & 7;
            if (e & 0x400) row = 7 - row;
            if (e & 0x200) c = 7 - c;
            uint8_t px = tile_pixel(tile, row, c);
            color[x] = px + ((e & 0x800) ? 16 : 0);
            bg_prio[x] = (e & 0x1000) && px;
        }

        // sprites (up to 8 per line)
        uint16_t sat = gg_sat_base();
        uint8_t height = (reg[1] & 0x02) ? 16 : 8;
        uint8_t drawn = 0;
        uint8_t used[256] = {0};
        for (uint8_t i = 0; i < 64 && drawn < 8; i++) {
            uint8_t sy = gg_vram[sat + i];
            if (sy == 0xD0)
                break;
            int16_t top = sy + 1;
            if (top > 0xF0) top -= 256;
            if (line < top || line >= top + height)
                continue;
            drawn++;
            int16_t sx = gg_vram[sat + 0x80 + i * 2];
            uint16_t n = gg_vram[sat + 0x81 + i * 2];
            if (reg[6] & 0x04) n |= 0x100;
            if (height == 16) n &= ~1;
            if (reg[0] & 0x08) sx -= 8;
            uint8_t row = line - top;
            uint16_t tile = n + (row >> 3);
            for (uint8_t c = 0; c < 8; c++) {
                int16_t x = sx + c;
                if (x < 0 || x > 255 || used[x])
                    continue;
                uint8_t px = tile_pixel(tile, row & 7, c);
                if (!px)
                    continue;
                used[x] = 1;
                if (!bg_prio[x])
                    color[x] = 16 + px;
            }
        }
        if (reg[0] & 0x20)
            memset(color, backdrop, 8);
    }

    if (line >= GG_VIEW_Y && line < GG_VIEW_Y + GG_VIEW_H) {
        for (uint16_t x = 0; x < GG_VIEW_W; x++) {
            uint8_t ci = color[x + GG_VIEW_X];
            uint16_t c = gg_cram[ci * 2] | (gg_cram[ci * 2 + 1] << 8);
            fb[line - GG_VIEW_Y][x][0] = (c & 0xF) * 17;
            fb[line - GG_VIEW_Y][x][1] = ((c >> 4) & 0xF) * 17;
            fb[line - GG_VIEW_Y][x][2] = ((c >> 8) & 0xF) * 17;
        }
    }
}


void host_line_hook(uint16_t line) {
    if (line < GG_ACTIVE_LINES)
        render_line(line);
}

/* === WonderSwan Color renderer (4bpp planar) === */

static uint8_t ws_pixel(uint16_t tile, uint8_t row, uint8_t col) {
    const uint8_t *p = iram + 0x4000 + tile * 32 + row * 4;
    uint8_t b = 7 - col;
    return ((p[0] >> b) & 1) | (((p[1] >> b) & 1) << 1) | (((p[2] >> b) & 1) << 2) | (((p[3] >> b) & 1) << 3);
}


extern volatile uint8_t act_line[], act_val[], act_n;
static uint8_t scr1_x(int y) {
    uint8_t v = act_val[0];
    for (int i = 1; i < act_n; i++)
        if (y >= act_line[i]) v = act_val[i];
    return v;
}

static int screen_px(int scr, int x, int y, uint16_t *col) {
    uint16_t base = scr ? (ports[7] >> 4) << 11 : (ports[7] & 15) << 11;
    // (screen 2 inside the window: the background's priority tiles, its
    // scroll that of screen 1, set by line_irq on the WS: none here)
    int with1 = scr && !(ports[0] & 0x10);
    uint8_t sx = x + (scr && !with1 ? ports[0x12] : scr1_x(y)), sy = y + ports[scr && !with1 ? 0x13 : 0x11];
    uint16_t e = iram[base + ((sy >> 3) * 32 + (sx >> 3)) * 2] | (iram[base + ((sy >> 3) * 32 + (sx >> 3)) * 2 + 1] << 8);
    uint16_t t = (e & 0x1FF) | (((e >> 13) & 1) << 9);
    uint8_t r = sy & 7, c = sx & 7;
    if (e & 0x8000) r = 7 - r;
    if (e & 0x4000) c = 7 - c;
    uint8_t px = ws_pixel(t, r, c);
    if (!px) return 0;
    *col = ((uint16_t*) (iram + 0xFE00))[((e >> 9) & 15) * 16 + px];
    return 1;
}

static void ws_render(void) {
    uint8_t ctrl = ports[0];
    for (int y = 0; y < 144; y++) for (int x = 0; x < 224; x++) {
        uint16_t c = ((uint16_t*) (iram + 0xFE00))[((ports[1] >> 4) & 15) * 16 + (ports[1] & 15)];
        uint16_t t;
        if ((ctrl & 1) && screen_px(0, x, y, &t)) c = t;
        // sprites: lower index on top
        int spr_lo = -1, spr_hi = -1; uint16_t clo = 0, chi = 0;
        if (ctrl & 4) {
            uint8_t *tab = iram + ((ports[4] & 0x3F) << 9);
            for (int i = ports[5] + ports[6] - 1; i >= ports[5]; i--) {
                uint8_t *s = tab + i * 4;
                uint8_t dy = y - s[2], dx = x - s[3];
                if (dy >= 8 || dx >= 8) continue;
                uint16_t tile = s[0] | ((s[1] & 1) << 8);
                if (s[1] & 0x80) dy = 7 - dy;
                if (s[1] & 0x40) dx = 7 - dx;
                uint8_t px = ws_pixel(tile, dy, dx);
                if (!px) continue;
                uint16_t cc = ((uint16_t*) (iram + 0xFE00))[(8 + ((s[1] >> 1) & 7)) * 16 + px];
                if (s[1] & 0x20) { spr_hi = i; chi = cc; } else { spr_lo = i; clo = cc; }
            }
        }
        if (spr_lo >= 0) c = clo;
        if (ctrl & 2) {
            int in = x >= ports[8] && x <= ports[0xA] && y >= ports[9] && y <= ports[0xB];
            int show = !(ctrl & 0x20) || ((ctrl & 0x10) ? !in : in);
            if (show && screen_px(1, x, y, &t)) c = t;
        }
        if (spr_hi >= 0) c = chi;
        wsfb[y][x][0] = ((c >> 8) & 15) * 17;
        wsfb[y][x][1] = ((c >> 4) & 15) * 17;
        wsfb[y][x][2] = (c & 15) * 17;
    }
}

extern volatile uint8_t act_line[], act_val[], act_n;
void video_init(void);
void video_present(void);
void sound_init(void);

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: wssim rom frames prefix [keys] [every]\n"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    size_t n = fread(psram, 1, sizeof(psram), f);
    fclose(f);
    uint16_t banks = (n + 0x3FFF) >> 14;
    video_init();
    sound_init();
    gg_every_line = 1;   // the reference renderer draws line by line
    gg_init(banks);
    int frames = atoi(argv[2]);
    const char *keys = argc > 4 ? argv[4] : "";
    int every = argc > 5 ? atoi(argv[5]) : 60;
    for (int fr = 0; fr < frames; fr++) {
        const char *k = keys;
        while (*k) {
            int kf = atoi(k);
            const char *col = strchr(k, ':');
            if (!col) break;
            if (kf == fr) gg.keys = strtol(col + 1, NULL, 16);
            const char *comma = strchr(col, ',');
            if (!comma) break;
            k = comma + 1;
        }
        gg_run_frame();   // (video_present at line 168)
        if (every && (fr + 1) % every == 0) {
            ws_render();
            long diff = 0;
            for (int y = 0; y < 144; y++) for (int x = 0; x < 160; x++)
                if (memcmp(fb[y][x], wsfb[y][x + 32], 3)) diff++;
            printf("frame %5d: %5.1f%% pixels differ, splits %d:", fr + 1, diff * 100.0 / (160 * 144), act_n); for (int i = 0; i < act_n; i++) printf(" %d=%d", act_line[i], act_val[i]); printf("\n");
            char path[512];
            snprintf(path, sizeof(path), "%s_%05d.ppm", argv[3], fr + 1);
            FILE *o = fopen(path, "wb");
            fprintf(o, "P6\n%d %d\n255\n", 160 + 8 + 224, 144);
            for (int y = 0; y < 144; y++) {
                fwrite(fb[y], 1, 160 * 3, o);
                static const uint8_t gap[8 * 3] = {0};
                fwrite(gap, 1, sizeof(gap), o);
                fwrite(wsfb[y], 1, 224 * 3, o);
            }
            fclose(o);
        }
    }
    return 0;
}
