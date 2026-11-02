/*
 * mtkloader: boots XNU on the X6739 (MT6893) from boot_a, and brings Android back afterwards
 * with XNU's log in pstore.
 *
 * This is the reference MediaTek port's loader (hakios-mt6768, the Infinix Hot 20 X6826B,
 * MT6768) adapted to this board. The boot chain, the payload handling, the XNU symbol patching,
 * the panic log and the chain back to Android are the same work on every board and carry over
 * as they are. What is specific to the MT6893 is in mt6893.h and in the places marked "MT6893"
 * below.
 *
 * The boot image's kernel (tools/chainboot.py) is this program, then Android's own kernel Image
 * at CHAIN_OFFSET, then the XNU payload ("XNUP": kernel, ramdisk). Each boot, by the marker:
 *
 *   none     RUN: load XNU, give it the framebuffer, arm the watchdog, start it.
 *   RUNNING  XNU was cut short by the watchdog: copy its log from DRAM into ramoops, start
 *            Android.
 *   DONE     XNU came back to us (panic, early exception, orderly end) and its log was saved
 *            then: start Android.
 *
 * The log ends up in /sys/fs/pstore/console-ramoops-0 under Android. See mtkdbg.h for the
 * screen and ramoops output.
 *
 * MT6893: the display is not the reference board's. This board's panel is 1080x2400 over
 * DSI0/DSI1 with an nt37703 panel node, and the display engine is the mt6893-disp one at
 * 0x14116000 -- not the MT6768's OVL0 layer 0 at 0x787b0000 that LK left scanning. Until the
 * framebuffer this phone's preloader leaves is found, the loader's text goes to the ramoops
 * console only and the screen is left as the preloader left it.
 *
 * MT6893: the storage is UFS (ufshci@11270000), not an eMMC on MSDC0. The reference loader read
 * the system image out of the eMMC with plain CMD17/CMD18, which an eMMC card answers and UFS
 * does not. So here the kernel owns the root: the system image stays on the phone and the
 * kernel's own UFS driver reads it, the way the reference port's emmc_root=1 does. The loader
 * does not read it into DRAM.
 */
#include <stdint.h>
#include <stddef.h>
#include "mtkdbg.h"
#include "mt6893.h"
#include "msdc.h"
#include "mtkspi.h"
#include "tcm.h"
/* the loader's identity, made from the iPhone X's device tree (xnuloader/mkidentity.sh). The
 * common tree keeps it; the build adds that directory to the include path (see build.sh). */
#include "identity.h"

/* XNU's view of memory: one contiguous range that nothing else (LK, Android, TEE, ATF, the
 * framebuffer) uses. MT6893: the DRAM starts at 0x40000000 and is 8 GiB; the window is placed
 * high in it (mt6893.h). */
#define SYSTEM_PA           (XNU_PHYS_BASE + 0x08000000ULL)
#define SYSTEM_PART         "xnusys"
/* 0: all of it. Smaller is for trying the path out: packfs will not mount a half image. */
#ifndef SYSTEM_MAX_MB
#define SYSTEM_MAX_MB       0
#endif
/* boot_args.virtBase, as xnuloader/loader.c has it: the kernel's text is 48 MB above it. */
#define KERNEL_VIRT_BASE    0xfffffff004000000ULL
#define XNU_SLIDE           (XNU_PHYS_BASE - KERNEL_VIRT_BASE)
#define PAGE_SIZE_MAX       0x4000ULL

/* Seconds XNU may run before the watchdog resets into Android; 0: run until reset by hand. */
#ifndef XNU_WDT_SECONDS
#define XNU_WDT_SECONDS     0
#endif
/* Seconds after which the second core takes a snapshot of XNU and resets; 0: no watcher. */
#ifndef XNU_WATCH_SECONDS
#define XNU_WATCH_SECONDS   0
#endif
/*
 * How many processors the device tree offers the kernel. It takes them from the head of
 * core_order below, so what is left of that list is nobody's but ours -- the watcher takes the
 * next one. Leaving all eight in the tree and trusting "cpus=N" to stop the kernel sooner is a
 * guess about its parsing; this is not. XNU_EXTRA_ARGS should carry the same number as "cpus=".
 *
 * A watcher on a core the kernel wants is worse than none: PSCI CPU_ON answers "already on", the
 * kernel counts a processor that never checks in, and every rendezvous between cores waits for
 * it -- which is how a 15 minute run stayed on the Apple logo.
 *
 * So eight cores for the kernel means no core for the watcher: the rescue thread then has
 * nowhere to run and is not started (watcher_start()). What still ends a run is XNU's own
 * return to the loader (loader_return_after=) and the watchdog (XNU_WDT_SECONDS).
 */
#ifndef XNU_CORES
#define XNU_CORES           8
#endif
_Static_assert(XNU_CORES >= 1 && XNU_CORES <= CPU_COUNT, "as many cores as the phone has");

/*
 * The processors in the order the kernel is offered them: the one this code runs on first (LK
 * leaves us on core 0), then the big ones, fastest first -- the prime core, then the other three
 * Cortex-A78s -- then the Cortex-A55s. So "cpus=3" already gives iOS the fast ones.
 *
 * MT6893: the tree numbers the cores cpu@000-cpu@003 (the A55s), cpu@100-cpu@102 and cpu@200
 * (the A78s, the last the prime core), and the numbering skips 103, so the index in the tree is
 * not the MPIDR. The MPIDRs are in mt6893.h (core_mpidr), in this order.
 */
#define WATCH_CORE          core_order[XNU_CORES]
/*
 * The tree's own numbering of the cores, in the order the kernel is offered them: the loader's
 * own core first, then the prime core, then the other three A78s, then the A55s.
 *
 * These are the tree's reg values divided by 0x100 -- the tree gives the A55s reg 0x000-0x300
 * and the A78s reg 0x400-0x700 (cpu@200 is the prime core, reg 0x700), and the loader writes
 * reg = value << 8 into the tree it builds. The MPIDR of a core is its reg shifted left by
 * eight, so the same numbers are in mt6893.h (core_mpidr).
 */
static const uint8_t core_order[CPU_COUNT] = {
	0,                                  /* the little one the loader runs on */
	7, 6, 5, 4,                         /* the A78s, the prime core first */
	3, 2, 1,                            /* the rest of the A55s */
};
/* Checkpoint for XNU to return at (osfmk/arm/arm_init.c LOADER_CHECKPOINT), 0 = none. */
#ifndef XNU_CHECKPOINT
#define XNU_CHECKPOINT      0
#endif
/* loader_return_after: XNU comes back by itself after that long, with its log (loader_return.c) */
#ifndef XNU_RETURN_AFTER
#define XNU_RETURN_AFTER    "0"
#endif
/* 1: always start XNU (tools/xnu-cycle.sh); 0: Android unless asked once (tools/boot-xnu.sh). */
/*
 * How many touch report frames the loader reads before it gives up waiting for a point, 10 ms
 * apart. Set it high for a run whose point has to be caught by hand.
 */
#ifndef TOUCH_REPORT_TRIES
#define TOUCH_REPORT_TRIES   1000
#endif
/* 1: look at the touch screen's SPI and report, then go back to Android (no XNU). */
#ifndef TOUCH_PROBE
#define TOUCH_PROBE         0
#endif
/* 1: wake the GPU and ask it what it is, if Android asked for it (tools/gpu-probe.sh); then Android. */
#ifndef GPU_PROBE
#define GPU_PROBE           0
#endif
/* 1: read the eMMC and report, then go back to Android (no XNU). */
#ifndef MSDC_TEST
#define MSDC_TEST           0
#endif
/* 1: check at EL1 that the timer interrupts arrive at all (diagnostics, leaves GIC state). */
#ifndef TIMER_PROBE
#define TIMER_PROBE         0
#endif
#ifndef MENU_DEFAULT_XNU
#define MENU_DEFAULT_XNU    0
#endif
/*
 * 1: the kernel's text console on the screen (-v). It is a slow way to run -- scrolling reads
 * the whole framebuffer back -- and iOS cannot draw while it is on, so a real boot goes without
 * it and the log comes back through the loader instead (v_display = 1 says "graphics mode").
 */
#ifndef XNU_VERBOSE
#define XNU_VERBOSE         0
#endif
/* More for the kernel's command line: cores, the clock (epoch=), whatever a run wants to try. */
/* The serial number iOS is told (the device tree's serial-number): a placeholder, not any real device's. */
#ifndef XNU_SERIAL
#define XNU_SERIAL          "HAKIOS0000000000"
#endif
#ifndef XNU_EXTRA_ARGS
#define XNU_EXTRA_ARGS      "cpus=1"
#endif
/*
 * 1: the system image stays on the phone's storage and the kernel reads it as it needs it
 * (emmc_root=1 in the reference port, which was an eMMC; ufs_root=1 here), the default; 0: it is
 * read into DRAM here as md0 -- 2.4 GB of the kernel's memory, wired from the start, and three
 * and a half minutes of every boot.
 *
 * MT6893: the storage is UFS, so the loader cannot read it: an eMMC card answers CMD17/CMD18
 * and UFS does not (see the header). The kernel's own UFS driver reads the image, which is the
 * shape the reference port already used for its eMMC.
 */
#ifndef XNU_ROOT_STORAGE
#define XNU_ROOT_STORAGE    1
#endif
/*
 * 1: the small root (the payload's root.apfs) is the kernel's root, even though the big system
 * image is meant to be. 0: it is the second disk, as usual.
 *
 * MT6893: the big image stays on the UFS and the kernel's own driver reads it -- but that driver
 * does not exist yet, so there is nothing to read it and no root at all. The small root is what
 * the first runs use, so that the loader, the kernel and launchd are exercised at all.
 */
#ifndef XNU_SMALL_ROOT
#define XNU_SMALL_ROOT      1
#endif
#if XNU_ROOT_STORAGE
#define XNU_ROOT_ARG        "rd=md0 ufs_root=1 ufs_shared_pa=0x80200000 ufs_native=1 root_apfs=ufs_xnusys"     /* (rd=md0 for libignition's "ramdisk" boot) */
#else
#define XNU_ROOT_ARG        "rd=md0"
#endif
#if XNU_VERBOSE
#define XNU_BOOT_ARGS       "debug=0x8 serial=3 " XNU_ROOT_ARG " -v " XNU_EXTRA_ARGS " loader_return_after=" XNU_RETURN_AFTER
#else
#define XNU_BOOT_ARGS       "debug=0x8 serial=3 " XNU_ROOT_ARG " " XNU_EXTRA_ARGS " loader_return_after=" XNU_RETURN_AFTER
#endif

#define CHAIN_OFFSET        0x40000u
#define KERNEL_PA           0x40080000ULL
#define CHAIN_STUB_PA       0x4a000000ULL

/* Run state, kept in DRAM across the watchdog reset (three copies: a bit may flip). */
#define MARK_RUNNING        0x4e52584eu /* "NXRN" */
#define MARK_DONE           0x4e44584eu /* "NXDN" */

#define MH_MAGIC_64         0xfeedfacfu
#define LC_SEGMENT_64       0x19
#define LC_SYMTAB           0x2
#define LC_UNIXTHREAD       0x5
#define PAYLOAD_MAGIC       0x50554e58u /* "XNUP" */

struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

#define BOOT_LINE_LENGTH 608

/* pexpert/pexpert/arm64/boot.h: virtBase comes first (start.s reads BA_VIRT_BASE, BA_PHYS_BASE). */
struct boot_args {
	uint16_t revision;
	uint16_t version;
	uint64_t virtBase;
	uint64_t physBase;
	uint64_t memSize;
	uint64_t topOfKernelData;
	struct boot_video video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char commandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags;
	uint64_t memSizeActual;
};

_Static_assert(__builtin_offsetof(struct boot_args, virtBase) == 8, "boot_args");
_Static_assert(__builtin_offsetof(struct boot_args, physBase) == 16, "boot_args");
_Static_assert(__builtin_offsetof(struct boot_args, video) == 40, "boot_args");
_Static_assert(__builtin_offsetof(struct boot_args, deviceTreeP) == 96, "boot_args");
_Static_assert(__builtin_offsetof(struct boot_args, commandLine) == 108, "boot_args");
_Static_assert(__builtin_offsetof(struct boot_args, bootFlags) == 720, "boot_args");

struct mach_header_64 {
	uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved;
};

struct load_command {
	uint32_t cmd, cmdsize;
};

struct segment_command_64 {
	uint32_t cmd, cmdsize;
	char segname[16];
	uint64_t vmaddr, vmsize, fileoff, filesize;
	uint32_t maxprot, initprot, nsects, flags;
};

struct symtab_command {
	uint32_t cmd, cmdsize, symoff, nsyms, stroff, strsize;
};

struct nlist_64 {
	uint32_t n_strx;
	uint8_t n_type, n_sect;
	uint16_t n_desc;
	uint64_t n_value;
};

struct payload_header {
	uint32_t magic;
	uint32_t size;
};

struct payload_entry {
	char name[16];
	uint32_t size;
	uint32_t reserved;
};

struct payload_info {
	const uint8_t *kernel;
	uint32_t kernel_size;
	const uint8_t *ramdisk;
	uint32_t ramdisk_size;
};

/* The XNU symbols the loader works with (kernel virtual addresses, 0 if absent). */
struct xnu_syms {
	uint64_t stub_slot, vc_colors, log, log_len, log_ring_at, log_wrapped, panic_str, panic_args, iso_font,
	    msgbufp, cpu_ttep, live, uart_tx_ready, uart_tx_data, uart_rx_ready, uart_putc, uart_putc_options, log_putc,
	    nprocs, tasks_count, initproc, gic_cpu_init,
	    gic_unhandled_count, gic_last_unhandled, gic_active_iar;
};

static struct xnu_syms xsyms; /* of the kernel that was started; kept for when it comes back */
static const uint8_t *xkernel;  /* its Mach-O file, for symbols */
/* XNU's x29, x30, TCR_EL1 and TTBR1_EL1 when it came back (start.S) */
uint64_t xnu_fp[4];
static volatile uint64_t xnu_back __attribute__((aligned(64))); /* set by xnu_returned() */

struct dt_writer {
	uint8_t *base, *cur;
	uint32_t *counts[16];
	int depth;
};

extern char _start[];
extern char chain_stub[], chain_stub_end[];
extern char xnu_return_stub[];
extern void jump_to_kernel(uint64_t entry_pa, uint64_t boot_args_pa);
extern void psci_system_reset(void);
extern int64_t psci_cpu_on(uint64_t mpidr, uint64_t entry, uint64_t context);
extern void watcher_entry(void);
extern void mmu_on(uint64_t l1_table);
extern void dcache_clean_range(uint64_t va, uint64_t len);
extern uint64_t irq_wait(uint64_t until_ticks);

/* set by the IRQ handler in start.S */
uint64_t irq_count, irq_intid;

static uint64_t lk_dtb;

/* Dynamic runtime boot override parameters passed from Android via /dev/pmsg0 */
static uint32_t runtime_cpus = 0;
static uint32_t runtime_secs = 0;
static char runtime_extra_args[256];
static char runtime_cmdline[608];

/* ---- small libc (device memory: plain loops, word copies when aligned) ---- */

void *memset(void *dst, int c, size_t n)
{
	uint8_t *d = dst;
	if (c == 0 && !((uintptr_t)d & 7)) {
		for (; n >= 8; n -= 8, d += 8) {
			*(volatile uint64_t *)d = 0;
		}
	}
	while (n--) {
		*(volatile uint8_t *)d++ = (uint8_t)c;
	}
	return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;
	if (!(((uintptr_t)d | (uintptr_t)s) & 7)) {
		for (; n >= 8; n -= 8, d += 8, s += 8) {
			*(volatile uint64_t *)d = *(const volatile uint64_t *)s;
		}
	}
	while (n--) {
		*(volatile uint8_t *)d++ = *(const volatile uint8_t *)s++;
	}
	return dst;
}

size_t strlen(const char *s)
{
	size_t len = 0;
	while (*s++) {
		len++;
	}
	return len;
}

/* 0 when the first n bytes are the same; device memory, so byte by byte. */
static int memcmp_n(const volatile void *a, const void *b, size_t n)
{
	const volatile uint8_t *x = a;
	const uint8_t *y = b;

	for (size_t i = 0; i < n; i++) {
		if (x[i] != y[i]) {
			return 1;
		}
	}
	return 0;
}

