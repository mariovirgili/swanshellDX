// Whole-system test of the recompiler: ws/main.c (video), ws/drc_rt.c
// (runtime), drc/drc.c, gg.c and z80.c built for the PC with stand-in
// WonderSwan ports and memory. drc_enter() calls back into drcsys.py, which
// runs the translated x86 code in Unicorn on the same memory.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wsshim.h"
#include "../core/gg.h"
#include "../core/z80.h"
#include "../ws/drc_rt.h"

uint8_t iram[65536];
uint8_t ports[256];
uint8_t psram[32 * 65536];
uint8_t sram[8 * 65536];
uint32_t code_version;
static uint8_t fb[GG_VIEW_H][GG_VIEW_W][3];

void outportb(uint8_t p, uint8_t v) { ports[p] = v; }
void outportw(uint8_t p, uint16_t v) { ports[p] = v; ports[(uint8_t) (p + 1)] = v >> 8; }
uint8_t inportb(uint8_t p) { return ports[p]; }
uint16_t inportw(uint8_t p) { return ports[p] | (ports[(uint8_t) (p + 1)] << 8); }
void *MK_FP(uint16_t seg, uint16_t off) {
    uint16_t b;
    switch (seg) {
    case 0x1000:
        b = inportw(0xC1);
        if (ports[0xCE] & 1) return psram + (uint32_t) (b & 31) * 65536 + off;
        return sram + (uint32_t) (b & 7) * 65536 + off;
    case 0x2000: return psram + (uint32_t) (inportw(0xC2) & 31) * 65536 + off;
    case 0x3000: return psram + (uint32_t) (inportw(0xC3) & 31) * 65536 + off;
    case 0x4000: case 0x5000: case 0x6000: case 0x7000: case 0x8000: case 0x9000:
        return psram + (20 + ((seg - 0x4000) >> 12)) * 65536 + off;
    default: return iram + seg * 16 + off;
    }
}
uint16_t ws_keypad_scan(void) { return 0; }
void host_code_changed(void) { code_version++; }
void drc_check_failed(uint16_t pc, const z80_t *before, const z80_t *drc,
                      const z80_t *ref, int16_t used, int16_t used_ref,
                      uint16_t sum_drc, uint16_t sum_ref) {
    printf("SELF-CHECK difference at block %04X: pc drc %04X ref %04X, cycles %d/%d, ram %04X/%04X\n",
           pc, drc->pc, ref->pc, used, used_ref, sum_drc, sum_ref);
    exit(1);
}
void sys_check(int on) { drc_check = on; drc_no_links = on; }
int trace_on;
static uint32_t miss_empty, miss_evict;
void host_miss(int e) { if (e) miss_evict++; else miss_empty++; }
void sys_trace(int on) { trace_on = on; }
void host_trace(uint16_t pc, uint8_t b, uint16_t hl) { if (trace_on || pc == 0xBAD0) printf("  block %04X  B=%02X HL=%04X cyc=%d\n", pc, b, hl, (int) z80.cycles); }
static uint32_t tr_bytes, tr_instr, tr_count, tr_unique, tr_unique_bytes;
static uint32_t tr_op[256], tr_op_bytes[256];
static uint16_t last_flush_seen;
static int lf_count;
void host_lookup_fail(uint16_t pc, uint8_t bank, uint16_t h, uint16_t off, uint16_t tpc, uint8_t tbank) {
    if (off && lf_count < 12) { lf_count++; printf("  miss %04X/%d at h=%d: entry %04X/%d off %04X\n", pc, bank, h, tpc, tbank, off); }
}
static uint32_t retr_same, key_count[64][65536 / 64 * 64 / 64];
void host_translated(uint16_t pc, uint16_t len, uint16_t n) {
    {
        static uint16_t gen[64][65536];
        static uint16_t cur = 1;
        extern uint32_t drc_stat_flushes;
        if (drc_stat_flushes != last_flush_seen) { last_flush_seen = drc_stat_flushes; cur++; }
        uint8_t bk = gg.mapper[1 + (pc >> 14)] & 63;
        if (gen[bk][pc] == cur) {
            retr_same++;
            if (retr_same < 4) {
                extern uint8_t psram[];
                printf("  retranslated %04X bank %d (len %d)\n", pc, bk, len);
            }
        }
        if (!gen[bk][pc]) { FILE *kf = fopen("/tmp/claude-0/keys.txt", "a"); fprintf(kf, "%d %d\n", pc, gg.mapper[1 + (pc >> 14)]); fclose(kf); }
        gen[bk][pc] = cur;
    }
    tr_bytes += len; tr_instr += n; tr_count++;
    static uint8_t seen[64][8192];
    uint8_t bk = gg.mapper[1 + (pc >> 14)] & 63;
    if (!(seen[bk][pc >> 3] & (1 << (pc & 7)))) { seen[bk][pc >> 3] |= 1 << (pc & 7); tr_unique++; tr_unique_bytes += len; }
    uint8_t op = z80_read(pc);
    if (op == 0xED) op = z80_read(pc + 1);   // (ED ops counted as their second byte)
    tr_op[op]++; tr_op_bytes[op] += len;
}
extern uint32_t drc_stat_helper[5], drc_stat_dispatch, drc_stat_nolink;
void sys_tr_report(void) {
    printf("dispatches %u (exits that can not be linked: %u), helpers: write %u out %u in %u step %u\n",
           drc_stat_dispatch, drc_stat_nolink, drc_stat_helper[1], drc_stat_helper[2], drc_stat_helper[3], drc_stat_helper[4]);
    printf("retranslated without a flush: %u\n", retr_same);
    printf("misses: %u empty, %u evictions\n", miss_empty, miss_evict);
    printf("unique blocks %u (%u bytes)\n", tr_unique, tr_unique_bytes);
    printf("translated %u blocks, %u bytes, %u Z80 instructions (%.1f bytes per instruction)\n",
           tr_count, tr_bytes, tr_instr, tr_instr ? (double) tr_bytes / tr_instr : 0);
    for (int k = 0; k < 6; k++) {
        int best = 0;
        for (int i = 1; i < 256; i++) if (tr_op_bytes[i] > tr_op_bytes[best]) best = i;
        printf("  first byte %02X: %u blocks, %u bytes\n", best, tr_op[best], tr_op_bytes[best]);
        tr_op_bytes[best] = 0;
    }
}

