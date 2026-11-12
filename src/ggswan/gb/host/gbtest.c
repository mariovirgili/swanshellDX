/**
 * gbtest - the Game Boy core (../core) on the PC: runs a ROM and says how
 * it ended, for the test ROMs:
 * - Mooneye's: the ROM runs LD B,B when done; passed if B, C, D, E, H, L
 *   are 3, 5, 8, 13, 21, 34;
 * - Blargg's: text through the serial port, or in the cartridge RAM at
 *   A004h with a result at A000h (after DE B0 61 at A001h): "Passed" or
 *   "Failed".
 * usage: gbtest rom.gb [-f frames] [-o picture.pgm] [-t] [-k frame:keys,...] [-q]
 *   -f  frames to run at most (default 3600)
 *   -o  the picture at the end, 160 x 144
 *   -t  a line an instruction on stderr (PC, registers, clock)
 *   -k  keys (hex: right 01 left 02 up 04 down 08 A 10 B 20 SELECT 40 START 80) from a frame on
 *   -q  only the result's line
 *   -n  run all the frames (no stop at a test's end)
 *   -d  the video memory, work RAM, OAM and high RAM at the end, to a file
 *   -H  their checksums at each frame's end, to a file (as gbrun.py's GBRUN_FHASH: where the plugin and this part)
 *   -s  the background's map as text at the end (tests that write their result with a font at its ASCII places)
 *   -m  a Mooneye test: LD B,B ends it (Blargg's CPU tests run LD B,B too)
 * The last line: PASS, FAIL or END (no verdict), frames, clock, the
 * picture's hash. Exit code 0 PASS, 1 FAIL, 2 END, 3 could not load.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gb.h"

extern void (*gb_serial_hook)(uint8_t);
#ifdef GB_FAST
// (the plugin's machine: the same tool, built as gbftest)
uint8_t gb_vram[0x2000], gb_wram[0x2000];
const uint8_t *gb_rom;
uint8_t *gb_cart_ram;
gb_t gb;
void (*gb_serial_hook)(uint8_t);
#define gb_peek gbf_read
#else
#endif
static char serial[65536];
static unsigned serial_n;
static int quiet;
static void on_serial(uint8_t c) {
    if (serial_n < sizeof(serial) - 1) serial[serial_n++] = c;
    if (!quiet) fputc(c, stdout);
}

static uint32_t screen_hash(void) {
    uint32_t h = 2166136261u;
    for (int y = 0; y < 144; y++)
        for (int x = 0; x < 160; x++)
            h = (h ^ gb_screen[y][x]) * 16777619u;
    return h;
}

static FILE *hashes;
static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1));
    }
    return ~c;
}

int main(int argc, char **argv) {
    const char *rom_name = NULL, *out = NULL, *keys = NULL, *dump = NULL;
    long frames = 3600;
    int trace = 0, all = 0, mooneye = 0, text = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc) frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-k") && i + 1 < argc) keys = argv[++i];
        else if (!strcmp(argv[i], "-t")) trace = 1;
        else if (!strcmp(argv[i], "-q")) quiet = 1;
        else if (!strcmp(argv[i], "-n")) all = 1;
        else if (!strcmp(argv[i], "-m")) mooneye = 1;
        else if (!strcmp(argv[i], "-s")) text = 1;
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) dump = argv[++i];
        else if (!strcmp(argv[i], "-H") && i + 1 < argc) hashes = fopen(argv[++i], "w");
        else rom_name = argv[i];
    }
    if (!rom_name) { fprintf(stderr, "usage: gbtest rom.gb [-f frames] [-o out.pgm] [-t] [-k frame:keys,...] [-q] [-n] [-m]\n"); return 3; }
    FILE *f = fopen(rom_name, "rb");
    if (!f) { perror(rom_name); return 3; }
    static uint8_t rom[8 << 20], ram[0x20000];
    uint32_t size = fread(rom, 1, sizeof(rom), f);
    fclose(f);
#ifdef GB_FAST
    gb_rom = rom;
    gb_cart_ram = ram;
    uint8_t err = gbf_load(rom, size);
#else
    uint8_t err = gb_load(rom, size, ram);
#endif
    if (err) { printf("LOAD %d\n", err); return 3; }
    gb_serial_hook = on_serial;

    FILE *hashes_f = hashes;
    int verdict = 2;
    long fr = 0;
    while (fr < frames && (verdict == 2 || all)) {
        if (keys) {
            for (const char *p = keys; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL) {
                long kf; unsigned kv;
                if (sscanf(p, "%ld:%x", &kf, &kv) == 2 && kf == fr) gb_set_keys(kv);
            }
        }
        gb.frame_done = 0;
#ifdef GB_FAST
        while (!gb.frame_done) {
            if (gbf_cyc <= 0 || (gb.halted && !(gb_ie & gb_io[GB_IF] & 0x1F))) {
                gbf_host_event();
                continue;
            }
#else
        uint32_t end = gb.clock + GB_FRAME_TICKS;
        while (!gb.frame_done && (int32_t) (gb.clock - end) < 0) {
#endif
            uint16_t pc = gb.pc;
            uint8_t op = gb_peek(pc);
            if (trace)
                fprintf(stderr, "%04X %02X A=%02X F=%02X BC=%02X%02X DE=%02X%02X HL=%02X%02X SP=%04X ck=%lu%s\n",
                    pc, op, gb.a, gb.f, gb.b, gb.c, gb.d, gb.e, gb.h, gb.l, gb.sp, (unsigned long) gb.clock,
                    gb.halted ? " halt" : "");
            gb_step();
            if (mooneye && op == 0x40 && gb.pc == (uint16_t) (pc + 1) && verdict == 2) {
                verdict = gb.b == 3 && gb.c == 5 && gb.d == 8 && gb.e == 13 && gb.h == 21 && gb.l == 34 ? 0 : 1;
                if (!all) break;
            }
        }
        fr++;
        if (hashes_f)
            fprintf(hashes_f, "%ld %08lX %08lX %08lX %08lX\n", fr, (unsigned long) crc32(gb_vram, 0x2000),
                (unsigned long) crc32(gb_wram, 0x2000), (unsigned long) crc32(gb_oam, 0xA0), (unsigned long) crc32(gb_hram, 0x7F));
        if (verdict == 2) {
            serial[serial_n] = 0;
            if (strstr(serial, "Passed")) verdict = 0;
            else if (strstr(serial, "Failed")) verdict = 1;
            else if (gb.ram_banks && ram[1] == 0xDE && ram[2] == 0xB0 && ram[3] == 0x61 && ram[0] != 0x80) {
                verdict = ram[0] == 0 ? 0 : 1;
                if (!quiet) printf("%s", (char*) ram + 4);
            }
        }
    }
    if (dump) {
        // (the video memory, the work RAM, the OAM and the high RAM: 4000h + A0h + 7Fh bytes)
        FILE *o = fopen(dump, "wb");
        if (o) {
            fwrite(gb_vram, 1, 0x2000, o);
            fwrite(gb_wram, 1, 0x2000, o);
            fwrite(gb_oam, 1, 0xA0, o);
            fwrite(gb_hram, 1, 0x7F, o);
            fclose(o);
        }
    }
    if (out) {
        FILE *o = fopen(out, "wb");
        fprintf(o, "P5\n160 144\n255\n");
        for (int y = 0; y < 144; y++)
            for (int x = 0; x < 160; x++)
                fputc(255 - gb_screen[y][x] * 85, o);
        fclose(o);
    }
    if (text) {
        const uint8_t *map = gb_vram + ((gb_io[GB_LCDC] & 8) ? 0x1C00 : 0x1800);
        for (int r = 0; r < 18; r++) {
            for (int c = 0; c < 20; c++) {
                uint8_t ch = map[r * 32 + c];
                fputc(ch >= 0x20 && ch < 0x7F ? ch : ' ', stdout);
            }
            fputc('\n', stdout);
        }
    }
    if (!quiet) printf("\n");
    printf("%s frames %ld clock %lu hash %08X\n", verdict == 0 ? "PASS" : verdict == 1 ? "FAIL" : "END",
        fr, (unsigned long) gb.clock, screen_hash());
    return verdict;
}
