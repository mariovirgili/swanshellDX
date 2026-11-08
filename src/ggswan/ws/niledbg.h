/*
 * niledbg.h - optional homebrew API for the niledebug stub (nileswan USB).
 *
 * Add NILEDBG_API_TABLE once to one .c file of your program, build as
 * usual, then run `niledebug inject` (or `niledebug launch`) on the .ws:
 * the injector finds the "NDBGAPI" table and fills in the far pointers.
 * Without injection every call is a cheap no-op, so release builds can keep
 * the calls (or define NILEDBG_DISABLE to compile them out).
 *
 * The stub preserves every register and the interrupt flag.
 *
 * NOTE: written for gcc-ia16 (Wonderful toolchain, target-wswan, medium
 * model). Not yet compiled in this project's CI: check the generated code
 * the first time you use it.
 */
#ifndef NILEDBG_H_
#define NILEDBG_H_

#include <stdint.h>

typedef struct {
    char magic[8];                  /* "NDBGAPI\0" */
    uint16_t write_off, write_seg;  /* AL = channel, DS:SI = data, CX = len */
    uint16_t poll_off, poll_seg;    /* service the USB link (if no VBlank hook) */
    uint16_t brk_off, brk_seg;      /* pause until the PC says continue; AX = code */
    uint16_t hook_off, hook_seg;    /* same as poll, used by the VBlank thunk */
} niledbg_api_t;

extern const niledbg_api_t niledbg_api;

/* Put this in exactly one source file. It must end up in ROM (const). */
#define NILEDBG_API_TABLE \
    __attribute__((used)) const niledbg_api_t niledbg_api = \
        { "NDBGAPI", 0, 0, 0, 0, 0, 0, 0, 0 }

#ifdef NILEDBG_DISABLE
#define niledbg_write(ch, buf, len) ((void) 0)
#define niledbg_poll() ((void) 0)
#define niledbg_break(code) ((void) 0)
#else

static inline void niledbg_write(uint8_t channel, const void *buf, uint16_t len) {
    uint32_t fp = ((uint32_t) niledbg_api.write_seg << 16) | niledbg_api.write_off;
    if (!niledbg_api.write_seg) return;
    __asm__ volatile ("lcall *%[fp]"
        : : [fp] "m" (fp), "a" ((uint16_t) channel), "S" (buf), "c" (len)
        : "memory");
}

static inline void niledbg_poll(void) {
    uint32_t fp = ((uint32_t) niledbg_api.poll_seg << 16) | niledbg_api.poll_off;
    if (!niledbg_api.poll_seg) return;
    __asm__ volatile ("lcall *%[fp]" : : [fp] "m" (fp) : "memory");
}

static inline void niledbg_break(uint16_t code) {
    uint32_t fp = ((uint32_t) niledbg_api.brk_seg << 16) | niledbg_api.brk_off;
    if (!niledbg_api.brk_seg) return;
    __asm__ volatile ("lcall *%[fp]" : : [fp] "m" (fp), "a" (code) : "memory");
}
#endif

/* printf-like helper on top of niledbg_write (uses the caller's buffer) */
#define NILEDBG_LOG(buf, ...) do { \
        int n__ = snprintf((buf), sizeof(buf), __VA_ARGS__); \
        if (n__ > 0) niledbg_write(0, (buf), (uint16_t) (n__ < (int) sizeof(buf) ? n__ : (int) sizeof(buf) - 1)); \
    } while (0)

#endif /* NILEDBG_H_ */