int strcmp(const char *s1, const char *s2)
{
	while (*s1 && *s1 == *s2) {
		s1++;
		s2++;
	}
	return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

/* ---- output: screen + ramoops + UART ---- */

/* The console UART. MT6893: the tree's /soc/serial@11002000 (mediatek,mt6577-uart) -- the same
 * address as the reference board's, so the register layout carries over. */
#define UART_BASE 0x11002000ULL

static void uart_putc(char c)
{
	uint32_t timeout = 20000;

	while (!(rd32(UART_BASE + 0x14) & 0x20) && --timeout) {
	}
	wr32(UART_BASE, (uint8_t)c);
}
static void out_putc(char c)
{
	if (c == '\n') {
		uart_putc('\r');
	}
	uart_putc(c);
	dbg_putc(c);
}

static void out(const char *s)
{
	while (*s) {
		out_putc(*s++);
	}
}

static void out_hex(uint64_t v)
{
	static const char hex[] = "0123456789abcdef";
	int i = 60;
	out("0x");
	while (i > 0 && !((v >> i) & 0xf)) {
		i -= 4;
	}
	for (; i >= 0; i -= 4) {
		out_putc(hex[(v >> i) & 0xf]);
	}
}

static void out_dec(uint64_t v)
{
	char buf[21];
	int n = 0;
	do {
		buf[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n) {
		out_putc(buf[--n]);
	}
}

/* The hex digits of v, into the command line at *d (the boot-args' addresses are written out). */
static void cmdline_hex(uint32_t *d, uint64_t v)
{
	static const char hex[] = "0123456789abcdef";
	int i = 60;

	while (i > 0 && !((v >> i) & 0xf)) {
		i -= 4;
	}
	for (; i >= 0; i -= 4) {
		if (*d + 1 < sizeof(runtime_cmdline)) {
			runtime_cmdline[(*d)++] = hex[(v >> i) & 0xf];
		}
	}
}

#include "mtkconnsys_probe.h"   /* Wi-Fi step 1: the phone's tree on CONNSYS and the state of the domain */
#include "mtki2c.h"
#include "mt6360.h"

/* ---- run state ---- */

static void marker_set(uint32_t v)
{
	for (int i = 0; i < 3; i++) {
		wr32(MARKER_PA + 64 * i, v);
	}
}

static uint32_t marker_get(void)
{
	uint32_t a = rd32(MARKER_PA), b = rd32(MARKER_PA + 64), c = rd32(MARKER_PA + 128);
	return (a == b || a == c) ? a : (b == c ? b : 0);
}

/* ---- the display ---- */

static uint64_t fb_base;
static uint32_t fb_pitch;

/*
 * MT6893: the layer the phone's preloader leaves scanning.
 *
 * The reference board's display engine had OVL0 at 0x1400b000 and LK's framebuffer at
 * 0x787b0000 with pitch 2944 (736 pixels) for a 720x1640 panel. This board's engine is the
 * mt6893-disp one: OVL0 at 0x14000000 (the tree's disp_ovl0@14000000), and the panel is 1080x2400.
 *
 * The framebuffer this phone's preloader leaves is not known yet: the loader reads the OVL0
 * layer's address and pitch out of the hardware, but the layer layout (which register holds the
 * address on this engine) has to be confirmed on the phone before the values below are trusted.
 * Until then the fallback is the reference board's, which is wrong for this panel, and the
 * loader says so in the log rather than drawing into it.
 */
static uint64_t disp_ovl_base;

static void display_find(void)
{
	static const uint64_t ovls[2] = { DISP_OVL0_BASE, DISP_OVL0_2L_BASE };

	fb_base = 0;
	fb_pitch = 0;
	disp_ovl_base = 0;
	for (int i = 0; i < 2; i++) {
		uint32_t addr = rd32(ovls[i] + OVL_L0_ADDR);
		uint32_t pitch = rd32(ovls[i] + OVL_L0_PITCH) & 0xffff;
		if (addr != 0 && pitch >= PANEL_W * 4 && pitch <= 8192) {
			fb_base = addr;
			fb_pitch = pitch;
			disp_ovl_base = ovls[i];
			return;
		}
	}
}

static void display_report(void)
{
	static const uint64_t ovls[2] = { DISP_OVL0_BASE, DISP_OVL0_2L_BASE };
	static const char *const names[2] = { "OVL0", "OVL0_2L" };

	for (int i = 0; i < 2; i++) {
		uint64_t b = ovls[i];
		out(names[i]);
		out(" @ ");
		out_hex(b);
		out(": EN ");
		out_hex(rd32(b + 0x00c));
		out(" ROI ");
		out_hex(rd32(b + 0x020));
		out(" SRC_CON ");
		out_hex(rd32(b + OVL_SRC_CON));
		out(" L0_CON ");
		out_hex(rd32(b + OVL_L0_CON));
		out(" L0_SIZE ");
		out_hex(rd32(b + 0x038));
		out(" +0x40 ");
		out_hex(rd32(b + 0x040));
		out(" L0_PITCH ");
		out_hex(rd32(b + OVL_L0_PITCH));
		out(" L0_ADDR ");
		out_hex(rd32(b + OVL_L0_ADDR));
		out("\n");
	}
	if (fb_base) {
		out("framebuffer at ");
		out_hex(fb_base);
		out(" pitch ");
		out_dec(fb_pitch);
		out(" (on ");
		out_hex(disp_ovl_base);
		out(")\n");
	} else {
		out("framebuffer: no usable OVL layer found for ");
		out_dec(PANEL_W);
		out("x");
		out_dec(PANEL_H);
		out("; screen left as preloader left it\n");
	}
}

static void ufs_probe(void)
{
	out("== UFS Host Controller Handover (0x11270000) ==\n");
	uint32_t cap = rd32(UFSHCI_BASE + 0x00);
	uint32_t ver = rd32(UFSHCI_BASE + 0x08);
	uint32_t did = rd32(UFSHCI_BASE + 0x10);
	uint32_t pmid = rd32(UFSHCI_BASE + 0x14);
	uint32_t is_reg = rd32(UFSHCI_BASE + 0x20);
	uint32_t ie_reg = rd32(UFSHCI_BASE + 0x24);
	uint32_t hcs = rd32(UFSHCI_BASE + 0x30);
	uint32_t hce = rd32(UFSHCI_BASE + 0x34);
	uint32_t utrlba = rd32(UFSHCI_BASE + 0x50);
	uint32_t utrlbau = rd32(UFSHCI_BASE + 0x54);
	uint32_t utrldbr = rd32(UFSHCI_BASE + 0x58);
	uint32_t utrlrsr = rd32(UFSHCI_BASE + 0x60);
	uint32_t utmrlba = rd32(UFSHCI_BASE + 0x70);
	uint32_t utmrlbau = rd32(UFSHCI_BASE + 0x74);
	uint32_t utmrlrsr = rd32(UFSHCI_BASE + 0x80);

	out("  CAP:     "); out_hex(cap);
	out("  VER:     "); out_hex(ver);
	out(" (v"); out_dec((ver >> 8) & 0xff); out("."); out_dec((ver >> 4) & 0xf); out(")\n");
	out("  DID:     "); out_hex(did);
	out("  PMID:    "); out_hex(pmid);
	out("\n");
	out("  HCE:     "); out_hex(hce);
	out("  HCS:     "); out_hex(hcs);
	out(" (DP="); out_dec(hcs & 1);
	out(" UTRLRDY="); out_dec((hcs >> 1) & 1);
	out(" UTMRLRDY="); out_dec((hcs >> 2) & 1);
	out(" UCMD_RDY="); out_dec((hcs >> 3) & 1);
	out(" LinkState="); out_dec((hcs >> 8) & 0xf);
	out(")\n");
	out("  IS:      "); out_hex(is_reg);
	out("  IE:      "); out_hex(ie_reg);
	out("\n");
	out("  UTRLBA:  "); out_hex(utrlbau); out(":"); out_hex(utrlba);
	out("  RSR:     "); out_hex(utrlrsr);
	out("  DBR:     "); out_hex(utrldbr);
	out("\n");
	out("  UTMRLBA: "); out_hex(utmrlbau); out(":"); out_hex(utmrlba);
	out("  RSR:     "); out_hex(utmrlrsr);
	out("\n");

	if (utrlba != 0) {
		volatile uint32_t *slot0 = (volatile uint32_t *)(uintptr_t)utrlba;
		out("  Slot 0 UTRD:\n    ");
		out_hex(slot0[0]); out(" ");
		out_hex(slot0[1]); out(" ");
		out_hex(slot0[2]); out(" ");
		out_hex(slot0[3]); out("\n");
		out("    UCD: "); out_hex(slot0[5]); out(":"); out_hex(slot0[4]);
		out(" RESP: "); out_hex(slot0[6]);
		out(" PRDT: "); out_hex(slot0[7]);
		out("\n");
	}
}

#define BLOCK_MAGIC        0x4b434f4c42574452ULL       /* "RDWBLOCK" */
#define BLOCK_VERSION      3
#define BLOCK_MAX_PARTS    160
#define BLOCK_PART_UNIT    1u

struct loader_block_part {
	char name[36];
	uint32_t block_size;
	uint64_t blocks;
	uint64_t start_lba;
	uint16_t unit;
	uint8_t lun;
	uint8_t flags;
	uint32_t reserved;
};

struct loader_block {
	uint64_t magic;
	uint32_t version, size;
	uint32_t parts, buffer_size;
	uint64_t buffer_pa;
	uint32_t write_part_plus_one;
	uint32_t write_reserved;
	uint64_t write_first;
	struct {
		volatile uint32_t request;
		uint32_t part;
		uint64_t lba, count;
		uint32_t op;
		uint32_t reserved;
	} ask __attribute__((aligned(128)));
	struct {
		volatile uint32_t answered;
		uint32_t reserved;
		int64_t status;
		uint64_t served, failed, bytes, ticks;
	} answer __attribute__((aligned(128)));
	struct loader_block_part part[BLOCK_MAX_PARTS] __attribute__((aligned(128)));
	struct {
		uint64_t hc_pa;
		uint64_t area_pa;
		uint32_t area_size, ucd_offset, ucd_size, slots;
		uint32_t uefi_utrlba, uefi_utrlbau;
	} native __attribute__((aligned(128)));
	struct {
		volatile uint32_t take_back;
	} loader_native __attribute__((aligned(128)));
	struct {
		volatile uint32_t list_in;
		uint32_t switched_in, switched_out;
		volatile uint32_t cnr_at_switch;
	} kernel_native __attribute__((aligned(128)));
};

_Static_assert(__builtin_offsetof(struct loader_block, ask) == 128, "ask offset");
_Static_assert(__builtin_offsetof(struct loader_block, answer) == 256, "answer offset");
_Static_assert(__builtin_offsetof(struct loader_block, part) == 384, "part offset");
_Static_assert(__builtin_offsetof(struct loader_block, native) == 10624, "native offset");

static void ufs_setup_shared(void)
{
	volatile struct loader_block *lb = (volatile struct loader_block *)(uintptr_t)UFS_SHARED_BLOCK_PA;
	memset((void *)lb, 0, sizeof(*lb));
	memset((void *)(uintptr_t)UFS_HC_AREA_PA, 0, UFS_HC_AREA_SIZE);
	memset((void *)(uintptr_t)UFS_DATA_BUFFER_PA, 0, UFS_DATA_BUFFER_SIZE);

	lb->magic = BLOCK_MAGIC;
	lb->version = BLOCK_VERSION;
	lb->size = sizeof(struct loader_block);
	lb->buffer_size = UFS_DATA_BUFFER_SIZE;
	lb->buffer_pa = UFS_DATA_BUFFER_PA;

	uint32_t utrlba = rd32(UFSHCI_BASE + 0x50);
	uint32_t utrlbau = rd32(UFSHCI_BASE + 0x54);

	lb->native.hc_pa = UFSHCI_BASE;
	lb->native.area_pa = UFS_HC_AREA_PA;
	lb->native.area_size = UFS_HC_AREA_SIZE;
	lb->native.ucd_offset = 0x1000u;
	lb->native.ucd_size = 0x800u;
	lb->native.slots = 32u;
	lb->native.uefi_utrlba = utrlba;
	lb->native.uefi_utrlbau = utrlbau;

	/* Part 0: xnusys (iOS root APFS container) */
	memcpy((void *)lb->part[0].name, "xnusys", 7);
	lb->part[0].block_size = 4096;
	lb->part[0].start_lba = 61427456ULL;
	lb->part[0].blocks = 1048827ULL;
	lb->part[0].unit = 2;
	lb->part[0].lun = 2;
	lb->part[0].flags = 0;

	/* Part 1: userdata (carved) */
	memcpy((void *)lb->part[1].name, "userdata", 9);
	lb->part[1].block_size = 4096;
	lb->part[1].start_lba = 3223552ULL;
	lb->part[1].blocks = 58203904ULL;
	lb->part[1].unit = 2;
	lb->part[1].lun = 2;
	lb->part[1].flags = 0;

	/* Part 2: unit2 (whole LUN 2) */
	memcpy((void *)lb->part[2].name, "unit2", 6);
	lb->part[2].block_size = 4096;
	lb->part[2].start_lba = 0ULL;
	lb->part[2].blocks = 62480384ULL;
	lb->part[2].unit = 2;
	lb->part[2].lun = 2;
	lb->part[2].flags = BLOCK_PART_UNIT;

	lb->parts = 3;

	dcache_clean_range(UFS_SHARED_BLOCK_PA, sizeof(struct loader_block));
	dcache_clean_range(UFS_HC_AREA_PA, UFS_HC_AREA_SIZE);
	dcache_clean_range(UFS_DATA_BUFFER_PA, UFS_DATA_BUFFER_SIZE);

	out("ufs: shared block at ");
	out_hex(UFS_SHARED_BLOCK_PA);
	out(", 3 parts, native area ");
	out_hex(UFS_HC_AREA_PA);
	out("\n");
}

/*
 * The battery state, in the shape the kernel's battery driver wants it
 * (drivers/power/hakios.battery.c: the Qualcomm port's driver, which reads the cache the loader
 * publishes and nothing else).
 *
 * The numbers come from the phone's own gauge, read under Android when the XNU boot is asked
 * for and carried over in the request (the power supply class; see tools/boot-xnu.sh). The driver
 * wants them in a page of their own, at an address it is told on the command line, and it reads
 * that page with ml_io_map(): a whole 16 KB page, its own, in RAM nothing else uses.
 *
 * The values are the ones the gauge reported when the boot was asked for: the loader is handed
 * over to XNU and does not run again, so the page holds one reading. Android reads the same
 * gauge, so the reading is the phone's own.
 */
#define LOADER_BATTERY_MAGIC           0x2154544142574452ULL /* "RDWBATT!" */
#define LOADER_BATTERY_VERSION         1
#define LOADER_BATTERY_VALID           (1u << 0)
#define LOADER_BATTERY_PRESENT         (1u << 1)
#define LOADER_BATTERY_CHARGER_PRESENT (1u << 2)
#define LOADER_BATTERY_VOLTAGE_VALID   (1u << 3)
#define LOADER_BATTERY_CHARGING        (1u << 4)
#define LOADER_BATTERY_SOC_ESTIMATED   (1u << 5)

struct loader_battery {
	uint64_t magic;
	uint32_t version, size;
	volatile uint32_t sequence;
	uint32_t flags;
	int32_t soc_percent, voltage_mv, current_ma, temperature_c;
	uint32_t charger_action, polls, failures, reserved;
};

/*
 * What the request said about the battery: soc, voltage (mV), current (mA, positive while the
 * battery takes charge), temperature (C), status and whether a charger is attached. -1 (or -273)
 * is "not known"; the status is the power supply class's (1 charging, 2 discharging,
 * 3 not charging, 4 full).
 */
static int batt_soc = -1, batt_mv = -1, batt_ma = 0, batt_tc = -273, batt_status = 0, batt_online = 0;

/* Whether a reading was published: the kernel is told about the page only then. */
static int battery_published;

/*
 * Publish the battery state where the kernel's driver will look for it. True when a reading
 * worth publishing came in the request: without one the page is left alone and the kernel is
 * not told about it, so the driver stays out of the way and the percentage stays unknown.
 */
static int battery_publish(void)
{
	volatile struct loader_battery *b = (volatile struct loader_battery *)(uintptr_t)BATTERY_PAGE_PA;
	uint32_t flags;

	if (batt_soc < 0 || batt_soc > 100) {
		out("battery: no reading in the request\n");
		return 0;
	}
	flags = LOADER_BATTERY_VALID | LOADER_BATTERY_PRESENT;

	if (batt_mv >= 0) {
		flags |= LOADER_BATTERY_VOLTAGE_VALID;
	}
	if (batt_online) {
		flags |= LOADER_BATTERY_CHARGER_PRESENT;
	}
	if (batt_status == 1 && batt_soc < 100) {   /* POWER_SUPPLY_STATUS_CHARGING, and not yet full */
		flags |= LOADER_BATTERY_CHARGING;
	}

	memset((void *)b, 0, sizeof(*b));
	b->magic = LOADER_BATTERY_MAGIC;
	b->version = LOADER_BATTERY_VERSION;
	b->size = sizeof(struct loader_battery);
	b->flags = flags;
	b->soc_percent = batt_soc;
	b->voltage_mv = batt_mv;
	b->current_ma = batt_ma;
	b->temperature_c = batt_tc;
	b->charger_action = batt_status;
	b->sequence = 2;               /* even: the driver reads the page as settled */
	dcache_clean_range(BATTERY_PAGE_PA, sizeof(struct loader_battery));

	out("battery: published at ");
	out_hex(BATTERY_PAGE_PA);
	out(", SOC ");
	out_dec((uint64_t)batt_soc);
	out("%, V ");
	out_dec((uint64_t)batt_mv);
	out(" mV, I ");
	out_dec((uint64_t)batt_ma);
	out(" mA, T ");
	out_dec((uint64_t)batt_tc);
	out(" C, status ");
	out_dec((uint64_t)batt_status);
	out(", charger ");
	out_dec((uint64_t)batt_online);
	out("\n");
	battery_published = 1;
	return 1;
}

/* Opaque pixels only from here on: XNU's console writes alpha 0. */
static void display_alpha_off(void)
{
	if (!disp_ovl_base) {
		return;
	}
	uint32_t con = rd32(disp_ovl_base + OVL_L0_CON);

	wr32(disp_ovl_base + OVL_L0_CON, con & ~(1u << 8));
	out("OVL L0_CON ");
	out_hex(con);
	out(" -> ");
	out_hex(rd32(disp_ovl_base + OVL_L0_CON));

	uint32_t src = rd32(disp_ovl_base + OVL_SRC_CON);

	wr32(disp_ovl_base + OVL_SRC_CON, src & 1u);
	wr32(DISP_MUTEX_EN, 1);
	wr32(DISP_MUTEX_RUN, 1);
	out(", SRC_CON ");
	out_hex(src);
	out(" -> ");
	out_hex(rd32(disp_ovl_base + OVL_SRC_CON));
	out("\n");
}

/* ---- payload and the kernel's symbols ---- */

static const struct payload_header *payload_find(void)
{
	const volatile uint64_t *hdr = (const volatile uint64_t *)_start;
	uint64_t off = hdr[6]; /* res3 */
	const struct payload_header *ph = (const struct payload_header *)((uint64_t)_start + off);

	if (off == 0 || (off & 15) || ph->magic != PAYLOAD_MAGIC) {
		return NULL;
	}
	return ph;
}

static int parse_payload(struct payload_info *pi)
{
	const struct payload_header *ph = payload_find();

	memset(pi, 0, sizeof(*pi));
	if (!ph) {
		return 0;
	}
	const uint8_t *p = (const uint8_t *)(ph + 1), *pend = p + ph->size;
	while (p + sizeof(struct payload_entry) <= pend) {
		const struct payload_entry *e = (const struct payload_entry *)p;
		if (e->name[0] == 0) {
			break;
		}
		if (strcmp(e->name, "kernel") == 0) {
			pi->kernel = p + sizeof(*e);
			pi->kernel_size = e->size;
		} else if (strcmp(e->name, "ramdisk") == 0) {
			pi->ramdisk = p + sizeof(*e);
			pi->ramdisk_size = e->size;
		}
		p += sizeof(*e) + ((e->size + 15) & ~15u);
	}
	return pi->kernel != NULL && ((const struct mach_header_64 *)pi->kernel)->magic == MH_MAGIC_64;
}

static void find_syms(const uint8_t *kernel, struct xnu_syms *s)
{
	/* in the order of struct xnu_syms: the loop below fills it field by field */
	static const char *const names[] = {
		"_loader_stub_slot", "_vc_colors", "_loader_log", "_loader_log_len", "_loader_log_ring_at",
		"_loader_log_wrapped", "_debugger_panic_str", "_debugger_panic_args", "_iso_font",
		"_msgbufp", "_cpu_ttep", "_loader_live",
		"_vmapple_uart_transmit_ready", "_vmapple_uart_transmit_data",
		"_vmapple_uart_receive_ready", "_uart_putc", "_uart_putc_options", "_loader_log_putc",
		"_nprocs", "_tasks_count", "_initproc", "_gic_v3_irq_cpu_init",
		"_gic_unhandled_count", "_gic_last_unhandled", "_gic_active_iar",
	};
	_Static_assert(sizeof(struct xnu_syms) == sizeof(names), "every field needs its name, in order");
	uint64_t *slots = (uint64_t *)s;
	const struct mach_header_64 *mh = (const struct mach_header_64 *)kernel;
	const struct load_command *lc = (const struct load_command *)(mh + 1);
	const struct symtab_command *st = NULL;

	memset(s, 0, sizeof(*s));
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SYMTAB) {
			st = (const void *)lc;
		}
	}
	if (!st) {
		return;
	}
	const struct nlist_64 *sym = (const struct nlist_64 *)(kernel + st->symoff);
	const char *strs = (const char *)(kernel + st->stroff);
	for (uint32_t i = 0; i < st->nsyms; i++) {
		const char *n = strs + sym[i].n_strx;
		if (n[0] != '_' || (n[1] != 'l' && n[1] != 'v' && n[1] != 'd' && n[1] != 'i' && n[1] != 'm' && n[1] != 'c' && n[1] != 'g' && n[1] != 'n' && n[1] != 't')) {
			continue;
		}
		for (unsigned k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
			if (!slots[k] && strcmp(n, names[k]) == 0) {
				slots[k] = sym[i].n_value;
			}
		}
	}
}

static uint64_t kva_to_pa(uint64_t va)
{
	return va + XNU_SLIDE;
}


/* ---- the system image, on the flash ---- */

/*
 * userland/system.packfs is the root XNU mounts (md0), 2.8 GB of it: too large for a boot image,
 * so it lives in its own partition (tools/gpt-carve.py made "xnusys" out of the end of userdata)
 * and is read into DRAM here. The payload's small root.packfs stays the second disk (md1).
 */
static uint8_t gpt_entries[128 * 128] __attribute__((aligned(16)));
static uint8_t sector_buf[512] __attribute__((aligned(16)));

/* The partition's first block and length, by the name in the GPT. */
static int gpt_find(const char *name, uint64_t *lba, uint64_t *blocks)
{
	if (!msdc_read(1, 1, sector_buf)) {
		return 0;
	}
	if (memcmp_n(sector_buf, "EFI PART", 8)) {
		return 0;
	}
	uint64_t ent_lba = *(volatile uint64_t *)(sector_buf + 72);
	uint32_t count = *(volatile uint32_t *)(sector_buf + 80);
	uint32_t size = *(volatile uint32_t *)(sector_buf + 84);

	if (count > 128 || size != 128) {
		return 0;
	}
	if (!msdc_read(ent_lba, count * size / 512, gpt_entries)) {
		return 0;
	}
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *e = gpt_entries + i * size;
		int same = 1;

		for (int k = 0; same && name[k - (k ? 0 : 0)] != 0; k++) {
			same = e[56 + 2 * k] == (uint8_t)name[k] && e[57 + 2 * k] == 0;
		}
		if (same && e[56 + 2 * (int)strlen(name)] == 0) {
			uint64_t first = *(const volatile uint64_t *)(e + 32);
			uint64_t last = *(const volatile uint64_t *)(e + 40);
			if (first == 0 || last < first) {
				return 0;
			}
			*lba = first;
			*blocks = last - first + 1;
			return 1;
		}
	}
	return 0;
}

/* Reads the image into SYSTEM_PA; returns its length, 0 if there is none to read. */
static uint64_t load_system_image(void)
{
#if !XNU_ROOT_STORAGE
	uint64_t lba, blocks, size = 0, done = 0, t0 = dbg_ticks(), f = dbg_freq();

	if (!gpt_find(SYSTEM_PART, &lba, &blocks)) {
		out("no " SYSTEM_PART " partition on the flash (or the GPT could not be read)\n");
		return 0;
	}
	out(SYSTEM_PART " at block ");
	out_hex(lba);
	out(", ");
	out_dec(blocks / 2048);
	out(" MB\n");

	/*
	 * APFS: a container's superblock is at its first block -- "NXSB" at offset 32, then the
	 * block size and how many blocks it has. An image may hold several containers back to
	 * back (the cryptexes come that way), so follow them until one does not begin with NXSB.
	 */
	for (uint64_t at = 0;;) {
		if (!msdc_read(lba + at / 512, 1, sector_buf)) {
			out("cannot read it: ");
			out(msdc_last.what);
			out("\n");
			return 0;
		}
		if (*(volatile uint32_t *)(sector_buf + 32) != 0x4253584eu) { /* "NXSB" */
			break;
		}
		uint32_t bs = *(volatile uint32_t *)(sector_buf + 36);
		uint64_t count = *(volatile uint64_t *)(sector_buf + 40);

		if (bs < 512 || bs > 65536 || count == 0 || count > blocks) {
			out("the APFS container at ");
			out_dec(at / 1024 / 1024);
			out(" MB says ");
			out_dec(count);
			out(" blocks of ");
			out_dec(bs);
			out(": that cannot be\n");
			return 0;
		}
		at += count * bs;
		size = at;
		if (at >= blocks * 512) {
			break;
		}
	}
	if (size == 0) {
		out("no APFS container there (write userland/system.apfs to it)\n");
		return 0;
	}
	out("APFS: ");
	out_dec(size / 1024 / 1024);
	out(" MB\n");
	if (SYSTEM_MAX_MB && size > (uint64_t)SYSTEM_MAX_MB * 1024 * 1024) {
		size = (uint64_t)SYSTEM_MAX_MB * 1024 * 1024;
		out("(only the first ");
		out_dec(SYSTEM_MAX_MB);
		out(" MB of it, as built)\n");
	}

	out("reading ");
	out_dec(size / 1024 / 1024);
	out(" MB into ");
	out_hex(SYSTEM_PA);
	out(": ");
	uint32_t last_shown = 0;
	while (done < size) {
		uint64_t left = (size - done + 511) / 512;
		uint32_t n = left > 4096 ? 4096 : (uint32_t)left;   /* 2 MB at a time */

		if (!msdc_read(lba + done / 512, n, (void *)(SYSTEM_PA + done))) {
			out("\nfailed at ");
			out_dec(done / 1024 / 1024);
			out(" MB: ");
			out(msdc_last.what);
			out(" status ");
			out_hex(msdc_last.status);
			out("\n");
			return 0;
		}
		done += (uint64_t)n * 512;
		uint32_t pct = (uint32_t)(done * 100 / size);
		if (pct >= last_shown + 10) {
			last_shown = pct - pct % 10;
			out_dec(last_shown);
			out("% ");
		}
	}
	uint64_t ms = (dbg_ticks() - t0) * 1000 / f;
	out("done in ");
	out_dec(ms / 1000);
	out(" s (");
	out_dec(size / 1024 / (ms ? ms : 1) * 1000 / 1024);
	out(" MB/s)\n");
	return size;
#else
	/*
	 * MT6893: the storage is UFS and the kernel owns the root (ufs_root=1), so the loader does
	 * not read the system image. The reference port's eMMC path -- msdc_read over CMD17/CMD18 --
	 * is left in the tree but is not what this board uses; an eMMC card answers those commands
	 * and UFS does not.
	 */
	out("the system image is read by the kernel's own UFS driver, not by the loader\n");
	return 0;
#endif
}

