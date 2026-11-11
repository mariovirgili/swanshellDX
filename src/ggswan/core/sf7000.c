/**
 * sfswan - SC-3000 + SF-7000 emulator for the WonderSwan (nileswan)
 * What the SF-7000 adds to the SG-1000's machine: the two 8255s, the
 * keyboard matrix, the uPD765 floppy controller with its drive. The memory
 * (64 KiB of RAM, the IPL over the first 16) is in gg.c, with the rest of
 * the machine. See SF7000.md.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef GGSWAN_SF7000
#include <string.h>
#include "sf7000.h"
#include "z80.h"

sf_t sf;
uint8_t sf_ipl_fast;
uint16_t sf_ipl_font;
uint8_t *sf_cmap;
uint8_t GGFAR *sf_ram;
const uint8_t GGFAR *sf_ipl;
uint16_t sf_ipl_size;
uint16_t sf_keys[7];

/* === The keyboard === */

/*
 * The matrix, as the SC-3000's 8255 reads it: port C bits 0-2 choose a
 * row, port A gives its columns 0-7 and port B bits 0-3 its columns 8-11,
 * 0 = pressed. Row 7 is the two joypads (what an SG-1000 game reads: with
 * port C never written, or all its bits set, that is the row).
 *
 *        0   1   2   3   4     5     6     7   8     9     10   11
 *   0    1   Q   A   Z   ED    ,     K     I   8
 *   1    2   W   S   X   SPC   .     L     O   9
 *   2    3   E   D   C   HOME  /     ;     P   0
 *   3    4   R   F   V   INS   PI    :     @   -
 *   4    5   T   G   B         DOWN  ]     [   ^
 *   5    6   Y   H   N         LEFT  CR        YEN               FUNC
 *   6    7   U   J   M         RIGHT UP        BREAK GRAPH CTRL  SHIFT
 *   7    1U  1D  1L  1R  1TL   1TR   2U    2D  2L    2R    2TL   2TR
 */
static const char sf_matrix[7][9] = {
    { '1', 'Q', 'A', 'Z', 0,   ',', 'K', 'I', '8' },
    { '2', 'W', 'S', 'X', ' ', '.', 'L', 'O', '9' },
    { '3', 'E', 'D', 'C', 0,   '/', ';', 'P', '0' },
    { '4', 'R', 'F', 'V', 0,   0,   ':', '@', '-' },
    { '5', 'T', 'G', 'B', 0,   0,   ']', '[', '^' },
    { '6', 'Y', 'H', 'N', 0,   0,   '\r', 0,  '\\' },
    { '7', 'U', 'J', 'M', 0,   0,   0,   0,   0 },
};
// the characters made with SHIFT, each followed by its key's own
static const char sf_shifted[] = "!1\"2#3$4%5&6'7(8)9=-<,>.?/+;*:`@{[}]~^|\\";

uint8_t sf_char_key(char c) {
    uint8_t shift = 0;
    if (c == '\n')
        c = '\r';
    if (c >= 'a' && c <= 'z') {
        c -= 32;
        shift = SF_KEY_SHIFT_BIT;
    }
    for (const char *s = sf_shifted; *s; s += 2)
        if (*s == c) {
            c = s[1];
            shift = SF_KEY_SHIFT_BIT;
            break;
        }
    for (uint8_t row = 0; row < 7; row++)
        for (uint8_t col = 0; col < 9; col++)
            if (sf_matrix[row][col] == c)
                return SF_KEY(row, col) | shift;
    return 0xFF;
}

// the columns of the row port C chooses, 1 = pressed
static uint16_t key_row(void) {
    // (port C's low half an input, as after a reset: its pins are high)
    uint8_t row = (sf.sc_mode & 0x01) ? 7 : sf.sc_c & 7;
    if (row == 7)
        return gg.keys & 0x3F;
    return sf_keys[row];
}

/* === The floppy controller === */

/*
 * A uPD765 as its programs see it, with the one drive of the SF-7000 and
 * no time: a command is done when its last byte is written, a seek at
 * once, and in a transfer the next byte is always there (the programs
 * poll the main status register for every byte: nothing is ever late).
 * The phases are the chip's: command bytes in, then for a read, a write
 * or a format the bytes of the sectors through the data register (no
 * DMA: the status register has EXM set meanwhile), then the result bytes
 * out. A transfer ends when the 8255's PC2 raises TC, or with the "end of
 * cylinder" error when the byte after the last sector's (EOT) is asked
 * for, as on the chip.
 */
#define MSR_RQM 0x80
#define MSR_DIO 0x40   // the chip has a byte for the CPU
#define MSR_EXM 0x20
#define MSR_CB  0x10

