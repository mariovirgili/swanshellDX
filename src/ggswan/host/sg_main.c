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

#ifdef GGSWAN_SF7000
// sftest: the SF-7000 (-DGGSWAN_SF7000 too). The "ROM" is a disk image of
// 163,840 bytes; the IPL is the built-in one (mksfipl.py), or the file
// SF_IPL names (the SF-7000's own 8 KiB, to compare).
#include "../core/sf7000.h"
#define SF_IPL_PLAIN
#include "../core/sf_ipl.h"
static uint8_t disk[SF_DISK_SIZE];
static uint8_t ram64[0x10000];
static uint8_t ipl_file[0x2000];
static uint32_t map_changes;
void gg_platform_disk_read(uint16_t lba, uint8_t *buf) { memcpy(buf, disk + (uint32_t) lba * 256, 256); }
void gg_platform_disk_write(uint16_t lba, const uint8_t *buf) { memcpy(disk + (uint32_t) lba * 256, buf, 256); }
void gg_platform_code_write(uint16_t a) { (void) a; }
void gg_platform_map_changed(void) {
    map_changes++;
    if (getenv("SF_DUMP_AT_RAM") && map_changes == 1) {
        // (what the IPL and the disk's loader left: RAM, VRAM, registers)
        FILE *d = fopen(getenv("SF_DUMP_AT_RAM"), "wb");
        fwrite(ram64, 1, 0x10000, d); fwrite(gg_vram, 1, 0x4000, d); fwrite(gg.vdp.reg, 1, 8, d);
        fwrite(&gg.vdp.addr, 1, 2, d); fwrite(&gg.vdp.code, 1, 1, d); fwrite(&sf.pcn, 1, 1, d);
        fwrite(&sf.sc_c, 1, 5, d);
        fclose(d);
    }
    if (getenv("SF_TRACE"))
        fprintf(stderr, "frame %lu pc %04X: %s at 0000h\n", (unsigned long) gg.frame, z80.pc, sf.rom_on ? "IPL" : "RAM");
}
#include "sf_host.h"
// the text screen as text (the name table's 40x24 or 32x24 characters)
static void print_screen(void) {
    int cols = sg_mode() == SG_MODE_TEXT ? 40 : 32;
    uint16_t nt = gg_name_table_base();
    for (int r = 0; r < 24; r++) {
        char line[41];
        int last = -1;
        for (int c = 0; c < cols; c++) {
            uint8_t ch = gg_vram[(nt + r * cols + c) & 0x3FFF];
            line[c] = ch >= 32 && ch < 127 ? ch : ch ? '?' : ' ';
            if (line[c] != ' ') last = c;
        }
        line[last + 1] = 0;
        if (last >= 0) printf("  %2d|%s\n", r, line);
    }
}
#endif
#ifdef GGSWAN_SF7000
#define SF_BUILD 1
#else
#define SF_BUILD 0
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
// bytes changed this frame: patterns and colors, names, sprite table, rest
static uint32_t fr_pat, fr_name, fr_sat, fr_other;
static uint8_t spr_written = 1;
uint8_t gg_platform_sprites_written(void) {
    uint8_t w = spr_written;
    spr_written = 0;
    return w;
}
void gg_platform_vram_write(uint16_t a, uint8_t v) {
    (void) v;
    vram_changes++;
    uint16_t nt = gg_name_table_base(), sat = gg_sat_base();
    if ((a >= sat && a < sat + 128) || (uint16_t) (a - ((gg.vdp.reg[6] & 7) << 11)) < 0x800)
        spr_written = 1;
    if (a >= sat && a < sat + 128)
        fr_sat++;
    else if (a >= nt && a < nt + 768)
        fr_name++;
    else if (sg_mode() == SG_MODE_G2 && a < 0x3800
             && (uint16_t) (a - ((gg.vdp.reg[4] & 4) << 11)) < 0x1800)
        fr_pat++;
    else if (sg_mode() == SG_MODE_G2 && a < 0x3800)
        fr_pat++;
    else
        fr_other++;
}
void gg_platform_cram_write(uint8_t i) { (void) i; }
void gg_platform_vdp_reg(uint8_t r, uint8_t v) { (void) r; (void) v; reg_writes++; }
void gg_platform_vram_sync(void) {}
void gg_platform_frame_end(void) {}
void gg_platform_vram_sync_at(uint16_t a) { (void) a; }
uint8_t gg_platform_vram_defer(uint16_t a, uint8_t v) { (void) a; (void) v; return 0; }

