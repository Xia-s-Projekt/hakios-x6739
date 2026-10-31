/*
 * Debug output for bare-metal code on the X6739 (MT6893) that runs with the MMU off:
 *
 *  - plog_*: the ramoops console zone that Android's pstore keeps across a warm reset. What is
 *    written here shows up after the next Android boot as /sys/fs/pstore/console-ramoops-0.
 *    The zone comes from the phone's own kernel command line: "ramoops.mem_address=0x48090000
 *    ramoops.mem_size=0xe0000 ramoops.console_size=0x40000", so the console zone is at
 *    0x48090000 + 0xe0000 - 0x40000 = 0x48d90000. Header: sig "DBGC", start (next write), size.
 *  - fbcon_*: text on the panel.
 *
 * The panel part is NOT yet written for this board: the reference port drew into the framebuffer
 * LK left scanning on OVL0 layer 0 of the MT6768's display engine, and this board's engine is a
 * different pipeline with a different layer arrangement and a 1080x2400 panel. Until the
 * framebuffer the phone's own preloader leaves is found and read out, the loader's text goes to
 * the ramoops console only, and the screen stays as the preloader left it.
 *
 * Everything here is device memory (MMU off): no unaligned accesses, no SIMD, no dc zva.
 * Build with -mgeneral-regs-only -mstrict-align -fno-builtin.
 */
#ifndef MTKDBG_H
#define MTKDBG_H

#include <stdint.h>
#include "font8x16.h"

/*
 * The ramoops zone, as /proc/iomem on this phone gives it:
 *
 *   48090000-480eefff  dmesg (95 zones of 4 KB)
 *   480ef000-4812efff  console   (0x40000)
 *   4812f000-4812ffff  ftrace
 *   48130000-4816ffff  pmsg      (0x40000)
 *
 * so the console zone is at 0x480ef000 and the pmsg zone at 0x48130000. (Computing them from the
 * sizes in /proc/cmdline would put them in the wrong place: the zones are laid out in a fixed
 * order, not packed from the end.)
 */
#define RAMOOPS_BASE         0x48090000ULL
#define RAMOOPS_SIZE         0xe0000u
#define RAMOOPS_CONSOLE      0x480ef000ULL
#define RAMOOPS_CONSOLE_SIZE 0x40000u
#define RAMOOPS_PMSG         0x48130000ULL
#define RAMOOPS_PMSG_SIZE    0x40000u
#define RAMOOPS_SIG          0x43474244u

/*
 * The watchdog, as the tree's /soc/watchdog@10007000 says (mediatek,mt6893-wdt,
 * mediatek,mt6589-wdt, with a "mediatek,toprgu" syscon at 0x10007000): the reset register is
 * the same TOPRGU one the reference port used, because the reference board's compatible was the
 * mt6589 one as well.
 */
#define TOPRGU_BASE          0x10007000ULL
#define TOPRGU_MODE          (TOPRGU_BASE + 0x00)
#define TOPRGU_LENGTH        (TOPRGU_BASE + 0x04)
#define TOPRGU_RESTART       (TOPRGU_BASE + 0x08)
#define TOPRGU_SWRST         (TOPRGU_BASE + 0x14)
#define TOPRGU_MODE_KEY      0x22000000u
#define TOPRGU_RESTART_KEY   0x1971u
#define TOPRGU_SWRST_KEY     0x1209u

/* ---- the panel ---- */

/*
 * Not yet known. The reference port's panel was video mode with LK's framebuffer scanning on
 * OVL0 layer 0 (0x787b0000, pitch 2944 = 736 pixels, 720x1640) and per-pixel alpha on, which it
 * switched off because XNU's console writes transparent pixels.
 *
 * This board's panel is 1080x2400 over DSI0/DSI1 with an nt37703 panel node, and the display
 * engine is the mt6893-disp one at 0x14116000 with its own OVL0 (0x14000000) and OVL1
 * (0x14100000). Which layer the phone's preloader leaves scanning, where its framebuffer is
 * and what its pitch is: not yet read out of the phone.
 */
