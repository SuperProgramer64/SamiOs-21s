/*
 * Debug output for bare-metal code on the Galaxy A21s (SM-A217F/M, Exynos 850) with the MMU off:
 *
 *  - plog_*: the ramoops *pmsg* zone. This phone's ramoops (mem 0xf0f10000, size 0x8000) has
 *    no console zone (console_size = 0): one 16 KiB dump record, then the 16 KiB pmsg zone at
 *    0xf0f14000. A persistent_ram_zone header (sig "DBGC", start, size) and text after it show
 *    up after Android's next boot as /sys/fs/pstore/pmsg-ramoops-0 (logd's binary records and
 *    our text side by side; `strings` reads it).
 *  - fbcon_*: text on S-Boot's framebuffer ("bootloader_fb", 0xfa000000, handed over to
 *    Android's decon driver). 720x1600, assumed 4 bytes a pixel, pitch 2880; the colour order
 *    is what the coloured bars are for.
 *  - wdt_pet: the two Exynos watchdogs (cluster 0 and 1); if S-Boot left them running they
 *    are kicked with their own reload value, nothing else is changed.
 *
 * Everything is device memory (MMU off): no unaligned accesses, no SIMD, no dc zva.
 * Build with -mgeneral-regs-only -mstrict-align -fno-builtin.
 */
#ifndef EXYDBG_H
#define EXYDBG_H

#include <stdint.h>
#include "font8x16.h"

#define RAMOOPS_BASE         0xf0f10000ULL
#define RAMOOPS_PMSG         (RAMOOPS_BASE + 0x4000)
#define RAMOOPS_PMSG_SIZE    0x4000u
#define RAMOOPS_SIG          0x43474244u   /* "DBGC" */

#define BOOT_FB_BASE         0xfa000000ULL
#define BOOT_FB_SIZE         0x01400000ULL
#define PANEL_W              720u
#define PANEL_H              1600u
#define BOOT_FB_PITCH        (PANEL_W * 4)

#define WDT_CL0              0x10050000ULL
#define WDT_CL1              0x10060000ULL
#define WTCON                0x00
#define WTDAT                0x04
#define WTCNT                0x08
#define WTCON_ENABLE         (1u << 5)
#define WTCON_RESET          (1u << 0)

