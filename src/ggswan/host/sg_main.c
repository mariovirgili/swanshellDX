/**
 * sgswan host test: runs an SG-1000 ROM on the PC and draws it with a
 * reference renderer of the TMS9918A (the four screen modes, the sprites
 * with their four to a line), line by line: what the WonderSwan side has
 * to come as close to as it can. Writes screenshots (PPM, the whole
 * 256x192 picture). Usage:
 *   sgtest rom.sg frames out_prefix [keyscript] [shot_every]
 * keyscript: "frame:keys,frame:keys" (keys = hex GG_KEY_* mask; 80 is the
 * Pause button).
 * Every frame it also works out the sprite flags of the status register
 * (fifth sprite, overlap) the long way, pixel by pixel, and compares them
 * with core/sg_vdp.c sg_sprite_status(), which the plugin uses.
 * At the end, what the game used (for the WonderSwan side: see SG1000.md).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/gg.h"
#include "../core/z80.h"

#ifndef GGSWAN_SG1000
#error "build with -DGGSWAN_SG1000"
#endif

static uint8_t rom[0x10000];
static uint32_t rom_size;
static uint8_t ram_buf[0x2000];
static uint8_t vram_buf[0x4000];
uint8_t *gg_ram = ram_buf;
uint8_t *gg_vram = vram_buf;

static uint8_t fb[GG_ACTIVE_LINES][256][3];
static uint32_t psg_writes, vram_changes, reg_writes;

const uint8_t *gg_platform_bank(uint8_t slot, uint16_t bank) {
    (void) slot;
    return rom + (uint32_t) bank * 0x4000;
}
uint8_t *gg_platform_cart_ram(uint8_t page) { (void) page; return NULL; }
void gg_platform_psg(uint8_t v) { (void) v; psg_writes++; }
void gg_platform_stereo(uint8_t v) { (void) v; }
uint8_t gg_platform_sprite_status(void) { return 0; }
void gg_platform_vram_write(uint16_t a, uint8_t v) { (void) a; (void) v; vram_changes++; }
void gg_platform_cram_write(uint8_t i) { (void) i; }
void gg_platform_vdp_reg(uint8_t r, uint8_t v) { (void) r; (void) v; reg_writes++; }
void gg_platform_vram_sync(void) {}
void gg_platform_frame_end(void) {}
void gg_platform_vram_sync_at(uint16_t a) { (void) a; }
uint8_t gg_platform_vram_defer(uint16_t a, uint8_t v) { (void) a; (void) v; return 0; }

/* === Reference renderer === */

static inline uint8_t vr(uint16_t a) { return gg_vram[a & 0x3FFF]; }

// what the game used
static uint32_t st_mode[4], st_off, st_mag, st_16, st_over4, st_frames;
static uint8_t st_max_line, st_max_sprites;
static uint32_t st_status_bad;

// the sprites of a line: the first four of the table that are on it.
// Returns how many; *fifth: the number of the fifth, or -1.
static uint8_t line_sprites(uint16_t line, sg_sprite_t *out, uint8_t *index, int *fifth) {
    uint8_t n = 0, side = sg_sprite_size() * sg_sprite_mag();
    sg_sprite_t s;
    *fifth = -1;
    for (uint8_t i = 0; i < 32 && sg_sprite(i, &s); i++) {
        if ((int) line < s.y || (int) line >= s.y + side)
            continue;
        if (n == 4) {
            *fifth = i;
            break;
        }
        index[n] = i;
        out[n++] = s;
    }
    return n;
}