/* === Reference renderer === */

#include "sg_ref.h"

// what the game used
static uint32_t st_mode[4], st_off, st_mag, st_16, st_over4, st_frames;
static uint8_t st_max_sprites;
static uint32_t st_status_bad;
static uint8_t st_vis, st_kinds, st_vis_mag, st_kinds_mag;
// the characters of the screen with a pattern and colors of their own (the
// tiles the WonderSwan side needs): all 32x24, and the 29x19 of a view
static uint16_t st_chars_all, st_chars_view;
static uint32_t st_pat_max, st_pat_frames, st_pat_sum, st_name_max, st_name_sum;

static void count_chars(void) {
    static uint8_t seen[1024];
    uint16_t nt = gg_name_table_base(), all = 0, view = 0;
    uint16_t cmask = 0x3FF, pmask = 0x3FF;
    uint8_t mode = sg_mode();
    if (mode == SG_MODE_TEXT || !(gg.vdp.reg[1] & 0x40))
        return;
    if (mode == SG_MODE_G2) {
        cmask = ((gg.vdp.reg[3] & 0x7F) << 3) | 7;
        pmask = ((gg.vdp.reg[4] & 3) << 8) | (cmask & 0xFF);
    }
    for (uint8_t pass = 0; pass < 2; pass++) {
        memset(seen, 0, sizeof(seen));
        for (uint8_t row = pass ? 3 : 0; row < (pass ? 22 : 24); row++)
            for (uint8_t col = pass ? 2 : 0; col < (pass ? 31 : 32); col++) {
                uint16_t ch = vr(nt + row * 32 + col);
                if (mode == SG_MODE_G2)
                    ch = (ch + (row >> 3) * 256) & (pmask | cmask);
                else if (mode == SG_MODE_MC)
                    ch |= (row & 3) << 8;
                if (!seen[ch]) {
                    seen[ch] = 1;
                    if (pass) view++; else all++;
                }
            }
    }
    if (all > st_chars_all) st_chars_all = all;
    if (view > st_chars_view) st_chars_view = view;
}