static void fdc_result(uint8_t n) {
    sf.phase = SF_FDC_RESULT;
    sf.res_n = 0;
    sf.res_len = n;
}

// the seven result bytes of a read, write or format; then the interrupt
static void fdc_end(uint8_t st0, uint8_t st1) {
    sf.res[0] = st0 | (sf.cmd[1] & 7);
    sf.res[1] = st1;
    sf.res[2] = 0;
    sf.res[3] = sf.c;
    sf.res[4] = 0;
    sf.res[5] = sf.r;
    sf.res[6] = 1;
    if (st0)
        sf.errors++;
    fdc_result(7);
    sf.irq = 1;
}

static uint8_t fdc_sector_ok(void) {
    return sf.c == sf.pcn && sf.c < SF_TRACKS && sf.r >= 1 && sf.r <= SF_SECTORS;
}

static uint16_t fdc_lba(void) {
    return (uint16_t) sf.c * SF_SECTORS + sf.r - 1;
}

// the sector after the one transferred (the result bytes say it too)
static void fdc_next(void) {
    if (sf.r == sf.eot) {
        sf.c++;
        sf.r = 1;
    } else {
        sf.r++;
    }
}

static void fdc_flush(void) {
    if (!sf.dirty)
        return;
    // (TC before the last byte: the chip fills the sector with zeros)
    if (sf.pos < SF_SECTOR_SIZE)
        memset(sf.buf + sf.pos, 0, SF_SECTOR_SIZE - sf.pos);
    gg_platform_disk_write(fdc_lba(), sf.buf);
    sf.writes++;
    sf.dirty = 0;
}

static void fdc_execute(void) {
    uint8_t op = sf.cmd[0] & 0x1F;
    sf.last_cmd = sf.cmd[0];
    switch (op) {
    case 0x03:   // SPECIFY
        sf.phase = SF_FDC_COMMAND;
        break;
    case 0x04:   // SENSE DRIVE STATUS: ready, track 0, write protected
        sf.res[0] = (sf.cmd[1] & 7) | (sf.inserted ? 0x20 : 0) | (sf.pcn == 0 ? 0x10 : 0)
            | (sf.protect ? 0x40 : 0);
        fdc_result(1);
        break;
    case 0x07:   // RECALIBRATE
    case 0x0F:   // SEEK
        sf.pcn = op == 0x07 ? 0 : sf.cmd[2];
        // (seek end; no disk: abnormal, not ready)
        sf.st0_seek = (sf.cmd[1] & 3) | (sf.inserted ? 0x20 : 0x68);
        sf.seek_end = 1;
        sf.irq = 1;
        sf.phase = SF_FDC_COMMAND;
        break;
    case 0x08:   // SENSE INTERRUPT STATUS
        if (sf.seek_end) {
            sf.res[0] = sf.st0_seek;
            sf.res[1] = sf.pcn;
            fdc_result(2);
        } else {
            sf.res[0] = 0x80;   // (no interrupt to tell of: invalid)
            fdc_result(1);
        }
        sf.seek_end = 0;
        sf.irq = 0;
        break;
    case 0x0A:   // READ ID: the first sector that comes by
        sf.c = sf.pcn;
        sf.r = 1;
        if (!sf.inserted)
            fdc_end(0x48, 0);
        else
            fdc_end(0, 0);
        break;
    case 0x05:   // WRITE DATA
    case 0x06:   // READ DATA
        sf.c = sf.cmd[2];
        sf.r = sf.cmd[4];
        sf.eot = sf.cmd[6];
        if (!sf.inserted) {
            fdc_end(0x48, 0);           // not ready
        } else if (sf.cmd[3] != 0 || sf.cmd[5] != 1 || !fdc_sector_ok()) {
            fdc_end(0x40, 0x04);        // no data: no such sector under the head
        } else if (op == 0x05 && sf.protect) {
            fdc_end(0x40, 0x02);        // not writable
        } else {
            sf.pos = 0;
            sf.dirty = 0;
            if (op == 0x06) {
                gg_platform_disk_read(fdc_lba(), sf.buf);
                sf.reads++;
                sf.phase = SF_FDC_READ;
            } else {
                sf.phase = SF_FDC_WRITE;
            }
            sf.irq = 1;
        }
        break;
    case 0x0D:   // FORMAT A TRACK: sectors of cmd[2]'s size, cmd[3] of them, filled with cmd[5]
        sf.c = sf.pcn;
        sf.r = 1;
        sf.eot = SF_SECTORS;
        if (!sf.inserted) {
            fdc_end(0x48, 0);
        } else if (sf.protect) {
            fdc_end(0x40, 0x02);
        } else if (sf.cmd[2] != 1 || sf.cmd[3] == 0 || sf.cmd[3] > SF_SECTORS || sf.pcn >= SF_TRACKS) {
            fdc_end(0x40, 0x04);
        } else {
            sf.left = sf.cmd[3];
            sf.fill = sf.cmd[5];
            sf.pos = 0;
            sf.phase = SF_FDC_FORMAT;
            sf.irq = 1;
        }
        break;
    default:     // not one of the chip's, or not done here
        sf.res[0] = 0x80;
        fdc_result(1);
        break;
    }
}