#define PANEL_W              1080u
#define PANEL_H              2400u

static inline uint32_t rd32(uint64_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uint64_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

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
	return f ? f : 13000000u;
}

static inline void dbg_delay_us(uint32_t us)
{
	uint64_t end = dbg_ticks() + (dbg_freq() / 1000000u) * us + 1;

	while (dbg_ticks() < end) {
	}
}

static inline void dbg_delay_ms(uint32_t ms)
{
	uint64_t end = dbg_ticks() + dbg_freq() / 1000 * ms;
	while (dbg_ticks() < end) {
	}
}

/* ---- watchdog ---- */

static inline void wdt_disable(void)
{
	wr32(TOPRGU_RESTART, TOPRGU_RESTART_KEY);
	wr32(TOPRGU_MODE, TOPRGU_MODE_KEY);
}

/* A warm reset, as Linux's mtk_wdt_restart does it: DRAM (and so pstore) survives. */
static inline void __attribute__((noreturn)) wdt_reset(void)
{
	wr32(TOPRGU_SWRST, TOPRGU_SWRST_KEY);
	for (;;) {
		__asm__ volatile("wfe");
	}
}

/* ---- ramoops console ---- */

/*
 * MT6893: the console zone holds plain text -- what /sys/fs/pstore/console-ramoops-0 shows on
 * this phone begins with "0 returned 1 after 1251 usecs" and the kernel's log, with no header of
 * our own in front of it.
 *
 * The reference port wrote its own header (sig "DBGC", start, size) at the front of the zone and
 * kept its text after it, so that the loader could find where it had left off. That header is
 * not there on this board: the zone belongs to Android's pstore driver, and the driver expects
 * the text to start at the beginning of the zone.
 *
 * So the loader writes at the start of the zone and keeps its own position in a variable -- the
 * loader runs once per boot, and the zone is cleared by the driver when Android reads it. A
 * second run without an Android boot in between would append, which is visible in the log and
 * harmless.
 */
#define PLOG_DATA ((volatile char *)RAMOOPS_CONSOLE)
#define PLOG_CAP  RAMOOPS_CONSOLE_SIZE

/* Where the loader's text ends. Kept in a variable, not in the zone. */
static uint32_t plog_at;

/*
 * Clear the console zone and start at its beginning.
 *
 * The zone holds the *previous* boot's kernel log -- /sys/fs/pstore/console-ramoops-0 on this
 * phone begins with the last kernel's messages, not with ours. The loader writes its own text
 * from the start of the zone, so the zone is cleared first: otherwise the loader's log and the
 * old kernel's would be one run of text with no way to tell them apart.
 *
 * The cost is the previous kernel's console log, which is gone once the loader runs. The
 * loader's own rescue path (rescue_log) is what keeps the XNU log, and that is appended after
 * the loader's text.
 */
static inline void plog_reset(void)
{
	for (uint32_t at = 0; at < PLOG_CAP; at += 64) {
		*(volatile uint64_t *)(RAMOOPS_CONSOLE + at) = 0;
	}
	plog_at = 0;
}

static inline void plog_putc(char c)
{
	if (plog_at >= PLOG_CAP) {
		return;
	}
	PLOG_DATA[plog_at++] = c;
}

/* ---- the loader's text ---- */

/*
 * The framebuffer text of the reference port is not carried over: this board's display pipeline
 * is different and the framebuffer its preloader leaves is not known yet (see above). The
 * loader's text goes to the ramoops console, and to the UART when the kernel's driver is not
 * the one writing.
 */
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
	/* clear the line about to be written, and a marker line under it */
	fb_rect(0, fbc.y, fbc.w, lh, fbc.bg);
	if (fbc.y + 2 * lh <= fbc.h) {
		fb_rect(0, fbc.y + lh, fbc.w, 2, 0xffff3030u);
	}
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
	static const char hex[] = "0123456789abcdef";

	for (int i = (digits - 1) * 4; i >= 0; i -= 4) {
		dbg_putc(hex[(v >> i) & 0xf]);
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
	}
	fbc.base = fb;
}

#endif /* MTKDBG_H */
