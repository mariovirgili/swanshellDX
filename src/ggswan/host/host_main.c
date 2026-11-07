/**
 * ggswan host test: runs a .gg ROM on the PC with a reference renderer and
 * writes screenshots (PPM). Usage:
 *   ggtest rom.gg frames out_prefix [keyscript]
 * keyscript: "frame:keys,frame:keys" (keys = hex GG_KEY_* mask), e.g.
 *   "120:80,130:00" presses Start at frame 120 and releases it at 130.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/gg.h"
#include "../core/z80.h"

static uint8_t *rom;
static uint32_t rom_size;
static uint8_t ram_buf[0x2000];
static uint8_t vram_buf[0x4000];
static uint8_t cart_ram_buf[0x8000];
uint8_t *gg_ram = ram_buf;
uint8_t *gg_vram = vram_buf;

static uint8_t fb[GG_VIEW_H][GG_VIEW_W][3];
static uint32_t psg_writes;

const uint8_t *gg_platform_bank(uint8_t slot, uint16_t bank) {
    (void) slot;
    return rom + (uint32_t) bank * 0x4000;
}
uint8_t *gg_platform_cart_ram(uint8_t page) { return cart_ram_buf + page * 0x4000; }
void gg_platform_psg(uint8_t v) { (void) v; psg_writes++; }
void gg_platform_stereo(uint8_t v) { (void) v; }
void gg_platform_vram_write(uint16_t a, uint8_t v) { (void) a; (void) v; }
void gg_platform_cram_write(uint8_t i) { (void) i; }
void gg_platform_vdp_reg(uint8_t r, uint8_t v) { (void) r; (void) v; }
void gg_platform_vblank(void) {}
void gg_platform_vram_sync(void) {}
uint8_t gg_platform_vram_defer(uint16_t a, uint8_t v) { (void) a; (void) v; return 0; }

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

void gg_platform_line(uint16_t line) {
    if (line < GG_ACTIVE_LINES)
        render_line(line);
}

static void save_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", GG_VIEW_W, GG_VIEW_H);
    fwrite(fb, 1, sizeof(fb), f);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s rom.gg frames out_prefix [keyscript] [shot_every]\n", argv[0]);
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    rom_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t alloc = (rom_size + 0x3FFF) & ~0x3FFF;
    rom = calloc(1, alloc < 0x10000 ? 0x10000 : alloc);
    if (fread(rom, 1, rom_size, f) != rom_size) { perror(argv[1]); return 1; }
    fclose(f);

    gg_init(alloc / 0x4000);
    int frames = atoi(argv[2]);
    const char *keys = argc > 4 ? argv[4] : "";
    int every = argc > 5 ? atoi(argv[5]) : 60;
    char path[512];
    for (int fr = 0; fr < frames; fr++) {
        // key script
        const char *k = keys;
        while (*k) {
            int kf = atoi(k);
            const char *col = strchr(k, ':');
            if (!col) break;
            if (kf == fr)
                gg.keys = strtol(col + 1, NULL, 16);
            const char *comma = strchr(col, ',');
            if (!comma) break;
            k = comma + 1;
        }
        gg_run_frame();
        if (every && (fr + 1) % every == 0) {
            snprintf(path, sizeof(path), "%s_%05d.ppm", argv[3], fr + 1);
            save_ppm(path);
        }
    }
    snprintf(path, sizeof(path), "%s_last.ppm", argv[3]);
    save_ppm(path);
    printf("frames %d pc %04X psg writes %u display %d\n", frames, z80.pc, psg_writes, gg_display_enabled());
    return 0;
}