static uint8_t fdc_command_length(uint8_t first) {
    switch (first & 0x1F) {
    case 0x03: return 3;
    case 0x04: return 2;
    case 0x05: case 0x06: return 9;
    case 0x07: return 2;
    case 0x0A: return 2;
    case 0x0D: return 6;
    case 0x0F: return 3;
    default: return 1;
    }
}

static uint8_t fdc_status(void) {
    switch (sf.phase) {
    case SF_FDC_READ: return MSR_RQM | MSR_DIO | MSR_EXM | MSR_CB;
    case SF_FDC_WRITE:
    case SF_FDC_FORMAT: return MSR_RQM | MSR_EXM | MSR_CB;
    case SF_FDC_RESULT: return MSR_RQM | MSR_DIO | MSR_CB;
    default: return MSR_RQM | (sf.cmd_n ? MSR_CB : 0);
    }
}

static uint8_t fdc_read(void) {
    if (sf.phase == SF_FDC_RESULT) {
        uint8_t v = sf.res[sf.res_n++];
        sf.irq = 0;
        if (sf.res_n >= sf.res_len)
            sf.phase = SF_FDC_COMMAND;
        return v;
    }
    if (sf.phase != SF_FDC_READ)
        return 0xFF;
    if (sf.pos == SF_SECTOR_SIZE) {
        // no TC after the sector's last byte: the next one, or the end
        // of the cylinder
        if (sf.r == sf.eot) {
            fdc_end(0x40, 0x80);
            return 0xFF;
        }
        sf.r++;
        if (!fdc_sector_ok()) {
            fdc_end(0x40, 0x04);
            return 0xFF;
        }
        gg_platform_disk_read(fdc_lba(), sf.buf);
        sf.reads++;
        sf.pos = 0;
    }
    return sf.buf[sf.pos++];
}

static void fdc_write(uint8_t v) {
    switch (sf.phase) {
    case SF_FDC_COMMAND:
        if (sf.cmd_n == 0)
            sf.cmd_len = fdc_command_length(v);
        sf.cmd[sf.cmd_n++] = v;
        if (sf.cmd_n >= sf.cmd_len) {
            sf.cmd_n = 0;
            fdc_execute();
        }
        break;
    case SF_FDC_WRITE:
        if (sf.pos == SF_SECTOR_SIZE) {
            fdc_flush();
            if (sf.r == sf.eot) {
                fdc_end(0x40, 0x80);
                break;
            }
            sf.r++;
            if (!fdc_sector_ok()) {
                fdc_end(0x40, 0x04);
                break;
            }
            sf.pos = 0;
        }
        sf.buf[sf.pos++] = v;
        sf.dirty = 1;
        break;
    case SF_FDC_FORMAT:
        // four bytes for each sector (track, side, sector, size): the
        // third says which
        if ((sf.pos++ & 3) == 2)
            sf.r = v;
        if ((sf.pos & 3) == 0) {
            if (fdc_sector_ok()) {
                memset(sf.buf, sf.fill, SF_SECTOR_SIZE);
                gg_platform_disk_write(fdc_lba(), sf.buf);
                sf.writes++;
            }
            if (--sf.left == 0)
                fdc_end(0, 0);
        }
        break;
    default:
        break;
    }
}

// TC: the transfer ends here
static void fdc_tc(void) {
    switch (sf.phase) {
    case SF_FDC_READ:
        if (sf.pos == SF_SECTOR_SIZE)
            fdc_next();
        fdc_end(0, 0);
        break;
    case SF_FDC_WRITE:
        fdc_flush();
        if (sf.pos == SF_SECTOR_SIZE)
            fdc_next();
        fdc_end(0, 0);
        break;
    case SF_FDC_FORMAT:
        fdc_end(0, 0);
        break;
    default:
        break;
    }
}

static void fdc_reset(void) {
    sf.phase = SF_FDC_COMMAND;
    sf.cmd_n = 0;
    sf.irq = 0;
    sf.seek_end = 0;
    sf.dirty = 0;
}