// the sprites of a line over color[] (or nowhere: color = NULL). Returns
// 1 if two of them overlap on it; *fifth as line_sprites.
static uint8_t sprite_line(uint16_t line, uint8_t *color, int *fifth) {
    sg_sprite_t s[4];
    uint8_t index[4];
    uint8_t n = line_sprites(line, s, index, fifth);
    uint8_t mag = sg_sprite_mag(), side = sg_sprite_size() * mag;
    uint8_t taken[256], hit = 0;
    memset(taken, 0, sizeof(taken));
    for (uint8_t i = 0; i < n; i++) {
        uint16_t bits = sg_sprite_row(&s[i], (line - s[i].y) / mag);
        for (uint8_t k = 0; k < side; k++) {
            int x = s[i].x + k;
            if (x < 0 || x > 255 || !((bits << (k / mag)) & 0x8000))
                continue;
            if (taken[x]) {
                hit = 1;
                continue;   // (the first of the table is in front)
            }
            taken[x] = 1;
            if (color && s[i].color)
                color[x] = s[i].color;
        }
    }
    if (n + (*fifth >= 0) > st_max_line)
        st_max_line = n + (*fifth >= 0);
    return hit;
}

// one line: color[] gets the TMS color (0-15) of each pixel, colour 0
// being the backdrop's already. Returns 1 if two sprites overlap on it.
static uint8_t render_line(uint16_t line, uint8_t *color, int *fifth) {
    const uint8_t *reg = gg.vdp.reg;
    uint8_t backdrop = reg[7] & 0x0F;
    uint8_t mode = sg_mode();
    *fifth = -1;
    if (!(reg[1] & 0x40)) {
        memset(color, backdrop, 256);
        return 0;
    }
    uint16_t nt = gg_name_table_base();
    uint8_t row = line >> 3, y = line & 7;
    if (mode == SG_MODE_TEXT) {
        // 40 columns of 6 pixels, 8 pixels of backdrop on each side; the
        // two colors of register 7
        uint8_t fg = reg[7] >> 4, bg = backdrop;
        uint16_t pg = (reg[4] & 7) << 11;
        memset(color, backdrop, 256);
        for (uint16_t x = 0; x < 240; x++) {
            uint8_t name = vr(nt + row * 40 + x / 6);
            uint8_t p = vr(pg + name * 8 + y);
            color[8 + x] = ((p << (x % 6)) & 0x80) ? fg : bg;
        }
    } else {
        for (uint16_t x = 0; x < 256; x++) {
            uint16_t name = vr(nt + row * 32 + (x >> 3));
            uint8_t bit = 0x80 >> (x & 7), c;
            if (mode == SG_MODE_G2) {
                // 768 patterns and 8 colors bytes each: the tables are
                // at 0000h or 2000h, with masks from registers 3 and 4
                uint16_t cmask = ((reg[3] & 0x7F) << 3) | 7;
                uint16_t pmask = ((reg[4] & 3) << 8) | (cmask & 0xFF);
                uint16_t ch = name + (row >> 3) * 256;
                uint8_t p = vr(((reg[4] & 4) << 11) + ((ch & pmask) << 3) + y);
                uint8_t k = vr(((reg[3] & 0x80) << 6) + ((ch & cmask) << 3) + y);
                c = (p & bit) ? k >> 4 : k & 15;
            } else if (mode == SG_MODE_MC) {
                // blocks of 4x4 pixels: two colors a byte, two bytes of
                // the pattern a row of characters
                uint8_t k = vr(((reg[4] & 7) << 11) + name * 8 + ((row & 3) << 1) + (y >> 2));
                c = (x & 4) ? k & 15 : k >> 4;
            } else {
                uint8_t p = vr(((reg[4] & 7) << 11) + name * 8 + y);
                uint8_t k = vr((reg[3] << 6) + (name >> 3));
                c = (p & bit) ? k >> 4 : k & 15;
            }
            color[x] = c;
        }
    }
    for (uint16_t x = 0; x < 256; x++)
        if (!color[x])
            color[x] = backdrop;
    if (mode == SG_MODE_TEXT)
        return 0;   // (no sprites)
    return sprite_line(line, color, fifth);
}