/* ---- device tree for XNU ---- */

static void dt_prop(struct dt_writer *w, const char *name, const void *data, uint32_t len)
{
	memset(w->cur, 0, 32);
	memcpy(w->cur, name, strlen(name));
	*(volatile uint32_t *)(w->cur + 32) = len;
	memset(w->cur + 36, 0, (len + 3) & ~3u);
	memcpy(w->cur + 36, data, len);
	w->cur += 36 + ((len + 3) & ~3u);
	w->counts[w->depth - 1][0]++;
}

static void dt_prop_str(struct dt_writer *w, const char *name, const char *value)
{
	dt_prop(w, name, value, (uint32_t)strlen(value) + 1);
}

static void dt_prop_u32(struct dt_writer *w, const char *name, uint32_t value)
{
	dt_prop(w, name, &value, sizeof(value));
}

static void dt_prop_u64(struct dt_writer *w, const char *name, uint64_t value)
{
	dt_prop(w, name, &value, sizeof(value));
}

static void dt_prop_padded(struct dt_writer *w, const char *name, const char *value, uint32_t size)
{
	uint8_t field[64];
	memset(field, 0, sizeof(field));
	memcpy(field, value, strlen(value));
	dt_prop(w, name, field, size);
}

static void dt_begin(struct dt_writer *w, const char *name)
{
	if (w->depth > 0) {
		w->counts[w->depth - 1][1]++;
	}
	w->counts[w->depth++] = (uint32_t *)w->cur;
	memset(w->cur, 0, 8);
	w->cur += 8;
	dt_prop_str(w, "name", name);
}

static void dt_end(struct dt_writer *w)
{
	w->depth--;
}


/* What the iPhone's own device tree says (model, compatible, product ...), from xnuloader. */
static void dt_identity(struct dt_writer *w, const struct identity_prop *props, uint32_t count, const unsigned char *data)
{
	for (uint32_t i = 0; i < count; i++) {
		/* the panel is sRGB, not the iPhone X's P3 (see xnuloader/loader.c) */
		if (strcmp(props[i].name, "artwork-display-gamut") == 0) {
			dt_prop(w, props[i].name, "sRGB", 5);
			continue;
		}
		/*
		 * The screen class iOS lays itself out for. The identity was generated with the panel's
		 * own height here, 1640, which no iPhone has: iOS takes an unknown class for a phone
		 * with a home button, and the Control Centre came up from the bottom edge as on an
		 * iPhone 8. The display driver reports the iPhone X's 1125 x 2436 whatever the panel,
		 * so the class has to be the iPhone X's too.
		 */
		if (strcmp(props[i].name, "artwork-device-subtype") == 0) {
			static const uint32_t subtype = 2436;

			dt_prop(w, props[i].name, &subtype, sizeof(subtype));
			continue;
		}
		dt_prop(w, props[i].name, data + props[i].offset, props[i].length);
	}
}

/* An empty NVRAM image in CHRP format (iokit/Kernel/IONVRAMCHRPHandler.cpp): IOKit needs one. */
#define NVRAM_BANK_SIZE 0x2000u
static uint8_t nvram_image[NVRAM_BANK_SIZE];

static void nvram_chrp_header(uint8_t *h, uint8_t sig, uint32_t bytes, const char *name)
{
	uint16_t blocks = (uint16_t)(bytes / 16), sum;

	memset(h, 0, 16);
	h[0] = sig;
	h[2] = (uint8_t)blocks;
	h[3] = (uint8_t)(blocks >> 8);
	memcpy(h + 4, name, strlen(name));
	sum = h[0];
	for (int i = 2; i < 16; i++) {
		sum += h[i];
	}
	while (sum > 0xff) {
		sum = (sum & 0xff) + (sum >> 8);
	}
	h[1] = (uint8_t)sum;
}

static void build_nvram_image(uint8_t *image)
{
	uint32_t lo = 1, hi = 0, generation = 1;

	memset(image, 0, NVRAM_BANK_SIZE);
	nvram_chrp_header(image, 0x5a, 0x20, "nvram");
	nvram_chrp_header(image + 0x20, 0x70, NVRAM_BANK_SIZE - 0x20, "common");
	memcpy(image + 20, &generation, 4);
	for (uint32_t i = 20; i < NVRAM_BANK_SIZE; i++) {
		lo = (lo + image[i]) % 65521;
		hi = (hi + lo) % 65521;
	}
	uint32_t adler = (hi << 16) | lo;
	memcpy(image + 16, &adler, 4);
}

/* The panic log region and the run marker are in mt6893.h (PANIC_LOG_BASE, MARKER_PA). */

/* As xnuloader/loader.c builds it for x6826b, minus what needs UEFI. */
static uint32_t build_device_tree(uint64_t dt_pa, uint64_t rd_pa, uint64_t rd_size,
    uint64_t sys_pa, uint64_t sys_size)
{
	struct dt_writer w = { .base = (uint8_t *)dt_pa, .cur = (uint8_t *)dt_pa, .depth = 0 };
	uint64_t cntfrq = dbg_freq();
	uint8_t seed[64];

	for (unsigned i = 0; i < sizeof(seed); i++) {
		seed[i] = (uint8_t)(dbg_ticks() >> (i & 7)) ^ (uint8_t)(i * 37);
	}

	dt_begin(&w, "device-tree");
	dt_identity(&w, identity_root, sizeof(identity_root) / sizeof(identity_root[0]), identity_root_data);
	dt_prop_str(&w, "platform-name", "t8015");
	dt_prop_padded(&w, "serial-number", XNU_SERIAL, 32);
	dt_prop_padded(&w, "mlb-serial-number", XNU_SERIAL "MLB", 32);
	dt_prop_padded(&w, "model-number", "MQAC2", 32);
	dt_prop_padded(&w, "region-info", "LL/A", 32);
	dt_prop_padded(&w, "regulatory-model-number", "A1865", 32);
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);

	dt_begin(&w, "chosen");
	dt_prop_str(&w, "darwin-board", "x6739");
	dt_prop(&w, "random-seed", seed, sizeof(seed));
	dt_prop_u64(&w, "dram-base", XNU_PHYS_BASE);
	dt_prop_u64(&w, "dram-size", XNU_MEM_SIZE);
	dt_prop_u32(&w, "debug-enabled", 1);
	/*
	 * What MobileGestalt answers "main screen scale" from: the device pixels one point is
	 * drawn with. The display driver reports the iPhone X's 1125 x 2436 whatever the panel
	 * is, and that is 375 x 812 points at three pixels a point -- so three is what the
	 * scale has to be. With two, the window server read "main screen size:{1125, 2436}
	 * scale:2" and laid the user interface out for 562 x 1218 points, and everything on it
	 * came out small: the icons of the home screen are a size in points, so on a screen
	 * the system believes is nearly twice as wide they are half as big as they should be.
	 */
	dt_prop_u32(&w, "display-scale", 3);
	dt_prop_u32(&w, "display-rotation", 0);
	dt_prop_str(&w, "psci-method", "smc");
	static const uint8_t wifi_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x01 };
	static const uint8_t bluetooth_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x02 };
	static const uint8_t manifest_hash[48] = { 0x72, 0x65, 0x64, 0x77, 0x6f, 0x6f, 0x64 };
	static const uint8_t no_behavior[16];
	dt_prop_u64(&w, "unique-chip-id", 0x000052445730001ULL);
	dt_prop_u32(&w, "chip-id", 0x8015);
	dt_prop_u32(&w, "board-id", 0x06);
	dt_prop(&w, "mac-address-wifi0", wifi_address, sizeof(wifi_address));
	dt_prop(&w, "mac-address-bluetooth0", bluetooth_address, sizeof(bluetooth_address));
	dt_prop(&w, "boot-manifest-hash", manifest_hash, sizeof(manifest_hash));
	dt_prop(&w, "software-behavior", no_behavior, sizeof(no_behavior));
	dt_prop_str(&w, "firmware-version", "iBoot-8422.100.650");
	dt_prop_u32(&w, "certificate-production-status", 1);
	dt_prop_u32(&w, "certificate-security-mode", 1);
	dt_prop_u32(&w, "effective-production-status-ap", 1);
	dt_prop_u32(&w, "effective-security-mode-ap", 1);
	dt_prop_u32(&w, "secure-boot", 1);
	dt_prop_u32(&w, "production-cert", 1);
	dt_prop_u32(&w, "development-cert", 0);
	dt_prop_u32(&w, "embedded-panic-log-size", PANIC_LOG_SIZE);
	build_nvram_image(nvram_image);
	dt_prop_u32(&w, "nvram-bank-size", NVRAM_BANK_SIZE);
	dt_prop_u32(&w, "nvram-bank-count", 1);
	dt_prop_u32(&w, "nvram-current-bank", 0);
	dt_prop(&w, "nvram-proxy-data", nvram_image, sizeof(nvram_image));
	/*
	 * "RAMDisk" becomes /dev/md0, which is the root; "RAMDisk1" becomes /dev/md1. With the big
	 * system image present it is the root, and the payload's small one is the second disk --
	 * the same way round as xnuloader has it.
	 *
	 * MT6893: with the kernel owning the root (ufs_root=1) there is no big image in memory
	 * yet, because the kernel's UFS driver does not exist to fetch one. So md0 has to be the
	 * small root, or the kernel finds no root at all and never reaches launchd. That is what
	 * XNU_SMALL_ROOT is for: it is what the first runs use, and it goes away when the UFS
	 * driver can read the real image.
	 */
	dt_begin(&w, "memory-map");
	if (sys_size != 0) {
		uint64_t sys_reg[2] = { sys_pa, sys_size };
		dt_prop(&w, "RAMDisk", sys_reg, sizeof(sys_reg));
		if (rd_size != 0) {
			uint64_t rd_reg[2] = { rd_pa, rd_size };
			dt_prop(&w, "RAMDisk1", rd_reg, sizeof(rd_reg));
		}
	} else if (rd_size != 0) {
		uint64_t rd_reg[2] = { rd_pa, rd_size };
		/* (with the system image on the eMMC the payload stays md1, where stage0 mounts it) */
		dt_prop(&w, XNU_ROOT_STORAGE && !XNU_SMALL_ROOT ? "RAMDisk1" : "RAMDisk", rd_reg, sizeof(rd_reg));
	}
	dt_end(&w);
	dt_end(&w);

	dt_begin(&w, "defaults");
	dt_prop_u32(&w, "kern.vm_compressor", 2);
	dt_end(&w);

	dt_begin(&w, "product");
	dt_identity(&w, identity_product, sizeof(identity_product) / sizeof(identity_product[0]), identity_product_data);
	dt_begin(&w, "haptics");
	dt_identity(&w, identity_haptics, sizeof(identity_haptics) / sizeof(identity_haptics[0]), identity_haptics_data);
	dt_end(&w);
	dt_end(&w);

	dt_begin(&w, "options");
	dt_end(&w);

	uint64_t pram_reg[2] = { PANIC_LOG_BASE, PANIC_LOG_SIZE };
	memset((void *)PANIC_LOG_BASE, 0, PANIC_LOG_SIZE);
	dt_begin(&w, "pram");
	dt_prop(&w, "reg", pram_reg, sizeof(pram_reg));
	dt_end(&w);

	dt_begin(&w, "cpus");
	uint32_t active_cpus = (runtime_cpus >= 1 && runtime_cpus <= 7) ? runtime_cpus : XNU_CORES;
	for (uint32_t i = 0; i < active_cpus; i++) {
		char name[5] = { 'c', 'p', 'u', (char)('0' + i), 0 };
		int core = core_order[i];

		dt_begin(&w, name);
		dt_prop_str(&w, "device_type", "cpu");
		dt_prop_str(&w, "state", i == 0 ? "running" : "waiting");
		dt_prop_u32(&w, "reg", (uint32_t)(core << 8));
		dt_prop_u32(&w, "cpu-id", (uint32_t)i);
		dt_prop_u64(&w, "timebase-frequency", cntfrq);
		dt_prop_u64(&w, "fixed-frequency", cntfrq);
		uint64_t clock_freq = (core == 7) ? CPU_CLOCK_PRIME :
		                      (core >= 4) ? CPU_CLOCK_BIG :
		                                    CPU_CLOCK_LITTLE;
		dt_prop_u64(&w, "clock-frequency", clock_freq);
		dt_prop_u64(&w, "bus-frequency", 100000000ULL);
		dt_prop_u64(&w, "memory-frequency", 100000000ULL);
		dt_prop_u64(&w, "peripheral-frequency", 24000000ULL);
		if (core < SMALL_CORE_COUNT) { /* the four Cortex-A55s; the rest are the A78s */
			dt_prop_u32(&w, "small-core", 1);
		}
		dt_end(&w);
	}
	dt_end(&w);

	dt_begin(&w, "arm-io");
	dt_prop_str(&w, "device_type", "x6739");
	uint64_t ranges[3] = { 0, 0x0c000000, 0x14000000 };
	dt_prop(&w, "ranges", ranges, sizeof(ranges));
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);

	dt_begin(&w, "gic");
	dt_prop_str(&w, "device_type", "interrupt-controller");
	dt_prop_str(&w, "compatible", "arm,gic-v3");
	uint64_t gic_reg[4] = { GIC_DIST_BASE - ARMIO_BASE, GIC_DIST_SIZE, GIC_CPU_BASE - ARMIO_BASE, GIC_CPU_SIZE };
	dt_prop(&w, "reg", gic_reg, sizeof(gic_reg));
	dt_end(&w);

	/* The console the kernel is told about; its driver is patched in run_xnu(). */
	dt_begin(&w, "uart0");
	dt_prop_str(&w, "device_type", "uart");
	dt_prop_str(&w, "compatible", "mediatek,mt6577-uart");
	uint64_t uart_reg[2] = { 0x11002000 - 0x0c000000, 0x1000 };
	dt_prop(&w, "reg", uart_reg, sizeof(uart_reg));
	dt_prop_u32(&w, "boot-console", 1);
	dt_end(&w);

	dt_end(&w);
	dt_end(&w);
	return (uint32_t)(w.cur - w.base);
}

/* ---- leaving: Android, or a reset ---- */

static void plog_flush(void)
{
	dcache_clean_range(RAMOOPS_CONSOLE, RAMOOPS_CONSOLE_SIZE);
	dcache_clean_range(RAMOOPS_PMSG, RAMOOPS_PMSG_SIZE);
	dcache_clean_range(MARKER_PA, 192);
}

static void __attribute__((noreturn)) reset_board(void)
{
	plog_flush();
	psci_system_reset();
	wdt_reset();
}

#if TOUCH_PROBE
static void touch_hand_back(void);
#endif

static void __attribute__((noreturn)) chain_android(void)
{
	const volatile uint64_t *hdr = (const volatile uint64_t *)_start;
	const volatile uint32_t *img = (const volatile uint32_t *)((uint64_t)_start + CHAIN_OFFSET);
	uint64_t image_size = (hdr[5] + 7) & ~7ULL; /* res2 */

	marker_set(0);
	if (image_size == 0 || img[14] != 0x644d5241u) { /* "ARM\x64" */
		out("no Android kernel in this image: reset\n");
		dbg_delay_ms(3000);
		reset_board();
	}
#if TOUCH_PROBE
	touch_hand_back();
#endif
	out("starting Android\n");
	plog_flush();

	volatile uint32_t *dst = (volatile uint32_t *)CHAIN_STUB_PA;
	const volatile uint32_t *src = (const volatile uint32_t *)chain_stub;
	for (uint64_t i = 0; i < (uint64_t)(chain_stub_end - chain_stub) / 4; i++) {
		dst[i] = src[i];
	}
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	void (*stub)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) = (void *)CHAIN_STUB_PA;
	stub(lk_dtb, KERNEL_PA, (uint64_t)img, image_size, KERNEL_PA);
	__builtin_unreachable();
}

void loader_fault(uint64_t vec, uint64_t esr, uint64_t elr, uint64_t far)
{
	fbc.fg = 0xffff4040u;
	out("\n*** loader fault: vector ");
	out_hex(vec);
	out(" ESR ");
	out_hex(esr);
	out(" ELR ");
	out_hex(elr);
	out(" FAR ");
	out_hex(far);
	out("\n");
	dbg_delay_ms(5000);
	chain_android();
}

/* ---- XNU's log into ramoops ---- */

struct span {
	const volatile char *p;
	uint64_t n;
};

/* Append spans (in order) to the ramoops console: all of it if it fits, else its start and
 * as much of its end as there is room for. */
static void save_log(const struct span *sp, int nsp)
{
	uint64_t total = 0, room, head, tail, skip_to, at = 0;

	for (int i = 0; i < nsp; i++) {
		total += sp[i].n;
	}
	room = PLOG_CAP > PLOG_HDR->size ? PLOG_CAP - PLOG_HDR->size : 0;
	room = room > 1024 ? room - 1024 : 0;
	head = total <= room ? total : room / 4;
	tail = total <= room ? 0 : room - head;
	skip_to = total - tail;

	uint64_t fb = fbc.base;
	fbc.base = 0; /* the screen gets the tail, separately */
	for (int i = 0; i < nsp; i++) {
		for (uint64_t k = 0; k < sp[i].n; k++, at++) {
			if (at < head || at >= skip_to) {
				char c = sp[i].p[k];
				plog_putc(c ? c : ' ');
			} else if (at == head) {
				dbg_puts("\n\n[... ");
				dbg_dec(skip_to - head);
				dbg_puts(" bytes of the XNU log left out here ...]\n\n");
			}
		}
	}
	plog_putc('\n');
	fbc.base = fb;
}

/* The last lines of what is in ramoops, on the lower part of the screen. */
static void show_log_tail(uint32_t lines)
{
	uint32_t size = PLOG_HDR->size, n = 0;
	uint32_t top = PANEL_H - (lines + 1) * 16;
	uint64_t i;

	if (!fbc.base || size == 0) {
		return;
	}
	for (i = size; i > 0 && n <= lines; i--) {
		n += PLOG_DATA[i - 1] == '\n';
	}
	fb_rect(0, top - 4, PANEL_W, PANEL_H - top + 4, 0xff000000u);
	fb_rect(0, top - 4, PANEL_W, 2, 0xffff3030u);
	fbc.top = fbc.y = top;
	fbc.x = 0;
	fbc.scale = 1;
	for (; i < size; i++) {
		fbcon_putc(PLOG_DATA[i]);
	}
}

/*
 * A kernel virtual address to physical through XNU's own page tables (TTBR1_EL1; 4K or 16K
 * granule), as they were when it came back. 0 if not mapped.
 */
static uint64_t xnu_va_to_pa(uint64_t va)
{
	uint64_t tcr = xnu_fp[2], ttbr1 = xnu_fp[3];
	uint32_t t1sz = (tcr >> 16) & 0x3f, tg1 = (tcr >> 30) & 3;
	uint32_t g = tg1 == 2 ? 12 : tg1 == 1 ? 14 : 0;
	if (!g || t1sz < 16) {
		return 0;
	}
	uint32_t vabits = 64 - t1sz, per = g - 3;
	uint32_t levels = (vabits - g + per - 1) / per;
	uint64_t table = ttbr1 & 0x0000fffffffffffeULL & ~((1ULL << g) - 1);
	for (uint32_t l = 0; l < levels; l++) {
		uint32_t shift = g + per * (levels - 1 - l);
		uint64_t idx = (va >> shift) & ((1ULL << per) - 1);
		if (l == 0) {
			idx &= (1ULL << (vabits - shift)) - 1;
		}
		uint64_t d = *(volatile uint64_t *)(table + idx * 8);
		if (!(d & 1)) {
			return 0;
		}
		uint64_t oa = d & 0x0000ffffffffffffULL & ~((1ULL << g) - 1);
		if (l == levels - 1 || !(d & 2)) { /* page, or block */
			uint64_t span = 1ULL << shift;
			return (oa & ~(span - 1)) | (va & (span - 1));
		}
		table = oa;
	}
	return 0;
}

static uint64_t xnu_rd64(uint64_t va)
{
	uint64_t pa = xnu_va_to_pa(va);
	return pa && !(pa & 7) ? *(volatile uint64_t *)pa : 0;
}

static char xnu_rd8(uint64_t va)
{
	uint64_t pa = xnu_va_to_pa(va);
	return pa ? *(volatile char *)pa : 0;
}

static void out_xnu_str(uint64_t va, int max)
{
	char c;
	for (int i = 0; i < max && (c = xnu_rd8(va + i)) != 0; i++) {
		out_putc(c);
	}
}

/* AAPCS64 va_list, as debugger_panic_args points at it */
struct xnu_va_list {
	uint64_t stack, gr_top, vr_top;
	int32_t gr_offs, vr_offs;
};

static uint64_t va_next(struct xnu_va_list *ap)
{
	uint64_t v;
	if (ap->gr_offs < 0) {
		v = xnu_rd64(ap->gr_top + (int64_t)ap->gr_offs);
		ap->gr_offs += 8;
	} else {
		v = xnu_rd64(ap->stack);
		ap->stack += 8;
	}
	return v;
}

/* The panic message: debugger_panic_str formatted with debugger_panic_args (integers in hex). */
static void show_panic_string(const struct xnu_syms *x)
{
	uint64_t fmt = x->panic_str ? xnu_rd64(x->panic_str) : 0;
	uint64_t argp = x->panic_args ? xnu_rd64(x->panic_args) : 0;
	struct xnu_va_list ap = { 0 };

	if (!fmt) {
		out("(no panic string)\n");
		return;
	}
	if (argp) {
		ap.stack = xnu_rd64(argp);
		ap.gr_top = xnu_rd64(argp + 8);
		ap.vr_top = xnu_rd64(argp + 16);
		uint64_t offs = xnu_rd64(argp + 24);
		ap.gr_offs = (int32_t)offs;
		ap.vr_offs = (int32_t)(offs >> 32);
	}
	out("panic: ");
	for (int i = 0; i < 1024; i++) {
		char c = xnu_rd8(fmt + i);
		if (!c) {
			break;
		}
		if (c != '%') {
			out_putc(c);
			continue;
		}
		/* flags, width, precision, length */
		do {
			c = xnu_rd8(fmt + ++i);
		} while (c && ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == ' ' || c == '#' || c == '.' ||
		    c == 'l' || c == 'h' || c == 'z' || c == 'j' || c == 't' || c == 'q' || c == '*'));
		if (!argp && c != '%') {
			out("<?>");
			continue;
		}
		switch (c) {
		case '%':
			out_putc('%');
			break;
		case 's':
			out_xnu_str(va_next(&ap), 200);
			break;
		case 'c':
			out_putc((char)va_next(&ap));
			break;
		case 'd':
		case 'i':
		case 'u': {
			uint64_t v = va_next(&ap);
			out_dec(v);
			out("(");
			out_hex(v);
			out(")");
			break;
		}
		case 0:
			i = 1024;
			break;
		default:
			out_hex(va_next(&ap));
			break;
		}
	}
	out("\n");
}

