// Minimal stand-ins for libws/libnile, to build ws/main.c on the PC.
#pragma once
#include <stdint.h>
#include <string.h>
#define __far
extern uint8_t iram[65536];
extern uint8_t ports[256];
void outportb(uint8_t p, uint8_t v);
void outportw(uint8_t p, uint16_t v);
uint8_t inportb(uint8_t p);
uint16_t inportw(uint8_t p);
void *MK_FP(uint16_t seg, uint16_t off);
uint16_t ws_keypad_scan(void);
void host_line_hook(uint16_t line);
#define IRAM(a) (iram + (a))
#define IRAM_OFF(p) ((uint16_t) ((uint8_t*) (p) - iram))
#define WS_DISPLAY_COLOR_MEM(n) ((uint16_t*) (iram + 0xFE00 + (n) * 32))
#define WS_DISPLAY_CTRL_PORT 0x00
#define WS_SCR_BASE_PORT 0x07
#define WS_DISPLAY_LINE_PORT 0x02
#define WS_CART_EXTBANK_RAM_PORT 0xC1
#define WS_CART_EXTBANK_ROM0_PORT 0xC2
#define WS_CART_EXTBANK_ROM1_PORT 0xC3
#define WS_CART_BANK_FLASH_PORT 0xCE
#define WS_CART_BANK_FLASH_ENABLE 1
#define WS_CART_BANK_FLASH_DISABLE 0
#define WS_SOUND_WAVE_BASE_PORT 0x8F
#define WS_SOUND_CH_CTRL_PORT 0x90
#define WS_SOUND_OUT_CTRL_PORT 0x91
#define WS_SOUND_NOISE_CTRL_PORT 0x8E
#define WS_SOUND_OUT_CTRL_HEADPHONE_ENABLE 0x08
#define WS_SOUND_OUT_CTRL_SPEAKER_ENABLE 0x01
#define WS_SOUND_OUT_CTRL_SPEAKER_VOLUME_100 0
#define WS_SOUND_NOISE_CTRL_LENGTH_1953 0
#define WS_SOUND_NOISE_CTRL_ENABLE 0x10
#define WS_SOUND_NOISE_CTRL_RESET 0x08
#define WS_KEY_X1 0x0010
#define WS_KEY_X2 0x0020
#define WS_KEY_X3 0x0040
#define WS_KEY_X4 0x0080
#define WS_KEY_A 0x0004
#define WS_KEY_B 0x0008
#define WS_KEY_START 0x0002