void gg_platform_line(uint16_t line) {
    static uint8_t color[256];
    if (line < GG_ACTIVE_LINES) {
        int fifth;
        sg_ref_line(line, color, &fifth);
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
                if (sg_ref_sprite_line(l, NULL, &fifth))
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
        count_chars();
        if (fr_pat > st_pat_max) st_pat_max = fr_pat;
        if (fr_pat) st_pat_frames++;
        st_pat_sum += fr_pat;
        if (fr_name > st_name_max) st_name_max = fr_name;
        st_name_sum += fr_name;
        if (getenv("SGTEST_WRITES") && (fr_pat || fr_name > 64))
            fprintf(stderr, "frame %u: patterns %u names %u sprites %u other %u\n", gg.frame, fr_pat, fr_name, fr_sat, fr_other);
        fr_pat = fr_name = fr_sat = fr_other = 0;
        {
            // sprites on the picture, and their different pattern and color
            static uint16_t keys[32];
            uint8_t vis = 0, nk = 0, side = sg_sprite_size() * sg_sprite_mag();
            for (uint8_t i = 0; i < 32 && sg_sprite(i, &s); i++) {
                if (s.y >= GG_ACTIVE_LINES || s.y + side <= 0 || s.x >= 256 || s.x + side <= 0 || !s.color)
                    continue;
                vis++;
                uint16_t key = (s.pattern >> 3) | (s.color << 12);
                uint8_t k = 0;
                while (k < nk && keys[k] != key) k++;
                if (k == nk) keys[nk++] = key;
            }
            if ((gg.vdp.reg[1] & 0x40) && sg_mode() != SG_MODE_TEXT) {
                if (vis > st_vis) st_vis = vis;
                if (nk > st_kinds) st_kinds = nk;
                if (sg_sprite_mag() == 2 && vis > st_vis_mag) st_vis_mag = vis;
                if (sg_sprite_mag() == 2 && nk > st_kinds_mag) st_kinds_mag = nk;
            }
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
#ifdef GGSWAN_SF7000
    if (fread(disk, 1, SF_DISK_SIZE, f) != SF_DISK_SIZE || fgetc(f) != EOF) {
        fprintf(stderr, "%s: an SF-7000 disk image has %lu bytes (40 tracks of 16 sectors of 256)\n", argv[1], (unsigned long) SF_DISK_SIZE);
        return 1;
    }
    fclose(f);
    sf_ram = ram64;
    sf_ipl = sf_ipl_builtin;
    sf_ipl_size = sizeof(sf_ipl_builtin);
    sf_ipl_fast = 1;
    sf_ipl_font = SF_IPL_FONT;
    if (getenv("SF_IPL_PLAIN")) {
        // (the same IPL with all of it Z80 code: to compare)
        sf_ipl = sf_ipl_plain;
        sf_ipl_size = sizeof(sf_ipl_plain);
        sf_ipl_fast = 0;
    }
    if (getenv("SF_IPL")) {
        FILE *i = fopen(getenv("SF_IPL"), "rb");
        if (!i || fread(ipl_file, 1, 0x2000, i) != 0x2000) { fprintf(stderr, "SF_IPL: 8192 bytes wanted\n"); return 1; }
        fclose(i);
        sf_ipl = ipl_file;
        sf_ipl_size = 0x2000;
        sf_ipl_fast = 0;
    }
    sf.inserted = 1;
    printf("disk: %s, %s\n", argv[1], memcmp(disk, "SYS:", 4) ? "no system on it (it does not start)" : "a system disk");
    f = fopen(argv[1], "rb");
#endif
#ifdef GGSWAN_COLECO
    // ColecoVision: 8 KiB BIOS from CV_BIOS at 0000h-1FFFh, cartridge ROM (up
    // to 32 KiB) at 4000h-BFFFh (mapped to Z80 8000h-FFFFh via slots 1 and 2)
    const char *bios_path = getenv("CV_BIOS");
    if (!bios_path || !*bios_path) {
        fprintf(stderr, "CV_BIOS: set to the 8192-byte ColecoVision BIOS ROM path\n");
        fclose(f);
        return 1;
    }
    FILE *bf = fopen(bios_path, "rb");
    if (!bf) { perror(bios_path); fclose(f); return 1; }
    memset(rom, 0xFF, sizeof(rom));
    if (fread(rom, 1, 0x2000, bf) != 0x2000 || fgetc(bf) != EOF) {
        fprintf(stderr, "%s: a ColecoVision BIOS ROM has 8192 bytes\n", bios_path);
        fclose(bf);
        fclose(f);
        return 1;
    }
    fclose(bf);
    rom_size = fread(rom + 0x4000, 1, 0x8000, f);
    if (!rom_size || fgetc(f) != EOF) {
        fprintf(stderr, "%s: a ColecoVision ROM has 1 to 32768 bytes (no MegaCart)\n", argv[1]);
        fclose(f);
        return 1;
    }
    fclose(f);
    uint32_t alloc = (rom_size + 0x3FFF) & ~0x3FFFu;
#else
    rom_size = fread(rom, 1, 0xC000, f);
    if (!SF_BUILD && (!rom_size || fgetc(f) != EOF)) {
        fprintf(stderr, "%s: an SG-1000 ROM has 1 to 49152 bytes (no mapper)\n", argv[1]);
        return 1;
    }
    fclose(f);
    // (a ROM shorter than its 16 KiB bank: FFh after it, as the plugin loads it)
    uint32_t alloc = (rom_size + 0x3FFF) & ~0x3FFFu;
    for (uint32_t a = rom_size; a < alloc; a++)
        rom[a] = 0xFF;
#endif

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
            if (kf == fr) {
#ifdef GGSWAN_COLECO
                const char *v = col + 1;
                if (*v == '-') {
                    gg.keys = 0;
                    gg.cv_keypad = 0xFF;
                } else if (*v == 'K' || *v == 'k' || *v == 'Y' || *v == 'y') {
                    char c = v[1];
                    if (c >= '0' && c <= '9') gg.cv_keypad = (uint8_t) (c - '0');
                    else if (c == '*') gg.cv_keypad = 10;
                    else if (c == '#') gg.cv_keypad = 11;
                    else gg.cv_keypad = 0xFF;
                } else {
                    gg.keys = strtol(v, NULL, 16);
                }
#else
                gg.keys = strtol(col + 1, NULL, 16);
#endif
            }
            const char *comma = strchr(col, ',');
            if (!comma) break;
            k = comma + 1;
        }
#ifdef GGSWAN_SF7000
        type_frame(fr, getenv("SF_TYPE"));
        // SF_SWAP="frame:other.sf": another disk in the drive from there
        // (frame 0: when the first disk's loader has given the memory
        // to what it loaded, which is when the plugin changes the disk
        // for one that does not start by itself)
        static int swapped;
        if (getenv("SF_SWAP") && !swapped && (atoi(getenv("SF_SWAP")) ? atoi(getenv("SF_SWAP")) == fr : !sf.rom_on)) {
            swapped = 1;
            FILE *d2 = fopen(strchr(getenv("SF_SWAP"), ':') + 1, "rb");
            if (!d2 || fread(disk, 1, SF_DISK_SIZE, d2) != SF_DISK_SIZE) { fprintf(stderr, "SF_SWAP: no such disk\n"); return 1; }
            fclose(d2);
            printf("frame %d: the disk changed\n", fr);
        }
#endif
        gg_run_frame();
        if (every && (fr + 1) % every == 0) {
            snprintf(path, sizeof(path), "%s_%05d.ppm", argv[3], fr + 1);
            save_ppm(path);
#ifdef GGSWAN_SF7000
            printf("frame %d: pc %04X %s, mode %d, disk reads %u writes %u errors %u\n", fr + 1, z80.pc,
                sf.rom_on ? "IPL" : "RAM", sg_mode(), sf.reads, sf.writes, sf.errors);
            if (getenv("SF_SCREEN")) print_screen();
#endif
        }
    }
    snprintf(path, sizeof(path), "%s_last.ppm", argv[3]);
    save_ppm(path);
#ifdef GGSWAN_SF7000
    printf("SF-7000: %s at 0000h (changed %u times), sectors read %u, written %u, errors %u, last command %02X\n",
        sf.rom_on ? "IPL" : "RAM", map_changes, sf.reads, sf.writes, sf.errors, sf.last_cmd);
    print_screen();
    if (getenv("SF_VRAM")) {
        // (what is in the VRAM, by 2 KiB: bytes not 0)
        printf("VRAM bytes not 0, by 2 KiB:");
        for (int b = 0; b < 8; b++) { int n = 0; for (int i = 0; i < 0x800; i++) n += gg_vram[b * 0x800 + i] != 0; printf(" %d", n); }
        printf("\n");
        FILE *v = fopen(getenv("SF_VRAM"), "wb"); fwrite(gg_vram, 1, 0x4000, v); fclose(v);
        v = fopen("sf_ram.bin", "wb"); fwrite(ram64, 1, 0x10000, v); fclose(v);
    }
#endif
    printf("frames %d pc %04X sp %04X psg writes %u vram bytes changed %u register writes %u\n",
        frames, z80.pc, z80.sp, psg_writes, vram_changes, reg_writes);
    printf("screen: off %u, Graphics I %u, Graphics II %u, multicolor %u, text %u frames\n",
        st_off, st_mode[SG_MODE_G1], st_mode[SG_MODE_G2], st_mode[SG_MODE_MC], st_mode[SG_MODE_TEXT]);
    printf("sprites: up to %u in the table, %u on a line; frames with a fifth sprite %u, 16x16 %u, magnified %u\n",
        st_max_sprites, sg_ref_max_line, st_over4, st_16, st_mag);
    printf("sprites on the picture: up to %u, of %u kinds (pattern and color); magnified: %u, of %u kinds\n",
        st_vis, st_kinds, st_vis_mag, st_kinds_mag);
    printf("characters with their own pattern: up to %u on the screen, %u in a 29x19 view\n", st_chars_all, st_chars_view);
    printf("bytes changed a frame: patterns and colors up to %u (in %u frames, %u in all), names up to %u (%u in all)\n",
        st_pat_max, st_pat_frames, st_pat_sum, st_name_max, st_name_sum);
    printf("registers:");
    for (int r = 0; r < 8; r++)
        printf(" %02X", gg.vdp.reg[r]);
#ifdef GGSWAN_COLECO
    printf("\nram 6000:");
#else
    printf("\nram C000:");
#endif
    for (int a = 0; a < 16; a++)
        printf(" %02X", ram_buf[a]);
    printf("\nsprite status: %u of %u frames unlike the reference\n", st_status_bad, st_frames);
    return st_status_bad ? 2 : 0;
}