/* The symbol at or below a kernel address, from the Mach-O symbol table: "name+0xoff". */
static void out_sym(uint64_t addr)
{
	const struct mach_header_64 *mh = (const struct mach_header_64 *)xkernel;
	const struct load_command *lc;
	const struct symtab_command *st = NULL;
	uint64_t best = 0;
	const char *best_name = NULL;

	out_hex(addr);
	if (!xkernel) {
		return;
	}
	lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SYMTAB) {
			st = (const void *)lc;
		}
	}
	if (!st) {
		return;
	}
	const struct nlist_64 *sym = (const struct nlist_64 *)(xkernel + st->symoff);
	const char *strs = (const char *)(xkernel + st->stroff);
	for (uint32_t i = 0; i < st->nsyms; i++) {
		if ((sym[i].n_type & 0x0e) == 0x0e && sym[i].n_value <= addr && sym[i].n_value > best) { /* N_SECT */
			best = sym[i].n_value;
			best_name = strs + sym[i].n_strx;
		}
	}
	if (best_name && addr - best < 0x100000) {
		out(" ");
		out(best_name);
		out("+");
		out_hex(addr - best);
	}
}

static void show_backtrace(void)
{
	uint64_t fp = xnu_fp[0];

	out("TCR_EL1 ");
	out_hex(xnu_fp[2]);
	out(" TTBR1_EL1 ");
	out_hex(xnu_fp[3]);
	out(" fp ");
	out_hex(fp);
	out(" -> pa ");
	out_hex(xnu_va_to_pa(fp));
	out(" sym debugger_panic_str -> pa ");
	out_hex(xnu_va_to_pa(xsyms.panic_str));
	out(" (slide: ");
	out_hex(kva_to_pa(xsyms.panic_str));
	out(")\nbacktrace:\n  lr ");
	out_sym(xnu_fp[1]);
	out("\n");
	for (int i = 0; i < 24 && fp; i++) {
		uint64_t lr = xnu_rd64(fp + 8);
		if (!lr) {
			break;
		}
		out("  ");
		out_sym(lr);
		out("\n");
		fp = xnu_rd64(fp);
	}
}

/*
 * What XNU's text console shows, read back from the framebuffer: each 8x16 cell is looked up in
 * XNU's own font (iso_font, 16 bytes a character, bit 0 the leftmost pixel; video_console.c).
 * Proof that the text reached the screen, and a copy of it.
 */
static void read_screen(void)
{
	static uint32_t font_hash[256];
	uint64_t font = xsyms.iso_font ? kva_to_pa(xsyms.iso_font) : 0;
	const uint32_t cols = PANEL_W / 8, rows = PANEL_H / 16;
	uint32_t lit_cells = 0, known = 0, last_row = 0;
	static char line[PANEL_W / 8 + 1];

	if (!font || !fb_base) {
		return;
	}
	for (int ch = 0; ch < 256; ch++) {
		uint32_t h = 0;
		for (int r = 0; r < 16; r++) {
			h = h * 0x01000193u ^ *(volatile uint8_t *)(font + ch * 16 + r);
		}
		font_hash[ch] = h;
	}
	for (uint32_t row = 0; row < rows; row++) {
		for (uint32_t col = 0; col < cols; col++) {
			uint8_t bits[16];
			uint32_t h = 0, any = 0;
			for (uint32_t r = 0; r < 16; r++) {
				const volatile uint32_t *px = (const volatile uint32_t *)(fb_base + (uint64_t)(row * 16 + r) * fb_pitch) + col * 8;
				uint8_t b = 0;
				for (uint32_t x = 0; x < 8; x++) {
					uint32_t p = px[x];
					uint32_t lum = ((p >> 16) & 0xff) + ((p >> 8) & 0xff) + (p & 0xff);
					if (lum > 3 * 0x60) {
						b |= (uint8_t)(1u << x);
					}
				}
				bits[r] = b;
				any |= b;
			}
			char out_c = ' ';
			if (any) {
				lit_cells++;
				for (int inv = 0; inv < 2 && out_c == ' '; inv++) {
					h = 0;
					for (int r = 0; r < 16; r++) {
						h = h * 0x01000193u ^ (uint8_t)(inv ? ~bits[r] : bits[r]);
					}
					for (int ch = 0x21; ch < 0x7f; ch++) {
						if (font_hash[ch] == h) {
							out_c = (char)ch;
							known++;
							break;
						}
					}
				}
				if (out_c == ' ') {
					out_c = '#';
				}
			}
			line[col] = out_c;
		}
		line[cols] = 0;
		/* trailing spaces off; blank rows kept short */
		int n = (int)cols;
		while (n > 0 && line[n - 1] == ' ') {
			n--;
		}
		line[n] = 0;
		if (n) {
			last_row = row + 1;
		}
		dbg_puts(line);
		dbg_putc('\n');
	}
	dbg_puts("[screen: ");
	dbg_dec(lit_cells);
	dbg_puts(" cells lit, ");
	dbg_dec(known);
	dbg_puts(" read as characters, last text row ");
	dbg_dec(last_row);
	dbg_puts("]\n");
}

/*
 * A picture of the panel, in the form the host's tools/fbshot.py makes a PNG of: the picture
 * is one line per 150 bytes, base64, under "fbshot N:". What was on the panel when XNU came
 * back is the only way to tell what the window server put there -- the phone cannot be looked
 * at while it runs, and its own console says nothing about the picture.
 *
 * The pixels are sampled down: the panel is 1080x2400, and what is wanted is a picture small
 * enough to fit in the console ring. One pixel of every STEP in each direction is taken, so
 * the picture is PANEL_W/STEP wide.
 *
 * The colours are as they are in memory: a word is B G R A from the lowest byte up, and the
 * picture wants R G B.
 */
#define SHOT_STEP    27
#define SHOT_W       (PANEL_W / SHOT_STEP)
#define SHOT_H       (PANEL_H / SHOT_STEP)
#define SHOT_BYTES   (SHOT_W * SHOT_H * 3)

static uint8_t shot_pixels[SHOT_BYTES];

/* Downsample the panel into shot_pixels. */
static void shot_panel(void)
{
	uint32_t used = 0;

	if (!fb_base || !fb_pitch) {
		return;
	}
	for (uint32_t y = 0; y < SHOT_H; y++) {
		const volatile uint32_t *row = (const volatile uint32_t *)
		    (fb_base + (uint64_t) (y * SHOT_STEP + SHOT_STEP / 2) * fb_pitch);

		for (uint32_t x = 0; x < SHOT_W; x++) {
			uint32_t pixel = row[x * SHOT_STEP + SHOT_STEP / 2];

			shot_pixels[used++] = (uint8_t) ((pixel >> 16) & 0xff);    /* R */
			shot_pixels[used++] = (uint8_t) ((pixel >> 8) & 0xff);     /* G */
			shot_pixels[used++] = (uint8_t) (pixel & 0xff);            /* B */
		}
	}
}

/* The picture, as the host's tools/fbshot.py reads it. The number tells them apart. */
static void shot_print(int number, const char *what)
{
	static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	uint64_t fb = fbc.base;

	fbc.base = 0;  /* the picture is in the log, not on the panel */
	dbg_puts("fbshot ");
	dbg_dec(number);
	dbg_puts(": begin ");
	dbg_dec(SHOT_W);
	dbg_putc(' ');
	dbg_dec(SHOT_H);
	dbg_puts(" (");
	dbg_puts(what);
	dbg_puts(")\n");
	for (uint32_t at = 0; at < SHOT_BYTES; at += 150) {
		uint32_t n = SHOT_BYTES - at < 150 ? SHOT_BYTES - at : 150;
		char line[205];
		uint32_t k = 0;

		for (uint32_t i = 0; i + 2 < n; i += 3) {
			uint32_t v = (uint32_t) shot_pixels[at + i] << 16 |
			    (uint32_t) shot_pixels[at + i + 1] << 8 | shot_pixels[at + i + 2];

			line[k++] = digits[v >> 18 & 63];
			line[k++] = digits[v >> 12 & 63];
			line[k++] = digits[v >> 6 & 63];
			line[k++] = digits[v & 63];
		}
		line[k] = 0;
		dbg_puts("fbshot ");
		dbg_dec(number);
		dbg_puts(": ");
		dbg_puts(line);
		dbg_putc('\n');
	}
	dbg_puts("fbshot ");
	dbg_dec(number);
	dbg_puts(": end\n");
	fbc.base = fb;
}

/*
 * XNU is back (start.S xnu_return_stub): MMU off, our vectors, our stack. Its log was
 * linearized and written back to RAM by XNU. Save it, show its end, then reset into Android.
 */
void xnu_returned(uint64_t log_pa, uint64_t len, uint64_t reason, uint64_t code, uint64_t elr, uint64_t far)
{
	struct span sp = { (const volatile char *)log_pa, len };

	wdt_disable();
	xnu_back = 1;
	marker_set(MARK_DONE);
	uint64_t screen = fbc.base;
	fbc.base = 0; /* nothing drawn over XNU's console before it has been read */
	dbg_puts("\n=== what the screen showed when XNU came back ===\n");
	/*
	 * XNU wrote the picture with its caches on, and the stub wrote back only its log: what
	 * it left on the panel is still in the cache. Write it back before it is read.
	 */
	if (fb_base && fb_pitch) {
		dcache_clean_range(fb_base, (uint64_t) fb_pitch * PANEL_H);
	}
	read_screen();
	shot_panel();
	fbc.base = screen;
	fbc.fg = 0xffffff40u;
	out("\n=== XNU returned to mtkloader: reason ");
	out_dec(reason);
	out(reason == 0 ? " (end)" : reason == 1 ? " (early exception)" : reason == 2 ? " (checkpoint)" : "");
	out(" code ");
	out_hex(code);
	if (reason == 1) {
		out(" ELR ");
		out_hex(elr);
		out(" FAR ");
		out_hex(far);
	}
	out("\n");
	/* Did user space ever start? tasks_count is 1 while only the kernel's task exists. */
	if (xsyms.tasks_count) {
		out("tasks ");
		out_dec(rd32(kva_to_pa(xsyms.tasks_count)));
		if (xsyms.nprocs) {
			out(" processes ");
			out_dec(rd32(kva_to_pa(xsyms.nprocs)));
		}
		if (xsyms.initproc) {
			out(" initproc ");
			out_hex(*(volatile uint64_t *)kva_to_pa(xsyms.initproc));
		}
		out("\n");
	}
	if (reason != 1) {
		show_panic_string(&xsyms);
		show_backtrace();
	}
	out("=== XNU log (");
	out_dec(len);
	out(" bytes) ===\n");
	if (len > 4 * 1024 * 1024) {
		sp.n = 0;
	}
	save_log(&sp, 1);
	dbg_puts("=== end of XNU log ===\n");
	shot_print(2, "the panel when XNU came back");
	show_log_tail(40);
	dbg_delay_ms(2000);
	reset_board();
}

/*
 * After a watchdog reset: what XNU had printed (its message buffer, through its own page tables:
 * cpu_ttep, 4K granule, 39-bit kernel addresses as TCR_EL1 had them), whether its clock ticked
 * (loader_live.heartbeat), and any panic string.
 */
static void rescue_state(void)
{
	if (xsyms.cpu_ttep) {
		xnu_fp[3] = rd32(kva_to_pa(xsyms.cpu_ttep)) | (uint64_t)rd32(kva_to_pa(xsyms.cpu_ttep) + 4) << 32;
		xnu_fp[2] = 0x22a51a251aULL;
	}
	out("cpu_ttep ");
	out_hex(xnu_fp[3]);
	if (xsyms.live) {
		out(" heartbeat ");
		out_dec(*(volatile uint64_t *)kva_to_pa(xsyms.live + 8));
	}
	out("\n");
	show_panic_string(&xsyms);
	if (!xsyms.msgbufp || !xnu_fp[3]) {
		return;
	}
	uint64_t mb = xnu_rd64(xsyms.msgbufp);
	uint64_t w0 = xnu_rd64(mb), w1 = xnu_rd64(mb + 8), bufc = xnu_rd64(mb + 16);
	int32_t size = (int32_t)(w0 >> 32), bufx = (int32_t)w1;
	out("msgbuf ");
	out_hex(mb);
	out(" size ");
	out_dec((uint32_t)size);
	out(" bufx ");
	out_dec((uint32_t)bufx);
	out("\n--- kernel message buffer (from DRAM) ---\n");
	if (size <= 0 || size > 1024 * 1024 || bufx < 0 || bufx > size) {
		return;
	}
	uint64_t fbsave = fbc.base;
	fbc.base = 0;
	for (int pass = 0; pass < 2; pass++) {
		int from = pass == 0 ? bufx : 0, to = pass == 0 ? size : bufx;
		for (int i = from; i < to; i++) {
			char c = xnu_rd8(bufc + i);
			if (c) {
				dbg_putc(c);
			}
		}
	}
	fbc.base = fbsave;
	dbg_puts("\n--- end of message buffer ---\n");
}

/* The watchdog cut XNU short: its log is still in DRAM, at the addresses of its symbols. */
static void rescue_log(void)
{
	struct payload_info pi;
	struct xnu_syms s;

	out("\n=== mtkloader: XNU ran until the watchdog reset ===\n");
	if (!parse_payload(&pi)) {
		out("no XNU payload\n");
		return;
	}
	find_syms(pi.kernel, &s);
	if (!s.log || !s.log_len || !s.log_ring_at || !s.log_wrapped) {
		out("XNU log symbols not found\n");
		return;
	}
	/* osfmk/arm64/loader_return.c: a 256 KB head, then a 1 MB ring */
	const uint64_t head_sz = 256 * 1024, ring_sz = 1024 * 1024;
	const volatile char *log = (const volatile char *)kva_to_pa(s.log);
	uint32_t len = rd32(kva_to_pa(s.log_len));
	uint32_t ring_at = rd32(kva_to_pa(s.log_ring_at));
	uint8_t wrapped = *(const volatile uint8_t *)kva_to_pa(s.log_wrapped);
	struct span sp[3];
	int n = 0;

	out("loader_log_len ");
	out_dec(len);
	out(" ring_at ");
	out_dec(ring_at);
	out(" wrapped ");
	out_dec(wrapped);
	out("\n");
	if (ring_at > ring_sz) {
		ring_at = 0;
	}
	if (!wrapped) {
		if (len > head_sz + ring_sz) {
			len = head_sz + ring_sz;
		}
		sp[n++] = (struct span){ log, len };
	} else {
		sp[n++] = (struct span){ log, head_sz };
		sp[n++] = (struct span){ log + head_sz + ring_at, ring_sz - ring_at };
		sp[n++] = (struct span){ log + head_sz, ring_at };
	}
	xsyms = s;
	xkernel = pi.kernel;
	rescue_state();
	dbg_puts("=== XNU log (from DRAM; a few bits may have flipped in the reset) ===\n");
	save_log(sp, n);
	dbg_puts("=== end of XNU log ===\n");
	show_log_tail(40);
}

/*
 * Do the timers reach EL1 here? The virtual timer's PPI 27 is what XNU uses, and its interrupt
 * never arrives: the kernel idles for ever waiting for a root device that no timeout ever wakes.
 * Set up the GIC as XNU does, arm each timer in turn and see which one is taken.
 */
#define GICD_BASE   0x0c000000ULL
#define GICR_SGI    (0x0c040000ULL + 0x10000)

#if TIMER_PROBE
static void gic_cpu_prepare(void)
{
	uint64_t sre, ctlr;

	/*
	 * ICC_CTLR_EL1.EOImode: with it set, ICC_EOIR1_EL1 only drops the priority and the
	 * interrupt stays active until ICC_DIR_EL1 -- and XNU never writes that, so its first
	 * timer interrupt would also be its last. Clear it for whoever runs on this core next.
	 */
	__asm__ volatile("mrs %0, ICC_CTLR_EL1" : "=r"(ctlr));
	out("  ICC_CTLR_EL1 ");
	out_hex(ctlr);
	out(ctlr & 2 ? " (EOImode=1: EOI does not deactivate)" : " (EOImode=0)");
	__asm__ volatile("msr ICC_CTLR_EL1, %0; isb" : : "r"(ctlr & ~2ULL));
	__asm__ volatile("mrs %0, ICC_CTLR_EL1" : "=r"(ctlr));
	out(" -> ");
	out_hex(ctlr);
	out("\n");

	__asm__ volatile("mrs %0, ICC_SRE_EL1" : "=r"(sre));
	__asm__ volatile("msr ICC_SRE_EL1, %0; isb" : : "r"(sre | 1));
	__asm__ volatile("msr ICC_AP1R0_EL1, xzr");
	__asm__ volatile("msr ICC_PMR_EL1, %0" : : "r"(0xffUL));
	__asm__ volatile("msr ICC_BPR1_EL1, xzr");
	__asm__ volatile("msr ICC_IGRPEN1_EL1, %0; isb" : : "r"(1UL));
}
#endif

/*
 * EOImode (ICC_CTLR_EL1 bit 1) is what ATF/GenieZone leaves set on this board: ICC_EOIR1_EL1
 * then only drops the running priority, and the interrupt stays *active* until a write to
 * ICC_DIR_EL1. XNU writes only EOIR, so with EOImode set it takes one timer interrupt and never
 * sees another: it idles for ever, waiting for timeouts that do not come. Clear it here, on the
 * core the kernel is about to run on.
 */
static void gic_fix_eoimode(void)
{
	uint64_t ctlr;

	__asm__ volatile("mrs %0, ICC_CTLR_EL1" : "=r"(ctlr));
	out("ICC_CTLR_EL1 ");
	out_hex(ctlr);
	out(ctlr & 2 ? " (EOImode set: clearing)" : " (EOImode clear)");
	if (ctlr & 2) {
		__asm__ volatile("msr ICC_CTLR_EL1, %0; isb" : : "r"(ctlr & ~2ULL));
		__asm__ volatile("mrs %0, ICC_CTLR_EL1" : "=r"(ctlr));
		out(" -> ");
		out_hex(ctlr);
	}
	out("\n");
}

#if TIMER_PROBE
static int timer_try(int ppi, int virt)
{
	uint64_t f = dbg_freq(), now, grp;
	uint32_t before;

	/* Group 1 NS, default priority, enabled: the same as XNU's gic_v3_irq_cpu_init(). */
	wr32(GICR_SGI + 0x080, rd32(GICR_SGI + 0x080) | (1u << ppi));
	wr32(GICR_SGI + 0xd00, rd32(GICR_SGI + 0xd00) & ~(1u << ppi));
	*(volatile uint8_t *)(GICR_SGI + 0x400 + ppi) = 0xa0;
	wr32(GICR_SGI + 0x100, 1u << ppi);
	grp = rd32(GICR_SGI + 0x080);
	wr32(GICR_SGI + 0x380, 1u << ppi); /* nothing of an earlier try left active */
	irq_count = 0;
	irq_intid = 0;

  again:
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(now));
	if (virt) {
		__asm__ volatile("msr cntv_cval_el0, %0; msr cntv_ctl_el0, %1; isb" : : "r"(now + f / 10), "r"(1UL));
	} else {
		__asm__ volatile("msr cntp_cval_el0, %0; msr cntp_ctl_el0, %1; isb" : : "r"(now + f / 10), "r"(1UL));
	}
	before = rd32(GICR_SGI + 0x200);
	irq_wait(now + f / 2);

	out(virt ? "  virtual timer  PPI " : "  physical timer PPI ");
	out_dec((uint64_t)ppi);
	out(": IGROUPR0 ");
	out_hex(grp);
	out(grp & (1u << ppi) ? " (group 1)" : " (GROUP 0: not ours)");
	out(", ISPENDR0 ");
	out_hex(before);
	out("->");
	out_hex(rd32(GICR_SGI + 0x200));
	out(", ISACTIVER0 after EOI ");
	out_hex(rd32(GICR_SGI + 0x300));
	out(", irqs ");
	out_dec(irq_count);
	if (irq_count) {
		out(" intid ");
		out_dec(irq_intid);
	}
	out(", ISACTIVER0 ");
	out_hex(rd32(GICR_SGI + 0x300));
	out("\n");
	if (irq_count == 1) {
		goto again;     /* a second one proves the first was deactivated */
	}
	__asm__ volatile("msr cntv_ctl_el0, %0; msr cntp_ctl_el0, %0; isb" : : "r"(2UL));
	/* Leave this PPI as XNU must find it: disabled, not pending, not active. */
	wr32(GICR_SGI + 0x180, 1u << ppi); /* ICENABLER0 */
	wr32(GICR_SGI + 0x280, 1u << ppi); /* ICPENDR0   */
	wr32(GICR_SGI + 0x380, 1u << ppi); /* ICACTIVER0 */
	__asm__ volatile("dsb sy; isb");
	return irq_count != 0;
}

static void timer_probe(void)
{
	out("timer probe at EL1:\n");
	gic_cpu_prepare();
	timer_try(27, 1);
	timer_try(30, 0);
	/* and the CPU interface as it was, so nothing of ours is left in it */
	__asm__ volatile("msr ICC_IGRPEN1_EL1, xzr; msr ICC_AP1R0_EL1, xzr; isb");
	out("  GICR after cleanup: ISENABLER0 ");
	out_hex(rd32(GICR_SGI + 0x100));
	out(" ISPENDR0 ");
	out_hex(rd32(GICR_SGI + 0x200));
	out(" ISACTIVER0 ");
	out_hex(rd32(GICR_SGI + 0x300));
	out("\n");
}
#endif /* TIMER_PROBE */