void gg_platform_line(uint16_t line) {
    static uint8_t color[256];
    if (line < GG_ACTIVE_LINES) {
        int fifth;
        render_line(line, color, &fifth);
        for (uint16_t x = 0; x < 256; x++) {
            uint16_t c = gg_color12(color[x]);
            fb[line][x][0] = ((c >> 8) & 15) * 17;
            fb[line][x][1] = ((c >> 4) & 15) * 17;
            fb[line][x][2] = (c & 15) * 17;
        }
    }
    if (line == GG_VBL_LINE) {
        // the sprite flags the long way, from the table as it is now (as
        // the plugin works them out: once, here; the chip sets them line
        // after line, which is the same for a game that writes the table
        // during the vertical blank)
        uint8_t ref = 0, n = 0;
        sg_sprite_t s;
        if ((gg.vdp.reg[1] & 0x40) && sg_mode() != SG_MODE_TEXT) {
            for (uint16_t l = 0; l < GG_ACTIVE_LINES; l++) {
                int fifth;
                if (sprite_line(l, NULL, &fifth))
                    ref |= 0x20;
                if (fifth >= 0 && !(ref & 0x40))
                    ref = (ref & 0x20) | 0x40 | fifth;
            }
        }
        uint8_t got = sg_sprite_status();
        if (got != ref) {
            if (st_status_bad < 5)
                fprintf(stderr, "frame %u: sprite status %02X, the reference says %02X\n", gg.frame, got, ref);
            st_status_bad++;
        }
        while (n < 32 && sg_sprite(n, &s))
            n++;
        if (n > st_max_sprites)
            st_max_sprites = n;
        if (ref & 0x40)
            st_over4++;
        st_frames++;
        if (!(gg.vdp.reg[1] & 0x40))
            st_off++;
        else
            st_mode[sg_mode()]++;
        if (n && sg_sprite_mag() == 2)
            st_mag++;
        if (n && sg_sprite_size() == 16)
            st_16++;
    }
}

static void save_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n256 %d\n255\n", GG_ACTIVE_LINES);
    fwrite(fb, 1, sizeof(fb), f);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s rom.sg frames out_prefix [keyscript] [shot_every]\n", argv[0]);
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    rom_size = fread(rom, 1, 0xC000, f);
    if (!rom_size || fgetc(f) != EOF) {
        fprintf(stderr, "%s: an SG-1000 ROM has 1 to 49152 bytes (no mapper)\n", argv[1]);
        return 1;
    }
    fclose(f);
    // (a ROM shorter than its 16 KiB bank is there again and again)
    uint32_t alloc = (rom_size + 0x3FFF) & ~0x3FFFu;
    for (uint32_t a = rom_size; a < alloc; a++)
        rom[a] = rom[a % rom_size];

    gg_every_line = 1;   // the reference renderer draws line by line
    gg_init(alloc / 0x4000);
    int frames = atoi(argv[2]);
    const char *keys = argc > 4 ? argv[4] : "";
    int every = argc > 5 ? atoi(argv[5]) : 60;
    char path[512];
    for (int fr = 0; fr < frames; fr++) {
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
    printf("frames %d pc %04X psg writes %u vram bytes changed %u register writes %u\n",
        frames, z80.pc, psg_writes, vram_changes, reg_writes);
    printf("screen: off %u, Graphics I %u, Graphics II %u, multicolor %u, text %u frames\n",
        st_off, st_mode[SG_MODE_G1], st_mode[SG_MODE_G2], st_mode[SG_MODE_MC], st_mode[SG_MODE_TEXT]);
    printf("sprites: up to %u in the table, %u on a line; frames with a fifth sprite %u, 16x16 %u, magnified %u\n",
        st_max_sprites, st_max_line, st_over4, st_16, st_mag);
    printf("registers:");
    for (int r = 0; r < 8; r++)
        printf(" %02X", gg.vdp.reg[r]);
    printf("\nram C000:");
    for (int a = 0; a < 16; a++)
        printf(" %02X", ram_buf[a]);
    printf("\nsprite status: %u of %u frames unlike the reference\n", st_status_bad, st_frames);
    return st_status_bad ? 2 : 0;
}