static inline uint32_t rd32(uint64_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uint64_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

/* ---- watchdog ---- */

static inline void wdt_pet(void)
{
	if (rd32(WDT_CL0 + WTCON) & WTCON_ENABLE) {
		wr32(WDT_CL0 + WTCNT, rd32(WDT_CL0 + WTDAT));
	}
	if (rd32(WDT_CL1 + WTCON) & WTCON_ENABLE) {
		wr32(WDT_CL1 + WTCNT, rd32(WDT_CL1 + WTDAT));
	}
}

/* ---- time ---- */

static inline uint64_t dbg_ticks(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static inline uint64_t dbg_freq(void)
{
	uint64_t f;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	return f ? f : 26000000u;          /* the device tree's timer clock-frequency */
}

static inline void dbg_delay_ms(uint32_t ms)
{
	uint64_t end = dbg_ticks() + dbg_freq() / 1000 * ms;
	uint64_t pet = 0;

	while (dbg_ticks() < end) {
		if (dbg_ticks() >= pet) {
			wdt_pet();
			pet = dbg_ticks() + dbg_freq() / 10;
		}
	}
}

/* ---- ramoops pmsg zone ---- */

struct ramoops_hdr {
	uint32_t sig;
	uint32_t start;
	uint32_t size;
};

#define PLOG_HDR  ((volatile struct ramoops_hdr *)RAMOOPS_PMSG)
#define PLOG_DATA ((volatile char *)(RAMOOPS_PMSG + sizeof(struct ramoops_hdr)))
#define PLOG_CAP  (RAMOOPS_PMSG_SIZE - sizeof(struct ramoops_hdr))

static inline void plog_reset(void)
{
	PLOG_HDR->sig = RAMOOPS_SIG;
	PLOG_HDR->start = 0;
	PLOG_HDR->size = 0;
}

static inline void plog_putc(char c)
{
	uint32_t at = PLOG_HDR->start, size = PLOG_HDR->size;

	if (PLOG_HDR->sig != RAMOOPS_SIG || at >= PLOG_CAP || size > PLOG_CAP) {
		plog_reset();
		at = size = 0;
	}
	PLOG_DATA[at] = c;
	if (++at == PLOG_CAP) {
		at = 0;
	}
	if (size < PLOG_CAP) {
		size++;
	}
	PLOG_HDR->start = at;
	PLOG_HDR->size = size;
}

/* ---- framebuffer text ---- */

struct fbcon {
	uint64_t base;
	uint32_t pitch, w, h;
	uint32_t x, y, top, scale;
	uint32_t fg, bg;
};

static struct fbcon fbc;

static inline void fb_px(uint32_t x, uint32_t y, uint32_t c)
{
	*(volatile uint32_t *)(fbc.base + (uint64_t)y * fbc.pitch + (uint64_t)x * 4) = c;
}

static inline void fb_rect(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t c)
{
	if (!fbc.base) {
		return;
	}
	for (uint32_t y = y0; y < y0 + h && y < fbc.h; y++) {
		for (uint32_t x = x0; x < x0 + w && x < fbc.w; x++) {
			fb_px(x, y, c);
		}
	}
}

static inline void fbcon_init(uint64_t base, uint32_t pitch, uint32_t top, uint32_t scale)
{
	fbc.base = base;
	fbc.pitch = pitch;
	fbc.w = PANEL_W;
	fbc.h = PANEL_H;
	fbc.x = 0;
	fbc.y = fbc.top = top;
	fbc.scale = scale;
	fbc.fg = 0xffffffffu;
	fbc.bg = 0xff000000u;
}

static inline void fbcon_newline(void)
{
	uint32_t lh = 16 * fbc.scale;

	fbc.x = 0;
	fbc.y += lh;
	if (fbc.y + lh > fbc.h) {
		fbc.y = fbc.top;
	}
	fb_rect(0, fbc.y, fbc.w, lh, fbc.bg);
}

static inline void fbcon_putc(char c)
{
	uint32_t cw = 8 * fbc.scale;

	if (!fbc.base) {
		return;
	}
	if (c == '\n') {
		fbcon_newline();
		return;
	}
	if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) {
		c = '?';
	}
	if (fbc.x + cw > fbc.w) {
		fbcon_newline();
	}
	const unsigned char *g = font8x16[(unsigned char)c - 0x20];
	for (uint32_t row = 0; row < 16 * fbc.scale; row++) {
		unsigned char bits = g[row / fbc.scale];
		for (uint32_t col = 0; col < cw; col++) {
			fb_px(fbc.x + col, fbc.y + row, (bits & (0x80 >> (col / fbc.scale))) ? fbc.fg : fbc.bg);
		}
	}
	fbc.x += cw;
}

/* ---- both ---- */

static inline void dbg_putc(char c)
{
	plog_putc(c);
	fbcon_putc(c);
}

static inline void dbg_puts(const char *s)
{
	while (*s) {
		dbg_putc(*s++);
	}
}

static inline void dbg_hex(uint64_t v, int digits)
{
	for (int i = (digits - 1) * 4; i >= 0; i -= 4) {
		uint32_t n = (uint32_t)(v >> i) & 0xf;
		dbg_putc((char)(n < 10 ? '0' + n : 'a' + n - 10));
	}
}

static inline void dbg_dec(uint64_t v)
{
	char buf[21];
	int n = 0;
	do {
		buf[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n) {
		dbg_putc(buf[--n]);
	}
}

/* "name+off: w0 w1 w2 w3" lines for a register block; only the log gets these when quiet. */
static inline void dbg_dump(const char *name, uint64_t base, uint32_t from, uint32_t to, int quiet)
{
	uint64_t fb = fbc.base;

	if (quiet) {
		fbc.base = 0;
	}
	for (uint32_t off = from; off < to; off += 16) {
		dbg_puts(name);
		dbg_putc('+');
		dbg_hex(off, 3);
		dbg_putc(':');
		for (uint32_t i = 0; i < 16 && off + i < to; i += 4) {
			dbg_putc(' ');
			dbg_hex(rd32(base + off + i), 8);
		}
		dbg_putc('\n');
		wdt_pet();
	}
	fbc.base = fb;
}

#endif /* EXYDBG_H */
