// Test harness: the Z80 core with a flat 64 KiB memory, for ctypes.
#include <stdint.h>
#include <string.h>
#include "../core/z80.h"

uint8_t mem[65536];
uint8_t io_in_value = 0xFF;
uint8_t z80_read(uint16_t a) { return mem[a]; }
void z80_write(uint16_t a, uint8_t v) { mem[a] = v; }
uint8_t z80_in(uint8_t p) { (void) p; return io_in_value; }
void z80_out(uint8_t p, uint8_t v) { (void) p; (void) v; }

z80_t *t_state(void) { return &z80; }
uint8_t *t_mem(void) { return mem; }
void t_init(void) { z80_init(); }
int t_step(void) { z80.cycles = 0; return -z80_run(1) + 1; }
