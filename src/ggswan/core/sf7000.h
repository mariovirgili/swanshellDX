/**
 * sfswan - SC-3000 + SF-7000 emulator for the WonderSwan (nileswan)
 * The SF-7000 on top of the SG-1000 machine (gg.c, sg_vdp.c): 64 KiB of
 * RAM, the IPL ROM over its first 16, two 8255s, the keyboard, the
 * uPD765 and its one drive. See SF7000.md.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GGSWAN_SF7000_H_
#define GGSWAN_SF7000_H_
#ifdef GGSWAN_SF7000
#include <stdint.h>
#include "gg.h"

#define SF_TRACKS 40
#define SF_SECTORS 16
#define SF_SECTOR_SIZE 256
#define SF_DISK_SIZE ((uint32_t) SF_TRACKS * SF_SECTORS * SF_SECTOR_SIZE)   // 163,840
// a turn of the disk, in CPU cycles: 200 ms (300 turns a minute), and the
// part of it the index hole is under the sensor
#define SF_TURN_CYCLES 716000ul
#define SF_INDEX_CYCLES 14000ul

enum { SF_FDC_COMMAND, SF_FDC_READ, SF_FDC_WRITE, SF_FDC_FORMAT, SF_FDC_RESULT };

typedef struct {
    // the SC-3000's 8255 (DC-DF): port C's latch, the mode
    uint8_t sc_c, sc_mode;
    // the SF-7000's (E4-E7): ports B and C, the mode
    uint8_t sf_b, sf_c, sf_mode;
    uint8_t rom_on;         // the IPL is read at 0000h-3FFFh (PC6 = 0)
    uint8_t inserted;       // a disk in the drive
    uint8_t protect;        // it cannot be written
    // the uPD765
    uint8_t phase;          // SF_FDC_*
    uint8_t cmd[9], cmd_n, cmd_len;
    uint8_t res[7], res_n, res_len;
    uint8_t irq;            // its INT line (the 8255's PA0)
    uint8_t seek_end;       // a seek or recalibrate ended: SENSE INTERRUPT STATUS says so
    uint8_t st0_seek;
    uint8_t pcn;            // the track the head is on
    uint8_t c, r, eot;      // the sector being transferred
    uint8_t fill;           // FORMAT: the byte the sectors are filled with
    uint8_t left;           // FORMAT: sectors still to come
    uint16_t pos;           // bytes of the sector transferred (256: all)
    uint8_t dirty;          // the buffer was written by the CPU
    uint8_t buf[SF_SECTOR_SIZE];
    // diagnostics
    uint16_t reads, writes, errors;
    uint8_t last_cmd;
} sf_t;

extern sf_t sf;
// the 64 KiB of RAM and the IPL (8 KiB, there again at 2000h), set by the
// platform before gg_init
extern uint8_t GGFAR *sf_ram;
extern const uint8_t GGFAR *sf_ipl;
extern uint16_t sf_ipl_size;
// the keyboard: for each row of the matrix (0-6) its 12 columns, 1 =
// pressed (row 7 is the joypads: gg.keys)
extern uint16_t sf_keys[7];

void sf_reset(void);
uint8_t sf_in(uint8_t port);
void sf_out(uint8_t port, uint8_t v);
// the matrix position of a character: row in bits 4-6, column in 0-3,
// bit 7: with SHIFT; 0xFF: none. And the keys with no character.
uint8_t sf_char_key(char c);
#define SF_KEY(row, col) (((row) << 4) | (col))
#define SF_KEY_SHIFT_BIT 0x80
#define SF_K_ENTER  SF_KEY(5, 6)
#define SF_K_SPACE  SF_KEY(1, 4)
#define SF_K_BREAK  SF_KEY(6, 8)
#define SF_K_SHIFT  SF_KEY(6, 11)
#define SF_K_CTRL   SF_KEY(6, 10)
#define SF_K_GRAPH  SF_KEY(6, 9)
#define SF_K_FUNC   SF_KEY(5, 11)
#define SF_K_HOME   SF_KEY(2, 4)
#define SF_K_INSDEL SF_KEY(3, 4)
#define SF_K_UP     SF_KEY(6, 6)
#define SF_K_DOWN   SF_KEY(4, 5)
#define SF_K_LEFT   SF_KEY(5, 5)
#define SF_K_RIGHT  SF_KEY(6, 5)
static inline void sf_key_set(uint8_t key, uint8_t down) {
    uint16_t bit = (uint16_t) 1 << (key & 15);
    uint8_t row = (key >> 4) & 7;
    if (row > 6)
        return;
    if (down) sf_keys[row] |= bit;
    else sf_keys[row] &= ~bit;
}

/* Provided by the machine (gg.c) */

// CPU cycles since the reset (the disk turns by them)
uint32_t sf_clock(void);
// the IPL came or went at 0000h-3FFFh
void sf_map_changed(void);

/* Provided by the platform */

// a sector of the disk (0-639: track * 16 + sector - 1) into or from buf
void gg_platform_disk_read(uint16_t lba, uint8_t *buf);
void gg_platform_disk_write(uint16_t lba, const uint8_t *buf);
// what the CPU reads at 0000h-3FFFh changed (sf.rom_on): translated code
// from there is no longer what is there
void gg_platform_map_changed(void);

#endif
#endif