/* === The 8255s === */

/*
 * The SF-7000's port C (E6h, or bit by bit through E7h):
 *   0  /INUSE to the drive       1  /MOTOR ON
 *   2  TC to the uPD765          3  RESET to the uPD765
 *   6  /ROM SEL: 0 the IPL at 0000h-3FFFh, 1 RAM
 *   7  /STROBE to the printer
 * and its port A (E4h): 0 the uPD765's INT, 1 the printer's BUSY, 2 the
 * drive's INDEX. Port B (E5h) is the printer's data: nothing here.
 */
static uint32_t turn;   // the start of the turn the disk is in
static void sf_c_write(uint8_t v) {
    uint8_t old = sf.sf_c;
    sf.sf_c = v;
    if ((old & ~v) & 0x02)
        turn = sf_clock();   // the motor starts: the hole is at the sensor
    if ((v & ~old) & 0x04)
        fdc_tc();
    if ((v & ~old) & 0x08)
        fdc_reset();
    uint8_t rom = !(v & 0x40);
    if (rom != sf.rom_on) {
        sf.rom_on = rom;
        sf_map_changed();
    }
}

// the index hole: once a turn, while the motor is on
static uint8_t sf_index(void) {
    if ((sf.sf_c & 0x02) || !sf.inserted)
        return 0;
    // (a turn starts when the motor does: sf_c_write. So the hole is where
    // the time since then puts it, whenever this is asked)
    uint32_t now = sf_clock();
    if (now < turn)
        turn = now;         // (a reset)
    if (now - turn > 16 * SF_TURN_CYCLES)
        turn = now - (now - turn) % SF_TURN_CYCLES;
    while (now - turn >= SF_TURN_CYCLES)
        turn += SF_TURN_CYCLES;
    return now - turn < SF_INDEX_CYCLES;
}

// Port A of the SF-7000's 8255 for the next `cycles` CPU cycles, if time
// alone does not change it meanwhile (the index hole coming or going): 1
// and *value (for a recompiler: translated code reads it without asking,
// until something is done to the 8255 or the controller); else 0
uint8_t sf_port_a_steady(uint32_t cycles, uint8_t *value) {
    uint8_t index = 0;
    if (!(sf.sf_c & 0x02) && sf.inserted) {
        index = sf_index();
        uint32_t at = sf_clock() - turn;
        if ((index ? SF_INDEX_CYCLES : SF_TURN_CYCLES) - at <= cycles)
            return 0;
    }
    *value = (sf.irq ? 0x01 : 0) | (index ? 0x04 : 0);
    return 1;
}

// a write to an 8255's control port: the mode (all its outputs then 0),
// or one bit of port C set or cleared
static uint8_t ppi_control(uint8_t v, uint8_t *mode, uint8_t c) {
    if (v & 0x80) {
        *mode = v;
        return 0;
    }
    uint8_t bit = 1 << ((v >> 1) & 7);
    return (v & 1) ? c | bit : c & ~bit;
}

/* === The built-in IPL's long parts === */

/*
 * The IPL is run by the interpreter (the recompiler knows the SF-7000's
 * RAM only), and its loops are long: the memory filled, the font written,
 * and above all the disk's loader calling RST 10h for the 121 sectors of
 * Disk BASIC, each read with a status poll for every byte. The built-in
 * IPL (host/mksfipl.py) asks for them here instead, a byte to port ECh
 * while it is at 0000h: what is done is what its Z80 code does (the same
 * IPL built with fast=False: the PC tools compare the memory the two
 * leave), through the same ports and memory functions.
 *   1  a first start: the VRAM and the RAM as the SF-7000's self test
 *      leaves them
 *   2  the screen: nothing in characters 00h-1Fh, the font from 20h, 960
 *      spaces at 0800h
 *   3  read, 4 write: the sector B of track C, at DE (RST 10h, 18h). The
 *      result is read from ECh: 0, or not 0 for an error
 */
static uint8_t svc_result;