/* ---- the watcher on the second core ---- */

static uint64_t watcher_l1[512] __attribute__((aligned(4096)));
static volatile uint64_t watcher_started __attribute__((aligned(64)));

/*
 * XNU never came back (no timer, a hang): with its caches visible through our own cacheable
 * mapping, take what it shows and has written, put it into ramoops and reset the board.
 */
void watcher_main(void)
{
	for (int i = 0; i < 512; i++) {
		uint64_t pa = (uint64_t)i << 30;
		if (i == 0) {
			watcher_l1[i] = pa | (1u << 10) | (0u << 2) | 1;            /* MMIO: device */
		} else if (i < 8) {
			watcher_l1[i] = pa | (1u << 10) | (3u << 8) | (1u << 2) | 1; /* DRAM: normal WB, inner shareable */
		} else {
			watcher_l1[i] = 0;
		}
	}
	mmu_on((uint64_t)watcher_l1);
	watcher_started = 1;
	dcache_clean_range((uint64_t)&watcher_started, 8);

	uint32_t watch_secs = (runtime_secs > 0) ? (runtime_secs + 240) : XNU_WATCH_SECONDS;
	uint64_t end = dbg_ticks() + dbg_freq() * watch_secs;
	while (dbg_ticks() < end) {
		dcache_clean_range((uint64_t)&xnu_back, 8);
		if (xnu_back) {
			for (;;) {
				__asm__ volatile("wfe");
			}
		}
		dbg_delay_ms(100);
	}
	dcache_clean_range((uint64_t)&xnu_back, 8);
	if (xnu_back) {
		for (;;) {
			__asm__ volatile("wfe");
		}
	}

	wdt_disable();
	fbc.base = 0; /* the screen is XNU's: read it, do not draw on it */
	marker_set(MARK_DONE);
	dbg_puts("\n=== watcher: XNU still running after " );
	dbg_dec(watch_secs);
	dbg_puts(" s, snapshot ===\n");
	if (xsyms.cpu_ttep) {
		xnu_fp[3] = *(volatile uint64_t *)kva_to_pa(xsyms.cpu_ttep);
		xnu_fp[2] = 0x22a51a251aULL;
	}
	dbg_puts("cpu_ttep ");
	dbg_hex(xnu_fp[3], 10);
	if (xsyms.live) {
		dbg_puts(" heartbeat ");
		dbg_dec(*(volatile uint64_t *)kva_to_pa(xsyms.live + 8));
	}
	if (xsyms.log_len) {
		dbg_puts(" loader_log_len ");
		dbg_dec(rd32(kva_to_pa(xsyms.log_len)));
	}
	dbg_puts("\n");
	/*
	 * Is the virtual timer's PPI 27 even reaching the first core? GICR frame 0 is its
	 * redistributor (SGI/PPI page at +0x10000); pending but not active means the GIC latched
	 * the timer and the CPU interface refused it, nothing pending means no timer at all.
	 */
	{
		const uint64_t gicd = 0x0c000000ULL, gicr = 0x0c040000ULL, sgi = gicr + 0x10000;
		dbg_puts("GICD_CTLR ");
		dbg_hex(rd32(gicd + 0x000), 8);
		dbg_puts(" TYPER ");
		dbg_hex(rd32(gicd + 0x004), 8);
		dbg_puts("\nGICR_CTLR ");
		dbg_hex(rd32(gicr + 0x000), 8);
		dbg_puts(" WAKER ");
		dbg_hex(rd32(gicr + 0x014), 8);
		dbg_puts(" TYPER ");
		dbg_hex(rd32(gicr + 0x008), 8);
		dbg_putc(' ');
		dbg_hex(rd32(gicr + 0x00c), 8);
		dbg_puts("\nGICR_SGI: IGROUPR0 ");
		dbg_hex(rd32(sgi + 0x080), 8);
		dbg_puts(" IGRPMODR0 ");
		dbg_hex(rd32(sgi + 0xd00), 8);
		dbg_puts(" ISENABLER0 ");
		dbg_hex(rd32(sgi + 0x100), 8);
		dbg_puts("\n          ISPENDR0 ");
		dbg_hex(rd32(sgi + 0x200), 8);
		dbg_puts(" ISACTIVER0 ");
		dbg_hex(rd32(sgi + 0x300), 8);
		dbg_puts(" IPRIORITY27 ");
		dbg_hex(*(volatile uint8_t *)(sgi + 0x400 + 27), 2);
		dbg_puts(" ICFGR1 ");
		dbg_hex(rd32(sgi + 0xc04), 8);
		dbg_puts("\n");
	}
	if (xsyms.gic_unhandled_count) {
		dbg_puts("kernel: gic_unhandled_count ");
		dbg_dec(*(volatile uint64_t *)kva_to_pa(xsyms.gic_unhandled_count));
		dbg_puts(" last_unhandled ");
		dbg_dec(rd32(kva_to_pa(xsyms.gic_last_unhandled)));
		dbg_puts(" active_iar[0] ");
		dbg_hex(*(volatile uint64_t *)kva_to_pa(xsyms.gic_active_iar), 16);
		dbg_puts("\n");
	}

	dbg_puts("\n=== what the screen shows ===\n");
	read_screen();
	show_panic_string(&xsyms);
	if (xsyms.msgbufp && xnu_fp[3]) {
		uint64_t mb = xnu_rd64(xsyms.msgbufp);
		uint64_t w0 = xnu_rd64(mb), w1 = xnu_rd64(mb + 8), bufc = xnu_rd64(mb + 16);
		int32_t size = (int32_t)(w0 >> 32), bufx = (int32_t)w1;
		dbg_puts("--- kernel message buffer ---\n");
		if (size > 0 && size <= 1024 * 1024 && bufx >= 0 && bufx <= size) {
			for (int pass = 0; pass < 2; pass++) {
				int from = pass == 0 ? bufx : 0, to = pass == 0 ? size : bufx;
				for (int i = from; i < to; i++) {
					char c = xnu_rd8(bufc + i);
					if (c) {
						dbg_putc(c);
					}
				}
			}
		}
		dbg_puts("\n--- end of message buffer ---\n");
	}
	if (xsyms.log && xsyms.log_len) {
		uint32_t len = rd32(kva_to_pa(xsyms.log_len));
		struct span sp = { (const volatile char *)kva_to_pa(xsyms.log), len > 256 * 1024 ? 256 * 1024 : len };
		dbg_puts("--- loader_log (head) ---\n");
		save_log(&sp, 1);
	}
	dbg_puts("=== end of watcher snapshot ===\n");
	plog_flush();
	psci_system_reset();
	wdt_reset();
}

static void watcher_start(void)
{
	if (!XNU_WATCH_SECONDS && !runtime_secs) {
		return;
	}
	watcher_started = 0;
	uint32_t active_cpus = (runtime_cpus >= 1 && runtime_cpus <= 7) ? runtime_cpus : XNU_CORES;
	uint8_t watch_core = core_order[active_cpus];
	int64_t r = psci_cpu_on((uint64_t)watch_core << 8, (uint64_t)watcher_entry, 0);
	out("watcher on cpu");
	out_dec(watch_core);
	out(": PSCI CPU_ON ");
	out_dec((uint64_t)r);
	for (int i = 0; i < 100 && !watcher_started; i++) {
		dbg_delay_ms(10);
	}
	out(watcher_started ? ", running\n" : ", not seen\n");
}

/* ---- starting XNU ---- */

/* ---- how fast the cores are running ---- */

/*
 * What the boot loader left the cores at, in megahertz. The system counter runs at a fixed rate
 * whatever the cores do, so counting core cycles between two of its ticks gives the core's clock
 * directly. Nothing here changes a frequency -- it only asks, because a kernel that finds itself
 * on a core the boot loader parked at a third of its speed is worth knowing about before anyone
 * sets about raising it.
 */
static uint32_t cpu_mhz(void)
{
	uint64_t pmcr, cycles_before, cycles_after, ticks_before, ticks_after, hz = dbg_freq();

	__asm__ volatile("mrs %0, pmcr_el0" : "=r"(pmcr));
	__asm__ volatile("msr pmcr_el0, %0" : : "r"((pmcr | 5u) & ~2u));   /* enable, reset the count */
	__asm__ volatile("msr pmcntenset_el0, %0" : : "r"(1ull << 31));
	__asm__ volatile("isb");

	ticks_before = dbg_ticks();
	__asm__ volatile("mrs %0, pmccntr_el0" : "=r"(cycles_before));
	dbg_delay_ms(20);
	__asm__ volatile("mrs %0, pmccntr_el0" : "=r"(cycles_after));
	ticks_after = dbg_ticks();

	if (cycles_after <= cycles_before || ticks_after <= ticks_before) {
		return 0;                       /* the counter is not ours to read */
	}
	return (uint32_t)(((cycles_after - cycles_before) * hz) /
	    ((ticks_after - ticks_before) * 1000000ull));
}

/* ---- the touch screen, before the kernel wants it ---- */

/*
 * Room for the firmware and the fourteen header bytes in front of it: plain DRAM that nothing
 * else wants.
 *
 * MT6893: the reference board's map was "the loader's image ends before 0x400c0000, XNU's log
 * ring sits at 0x47b1c9c0 and ramoops at 0x4d0e0000". On this board the DRAM starts at
 * 0x40000000 and this phone's ramoops is at 0x48090000 (size 0xe0000), so the staging area is
 * put above the ramoops, clear of it.
 */
#define TOUCH_STAGE         0x49000000ULL

#if 0
/* The SPI clock mux as we found it, so that Android can be given it back that way. */
static uint32_t touch_clk_cfg2;
static int touch_clk_cfg2_seen;

static uint8_t touch_bring_up_once(void)
{
	static const uint32_t gate_clr[4] = { 0x8c, 0xa8, 0xc4, 0x84 };
	uint8_t buf[64], code = 0;
	uint32_t got = 0;
	uint8_t *stage = (uint8_t *)TOUCH_STAGE;

	for (int i = 0; i < 4; i++) {
		wr32(INFRACFG_BASE + gate_clr[i], 0xffffffffu);
	}
	__asm__ volatile("dsb sy");
	touch_clk_cfg2 = rd32(TOPCKGEN_BASE + CLK_CFG_2);
	touch_clk_cfg2_seen = 1;
	spi_pins_to_spi2();
	spi_setup();

	tcm_reset();
	if (tcm_read_patiently(&code, buf, sizeof(buf), &got) != 1 || got < 2) {
		out("touch: the controller said nothing after a reset (code ");
		out_hex(code);
		out(")\n");
		return 0;
	}
	out("touch: after a reset, code ");
	out_hex(code);
	out(", mode ");
	out_hex(buf[1]);
	out("\n");
	if (buf[1] != TCM_MODE_ROMBOOT) {
		out("touch: not in its ROM bootloader (mode ");
		out_hex(buf[1]);
		out("), leaving it alone\n");
		return buf[1];
	}

	/* The message whole in DRAM -- command, length, header, image -- to go out in one breath. */
	memcpy(stage + TCM_MESSAGE_HEAD + TCM_ROMBOOT_HEADER, touch_fw, TOUCH_FW_SIZE);
	tcm_load_firmware(stage);
	if (tcm_read_patiently(&code, buf, sizeof(buf), &got) != 1 || code != TCM_STATUS_OK) {
		out("touch: the image was not taken (");
		out_hex(code);
		out(")\n");
		return 0;
	}
	out("touch: the image was taken\n");
	tcm_run_firmware();
	for (uint32_t waited = 0; waited < 4000; waited += 10) {
		if (!tcm_irq()) {
			dbg_delay_ms(10);
			continue;
		}
		if (tcm_read(&code, buf, sizeof(buf), &got) != 1) {
			dbg_delay_ms(10);
			waited += 10;
			continue;
		}
		out("touch: it said code ");
		out_hex(code);
		out(" after ");
		out_hex(waited);
		out(" ms\n");
		if (code == TCM_REPORT_IDENTIFY && got >= 2) {
			out("touch: ");
			for (uint32_t i = 2; i < got && i < 18; i++) {
				out_putc(buf[i] >= 0x20 && buf[i] < 0x7f ? (char)buf[i] : ' ');
			}
			out(" is up, mode ");
			out_hex(buf[1]);
			out("\n");
			return buf[1];
		}
	}
	out("touch: it never said it was up\n");
	return 0;
}
#endif

/*
 * The touch report, as the phone's own driver reads it: write 0x01, then read 62 bytes. Byte 1's
 * high nibble is the report type and its low nibble the number of points, and the point block
 * starts at byte 2, six bytes a point:
 *
 *   b0: bits 7.6 = event (2 = contact), bits 3.0 = x[15:12]
 *   b1: x[11:4]
 *   b2: bits 7.4 = finger id, bits 3.0 = y[15:12]
 *   b3: y[11:4]
 *   b4: bits 7.4 = x[3:0], bits 3.0 = y[3:0]
 *   b5: area
 *
 * so x = ((b0 & 0x0f) << 12) | (b1 << 4) | (b4 >> 4) and y = ((b2 & 0x0f) << 12) | (b3 << 4) |
 * (b4 & 0x0f). The coordinates are the panel's 1080 x 2400 in sixteenths of a pixel.
 */
static void fts_say_point(const struct fts_point *p)
{
	out("    point id=");
	out_hex(p->id);
	out(" event=");
	out_hex(p->event);
	out(" x=");
	out_dec(p->x);
	out(" y=");
	out_dec(p->y);
	out(" area=");
	out_dec(p->area);
	out("\n");
}

/*
 * The whole sequence, up to six times, waiting a little longer between tries each time. Now and then the chip takes the image and then says
 * nothing after the go command -- two runs in four did -- and a fresh start from a reset brings
 * it up; this is what left iOS without a touch screen.
 *
 * MT6893: the reference board's touch is an OmniVision TD4160, a TCM controller on SPI2. This
 * board's is a FocalTech FT3681 on I2C0 (see docs/touchscreen-ft3681.md). The path below probes
 * it over I2C0; the controller's register table, clocks, pinmux and timings are all in
 * docs/handoff-touch-i2c0.md, which also lists what mtki2c.h still gets wrong.
 */
static uint8_t touch_bring_up(void)
{
	out("touch: probing MT6893 I2C0 for FocalTech FT3681...\n");
	i2c0_dump_state("pre");
	/* the touch rail is MT6360 LDO2 on I2C5; it is off at this point, and the chip cannot
	 * answer on I2C0 until it is on. The order is the vendor driver's: hold the chip in
	 * reset, power the rail, then let it go. */
	fts_reset_assert();
	mt6360_enable_touch_rail();
	dbg_delay_us(20000);
	fts_reset_release();
	mtk_i2c0_ungate_clocks();
	i2c0_pin_pull_up();
	i2c_bb_scan();
	i2c_hw_pinmux();
	mtk_i2c0_init_hw();
	i2c0_dump_state("hw");

	out("i2c0: TIMING=");
	out_hex(rd16(I2C0_BASE + I2C_REG_TIMING));
	out(" LTIMING=");
	out_hex(rd16(I2C0_BASE + I2C_REG_LTIMING));
	out(" EXT_CONF=");
	out_hex(rd16(I2C0_BASE + I2C_REG_EXT_CONF));
	out(" CONTROL=");
	out_hex(rd16(I2C0_BASE + I2C_REG_CONTROL));
	out(" DEBUGSTAT=");
	out_hex(rd16(I2C0_BASE + I2C_REG_DEBUGSTAT));
	out("\n");

	uint8_t chip_h = 0, chip_l = 0, fw_ver = 0, vendor_id = 0, td_status = 0;
	int r_h = fts_read_byte(FTS_REG_CHIP_ID_H, &chip_h);
	int r_l = fts_read_byte(FTS_REG_CHIP_ID_L, &chip_l);
	int r_fw = fts_read_byte(FTS_REG_FW_VER, &fw_ver);
	int r_vid = fts_read_byte(FTS_REG_VENDOR_ID, &vendor_id);
	int r_td = fts_read_byte(FTS_REG_TD_STATUS, &td_status);

	out("touch: probe chip_id=");
	if (r_h == 0 && r_l == 0) {
		out_hex(((uint32_t)chip_h << 8) | chip_l);
	} else {
		out("FAIL(err=");
		out_dec((uint32_t)(r_h ? -r_h : -r_l));
		out(")");
	}
	out(" fw_ver=");
	if (r_fw == 0) {
		out_hex(fw_ver);
	} else {
		out("ERR");
	}
	out(" vendor=");
	if (r_vid == 0) {
		out_hex(vendor_id);
	} else {
		out("ERR");
	}
	out(" points=");
	if (r_td == 0) {
		out_dec(td_status & 0x0fu);
	} else {
		out("ERR");
	}
	out("\n");

	/*
	 * The report, as the phone's own driver reads it: write 0x01, then read 62 bytes. The panel
	 * answers with 0xff in every byte when it has nothing to say -- the phone's own driver calls
	 * that "touch buff is 0xff, need recovery state" -- so the frame is read again and again until
	 * one carries a point. TOUCH_REPORT_TRIES frames, 10 ms apart.
	 */
	uint32_t tries = 0;

	/* what each candidate register holds, so the report register can be identified */
	for (uint32_t reg = 0; reg < 4; reg++) {
		uint8_t r = (uint8_t)reg;
		uint8_t report[FTS_REPORT_LEN];

		if (mtk_i2c0_write_read(FTS_SLAVE_ADDR, &r, 1, report, sizeof(report)) != 0) {
			out("touch: read of register 0x");
			out_hex(r);
			out(" failed\n");
			continue;
		}
		out("touch: register 0x");
		out_hex(r);
		out(": ");
		for (uint32_t i = 0; i < 8; i++) {
			out_hex(report[i]);
			out(" ");
		}
		out(fts_report_is_idle(report) ? " (all 0xff)\n" : "\n");
	}

	/* the long read itself: the chip id register holds 0x56, so the first byte must come back */
	{
		uint8_t r = FTS_REG_CHIP_ID_H;
		uint8_t report[FTS_REPORT_LEN];

		if (mtk_i2c0_write_read(FTS_SLAVE_ADDR, &r, 1, report, sizeof(report)) == 0) {
			out("touch: 62-byte read of the chip id register: ");
			for (uint32_t i = 0; i < 4; i++) {
				out_hex(report[i]);
				out(" ");
			}
			out(report[0] == 0x56 ? "(the long read works)\n" : "(the long read does not work)\n");
		} else {
			out("touch: the long read of the chip id register failed\n");
		}
	}

	for (tries = 0; tries < TOUCH_REPORT_TRIES; tries++) {
		uint8_t reg = FTS_REG_REPORT;
		uint8_t report[FTS_REPORT_LEN];
		struct fts_point pts[FTS_MAX_POINTS];
		uint32_t n;

		if (mtk_i2c0_write_read(FTS_SLAVE_ADDR, &reg, 1, report, sizeof(report)) != 0) {
			out("touch: 62-byte report over DMA FAIL\n");
			break;
		}
		if (fts_report_is_idle(report)) {
			dbg_delay_ms(10);
			continue;
		}
		n = fts_decode_report(report, pts, FTS_MAX_POINTS);
		out("touch: 62-byte report over DMA ok, type=");
		out_hex(fts_report_type(report));
		out(" count=");
		out_dec(fts_report_count(report));
		out(" points=");
		out_dec(n);
		out("\n");
		for (uint32_t i = 0; i < n; i++) {
			fts_say_point(&pts[i]);
		}
		if (n > 0) {
			break;
		}
		dbg_delay_ms(10);
	}
	if (tries == TOUCH_REPORT_TRIES) {
		out("touch: no point in ");
		out_dec(tries);
		out(" report frames (the panel said 0xff every time)\n");
	}
#ifdef TOUCH_WAIT_MS
	/*
	 * TOUCH_WAIT_MS=N: keep reading the panel for N milliseconds after the first window, so a
	 * finger put on the panel while the phone boots is seen and its coordinates printed. The
	 * window above is TOUCH_REPORT_TRIES frames 10 ms apart, which is over before anyone can
	 * reach the phone; this one is long enough to put a finger down in it. The reading itself
	 * is the same one the kernel's driver makes, over the same DMA engine.
	 */
	{
		uint32_t waited = 0;
		uint8_t reg = FTS_REG_REPORT;
		uint8_t report[FTS_REPORT_LEN];
		struct fts_point pts[FTS_MAX_POINTS];
		uint32_t n;

		out("touch: waiting ");
		out_dec(TOUCH_WAIT_MS);
		out(" ms for a finger on the panel\n");
		while (waited < TOUCH_WAIT_MS) {
			if (mtk_i2c0_write_read(FTS_SLAVE_ADDR, &reg, 1, report, sizeof(report)) == 0 &&
			    !fts_report_is_idle(report)) {
				n = fts_decode_report(report, pts, FTS_MAX_POINTS);
				out("touch: finger: report over DMA ok, type=");
				out_hex(fts_report_type(report));
				out(" count=");
				out_dec(fts_report_count(report));
				out(" points=");
				out_dec(n);
				out("\n");
				for (uint32_t i = 0; i < n; i++) {
					fts_say_point(&pts[i]);
				}
				break;
			}
			dbg_delay_ms(20);
			waited += 20;
		}
		if (waited >= TOUCH_WAIT_MS) {
			out("touch: no finger in ");
			out_dec(TOUCH_WAIT_MS);
			out(" ms\n");
		}
	}
#endif

	if (r_h == 0 && r_l == 0 && chip_h == 0x56 && chip_l == 0x62) {
		out("touch: FocalTech FT3681 active and ready on I2C0 (0x38)\n");
		return 1;
	}

	return 0;
}

/* ---- the big cores' clock ---- */