/* reference renderer (screenshots) */
static inline uint8_t tile_pixel(uint16_t tile, uint8_t row, uint8_t col) {
    const uint8_t *p = gg_vram + ((tile * 32 + row * 4) & 0x3FFF);
    uint8_t bit = 7 - col;
    return ((p[0] >> bit) & 1) | (((p[1] >> bit) & 1) << 1)
        | (((p[2] >> bit) & 1) << 2) | (((p[3] >> bit) & 1) << 3);
}
static void render_line(uint16_t line) {
    uint8_t color[256], bg_prio[256], used[256] = {0};
    uint8_t *reg = gg.vdp.reg;
    uint8_t backdrop = 16 + (reg[7] & 0x0F);
    if (!gg_display_enabled()) {
        memset(color, backdrop, sizeof(color));
    } else {
        uint16_t nt = gg_name_table_base();
        uint8_t hscroll = (line < 16 && (reg[0] & 0x40)) ? 0 : reg[8];
        for (uint16_t x = 0; x < 256; x++) {
            uint16_t vs = ((x >> 3) >= 24 && (reg[0] & 0x80)) ? 0 : gg.vdp.vscroll_frame;
            uint16_t y = (line + vs) % 224;
            uint8_t sx = (uint8_t) (x - hscroll);
            uint16_t ea = nt + ((y >> 3) * 32 + (sx >> 3)) * 2;
            uint16_t e = gg_vram[ea & 0x3FFF] | (gg_vram[(ea + 1) & 0x3FFF] << 8);
            uint8_t row = y & 7, c = sx & 7;
            if (e & 0x400) row = 7 - row;
            if (e & 0x200) c = 7 - c;
            uint8_t px = tile_pixel(e & 0x1FF, row, c);
            color[x] = px + ((e & 0x800) ? 16 : 0);
            bg_prio[x] = (e & 0x1000) && px;
        }
        uint16_t sat = gg_sat_base();
        uint8_t height = (reg[1] & 0x02) ? 16 : 8, drawn = 0;
        for (uint8_t i = 0; i < 64 && drawn < 8; i++) {
            uint8_t sy = gg_vram[sat + i];
            if (sy == 0xD0) break;
            int16_t top = sy + 1;
            if (top > 0xF0) top -= 256;
            if (line < top || line >= top + height) continue;
            drawn++;
            int16_t sx = gg_vram[sat + 0x80 + i * 2];
            uint16_t n = gg_vram[sat + 0x81 + i * 2];
            if (reg[6] & 0x04) n |= 0x100;
            if (height == 16) n &= ~1;
            if (reg[0] & 0x08) sx -= 8;
            uint8_t row = line - top;
            for (uint8_t c = 0; c < 8; c++) {
                int16_t x = sx + c;
                if (x < 0 || x > 255 || used[x]) continue;
                uint8_t px = tile_pixel(n + (row >> 3), row & 7, c);
                if (!px) continue;
                used[x] = 1;
                if (!bg_prio[x]) color[x] = 16 + px;
            }
        }
        if (reg[0] & 0x20) memset(color, backdrop, 8);
    }
    if (line >= GG_VIEW_Y && line < GG_VIEW_Y + GG_VIEW_H)
        for (uint16_t x = 0; x < GG_VIEW_W; x++) {
            uint8_t ci = color[x + GG_VIEW_X];
            uint16_t c = gg_cram[ci * 2] | (gg_cram[ci * 2 + 1] << 8);
            fb[line - GG_VIEW_Y][x][0] = (c & 0xF) * 17;
            fb[line - GG_VIEW_Y][x][1] = ((c >> 4) & 0xF) * 17;
            fb[line - GG_VIEW_Y][x][2] = ((c >> 8) & 0xF) * 17;
        }
}
void host_line_hook(uint16_t line) { if (line < GG_ACTIVE_LINES) render_line(line); }