static void sf_service(uint8_t fn) {
    switch (fn) {
    case 1: {
        uint16_t n = 0x4000, a = 0;
        z80_in(0xBF);
        z80_out(0xBF, 0x90); z80_out(0xBF, 0x81);     // register 1: the display off
        z80_out(0xBF, 0x00); z80_out(0xBF, 0x40);     // write from 0000h
        do z80_out(0xBE, (uint8_t) (n + (n >> 8))); while (--n);
        do z80_write(a, (uint8_t) (a + (a >> 8))); while (++a);
        break;
    }
    case 2:
        z80_out(0xBF, 0x00); z80_out(0xBF, 0x58);     // write from 1800h
        for (uint16_t i = 0; i < 256; i++)
            z80_out(0xBE, 0);
        for (uint16_t i = 0; i < 224 * 8; i++)
            z80_out(0xBE, sf_ipl[sf_ipl_font + i]);
        z80_out(0xBF, 0x00); z80_out(0xBF, 0x48);     // write from 0800h
        for (uint16_t i = 0; i < 960; i++)
            z80_out(0xBE, 0x20);
        break;
    case 3:
    case 4: {
        uint8_t track = z80.c, sector = z80.b;
        uint16_t at = ((uint16_t) z80.d << 8) | z80.e;
        svc_result = 1;
        // (the seek: the head there, its interrupt taken)
        sf.pcn = track;
        sf.seek_end = 0;
        sf.irq = 0;
        sf.phase = SF_FDC_COMMAND;
        sf.cmd_n = 0;
        if (!sf.inserted || track >= SF_TRACKS || sector < 1 || sector > SF_SECTORS
                || (fn == 4 && sf.protect)) {
            sf.errors++;
            break;
        }
        uint16_t lba = (uint16_t) track * SF_SECTORS + sector - 1;
        if (fn == 3) {
            gg_platform_disk_read(lba, sf.buf);
            for (uint16_t i = 0; i < SF_SECTOR_SIZE; i++)
                z80_write(at + i, sf.buf[i]);
            sf.reads++;
        } else {
            for (uint16_t i = 0; i < SF_SECTOR_SIZE; i++)
                sf.buf[i] = z80_read(at + i);
            gg_platform_disk_write(lba, sf.buf);
            sf.writes++;
        }
        svc_result = 0;
        break;
    }
    default:
        break;
    }
}

uint8_t sf_in(uint8_t port) {
    if (port < 0xE0) {
        // the SC-3000's 8255: the keyboard
        uint16_t k;
        switch (port & 3) {
        case 0:
            return (uint8_t) ~key_row();
        case 1:
            // (bits 4-7: the printer not busy and with no fault, nothing
            // from the cassette)
            k = key_row();
            return (uint8_t) ((~(k >> 8) & 0x0F) | 0xB0);
        case 2:
            return sf.sc_c;
        default:
            return 0xFF;
        }
    }
    switch ((port >> 2) & 7) {
    case 0:   // E0h-E3h: the uPD765
        return (port & 1) ? fdc_read() : fdc_status();
    case 1:   // E4h-E7h: the SF-7000's 8255
        switch (port & 3) {
        case 0: return (sf.irq ? 0x01 : 0) | (sf_index() ? 0x04 : 0);
        case 1: return sf.sf_b;
        case 2: return sf.sf_c;
        default: return 0xFF;
        }
    case 2:   // E8h-EBh: the 8251 (RS-232C): nothing on its line, ready to send
        return (port & 1) ? 0x05 : 0xFF;
    case 3:   // ECh: the built-in IPL's result (sf_service)
        if (port == 0xEC && sf_ipl_fast && sf.rom_on)
            return svc_result;
        return 0xFF;
    default:
        return 0xFF;
    }
}

void sf_out(uint8_t port, uint8_t v) {
    if (port < 0xE0) {
        switch (port & 3) {
        case 2:
            sf.sc_c = v;
            break;
        case 3:
            sf.sc_c = ppi_control(v, &sf.sc_mode, sf.sc_c);
            break;
        default:
            break;
        }
        return;
    }
    switch ((port >> 2) & 7) {
    case 0:
        if (port & 1)
            fdc_write(v);
        break;
    case 1:
        switch (port & 3) {
        case 1: sf.sf_b = v; break;
        case 2: sf_c_write(v); break;
        case 3: sf_c_write(ppi_control(v, &sf.sf_mode, sf.sf_c)); break;
        default: break;
        }
        break;
    case 3:
        if (port == 0xEC && sf_ipl_fast && sf.rom_on)
            sf_service(v);
        break;
    default:
        break;
    }
}

void sf_reset(void) {
    uint8_t inserted = sf.inserted, protect = sf.protect;
    memset(&sf, 0, sizeof(sf));
    sf.inserted = inserted;
    sf.protect = protect;
    // (the 8255s: all inputs; port C's pins of the SF-7000's are then
    // low for what they drive: the IPL is there)
    sf.sc_mode = 0x9B;
    sf.sf_mode = 0x9B;
    sf.rom_on = 1;
    memset(sf_keys, 0, sizeof(sf_keys));
}
#endif