/*
 * The reference board (MT6768): LK leaves the big cluster (Cortex-A75) at 1087 MHz with its rail
 * at 800 mV, and MediaTek's own table for that chip (cpufreq_v1, mach/mt6768, "OPP table B") runs
 * it at 1176 MHz at that very voltage, so it goes there: no rail is touched. The cluster is still
 * off while the loader runs, so nothing is executing on the PLL while it is reprogrammed.
 *
 * ARMPLL_CON1 (apmixedsys + 0x20c): bit 31 asks for the new value, bits 24..26 the post divider
 * (here /2), bits 0..21 the multiplier of 26 MHz / 2^14 -- the same write the vendor's GPU driver
 * makes to its PLL.
 *
 * MT6893: this is NOT the reference's clock, and the reference's write would be wrong on this
 * board. This chip has three clusters with their own PLLs and a DVFS operating-points table per
 * cluster, and the tree gives the frequencies the phone runs at:
 *
 *   the A55s      0x77359400 = 2.000 GHz   (opp_table0, phandle 0x20)
 *   the A78s      0x9af8da00 = 2.600 GHz   (opp_table1, phandle 0x25)
 *   the prime     0xa1b51840 = 2.711 GHz   (opp_table2, phandle 0x28)
 *
 * MT6893's APMIXED is at 0x1000c000 (/apmixed@1000c000, mediatek,apmixed) -- the same address as
 * the reference board's, but the PLL layout inside it is not the same, and which register holds
 * which cluster's PLL is not yet read out of this board. So nothing is reprogrammed here: the
 * loader reads what the preloader left and prints it, and the clock is the kernel's business
 * (the reference port's mtk_cpufreq driver does it from the kernel).
 */
#define APMIXED_BASE        0x1000c000ULL
#define ARMPLL_CON1         0x20c

static void big_cores_clock(void)
{
	uint32_t con = rd32(APMIXED_BASE + ARMPLL_CON1);

	/*
	 * MT6893: print what the preloader left, and change nothing. The clusters are off while
	 * the loader runs; XNU wakes them, and the kernel's own clock driver sets them.
	 */
	out("big cores: the clock is the kernel's here; APMIXED ARMPLL_CON1 ");
	out_hex(con);
	out(" (");
	out_dec((uint32_t)(((26ull * (con & 0x3fffffu)) >> 14) >> ((con >> 24) & 7u)));
	out(" MHz as left, unchanged)\n");
}