/* drc_enter: Python runs the code (z80 and drc_st are mirrored in IRAM) */
typedef void (*run_cb_t)(uint16_t offset);
static run_cb_t run_cb;
void sys_set_callback(run_cb_t cb) { run_cb = cb; }
void drc_enter(uint16_t offset) {
    memcpy(iram + HOST_Z80_ADDR, &z80, sizeof(z80));
    memcpy(iram + HOST_ST_ADDR, &drc_st, sizeof(drc_st));
    run_cb(offset);
    memcpy(&z80, iram + HOST_Z80_ADDR, sizeof(z80));
    memcpy(&drc_st, iram + HOST_ST_ADDR, sizeof(drc_st));
}

void video_init(void);
void video_present(void);
void sound_init(void);

int sys_init(const char *path, int use_drc) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(psram, 1, 512 * 1024, f);
    fclose(f);
    uint16_t banks = (n + 0x3FFF) >> 14;
    video_init();
    sound_init();
    if (use_drc) {
        drc_rt_init(20);
        gg_cpu_run = drc_z80_run;
    }
    gg_init(banks);
    return banks;
}
void sys_frame(int keys) { gg.keys = keys; gg_run_frame(); video_present(); }
uint8_t *sys_fb(void) { return &fb[0][0][0]; }
uint8_t *sys_iram(void) { return iram; }
uint8_t *sys_psram(void) { return psram; }
uint8_t *sys_sram(void) { return sram; }
uint8_t *sys_ports(void) { return ports; }
uint32_t sys_code_version(void) { return code_version; }
uint32_t sys_stat(int i) { return i == 0 ? drc_stat_blocks : i == 1 ? drc_stat_flushes : drc_stat_interp; }
int sys_pc(void) { return z80.pc; }
