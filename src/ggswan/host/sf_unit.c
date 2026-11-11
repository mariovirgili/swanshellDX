/**
 * sfswan: the SF-7000's parts tested one by one through its ports, with no
 * program running (make sfcheck): the memory with the IPL over it, the two
 * 8255s, the keyboard matrix, the uPD765 and the disk's geometry.
 *
 *   ./sf_unit disk.sf      (a system disk: "SYS:" at its start)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/gg.h"
#include "../core/z80.h"
#include "../core/sf7000.h"
#include "../core/sf_ipl.h"

static uint8_t disk[SF_DISK_SIZE], ram64[0x10000], vram_buf[0x4000], ram_buf[0x2000];
uint8_t *gg_ram = ram_buf;
uint8_t *gg_vram = vram_buf;
static unsigned map_changes, failed, checks;

const uint8_t *gg_platform_bank(uint8_t slot, uint16_t bank) { (void) slot; (void) bank; return ram64; }
uint8_t *gg_platform_cart_ram(uint8_t page) { (void) page; return NULL; }
void gg_platform_psg(uint8_t v) { (void) v; }
void gg_platform_stereo(uint8_t v) { (void) v; }
uint8_t gg_platform_sprite_status(void) { return 0; }
uint8_t gg_platform_sprites_written(void) { return 1; }
void gg_platform_vram_write(uint16_t a, uint8_t v) { (void) a; (void) v; }
void gg_platform_cram_write(uint8_t i) { (void) i; }
void gg_platform_vdp_reg(uint8_t r, uint8_t v) { (void) r; (void) v; }
void gg_platform_vram_sync(void) {}
void gg_platform_frame_end(void) {}
// (the index hole, looked at on every line while idx_on)
static int idx_on, idx_was, idx_pulses, idx_lines;
void gg_platform_line(uint16_t line) {
    (void) line;
    if (!idx_on)
        return;
    int now = (z80_in(0xE4) & 4) != 0;
    if (now && !idx_was) idx_pulses++;
    idx_lines += now;
    idx_was = now;
}
void gg_platform_vram_sync_at(uint16_t a) { (void) a; }
uint8_t gg_platform_vram_defer(uint16_t a, uint8_t v) { (void) a; (void) v; return 0; }
void gg_platform_disk_read(uint16_t lba, uint8_t *buf) { memcpy(buf, disk + (uint32_t) lba * 256, 256); }
void gg_platform_disk_write(uint16_t lba, const uint8_t *buf) { memcpy(disk + (uint32_t) lba * 256, buf, 256); }
void gg_platform_map_changed(void) { map_changes++; }
void gg_platform_code_write(uint16_t a) { (void) a; }

#define CHECK(what, cond) do { checks++; if (!(cond)) { failed++; printf("FAILED: %s (line %d)\n", what, __LINE__); } } while (0)

static uint8_t in(uint8_t p) { return z80_in(p); }
static void out(uint8_t p, uint8_t v) { z80_out(p, v); }

// a command's bytes to the controller: each asked for (RQM, not DIO)
static int command(const uint8_t *c, int n) {
    for (int i = 0; i < n; i++) {
        if ((in(0xE0) & 0xC0) != 0x80)
            return 0;
        out(0xE1, c[i]);
    }
    return 1;
}
// its result bytes, as many as it gives
static int result(uint8_t *r) {
    int n = 0;
    while ((in(0xE0) & 0xC0) == 0xC0 && n < 8)
        r[n++] = in(0xE1);
    return n;
}
static void tc(void) { out(0xE7, 0x05); out(0xE7, 0x04); }
static int seek(uint8_t track) {
    uint8_t c[3] = { 0x0F, 0, track }, r[8], s = 0x08;
    if (!command(c, 3) || !(in(0xE4) & 1))
        return 0;
    command(&s, 1);
    return result(r) == 2 && r[0] == 0x20 && r[1] == track && !(in(0xE4) & 1);
}
// one sector by READ DATA and TC; st: the first two result bytes
static int read_sector(uint8_t track, uint8_t side, uint8_t sector, uint8_t size, uint8_t *buf, uint8_t *st) {
    uint8_t c[9] = { 0x46, 0, track, side, sector, size, 0x10, 0x0E, 0xFF }, r[8];
    int n = 0;
    if (!command(c, 9))
        return -1;
    while ((in(0xE0) & 0xF0) == 0xF0 && n < 256)
        buf[n++] = in(0xE1);
    if (n == 256)
        tc();
    int k = result(r);
    st[0] = r[0]; st[1] = r[1];
    return k == 7 ? n : -1;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s system_disk.sf\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f || fread(disk, 1, SF_DISK_SIZE, f) != SF_DISK_SIZE) { perror(argv[1]); return 1; }
    fclose(f);
    sf_ram = ram64;
    sf_ipl = sf_ipl_builtin;
    sf_ipl_size = sizeof(sf_ipl_builtin);
    sf.inserted = 1;
    gg_init(1);
    uint8_t buf[256], st[2], r[8];

    // --- the memory: the IPL is read, a write goes to the RAM behind it
    CHECK("the IPL at reset", sf.rom_on && z80_read(0x0000) == 0xF3 && z80_read(0x2000) == 0xF3);
    z80_write(0x0000, 0x5A);
    z80_write(0x3FFF, 0xA5);
    z80_write(0x4000, 0x11);
    CHECK("a write under the IPL is not read back", z80_read(0x0000) == 0xF3 && z80_read(0x4000) == 0x11);
    CHECK("it went to the RAM", ram64[0x0000] == 0x5A && ram64[0x3FFF] == 0xA5);

    // --- the SF-7000's 8255: a mode word, then bits of port C one by one
    out(0xE7, 0x90);
    CHECK("the mode clears port C", in(0xE6) == 0x00 && sf.rom_on);
    out(0xE7, 0x03);
    CHECK("03h sets PC1 only (the motor off)", in(0xE6) == 0x02 && sf.rom_on);
    unsigned before = map_changes;
    out(0xE7, 0x0D);
    CHECK("0Dh sets PC6: RAM at 0000h", in(0xE6) == 0x42 && !sf.rom_on && map_changes == before + 1);
    CHECK("the byte written under the IPL is there", z80_read(0x0000) == 0x5A && z80_read(0x3FFF) == 0xA5);
    out(0xE7, 0x0C);
    CHECK("0Ch clears it: the IPL again", sf.rom_on && z80_read(0x0000) == 0xF3 && map_changes == before + 2);
    out(0xE6, 0x41);
    CHECK("port C written whole", in(0xE6) == 0x41 && !sf.rom_on);
    out(0xE7, 0x90);
    CHECK("a mode word again: the IPL back", sf.rom_on && in(0xE6) == 0);

    // --- the keyboard: rows by port C of the SC-3000's 8255, 0 = pressed
    CHECK("after a reset the joypads' row", in(0xDC) == 0xFF && (in(0xDD) & 0x0F) == 0x0F);
    gg.keys = GG_KEY_UP | GG_KEY_1;
    CHECK("joypad 1 up and button 1", in(0xDC) == (uint8_t) ~0x11);
    out(0xDF, 0x92);
    out(0xDE, 0x07);
    CHECK("the same with row 7 chosen", in(0xDC) == (uint8_t) ~0x11);
    out(0xDE, 0x05);
    CHECK("another row: no joypad there", in(0xDC) == 0xFF);
    sf_key_set(SF_K_ENTER, 1);
    CHECK("ENTER: row 5, PA6", in(0xDC) == (uint8_t) ~0x40);
    out(0xDE, 0x06);
    sf_key_set(SF_K_SHIFT, 1);
    CHECK("SHIFT: row 6, PB3", in(0xDC) == 0xFF && (in(0xDD) & 0x0F) == 0x07);
    sf_key_set(SF_K_ENTER, 0);
    sf_key_set(SF_K_SHIFT, 0);
    out(0xDF, 0x01);   // (bit 0 of port C set: row 7)
    CHECK("a bit of port C set through the control port", (in(0xDE) & 7) == 7);
    CHECK("'A': row 0 column 2", sf_char_key('A') == SF_KEY(0, 2));
    CHECK("'\"': SHIFT + 2", sf_char_key('"') == (SF_KEY(1, 0) | SF_KEY_SHIFT_BIT));
    CHECK("'a': SHIFT + A", sf_char_key('a') == (SF_KEY(0, 2) | SF_KEY_SHIFT_BIT));
    gg.keys = 0;

    // --- the uPD765
    out(0xE7, 0x07); out(0xE7, 0x06);          // its reset
    CHECK("idle: a command byte wanted, no interrupt", in(0xE0) == 0x80 && !(in(0xE4) & 1));
    { uint8_t c[3] = { 0x03, 0x60, 0x0B }; CHECK("SPECIFY", command(c, 3) && in(0xE0) == 0x80); }
    { uint8_t c = 0x08; command(&c, 1); CHECK("SENSE INTERRUPT with none: 80h", result(r) == 1 && r[0] == 0x80); }
    { uint8_t c[2] = { 0x07, 0 }, s = 0x08;
      CHECK("RECALIBRATE raises the interrupt", command(c, 2) && (in(0xE4) & 1));
      command(&s, 1);
      CHECK("and SENSE INTERRUPT says seek end, track 0", result(r) == 2 && r[0] == 0x20 && r[1] == 0 && !(in(0xE4) & 1)); }
    { uint8_t c[2] = { 0x04, 0 }; command(c, 2);
      CHECK("SENSE DRIVE STATUS: ready, track 0", result(r) == 1 && (r[0] & 0x30) == 0x30); }
    { uint8_t c[2] = { 0x4A, 0 }; command(c, 2);
      CHECK("READ ID", result(r) == 7 && r[0] == 0 && r[3] == 0 && r[6] == 1); }
    { uint8_t c[2] = { 0x4A, 0 }; out(0xE1, c[0]);
      CHECK("busy in the middle of a command", in(0xE0) == 0x90);
      out(0xE1, c[1]); result(r); }
    // the disk as its sectors
    CHECK("track 0 sector 1", read_sector(0, 0, 1, 1, buf, st) == 256 && st[0] == 0 && !memcmp(buf, "SYS:", 4));
    CHECK("the result phase ended", in(0xE0) == 0x80);
    CHECK("seek to track 20", seek(20));
    CHECK("track 20 sector 1: the directory", read_sector(20, 0, 1, 1, buf, st) == 256 && st[0] == 0
        && !memcmp(buf, disk + 20 * 16 * 256, 256) && buf[8] == '.');
    CHECK("track 20 sector 16", read_sector(20, 0, 16, 1, buf, st) == 256 && st[0] == 0
        && !memcmp(buf, disk + (20 * 16 + 15) * 256, 256));
    CHECK("sector 0: no data", read_sector(20, 0, 0, 1, buf, st) == 0 && st[0] == 0x40 && st[1] == 0x04);
    CHECK("sector 17: no data", read_sector(20, 0, 17, 1, buf, st) == 0 && st[0] == 0x40 && st[1] == 0x04);
    CHECK("the other side: no data", read_sector(20, 1, 1, 1, buf, st) == 0 && st[0] == 0x40);
    CHECK("sectors of 512 bytes: no data", read_sector(20, 0, 1, 2, buf, st) == 0 && st[0] == 0x40);
    CHECK("a track the head is not on: no data", read_sector(21, 0, 1, 1, buf, st) == 0 && st[0] == 0x40);
    CHECK("the controller takes commands again", in(0xE0) == 0x80);
    CHECK("seek to track 40: nothing there", seek(40) && read_sector(40, 0, 1, 1, buf, st) == 0 && st[0] == 0x40);
    CHECK("seek to track 39", seek(39) && read_sector(39, 0, 16, 1, buf, st) == 256 && st[0] == 0);
    // two sectors in one command, and the end of the track with no TC
    { uint8_t c[9] = { 0x46, 0, 39, 0, 15, 1, 0x10, 0x0E, 0xFF };
      int n = 0, ok = 1;
      command(c, 9);
      while ((in(0xE0) & 0xF0) == 0xF0 && n < 600) {
          uint8_t b = in(0xE1);
          if (n < 512 && b != disk[(39 * 16 + 14) * 256 + n]) ok = 0;
          n++;
      }
      int k = result(r);
      CHECK("sectors 15 and 16, then end of cylinder", n == 513 && ok && k == 7 && r[0] == 0x40 && r[1] == 0x80); }
    // TC in the middle of a sector
    { uint8_t c[9] = { 0x46, 0, 39, 0, 3, 1, 0x10, 0x0E, 0xFF };
      command(c, 9);
      for (int i = 0; i < 10; i++) in(0xE1);
      tc();
      CHECK("TC after 10 bytes: the result, an interrupt", in(0xE0) == 0xD0 && (in(0xE4) & 1));
      CHECK("normal end, the same sector", result(r) == 7 && r[0] == 0 && r[5] == 3 && !(in(0xE4) & 1)); }
    // writing
    { uint8_t c[9] = { 0x45, 0, 39, 0, 5, 1, 0x10, 0x0E, 0xFF };
      command(c, 9);
      CHECK("WRITE DATA wants bytes", in(0xE0) == 0xB0);
      for (int i = 0; i < 256; i++) out(0xE1, (uint8_t) (i ^ 0x3C));
      tc();
      int ok = result(r) == 7 && r[0] == 0 && r[5] == 6;
      for (int i = 0; i < 256; i++) if (disk[(39 * 16 + 4) * 256 + i] != (uint8_t) (i ^ 0x3C)) ok = 0;
      CHECK("the sector written", ok);
      CHECK("and read back", read_sector(39, 0, 5, 1, buf, st) == 256 && buf[1] == (1 ^ 0x3C)); }
    sf.protect = 1;
    { uint8_t c[9] = { 0x45, 0, 39, 0, 5, 1, 0x10, 0x0E, 0xFF };
      command(c, 9);
      CHECK("a protected disk: not writable", result(r) == 7 && r[0] == 0x40 && r[1] == 0x02); }
    sf.protect = 0;
    { uint8_t c[6] = { 0x4D, 0, 1, 16, 0x2A, 0xE5 };
      command(c, 6);
      for (int s = 1; s <= 16; s++) { out(0xE1, 39); out(0xE1, 0); out(0xE1, s); out(0xE1, 1); }
      int ok = result(r) == 7 && r[0] == 0;
      for (int i = 0; i < 16 * 256; i++) if (disk[39 * 16 * 256 + i] != 0xE5) ok = 0;
      CHECK("FORMAT A TRACK fills its 16 sectors", ok); }
    { uint8_t c = 0x1F; command(&c, 1); CHECK("not a command: 80h", result(r) == 1 && r[0] == 0x80); }
    sf.inserted = 0;
    { uint8_t c[2] = { 0x07, 0 }, s = 0x08; command(c, 2); command(&s, 1);
      CHECK("no disk: the seek ends abnormally, not ready", result(r) == 2 && (r[0] & 0xC8) == 0x48); }
    CHECK("no disk: a read ends at once", read_sector(0, 0, 1, 1, buf, st) == 0 && (st[0] & 0xC8) == 0x48);
    CHECK("no disk: no index", !(in(0xE4) & 4));
    sf.inserted = 1;
    // the index hole (the motor on: PC1 low) comes once in each turn
    out(0xE7, 0x02);
    gg_every_line = 1;
    idx_on = 1;
    for (int fr = 0; fr < 59; fr++) {
        z80.halted = 1;
        gg_run_frame();
    }
    idx_on = 0;
    // (716,000 cycles a turn, 14,000 of them with the hole there: 61 lines)
    CHECK("the index: 5 times in 59 frames (a turn is 12)", idx_pulses == 5);
    CHECK("each for about 61 lines", idx_lines >= 5 * 60 && idx_lines <= 5 * 63);
    out(0xE7, 0x03);
    CHECK("the motor off: no index", !(in(0xE4) & 4));

    printf("sf_unit: %u checks, %u failed\n", checks, failed);
    return failed ? 1 : 0;
}