static void __attribute__((noreturn)) run_xnu(void)
{
	struct payload_info pi;
	struct xnu_syms s;

	if (!parse_payload(&pi)) {
		out("ERROR: no XNU payload in this image\n");
		dbg_delay_ms(5000);
		chain_android();
	}
	out("XNU kernel ");
	out_dec(pi.kernel_size);
	out(" bytes, ramdisk ");
	out_dec(pi.ramdisk_size);
	out(" bytes\n");

	const struct mach_header_64 *mh = (const struct mach_header_64 *)pi.kernel;
	uint64_t vm_hi = 0, entry_va = 0;
	const struct load_command *lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *sg = (const void *)lc;
			if (sg->vmsize == 0) {
				continue;
			}
			if (sg->vmaddr + sg->vmsize > vm_hi) {
				vm_hi = sg->vmaddr + sg->vmsize;
			}
			uint64_t dst_pa = kva_to_pa(sg->vmaddr);
			memset((void *)dst_pa, 0, sg->vmsize);
			memcpy((void *)dst_pa, pi.kernel + sg->fileoff, sg->filesize);
		} else if (lc->cmd == LC_UNIXTHREAD) {
			entry_va = ((const uint64_t *)((const uint8_t *)lc + 16))[32];
		}
	}
	out("kernel at ");
	out_hex(kva_to_pa(0xfffffff007004000ULL));
	out(", entry ");
	out_hex(kva_to_pa(entry_va));
	out("\n");

	find_syms(pi.kernel, &s);
	xsyms = s;
	xkernel = pi.kernel;

	/* XNU's console colors (32 bpp column) opaque. */
	if (s.vc_colors) {
		volatile uint32_t *c = (volatile uint32_t *)kva_to_pa(s.vc_colors);
		for (int i = 0; i < 8; i++) {
			c[i * 4 + 2] |= 0xff000000u;
		}
		out("vc_colors: alpha set\n");
	}

	/*
	 * This kernel's serial console is a PL011 driver (vmapple_uart): its transmit_ready waits
	 * for a flag the MediaTek UART does not have, so "serial=3" hangs there forever. Make
	 * transmit_ready return 1 and transmit_data do nothing; kprintf then reaches loader_log
	 * (and mt6577_uart_putc, which the kernel calls as well and which drives this UART right).
	 */
	if (s.uart_tx_ready && s.uart_tx_data) {
		volatile uint32_t *r = (volatile uint32_t *)kva_to_pa(s.uart_tx_ready);
		volatile uint32_t *d = (volatile uint32_t *)kva_to_pa(s.uart_tx_data);
		r[0] = 0x52800020u; /* mov w0, #1 */
		r[1] = 0xd65f03c0u; /* ret        */
		d[0] = 0xd65f03c0u; /* ret        */
		/*
		 * ... and the byte itself goes to loader_log_putc, which keeps it in the kernel's log
		 * buffer: that is how anything the first programs print reaches us at all. (This
		 * build of loader_log_putc calls nothing, so there is no way back round.)
		 */
		if (s.log_putc) {
			int64_t rel = (int64_t)(s.log_putc - s.uart_tx_data) >> 2;

			if (rel >= -(1 << 25) && rel < (1 << 25)) {
				d[0] = 0x14000000u | ((uint32_t)rel & 0x03ffffffu);
			}
		}
		/* and no input: the kernel's serial keyboard would otherwise read this UART's
		 * receive register, which here is another register entirely, for ever. */
		if (s.uart_rx_ready) {
			volatile uint32_t *rx = (volatile uint32_t *)kva_to_pa(s.uart_rx_ready);
			rx[0] = 0x52800000u; /* mov w0, #0 */
			rx[1] = 0xd65f03c0u; /* ret        */
		}
		out("vmapple_uart: transmit_ready patched, transmit_data -> loader_log_putc, receive off\n");
	}

	/*
	 * Everything written to /dev/console -- the kernel's own text and whatever the first
	 * programs print -- goes through uart_putc, and on this phone that is a PL011 driver
	 * writing into nothing. Send it to loader_log_putc instead (same argument, a character in
	 * w0), which keeps it in the kernel's log buffer and puts it out of the MediaTek UART as
	 * well. Without this the only voice userland has is lost.
	 */
	if (s.log_putc) {
		/* uart_putc is kprintf's way out, uart_putc_options the console's (and so userland's) */
		const uint64_t from[2] = { 0, 0 };

		for (int i = 0; i < 2; i++) {
			if (!from[i]) {
				continue;
			}
			volatile uint32_t *at = (volatile uint32_t *)kva_to_pa(from[i]);
			int64_t rel = (int64_t)(s.log_putc - from[i]) >> 2;

			if (rel >= -(1 << 25) && rel < (1 << 25)) {
				at[0] = 0x14000000u | ((uint32_t)rel & 0x03ffffffu); /* b loader_log_putc */
			}
		}
		out("uart_putc and uart_putc_options -> loader_log_putc\n");
	}

	/*
	 * EOImode is per processor, and gic_fix_eoimode() below only reaches the one this code runs
	 * on. Every other core comes out of PSCI CPU_ON with the bit set again, takes one timer
	 * interrupt and goes deaf: with four cores iOS crawled on the one that worked, and the
	 * kernel's own heartbeat beat 3 times in 840 seconds.
	 *
	 * Each core calls gic_v3_irq_cpu_init() for itself, and in it writes zero to ICC_BPR1_EL1.
	 * That instruction becomes a write of zero to ICC_CTLR_EL1, which clears EOImode (and CBPR,
	 * which wants to be zero anyway). What is given up is BPR1 = 0, the preemption grouping,
	 * which keeps its reset value -- a small thing beside a core whose timer never fires again.
	 */
	if (s.gic_cpu_init) {
		volatile uint32_t *at = (volatile uint32_t *)kva_to_pa(s.gic_cpu_init);
		int done = 0;

		for (int i = 0; i < 256 && !done; i++) {
			if (at[i] == 0xd518cc7fu) {          /* msr ICC_BPR1_EL1, xzr */
				at[i] = 0xd518cc9fu;         /* msr ICC_CTLR_EL1, xzr */
				done = 1;
			}
		}
		out(done ? "gic_v3_irq_cpu_init: every core clears EOImode\n" :
		    "gic_v3_irq_cpu_init: the BPR1 write is not where it was -- EOImode stays set\n");
	}

	/* Have XNU come back here on a panic or an early exception. */
	if (s.stub_slot) {
		volatile uint64_t *slot = (volatile uint64_t *)kva_to_pa(s.stub_slot);
		if (slot[0] == 0x545352444c554e58ULL) { /* "XNULDRST" */
			slot[1] = (uint64_t)xnu_return_stub;
			slot[2] = XNU_CHECKPOINT;
			slot[3] = XNU_SLIDE;
			out("return stub at ");
			out_hex(slot[1]);
			out("\n");
		}
	}

	uint64_t kern_hi = (kva_to_pa(vm_hi) + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	uint64_t rd_pa = kern_hi;
	uint64_t rd_size = (pi.ramdisk_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	if (rd_size) {
		memset((void *)rd_pa, 0, rd_size);
		memcpy((void *)rd_pa, pi.ramdisk, pi.ramdisk_size);
	}
	/* and the big one off the flash, if it is there (with XNU_ROOT_STORAGE the kernel reads it itself) */
	uint64_t sys_size = XNU_ROOT_STORAGE ? 0 : load_system_image();
	uint64_t sys_pa = sys_size ? SYSTEM_PA : 0;
	uint64_t sys_span = (sys_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);

	uint64_t dt_pa = sys_size ? sys_pa + sys_span : rd_pa + rd_size;
	uint32_t dt_len = build_device_tree(dt_pa, rd_pa, rd_size, sys_pa, sys_span);
	/* what the window server will believe the screen is: 375 x 812 points at three pixels a point */
	out("display: 375 x 812 points at 3 pixels a point (display-scale 3)\n");
	uint64_t ba_pa = (dt_pa + dt_len + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	uint64_t top = ba_pa + PAGE_SIZE_MAX;

	struct boot_args *ba = (struct boot_args *)ba_pa;
	memset(ba, 0, sizeof(*ba));
	ba->revision = 2;
	ba->version = 2;
	ba->physBase = XNU_PHYS_BASE;
	ba->virtBase = KERNEL_VIRT_BASE;
	ba->memSize = XNU_MEM_SIZE;
	ba->memSizeActual = XNU_MEM_SIZE;
	ba->topOfKernelData = top;
	ba->deviceTreeP = dt_pa - XNU_SLIDE;
	ba->deviceTreeLength = dt_len;
	ba->video.v_baseAddr = fb_base;
	ba->video.v_display = XNU_VERBOSE ? 0 : 1; /* 0: the kernel's text console, 1: for iOS to draw */
	ba->video.v_rowBytes = fb_pitch;
	ba->video.v_width = PANEL_W;
	ba->video.v_height = PANEL_H;
	ba->video.v_depth = 32;

	/*
	 * The battery state, if the request carried a reading: published before the command line
	 * is assembled, because the kernel is told about the page only when there is one.
	 */
	battery_publish();

	/* Assemble command line, merging runtime dynamic overrides if present */
	{
		uint32_t active_cpus = (runtime_cpus >= 1 && runtime_cpus <= 7) ? runtime_cpus : 0;
		uint32_t d = 0;
		int seen_cpus = 0;
		int seen_secs = 0;
		const char *p = XNU_BOOT_ARGS;

		while (*p) {
			while (*p == ' ') p++;
			if (!*p) break;
			const char *wstart = p;
			while (*p && *p != ' ') p++;
			uint32_t wlen = (uint32_t)(p - wstart);

			if (wlen >= 5 && wstart[0] == 'c' && wstart[1] == 'p' && wstart[2] == 'u' &&
			    wstart[3] == 's' && wstart[4] == '=') {
				seen_cpus = 1;
				if (d && d + 1 < sizeof(runtime_cmdline)) runtime_cmdline[d++] = ' ';
				if (active_cpus > 0) {
					const char *prefix = "cpus=";
					while (*prefix && d + 1 < sizeof(runtime_cmdline)) runtime_cmdline[d++] = *prefix++;
					char num[12];
					uint32_t val = active_cpus;
					int pos = 0;
					if (val == 0) num[pos++] = '0';
					else {
						char tmp[12]; int tp = 0;
						while (val) { tmp[tp++] = (char)('0' + (val % 10)); val /= 10; }
						while (tp > 0) num[pos++] = tmp[--tp];
					}
					for (int i = 0; i < pos && d + 1 < sizeof(runtime_cmdline); i++) runtime_cmdline[d++] = num[i];
				} else {
					for (uint32_t i = 0; i < wlen && d + 1 < sizeof(runtime_cmdline); i++) runtime_cmdline[d++] = wstart[i];
				}
			} else if (wlen >= 20 && memcmp_n(wstart, "loader_return_after=", 20) == 0) {
				seen_secs = 1;
				if (d && d + 1 < sizeof(runtime_cmdline)) runtime_cmdline[d++] = ' ';
				if (runtime_secs > 0) {
					const char *prefix = "loader_return_after=";
					while (*prefix && d + 1 < sizeof(runtime_cmdline)) runtime_cmdline[d++] = *prefix++;
					char num[12];
					uint32_t val = runtime_secs;
					int pos = 0;
					if (val == 0) num[pos++] = '0';
					else {
						char tmp[12]; int tp = 0;
						while (val) { tmp[tp++] = (char)('0' + (val % 10)); val /= 10; }
						while (tp > 0) num[pos++] = tmp[--tp];
					}
					for (int i = 0; i < pos && d + 1 < sizeof(runtime_cmdline); i++) runtime_cmdline[d++] = num[i];
				} else {
					for (uint32_t i = 0; i < wlen && d + 1 < sizeof(runtime_cmdline); i++) runtime_cmdline[d++] = wstart[i];
				}
			} else {
				if (d && d + 1 < sizeof(runtime_cmdline)) runtime_cmdline[d++] = ' ';
				for (uint32_t i = 0; i < wlen && d + 1 < sizeof(runtime_cmdline); i++) runtime_cmdline[d++] = wstart[i];
			}
		}

		if (active_cpus > 0 && !seen_cpus) {
			if (d && d + 1 < sizeof(runtime_cmdline)) runtime_cmdline[d++] = ' ';
			const char *prefix = "cpus=";
			while (*prefix && d + 1 < sizeof(runtime_cmdline)) runtime_cmdline[d++] = *prefix++;
			char num[12];
			uint32_t val = active_cpus;
			int pos = 0;
			if (val == 0) num[pos++] = '0';
			else {
				char tmp[12]; int tp = 0;
				while (val) { tmp[tp++] = (char)('0' + (val % 10)); val /= 10; }
				while (tp > 0) num[pos++] = tmp[--tp];
			}
			for (int i = 0; i < pos && d + 1 < sizeof(runtime_cmdline); i++) runtime_cmdline[d++] = num[i];
		}

		if (runtime_secs > 0 && !seen_secs) {
			if (d && d + 1 < sizeof(runtime_cmdline)) runtime_cmdline[d++] = ' ';
			const char *prefix = "loader_return_after=";
			while (*prefix && d + 1 < sizeof(runtime_cmdline)) runtime_cmdline[d++] = *prefix++;
			char num[12];
			uint32_t val = runtime_secs;
			int pos = 0;
			if (val == 0) num[pos++] = '0';
			else {
				char tmp[12]; int tp = 0;
				while (val) { tmp[tp++] = (char)('0' + (val % 10)); val /= 10; }
				while (tp > 0) num[pos++] = tmp[--tp];
			}
			for (int i = 0; i < pos && d + 1 < sizeof(runtime_cmdline); i++) runtime_cmdline[d++] = num[i];
		}

		if (runtime_extra_args[0]) {
			if (d && d + 1 < sizeof(runtime_cmdline)) runtime_cmdline[d++] = ' ';
			const char *ex = runtime_extra_args;
			while (*ex && d + 1 < sizeof(runtime_cmdline)) runtime_cmdline[d++] = *ex++;
		}
		/*
		 * The battery page: the kernel's battery driver polls it every five seconds and reads
		 * it at the address it is told here (drivers/power/hakios.battery.c). It is only told
		 * about a page that holds a reading: a page without one would be read as a cache the
		 * loader never wrote.
		 */
		if (battery_published) {
			if (d && d + 1 < sizeof(runtime_cmdline)) runtime_cmdline[d++] = ' ';
			const char *pre = "battery=1 battery_shared_pa=0x";
			while (*pre && d + 1 < sizeof(runtime_cmdline)) runtime_cmdline[d++] = *pre++;
			cmdline_hex(&d, BATTERY_PAGE_PA);
		}
		runtime_cmdline[d] = '\0';
	}
	memcpy(ba->commandLine, runtime_cmdline, sizeof(ba->commandLine));

	out("md0 ");
	out_hex(sys_size ? sys_pa : rd_pa);
	out(sys_size ? " (the system image), md1 " : XNU_ROOT_STORAGE ? " is none: md1 (root.packfs), the system image on the UFS" :
	    " (root.packfs only)");
	if (sys_size) {
		out_hex(rd_pa);
	}
	out("\nboot-args: ");
	out(runtime_cmdline);
	out("\nframebuffer ");
	out_hex(fb_base);
	out(" pitch ");
	out_dec(fb_pitch);
	out(", XNU memory ");
	out_hex(XNU_PHYS_BASE);
	out("+");
	out_hex(XNU_MEM_SIZE);
	out("\n");
	display_alpha_off();

	marker_set(MARK_RUNNING);
	if (XNU_WDT_SECONDS) {
		uint32_t t = XNU_WDT_SECONDS > 31 ? 31 : XNU_WDT_SECONDS;
		wr32(TOPRGU_LENGTH, ((t * 32768u / 512u) << 5) | 0x8u);
		wr32(TOPRGU_RESTART, TOPRGU_RESTART_KEY);
		wr32(TOPRGU_MODE, TOPRGU_MODE_KEY | 0x1u);
		out("watchdog: back to Android in ");
		out_dec(t);
		out(" s\n");
	}
	/* The timers XNU will use: a virtual counter offset by EL2 (MediaTek's GenieZone) would
	 * leave its deadlines forever in the future, and the kernel idle with nothing to wake it. */
	{
		uint64_t pct, vct, frq, kctl, pfr0;
		__asm__ volatile("isb; mrs %0, cntpct_el0; mrs %1, cntvct_el0; mrs %2, cntfrq_el0;"
		    "mrs %3, cntkctl_el1; mrs %4, id_aa64pfr0_el1"
		    : "=r"(pct), "=r"(vct), "=r"(frq), "=r"(kctl), "=r"(pfr0));
		out("timers: CNTPCT ");
		out_hex(pct);
		out(" CNTVCT ");
		out_hex(vct);
		out(" delta ");
		out_hex(pct - vct);
		out(" CNTFRQ ");
		out_dec(frq);
		out(" CNTKCTL ");
		out_hex(kctl);
		out(" EL2 ");
		out_dec((pfr0 >> 8) & 0xf);
		out("\n");
	}

	{
		uint32_t mhz = cpu_mhz();

		out("this core is running at ");
		if (mhz) {
			out_dec(mhz);
			out(" MHz\n");
		} else {
			out("a speed its cycle counter will not say\n");
		}
	}

	/* must happen on the core the kernel runs on, and before it sets its GIC up */
	gic_fix_eoimode();
#if TIMER_PROBE
	timer_probe();
#endif
	watcher_start();
	/*
	 * The touch screen, while there is still a console to complain to. The controller has to be
	 * handed its firmware before anything can read a finger out of it, and the kernel is a bad
	 * place to do it from: it would have to carry those hundred kilobytes itself, and it comes
	 * up long after the chip has forgotten them.
	 */
	touch_bring_up();
	big_cores_clock();
	/*
	 * Nothing of ours is left on the panel: iOS paints only what it draws, and where it does not
	 * reach, what LK wrote there stays -- its "Orange State: your device has been unlocked"
	 * showed through the home screen.
	 */
	if (fb_base) {
		fb_rect(0, 0, PANEL_W, PANEL_H, 0xff000000u);
		for (uint64_t at = fb_base; at < fb_base + (uint64_t)PANEL_H * fb_pitch; at += 64) {
			__asm__ volatile("dc cvac, %0" : : "r"(at));
		}
		__asm__ volatile("dsb sy");
	}
	out("Jumping to XNU...\n");
	plog_flush();
	jump_to_kernel(kva_to_pa(entry_va), ba_pa);
	__builtin_unreachable();
}

#if MSDC_TEST
/*
 * Can we read the phone's eMMC with what LK left set up? Checked against bytes we know: the GPT
 * header at block 1 ("EFI PART") and the start of boot_a, which holds the image this very
 * program came from ("ANDROID!"). Reads only.
 */
static void msdc_dump(const char *name, uint64_t lba, const uint8_t *b, int ok)
{
	out(name);
	out(" lba ");
	out_hex(lba);
	if (!ok) {
		out(": FAILED at ");
		out(msdc_last.what);
		out(", status ");
		out_hex(msdc_last.status);
		out("\n");
		return;
	}
	out(": ");
	for (int i = 0; i < 16; i++) {
		out_hex(b[i]);
		out_putc(' ');
	}
	out(" |");
	for (int i = 0; i < 24; i++) {
		out_putc(b[i] >= 0x20 && b[i] < 0x7f ? (char)b[i] : '.');
	}
	out("|\n");
}

/*
 * The SD card slot (MSDC1). LK never touches it, so its clock may be gated and the card
 * unpowered (vmmc = MT6358 VMCH, vqmmc = ldo_vmc, both over PWRAP). This only looks: it reads
 * MSDC1's registers and tries the first commands of the SD sequence. If the card answers, the
 * rails and clocks are already on and no PMIC work is needed; if nothing answers, they are not.
 */
static void msdc1_probe(void)
{
	uint32_t resp;
	int ok;

	msdc_base = MSDC1_BASE;
	out("\nSD slot (MSDC1) as LK leaves it: CFG ");
	out_hex(rd32(MSDC1_BASE + MSDC_CFG));
	out(" PS ");
	out_hex(rd32(MSDC1_BASE + MSDC_PS));
	out(" SDC_CFG ");
	out_hex(rd32(MSDC1_BASE + SDC_CFG));
	out(" STS ");
	out_hex(rd32(MSDC1_BASE + SDC_STS));
	out(" INT ");
	out_hex(rd32(MSDC1_BASE + MSDC_INT));
	out("\n");
	if (rd32(MSDC1_BASE + MSDC_CFG) == 0 && rd32(MSDC1_BASE + MSDC_PS) == 0) {
		out("  all zero: the controller looks unclocked\n");
	}

	/* CMD0 (go idle, no answer), then CMD8 (which voltages do you take, R7) */
	wr32(MSDC1_BASE + MSDC_INT, rd32(MSDC1_BASE + MSDC_INT));
	msdc_int_seen = 0;
	wr32(MSDC1_BASE + SDC_ARG, 0);
	wr32(MSDC1_BASE + SDC_CMD, CMD_OPC(0));
	msdc_wait_int(INT_CMDRDY, INT_CMDTMO | INT_RSPCRCERR, "CMD0");
	dbg_delay_ms(10);

	wr32(MSDC1_BASE + SDC_ARG, 0x1aa);
	wr32(MSDC1_BASE + SDC_CMD, CMD_OPC(8) | CMD_RSP_R1);
	ok = msdc_wait_int(INT_CMDRDY, INT_CMDTMO | INT_RSPCRCERR, "CMD8");
	resp = rd32(MSDC1_BASE + SDC_RESP0);
	out("  CMD8 (interface condition): ");
	if (ok) {
		out("answered ");
		out_hex(resp);
		out(resp == 0x1aa ? " -- an SD card is powered and listening" : "");
	} else {
		out("no answer (");
		out(msdc_last.what);
		out(", status ");
		out_hex(msdc_last.status);
		out(") -- the slot needs its clock and power set up");
	}
	out("\n");
	msdc_base = MSDC0_BASE;
}

static void msdc_self_test(void)
{
	static uint8_t buf[4096] __attribute__((aligned(16)));
	const uint64_t boot_a_lba = 0xf5000; /* from LK's partition table */
	int ok;

	/* a hang in here must not keep the phone: the watchdog brings Android back */
	wr32(TOPRGU_LENGTH, ((31u * 32768u / 512u) << 5) | 0x8u);
	wr32(TOPRGU_RESTART, TOPRGU_RESTART_KEY);
	wr32(TOPRGU_MODE, TOPRGU_MODE_KEY | 0x1u);

	out("eMMC (MSDC0) as LK left it: CFG ");
	out_hex(rd32(MSDC0_BASE + MSDC_CFG));
	out(" PS ");
	out_hex(rd32(MSDC0_BASE + MSDC_PS));
	out(" SDC_CFG ");
	out_hex(rd32(MSDC0_BASE + SDC_CFG));
	out(" STS ");
	out_hex(rd32(MSDC0_BASE + SDC_STS));
	out(" INT ");
	out_hex(rd32(MSDC0_BASE + MSDC_INT));
	out("\n");

	/* the card's state, before touching the data lines at all */
	if (msdc_wait_idle()) {
		wr32(MSDC0_BASE + MSDC_INT, rd32(MSDC0_BASE + MSDC_INT));
		wr32(MSDC0_BASE + SDC_ARG, 1u << 16); /* RCA 1, as LK sets it */
		wr32(MSDC0_BASE + SDC_CMD, CMD_OPC(MMC_SEND_STATUS) | CMD_RSP_R1);
		ok = msdc_wait_int(INT_CMDRDY, INT_CMDTMO | INT_RSPCRCERR, "status");
		out("CMD13 (card status): ");
		if (ok) {
			out_hex(rd32(MSDC0_BASE + SDC_RESP0));
		} else {
			out("failed at ");
			out(msdc_last.what);
			out(" status ");
			out_hex(msdc_last.status);
		}
		out("\n");
	}

	memset(buf, 0, sizeof(buf));
	ok = msdc_read(1, 1, buf);
	msdc_dump("GPT header ", 1, buf, ok);

	memset(buf, 0, sizeof(buf));
	ok = msdc_read(boot_a_lba, 1, buf);
	msdc_dump("boot_a     ", boot_a_lba, buf, ok);

	memset(buf, 0, sizeof(buf));
	ok = msdc_read(boot_a_lba, 8, buf);
	out("boot_a 8 blocks: ");
	if (ok) {
		uint32_t ksize = *(volatile uint32_t *)(buf + 8);
		out(buf[0] == 'A' ? "ANDROID! kernel_size " : "NOT a boot image, ");
		out_hex(ksize);
		out(", block 7 starts ");
		for (int i = 0; i < 8; i++) {
			out_hex(buf[7 * 512 + i]);
			out_putc(' ');
		}
	} else {
		out("FAILED at ");
		out(msdc_last.what);
		out(" status ");
		out_hex(msdc_last.status);
	}
	out("\n");

	/* How fast, how far, and is what comes back right: a checksum of 4 MB of boot_a, which
	 * is the image this program was flashed in, so the host can check it (tools/msdc-test.sh). */
	for (uint32_t blocks = 256; blocks <= 8192; blocks *= 4) {
		const uint64_t at = 0xa0000000ULL;
		uint64_t total = 4u * 1024 * 1024, t0, t1, f = dbg_freq();
		uint32_t rounds = (uint32_t)(total / (blocks * 512));
		uint32_t sum = 0;
		int fine = 1;

		t0 = dbg_ticks();
		for (uint32_t r = 0; r < rounds && fine; r++) {
			fine = msdc_read(boot_a_lba + (uint64_t)r * blocks, blocks, (void *)at);
			for (uint32_t i = 0; fine && i < blocks * 512 / 4; i++) {
				sum = sum * 31u + *(volatile uint32_t *)(at + 4 * i);
			}
		}
		t1 = dbg_ticks();
		out("read 4 MB in ");
		out_dec(blocks);
		out("-block chunks: ");
		if (!fine) {
			out("FAILED at ");
			out(msdc_last.what);
			out(" status ");
			out_hex(msdc_last.status);
		} else {
			out_dec((t1 - t0) * 1000 / f);
			out(" ms, ");
			out_dec(total * f / ((t1 - t0) ? (t1 - t0) : 1) / 1024 / 1024);
			out(" MB/s (the checksum takes part of that), sum ");
			out_hex(sum);
		}
		out("\n");
	}
	msdc1_probe();
}
#endif /* MSDC_TEST */


#if TOUCH_PROBE
/*
 * Room for the firmware and the fourteen header bytes in front of it: plain DRAM that nothing
 * else keeps. The loader's image ends before 0x400c0000, XNU's log ring sits at 0x47b1c9c0 and
 * ramoops at 0x4d0e0000, and DRAM reaches well past this. Growing the loader instead would walk
 * it into the 256 KB chainboot gives it.
 */

/* What an identify report says about itself: the mode it is in, which part it is, which build. */
static void touch_say_identify(const uint8_t *buf, uint32_t got)
{
	if (got < 2) {
		out("  too short to say anything\n");
		return;
	}
	out("  mode ");
	out_hex(buf[1]);
	out(" (");
	out(buf[1] == TCM_MODE_ROMBOOT ? "the ROM bootloader, no firmware" :
	    buf[1] == TCM_MODE_HOSTDOWNLOAD ? "running what it was given" :
	    buf[1] == TCM_MODE_APPLICATION ? "running its application" : "something else");
	out("), part ");
	for (uint32_t i = 2; i < got && i < 18; i++) {
		out_putc(buf[i] >= 0x20 && buf[i] < 0x7f ? (char)buf[i] : ' ');
	}
	if (got >= 22) {
		out(", build ");
		out_hex((uint32_t)buf[18] | ((uint32_t)buf[19] << 8) |
		    ((uint32_t)buf[20] << 16) | ((uint32_t)buf[21] << 24));
	}
	out("\n");
}

/*
 * Wait for the chip to speak, and only then listen.
 *
 * After the go command the chip restarts into the firmware and introduces itself unasked, so
 * there is nothing to ask for and nothing to gain by clocking at it meanwhile -- reading a chip
 * that is not ready only puts its SPI side out of step. So this watches the interrupt line and
 * reads one message each time it goes low, printing whatever comes, until an identify report
 * arrives or the patience runs out. Returns the mode it found, or 0.
 */
static uint8_t touch_wait_identify(uint32_t patience_ms)
{
	static uint8_t buf[320];
	uint8_t code = 0;
	uint32_t got = 0, waited = 0;
	int heard = 0;

	while (waited < patience_ms) {
		if (!tcm_irq()) {
			dbg_delay_ms(10);
			waited += 10;
			continue;
		}
		heard = 1;
		out("  after ");
		out_hex(waited);
		out(" ms: ");
		if (tcm_read(&code, buf, sizeof(buf), &got) != 1) {
			out("the interrupt was up but the message was not readable (");
			out_hex(code);
			out(")\n");
			dbg_delay_ms(20);
			waited += 20;
			continue;
		}
		out("code ");
		out_hex(code);
		out(", ");
		out_hex(got);
		out(" bytes\n");
		if (code == TCM_REPORT_IDENTIFY) {
			touch_say_identify(buf, got);
			return got >= 2 ? buf[1] : 0;
		}
	}
	out(heard ? "  nothing more\n" : "  the interrupt line never went low\n");
	return 0;
}

/*
 * Bits out of a touch report, least significant first within each byte and on across the bytes,
 * which is how the chip packs them (touch_get_report_data in the driver).
 */
static uint32_t touch_bits(const uint8_t *buf, uint32_t at, uint32_t bits)
{
	uint32_t v = 0;

	for (uint32_t i = 0; i < bits; i++) {
		uint32_t bit = at + i;

		v |= (uint32_t)((buf[bit / 8] >> (bit % 8)) & 1u) << i;
	}
	return v;
}

/*
 * What a touch report holds, from the layout the chip itself describes when asked with 0x25:
 *
 *   0x0f 0x01   the capacitive buttons, one bit
 *   0x04        pad to the next byte, so the first byte is theirs alone
 *   0x01        and then, for each finger it can see:
 *   0x06 0x04     which finger      4 bits
 *   0x07 0x04     what it is        4 bits (0 lifted, 1 a finger, 2 a gloved one)
 *   0x08 0x0c     x                12 bits
 *   0x09 0x0c     y                12 bits
 *   0x0a 0x08     pressure          8 bits
 *   0x0b 0x08     width across      8 bits
 *   0x0c 0x08     width down        8 bits
 *   0x0d 0x10     column            16 bits
 *   0x0e 0x10     row               16 bits
 *   0x03 0x00   end of the list, end of the layout
 *
 * Eighty-eight bits a finger, and the report is as long as it has fingers to talk about.
 */
#define TOUCH_FINGER_BITS   88

static int touch_say_report(const uint8_t *buf, uint32_t got)
{
	uint32_t at = 8;                                /* the button byte comes first */
	int fingers = 0;

	while (at + TOUCH_FINGER_BITS <= got * 8) {
		uint32_t which = touch_bits(buf, at, 4);
		uint32_t kind = touch_bits(buf, at + 4, 4);
		uint32_t x = touch_bits(buf, at + 8, 12);
		uint32_t y = touch_bits(buf, at + 20, 12);
		uint32_t pressure = touch_bits(buf, at + 32, 8);

		at += TOUCH_FINGER_BITS;
		if (kind == 0) {
			continue;                       /* that finger has lifted */
		}
		out("  finger ");
		out_hex(which);
		out(" at ");
		out_hex(x);
		out(",");
		out_hex(y);
		out(" pressure ");
		out_hex(pressure);
		out("\n");
		fingers++;
	}
	return fingers;
}

/*
 * Is the touch screen reachable from here, and can it be made to run? The controller keeps its
 * firmware in RAM and loses it at every reset, so Android sends it afresh at every boot and so
 * must we. This walks that same sequence, printing what the chip says at each step: reset into
 * the ROM bootloader, the image as one message, the pause, the go command, and then listen until
 * it introduces itself with the mode it reached.
 */
static void touch_probe(void)
{
	static uint8_t buf[320];
	uint8_t code = 0, mode;
	uint32_t got = 0;
	int r;

	/*
	 * The clock gates of the infra bus (Linux clk-mt6768.c): a bit set in a status register
	 * means that clock is held off. SPI2's is one of them, so open all four registers' worth --
	 * opening a gate only lets a clock run, it tells nothing to do anything.
	 */
	static const uint32_t gate_clr[4] = { 0x8c, 0xa8, 0xc4, 0x84 };

	for (int i = 0; i < 4; i++) {
		wr32(INFRACFG_BASE + gate_clr[i], 0xffffffffu);
	}
	__asm__ volatile("dsb sy");
	touch_clk_cfg2 = rd32(TOPCKGEN_BASE + CLK_CFG_2);
	touch_clk_cfg2_seen = 1;
	spi_pins_to_spi2();
	spi_setup();
	out("\nSPI2: pins ");
	out_hex(rd32(GPIO_BASE + GPIO_MODE3));
	out(", clock mux ");
	out_hex(rd32(TOPCKGEN_BASE + CLK_CFG_2));
	out(", interrupt ");
	out(tcm_irq() ? "asserted\n" : "idle\n");

	/*
	 * A reset first, so the chip is where the sequence expects it however the reboot left it,
	 * and it answers a reset by introducing itself.
	 */
	tcm_reset();
	r = tcm_read_patiently(&code, buf, sizeof(buf), &got);
	if (r != 1) {
		out("nothing after a reset (");
		out_hex(code);
		out(")\n");
		return;
	}
	out("after a reset: code ");
	out_hex(code);
	out("\n");
	touch_say_identify(buf, got);
	if (got < 2 || buf[1] != TCM_MODE_ROMBOOT) {
		out("not the ROM bootloader, so there is nothing to send\n");
		return;
	}

	/*
	 * The message, laid out whole in DRAM so it can go out in one breath: the command, its two
	 * length bytes, the fourteen of header, and the image. Read it back afterwards, because an
	 * address that is not really ours would turn the download into noise.
	 */
	uint8_t *message = (uint8_t *)TOUCH_STAGE;
	uint32_t total = TCM_MESSAGE_HEAD + TCM_ROMBOOT_HEADER + TOUCH_FW_SIZE, bad = 0;

	memcpy(message + TCM_MESSAGE_HEAD + TCM_ROMBOOT_HEADER, touch_fw, TOUCH_FW_SIZE);
	for (uint32_t i = 0; i < TOUCH_FW_SIZE; i++) {
		if (message[TCM_MESSAGE_HEAD + TCM_ROMBOOT_HEADER + i] != touch_fw[i]) {
			bad++;
		}
	}
	if (bad) {
		out("the staged image came back wrong in ");
		out_hex(bad);
		out(" bytes, so that DRAM is not ours\n");
		return;
	}

	out("sending ");
	out_hex(total);
	out(" bytes in one transfer\n");
	tcm_load_firmware(message);
	r = tcm_read_patiently(&code, buf, sizeof(buf), &got);
	out("  answered ");
	if (r == 1) {
		out_hex(code);
		out(code == TCM_STATUS_OK ? " (taken)" : " (not a plain yes)");
		out(", ");
		out_hex(got);
		out(" bytes:");
		for (uint32_t k = 0; k < got && k < 8; k++) {
			out_putc(' ');
			out_hex(buf[k]);
		}
		if (code == TCM_STATUS_OK && got >= 2) {
			out(", blocks written ");
			out_hex((uint32_t)buf[0] | ((uint32_t)buf[1] << 8));
			out(" of ");
			out_hex(TOUCH_FW_SIZE / 16);
		}
	} else {
		out("nothing");
	}
	out("\n");

	out("go: ");
	tcm_run_firmware();
	mode = touch_wait_identify(3000);
	if (mode == TCM_MODE_APPLICATION || mode == TCM_MODE_HOSTDOWNLOAD) {
		out("the firmware is up\n");
	} else {
		out("it did not come up\n");
		return;
	}

	/* What it can do, and how it will lay out a touch report. */
	static const uint8_t asks[2] = { TCM_CMD_GET_APP_INFO, TCM_CMD_GET_REPORT_CONF };

	for (int i = 0; i < 2; i++) {
		if (!tcm_write(asks[i], 0, 0, 0, 0)) {
			continue;
		}
		/*
		 * The chip pushes reports of its own between answers -- a status report, a touch --
		 * and those are not what was asked for, so read past them.
		 */
		for (int skip = 0; skip < 4; skip++) {
			r = tcm_read_patiently(&code, buf, sizeof(buf), &got);
			if (r != 1 || code < 0x10) {
				break;
			}
			out("  (a report on the way: code ");
			out_hex(code);
			out(")\n");
		}
		out("answer to ");
		out_hex(asks[i]);
		out(": code ");
		out_hex(code);
		out(", ");
		out_hex(got);
		out(" bytes:");
		for (uint32_t k = 0; k < got && k < 48; k++) {
			out_putc(' ');
			out_hex(buf[k]);
		}
		out("\n");
	}

	/*
	 * Ask for touch reports. The phone's driver leaves this to the chip's own configuration and
	 * the log shows it never sending the command, but asking costs one message and a chip that
	 * was already reporting does not mind being told again.
	 */
	{
		uint8_t which = TCM_REPORT_TOUCH;

		out("asking for touch reports: ");
		if (tcm_write(TCM_CMD_ENABLE_REPORT, &which, 1, 0, 0) &&
		    tcm_read_patiently(&code, buf, sizeof(buf), &got) == 1) {
			out("code ");
			out_hex(code);
		} else {
			out("no answer");
		}
		out("\n");
	}

	/*
	 * And now the thing itself: a finger on the glass, and whether the chip says so.
	 */
	out("\n>>> TOUCH THE SCREEN NOW -- forty-five seconds <<<\n\n");

	uint32_t fingers = 0, reports = 0;

	for (uint32_t waited = 0; waited < 45000; waited += 5) {
		if (!tcm_irq()) {
			dbg_delay_ms(5);
			continue;
		}
		if (tcm_read(&code, buf, sizeof(buf), &got) != 1) {
			dbg_delay_ms(5);
			waited += 5;
			continue;
		}
		if (code != TCM_REPORT_TOUCH) {
			out("  (code ");
			out_hex(code);
			out(")\n");
			continue;
		}
		reports++;
		fingers += (uint32_t)touch_say_report(buf, got);
	}
	out("\nheard ");
	out_hex(reports);
	out(" reports, ");
	out_hex(fingers);
	out(" of them with a finger on the glass\n");
}
/*
 * Android's driver expects this chip where a power-on leaves it: in its ROM bootloader, with the
 * bus as the preloader set it. Finding anything else it gives up on the touch screen for that
 * whole boot -- "Failed to detect the sensor" -- and the phone comes up unable to be touched. So
 * a probe that has started firmware in the chip, or borrowed its pins, puts both back before
 * Android runs.
 */
static void touch_hand_back(void)
{
	if (touch_clk_cfg2_seen) {
		uint32_t mask = SPI_SEL_FIELD | SPI_SEL_PDN;

		wr32(TOPCKGEN_BASE + CLK_CFG_2_SET, touch_clk_cfg2 & mask);
		wr32(TOPCKGEN_BASE + CLK_CFG_2_CLR, ~touch_clk_cfg2 & mask);
	}
	spi_pins_to_spi2();
	__asm__ volatile("dsb sy");
	tcm_reset();
}
#endif /* TOUCH_PROBE */

/* ---- choosing what to boot ---- */

/*
 * Android unless asked for XNU, once. The keypad's state registers change by themselves (a scan
 * pattern), so keys are not used. The request is a line written to /dev/pmsg0 under Android
 * (tools/boot-xnu.sh): ramoops keeps it in its pmsg zone across the reboot, where this finds it
 * and wipes it, so the boot after XNU is Android again.
 *
 * MT6893: the pmsg zone is where /proc/iomem on this phone says it is (0x48130000, 64 KB) --
 * not the reference board's 0x4d0e0000.
 */
static const char boot_xnu_flag[] = "DARWIN-X6739:BOOT-XNU";

/*
 * Whether Android left `flag` in the pmsg zone before rebooting, and if so wipe it, so it is taken
 * once: a run that hangs costs one reboot, after which the loader finds nothing and starts Android.
 *
 * MT6893: the reference port's own header (sig "DBGC", start, size) does not exist on this
 * phone: /dev/pmsg0 writes go through Android's pmsg driver, which lays the zone out in its own
 * records (what /sys/fs/pstore/pmsg-ramoops-0 shows: "l0\0", then a length, then the text). So
 * there is no header to check, and the zone is simply scanned for the flag -- which is what the
 * reference did too, below the header it had put there itself.
 */
static int pmsg_take(const char *flag, uint32_t n)
{
	volatile struct ramoops_hdr *h = (volatile struct ramoops_hdr *)RAMOOPS_PMSG;
	volatile char *d = (volatile char *)(RAMOOPS_PMSG + sizeof(struct ramoops_hdr));
	uint32_t cap = RAMOOPS_PMSG_SIZE - sizeof(struct ramoops_hdr);
	int found = 0;

	if (h->sig != RAMOOPS_SIG) {
		return 0;
	}
	for (uint32_t at = 0; at + n <= cap; at++) {
		uint32_t k = 0;

		while (k < n && d[at + k] == flag[k]) {
			k++;
		}
		if (k == n) {
			uint32_t p = at + n;
			uint32_t wipe_end = p;

			/* Parse optional parameters: :cpus=N:secs=N:extra=ARGS */
			if (p < cap && d[p] == ':') {
				uint32_t end = p;
				while (end < cap && d[end] != '\r' && d[end] != '\n' && d[end] != '\0') {
					end++;
				}
				wipe_end = end;

				uint32_t cur = p + 1;
				while (cur < end) {
					uint32_t next = cur;
					while (next < end && d[next] != ':') {
						next++;
					}
					uint32_t param_len = next - cur;

					if (param_len >= 5 && d[cur] == 'c' && d[cur+1] == 'p' && d[cur+2] == 'u' &&
					    d[cur+3] == 's' && d[cur+4] == '=') {
						uint32_t val = 0;
						for (uint32_t i = cur + 5; i < next; i++) {
							if (d[i] >= '0' && d[i] <= '9') {
								val = val * 10 + (uint32_t)(d[i] - '0');
							}
						}
						if (val >= 1 && val <= 7) {
							runtime_cpus = val;
						}
					} else if (param_len >= 5 && d[cur] == 's' && d[cur+1] == 'e' && d[cur+2] == 'c' &&
					           d[cur+3] == 's' && d[cur+4] == '=') {
						uint32_t val = 0;
						for (uint32_t i = cur + 5; i < next; i++) {
							if (d[i] >= '0' && d[i] <= '9') {
								val = val * 10 + (uint32_t)(d[i] - '0');
							}
						}
						runtime_secs = val;
					} else if (param_len >= 6 && d[cur] == 'e' && d[cur+1] == 'x' && d[cur+2] == 't' &&
					           d[cur+3] == 'r' && d[cur+4] == 'a' && d[cur+5] == '=') {
						uint32_t elen = next - (cur + 6);
						if (elen >= sizeof(runtime_extra_args)) {
							elen = sizeof(runtime_extra_args) - 1;
						}
						for (uint32_t i = 0; i < elen; i++) {
							runtime_extra_args[i] = d[cur + 6 + i];
						}
						runtime_extra_args[elen] = '\0';
					} else if (param_len >= 5 && d[cur] == 'b' && d[cur+1] == 'a' && d[cur+2] == 't' &&
					           d[cur+3] == 't' && d[cur+4] == '=') {
						/*
						 * batt=soc,mv,ma,tc,status,online: the phone's gauge, read under Android
						 * when this boot was asked for (tools/batt-request.sh). The current is
						 * the one that takes charge, positive while charging.
						 */
						int vals[6];
						uint32_t vi = 0;
						uint32_t i = cur + 5;

						while (vi < 6 && i < next) {
							int sign = 1, v = 0, digits = 0;

							if (d[i] == '-') {
								sign = -1;
								i++;
							}
							while (i < next && d[i] >= '0' && d[i] <= '9') {
								v = v * 10 + (d[i] - '0');
								i++;
								digits++;
							}
							if (!digits) {
								break;
							}
							vals[vi++] = sign * v;
							if (i < next && d[i] == ',') {
								i++;
							}
						}
						if (vi == 6) {
							batt_soc = vals[0];
							batt_mv = vals[1];
							batt_ma = vals[2];
							batt_tc = vals[3];
							batt_status = vals[4];
							batt_online = vals[5];
						}
					}
					cur = next + 1;
				}
			}

			for (k = at; k < wipe_end; k++) {
				d[k] = '-';
			}
			found = 1;
		}
	}
	return found;
}

static int xnu_requested(void)
{
	return pmsg_take(boot_xnu_flag, sizeof(boot_xnu_flag) - 1);
}

static int boot_choice(void)
{
	int xnu = MENU_DEFAULT_XNU || xnu_requested();

	if (!fbc.base) {
		return xnu;
	}
	fb_rect(0, 600, PANEL_W, 160, 0xff000000u);
	fbc.scale = 3;
	fbc.x = 48;
	fbc.y = 640;
	fbc.fg = xnu ? 0xff40ff40u : 0xffffffffu;
	for (const char *c = xnu ? "Booting Darwin / XNU" : "Booting Android"; *c; c++) {
		fbcon_putc(*c);
	}
	fbc.scale = 1;
	fbc.fg = 0xffffffffu;
	dbg_delay_ms(1000);
	return xnu;
}

#if GPU_PROBE
#include "mtkpwrap.h"
#include "mtkmali.h"

static const char gpu_probe_flag[] = "DARWIN-X6739:GPU-PROBE";

static void say_mv(const char *name, int on, uint16_t selector)
{
	out(name);
	out(on ? " on at " : " off, set for ");
	out_dec(mt6358_millivolts(selector));
	out(" mV");
}

/*
 * Wake the GPU and ask it what it is. Each step names what it is about to touch before touching
 * it, so if one hangs the panel keeps its last line; and whatever this switches on it switches
 * off again before Android runs, which expects the GPU as the preloader left it.
 */
static void gpu_probe(void)
{
	uint16_t hwcid = 0, vgpu_con = 0, vgpu_dbg = 0, vgpu_sel = 0, vsram_con = 0, vsram_dbg = 0, vsram_sel = 0;
	int we_lit_vgpu = 0, we_lit_vsram = 0, up = 0, stage = 0;
	uint32_t status, status2, clk_cfg1;

	/* ---- the rails ---- */
	out("PMIC: ");
	if (!pwrap_read(MT6358_HWCID, &hwcid)) {
		out("the wrapper does not answer (WACS2_EN ");
		out_hex(rd32(PWRAP_BASE + PWRAP_WACS2_EN));
		out(")\n");
		return;
	}
	out("chip ");
	out_hex(hwcid);
	out("\n  ");
	pwrap_read(MT6358_BUCK_VGPU_CON0, &vgpu_con);
	pwrap_read(MT6358_BUCK_VGPU_DBG1, &vgpu_dbg);
	pwrap_read(MT6358_BUCK_VGPU_ELR0, &vgpu_sel);
	pwrap_read(MT6358_LDO_VSRAM_GPU_CON0, &vsram_con);
	pwrap_read(MT6358_LDO_VSRAM_GPU_DBG1, &vsram_dbg);
	pwrap_read(MT6358_LDO_VSRAM_CON3, &vsram_sel);
	say_mv("VGPU", vgpu_dbg & 1, vgpu_sel);
	out(", ");
	say_mv("VSRAM_GPU", vsram_dbg & 1, vsram_sel);
	out("\n");

	/*
	 * A rail that is off is switched on at the voltage it is already set for -- but only a
	 * voltage a GPU can sensibly run at: nothing here chooses one.
	 */
	if (!(vsram_dbg & 1) || !(vgpu_dbg & 1)) {
		uint32_t mv_gpu = mt6358_millivolts(vgpu_sel), mv_sram = mt6358_millivolts(vsram_sel);

		if (mv_gpu < 575 || mv_gpu > 900 || mv_sram < 750 || mv_sram > 1000) {
			out("  a rail is off and set for a voltage I will not switch on: stopping\n");
			return;
		}
		if (!(vsram_dbg & 1)) {
			out("  switching VSRAM_GPU on\n");
			pwrap_write(MT6358_LDO_VSRAM_GPU_CON0, vsram_con | 1);
			we_lit_vsram = 1;
		}
		if (!(vgpu_dbg & 1)) {
			out("  switching VGPU on\n");
			pwrap_write(MT6358_BUCK_VGPU_CON0, vgpu_con | 1);
			we_lit_vgpu = 1;
		}
		dbg_delay_ms(2);
		pwrap_read(MT6358_BUCK_VGPU_DBG1, &vgpu_dbg);
		pwrap_read(MT6358_LDO_VSRAM_GPU_DBG1, &vsram_dbg);
		out("  now VGPU ");
		out(vgpu_dbg & 1 ? "on" : "STILL OFF");
		out(", VSRAM_GPU ");
		out(vsram_dbg & 1 ? "on" : "STILL OFF");
		out("\n");
		if (!(vgpu_dbg & 1) || !(vsram_dbg & 1)) {
			goto rails_off;
		}
	}

	/*
	 * ---- the processor, as LK left it: read only ----
	 * The three CPU PLLs (Linux clk-mt6768.c: CON1 holds a 22-bit multiplier of 26 MHz / 2^14 and
	 * a power-of-two post divider in bits 24..26) and the two PMIC rails the clusters run on.
	 */
	{
		static const struct { const char *name; uint32_t con1; } plls[3] = {
			{ "ARMPLL (big)", 0x20c }, { "ARMPLL_L (little)", 0x21c }, { "CCIPLL (bus)", 0x22c },
		};
		uint16_t v11 = 0, v12 = 0, s11 = 0, s12 = 0;

		for (int i = 0; i < 3; i++) {
			uint32_t c = rd32(0x1000c000ULL + plls[i].con1);
			uint64_t mhz = (26ull * (c & 0x3fffffu)) >> 14;

			mhz >>= (c >> 24) & 7u;
			out(plls[i].name);
			out(": CON1 ");
			out_hex(c);
			out(" = ");
			out_dec((uint32_t)mhz);
			out(" MHz\n");
		}
		pwrap_read(0x139e, &v11);               /* VPROC11_DBG0: the voltage it is at */
		pwrap_read(0x141e, &v12);               /* VPROC12_DBG0 */
		pwrap_read(0x1b60, &s11);               /* VSRAM_PROC11_DBG0 (bits 14..8) */
		out("VPROC11 ");
		out_dec(mt6358_millivolts(v11));
		out(" mV, VPROC12 ");
		out_dec(mt6358_millivolts(v12));
		out(" mV, VSRAM_PROC11 ");
		out_dec(mt6358_millivolts((uint16_t)(s11 >> 8)));
		out(" mV\n");
		(void)s12;
	}

	/* ---- what the preloader left ---- */
	status = rd32(SPM_BASE + SPM_PWR_STATUS);
	status2 = rd32(SPM_BASE + SPM_PWR_STATUS_2ND);
	clk_cfg1 = rd32(TOPCKGEN_BASE + CLK_CFG_1);
	out("SPM power status ");
	out_hex(status);
	out(" / ");
	out_hex(status2);
	out(", mfg_sel ");
	out_hex(clk_cfg1 & 0xff);
	out(", bus protection ");
	out_hex(rd32(INFRACFG_BASE + INFRA_TOPAXI_PROTECTEN_STA1));
	out("\n");
	for (int i = 0; i < 4; i++) {
		if (status & mali_domains[i].status) {
			out("  ");
			out(mali_domains[i].name);
			out(" is on already; leaving the GPU alone\n");
			goto rails_off;
		}
	}

	/* ---- the clock, then the power ---- */
	out("mfg_sel to syspll_d3 (364 MHz)\n");
	wr32(TOPCKGEN_BASE + CLK_CFG_1_CLR, MFG_SEL_FIELD | MFG_SEL_PDN);
	wr32(TOPCKGEN_BASE + CLK_CFG_1_SET, MFG_SEL_SYSPLL_D3);
	__asm__ volatile("dsb sy");
	dbg_delay_us(10);

	for (up = 0; up < 4; up++) {
		const struct mali_domain *d = &mali_domains[up];

		out("  powering ");
		out(d->name);
		out(": ");
		if (!mali_domain_on(d, &stage)) {
			out("stalled at step ");
			out_dec((uint32_t)stage);
			out(", its control ");
			out_hex(rd32(SPM_BASE + d->con));
			out(", status ");
			out_hex(rd32(SPM_BASE + SPM_PWR_STATUS));
			out("\n");
			mali_domain_off(d);             /* whatever half of it did come up */
			break;
		}
		out("on\n");
		if (up == 1) {
			out("  opening the bus to the GPU block\n");
			mali_bus_open();
		}
	}
	if (up < 4) {
		goto power_off;
	}

	/* ---- inside the block ---- */
	out("mfgcfg clock gates ");
	out_hex(rd32(MFGCFG_BASE + MFG_CG_CON));
	wr32(MFGCFG_BASE + MFG_CG_CLR, MFG_CG_BG3D);
	__asm__ volatile("dsb sy");
	out(" -> ");
	out_hex(rd32(MFGCFG_BASE + MFG_CG_CON));
	out("\n");

	{
		uint32_t id = rd32(MALI_BASE + GPU_ID);
		uint32_t shaders = rd32(MALI_BASE + GPU_SHADER_PRESENT);
		uint32_t tiler = rd32(MALI_BASE + GPU_TILER_PRESENT);
		uint32_t l2 = rd32(MALI_BASE + GPU_L2_PRESENT);

		out("GPU_ID ");
		out_hex(id);
		out(": ");
		out(mali_name(id));
		out(" r");
		out_dec((id >> 12) & 0xf);
		out("p");
		out_dec((id >> 4) & 0xff);
		out("\n  L2 features ");
		out_hex(rd32(MALI_BASE + GPU_L2_FEATURES));
		out(", tiler ");
		out_hex(rd32(MALI_BASE + GPU_TILER_FEATURES));
		out(", memory ");
		out_hex(rd32(MALI_BASE + GPU_MEM_FEATURES));
		out(", MMU ");
		out_hex(rd32(MALI_BASE + GPU_MMU_FEATURES));
		out("\n  address spaces ");
		out_hex(rd32(MALI_BASE + GPU_AS_PRESENT));
		out(", job slots ");
		out_hex(rd32(MALI_BASE + GPU_JS_PRESENT));
		out(", threads ");
		out_dec(rd32(MALI_BASE + GPU_THREAD_MAX));
		out("\n  shader cores ");
		out_hex(shaders);
		out(", tiler ");
		out_hex(tiler);
		out(", L2 ");
		out_hex(l2);
		out("\n");

		/* A soft reset: what any driver does first, and the GPU answers it with an interrupt bit. */
		wr32(MALI_BASE + GPU_INT_MASK, 0);
		wr32(MALI_BASE + GPU_INT_CLEAR, 0xffffffffu);
		wr32(MALI_BASE + GPU_CMD, GPU_CMD_SOFT_RESET);
		out("soft reset: ");
		out(mali_wait(MALI_BASE + GPU_INT_RAWSTAT, GPU_IRQ_RESET_COMPLETED, GPU_IRQ_RESET_COMPLETED, 100) ?
		    "done\n" : "NO ANSWER\n");
		wr32(MALI_BASE + GPU_INT_CLEAR, 0xffffffffu);

		/* Its parts, each asked to power up and watched until it says it is ready. */
		out("L2 power: ");
		wr32(MALI_BASE + GPU_L2_PWRON, l2);
		out(mali_wait(MALI_BASE + GPU_L2_READY, l2, l2, 50) ? "ready" : "NOT READY");
		out(", tiler: ");
		wr32(MALI_BASE + GPU_TILER_PWRON, tiler);
		out(mali_wait(MALI_BASE + GPU_TILER_READY, tiler, tiler, 50) ? "ready" : "NOT READY");
		out(", shader cores: ");
		wr32(MALI_BASE + GPU_SHADER_PWRON, shaders);
		out(mali_wait(MALI_BASE + GPU_SHADER_READY, shaders, shaders, 50) ? "ready" : "NOT READY");
		out(" (");
		out_hex(rd32(MALI_BASE + GPU_SHADER_READY));
		out(")\n");

		/*
		 * The first job. Address space 0 is put in identity mode -- no page tables yet, a
		 * GPU address is the physical one -- and slot 1 is given one job: write 0xc0ffee01
		 * into a word of DRAM that the processor cleared. The processor reading it back is
		 * the GPU's job manager, its MMU and its path to memory all working at once.
		 */
		{
			volatile uint32_t *job = (volatile uint32_t *)0x50000000ULL;
			volatile uint32_t *target = (volatile uint32_t *)0x50001000ULL;
			uint64_t as = MALI_BASE + AS0_BASE, js = MALI_BASE + JS_BASE(1);
			uint32_t raw;

			for (int i = 0; i < 16; i++) {
				job[i] = 0;
			}
			*target = 0;
			job[4] = (MALI_JOB_WRITE_VALUE << 1) | 1u;        /* 64-bit pointers */
			job[4] |= 1u << 16;                                /* job index 1 */
			job[8] = 0x50001000u;                              /* where */
			job[9] = 0;
			job[10] = MALI_WRITE_IMM32;                        /* what kind of value */
			job[12] = 0xc0ffee01u;                             /* the value */
			__asm__ volatile("dsb sy");

			wr32(as + AS_TRANSCFG_LO, AS_TRANSCFG_IDENTITY);
			wr32(as + AS_TRANSCFG_HI, 0);
			wr32(as + AS_MEMATTR_LO, 0x88888888u);
			wr32(as + AS_MEMATTR_HI, 0x88888888u);
			wr32(as + AS_COMMAND, AS_COMMAND_UPDATE);
			out("MMU address space 0 (identity): ");
			out(mali_wait(as + AS_STATUS, AS_STATUS_ACTIVE, 0, 50) ? "set\n" : "STILL BUSY\n");

			wr32(MALI_BASE + JOB_INT_MASK, 0);
			wr32(MALI_BASE + JOB_INT_CLEAR, 0xffffffffu);
			wr32(js + JS_HEAD_NEXT_LO, 0x50000000u);
			wr32(js + JS_HEAD_NEXT_HI, 0);
			wr32(js + JS_AFFINITY_NEXT_LO, shaders);
			wr32(js + JS_AFFINITY_NEXT_HI, 0);
			wr32(js + JS_CONFIG_NEXT, JS_CONFIG_AS0_FLUSHED);
			wr32(js + JS_COMMAND_NEXT, JS_COMMAND_START);
			mali_wait(MALI_BASE + JOB_INT_RAWSTAT, (1u << 1) | (1u << 17), 0xffffffffu, 100);
			raw = rd32(MALI_BASE + JOB_INT_RAWSTAT);
			out("job: interrupts ");
			out_hex(raw);
			out(", slot status ");
			out_hex(rd32(js + JS_STATUS));
			out(", MMU fault ");
			out_hex(rd32(as + AS_FAULTSTATUS));
			out(" at ");
			out_hex(rd32(as + AS_FAULTADDRESS_LO));
			out(", job's own status ");
			out_hex(job[0]);
			out("\n  the word the GPU was to write: ");
			out_hex(*target);
			out(*target == 0xc0ffee01u ? "  <- THE GPU RAN A JOB\n" : "  (not written)\n");
			wr32(MALI_BASE + JOB_INT_CLEAR, 0xffffffffu);
		}

		/* And back down, inside out. */
		wr32(MALI_BASE + GPU_SHADER_PWROFF, shaders);
		mali_wait(MALI_BASE + GPU_SHADER_READY, shaders, 0, 50);
		wr32(MALI_BASE + GPU_TILER_PWROFF, tiler);
		mali_wait(MALI_BASE + GPU_TILER_READY, tiler, 0, 50);
		wr32(MALI_BASE + GPU_L2_PWROFF, l2);
		mali_wait(MALI_BASE + GPU_L2_READY, l2, 0, 50);
	}
	wr32(MFGCFG_BASE + MFG_CG_SET, MFG_CG_BG3D);
	__asm__ volatile("dsb sy");

power_off:
	/* Everything that came up goes down again, the cores first and the bridge last. */
	while (up-- > 0) {
		const struct mali_domain *d = &mali_domains[up];

		if (up == 1 && !mali_bus_close()) {
			out("  the bus protection did not close; leaving MFG up\n");
			break;
		}
		out("  powering ");
		out(d->name);
		out(mali_domain_off(d) ? " off\n" : " off: NO ANSWER\n");
	}
	wr32(TOPCKGEN_BASE + CLK_CFG_1_CLR, MFG_SEL_FIELD | MFG_SEL_PDN);
	wr32(TOPCKGEN_BASE + CLK_CFG_1_SET, clk_cfg1 & (MFG_SEL_FIELD | MFG_SEL_PDN));
	__asm__ volatile("dsb sy");

rails_off:
	if (we_lit_vgpu) {
		pwrap_write(MT6358_BUCK_VGPU_CON0, vgpu_con & ~1u);
	}
	if (we_lit_vsram) {
		pwrap_write(MT6358_LDO_VSRAM_GPU_CON0, vsram_con & ~1u);
	}
	out("GPU probe done\n");
}
#endif /* GPU_PROBE */

void mtkloader_main(uint64_t dtb)
{
	wdt_disable();
	lk_dtb = dtb;
	dcache_clean_range(MARKER_PA, 192);
	dcache_clean_range(RAMOOPS_PMSG, RAMOOPS_PMSG_SIZE);
	uint32_t mark = marker_get();

	if (mark == MARK_DONE) {
		/* XNU's log was saved when it came back; leave ramoops as it is. */
		chain_android();
	}
	display_find();
	fbcon_init(fb_base, fb_pitch, 0, 1);

	if (mark == MARK_RUNNING) {
		rescue_log();
		dbg_delay_ms(5000);
		chain_android();
	}

#if TOUCH_PROBE
	plog_reset();
	fb_rect(0, 0, PANEL_W, PANEL_H, 0xff000000u);
	fbc.x = 0;
	fbc.y = fbc.top = 0;
	out("== mtkloader: touch screen probe ==\n");
	touch_probe();
	dbg_delay_ms(4000);
	chain_android();
#endif
#if GPU_PROBE
	if (pmsg_take(gpu_probe_flag, sizeof(gpu_probe_flag) - 1)) {
		plog_reset();
		fb_rect(0, 0, PANEL_W, PANEL_H, 0xff000000u);
		fbc.x = 0;
		fbc.y = fbc.top = 0;
		out("== mtkloader: GPU probe ==\n");
		gpu_probe();
		dbg_delay_ms(4000);
	}
	chain_android();
#endif
#if MSDC_TEST
	plog_reset();
	fb_rect(0, 0, PANEL_W, PANEL_H, 0xff000000u);
	fbc.x = 0;
	fbc.y = fbc.top = 0;
	out("== mtkloader: eMMC read test ==\n");
	msdc_self_test();
	dbg_delay_ms(4000);
	wdt_disable();
	chain_android();
#endif
	if (!boot_choice()) {
		chain_android();
	}
	plog_reset();
	out("pmsg: cpus=");
	out_dec(runtime_cpus);
	out(" secs=");
	out_dec(runtime_secs);
	out("\n");
	fb_rect(0, 0, PANEL_W, PANEL_H, 0xff000000u);
	fbc.x = 0;
	fbc.y = fbc.top = 0;
	fbc.fg = 0xff40ff40u;
	/*
	 * A picture of the panel now, with the loader's own text on it: what the panel shows
	 * before XNU has run at all. The picture taken when XNU comes back is the same buffer,
	 * so the two together say whether the window server's drawing reached the panel or the
	 * panel kept what was there before.
	 */
	if (fb_base && fb_pitch) {
		dcache_clean_range(fb_base, (uint64_t) fb_pitch * PANEL_H);
	}
	shot_panel();
	shot_print(1, "the panel before XNU ran");
	out("== mtkloader: Darwin/XNU on X6739 (MT6893) ==\n");
	fbc.fg = 0xffffffffu;
	out("LK dtb ");
	out_hex(dtb);
	out("\n");
	display_report();
	ufs_probe();
	ufs_setup_shared();
	mt6893_connsys_dt_report(dtb);  /* Wi-Fi step 1: the phone's tree on CONNSYS */
	mt6893_connsys_state();          /* Wi-Fi step 1: the state of the conn_infra domain (read only) */
	run_xnu();
}
