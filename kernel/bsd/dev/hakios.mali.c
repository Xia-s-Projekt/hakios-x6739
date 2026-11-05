/*
 * The Mali-G77 of the Infinix GT 10 Pro (X6739, MT6893), brought up from the kernel for programs
 * to draw with. Only with boot-arg mali=N: every address below belongs to this phone.
 *
 * The GPU is a Valhall GPU of architecture version 9 (the phone reads 0x90800011), driven
 * through the job manager -- not CSF -- which is what ARM's own driver does on this phone, so
 * the job chains Mesa's panfrost builds are the ones the hardware expects.
 *
 * The order of the work is MediaTek's own (their clk-mt6893.c, mtk-scpsys-mt6893 and
 * clk-mt6885-pg.c), and it is the order the loader's GPU probe proved on the phone
 * (mtkloader/mtkloader.c, gpu_probe()):
 *
 *   1  the "mfg_sel" clock mux in topckgen, to the main PLL's
 *   2  the seven power domains in scpsys, each with its SRAM, and the bus protection between
 *      the GPU block and the rest of the chip
 *   3  the block's own clock gate (BG3D in mfgcfg)
 *   4  the GPU: which one it is, a soft reset, and its shader cores, tiler and L2 powered
 *   5  its address space: the tables, then the update that makes them live
 *
 * What there is for programs: the parameters of the GPU (as Linux's panfrost driver reports
 * them), buffers (made, entered into the GPU's address space, told their address, mapped), and
 * job chains (submitted and waited for). A chain is built by the program -- Mesa's panfrost
 * does it -- and the driver only points the GPU at it: the kernel never reads a descriptor.
 *
 * The GPU's own page tables are kbase's aarch64 mode, which is what the loader wrote and the
 * hardware walked: four levels of 512 entries, the entry's type in bits 1:0, the access flag in
 * bit 10, the AP field in bits 6:7, the NX flag in bit 54, and the count of the table's valid
 * entries in bits 55:58 of its first three words. The MT6768 reference port's tables are
 * Bifrost LPAE and do not work here.
 *
 * sysctl kern.hakios.mali says how far it got, and the log of the run repeats it.
 */

#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <stdarg.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

#include "sys/hakios_mali_mt6893.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
extern vm_offset_t phystokv(uint64_t pa);

extern void HakiosMaliStart(void);
extern uint32_t gMaliClients, gMaliBuffers, gMaliSubmits, gMaliFailed;
extern struct hakios_mali_made { uint64_t address, size; uint32_t asked, flags; } gMaliMade[32];

SYSCTL_DECL(_kern_hakios);

/* ---- the state ---- */

static vm_offset_t topckgen, infracfg, spm, mfgcfg, mali;
static uint32_t reached, last_step, gpu_id, shader_mask;
static LCK_GRP_DECLARE(mali_lck_grp, "hakios-mali");
static lck_mtx_t mali_lock;
static lck_mtx_t mali_job_lock;

SYSCTL_UINT(_kern_hakios, OID_AUTO, mali, CTLFLAG_RD | CTLFLAG_LOCKED, &reached, 0,
    "how far the Mali came up: 6 is a job run through its own page tables");

/* ---- reading and writing the registers ---- */

static inline uint32_t
rd(uint64_t reg)
{
	return *(volatile uint32_t *)(vm_offset_t)reg;
}

static inline void
wr(uint64_t reg, uint32_t value)
{
	*(volatile uint32_t *)(vm_offset_t)reg = value;
}

/* Until (register & mask) == want, for at most `ms`. 1 if it got there. */
static int
mali_wait(uint64_t reg, uint32_t mask, uint32_t want, uint32_t ms)
{
	uint64_t end = mach_absolute_time() + ms * (mach_absolute_time() / 1000) * 1000;
	uint32_t left = ms;

	while (left > 0) {
		if ((rd(reg) & mask) == want) {
			return 1;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
		left--;
	}
	return (rd(reg) & mask) == want;
}

static void
mali_set(uint64_t reg, uint32_t bits)
{
	wr(reg, rd(reg) | bits);
}

static void
mali_clear(uint64_t reg, uint32_t bits)
{
	wr(reg, rd(reg) & ~bits);
}

/* ---- the power domains ---- */

struct mali_domain {
	const char *name;
	uint32_t con;
	uint32_t status;
	int sram;
};

static const struct mali_domain mali_domains[7] = {
	{ "MFG0", MFG0_PWR_CON, 1u << 2, 1 },
	{ "MFG1", MFG1_PWR_CON, 1u << 3, 1 },
	{ "MFG2", MFG2_PWR_CON, 1u << 4, 1 },
	{ "MFG3", MFG3_PWR_CON, 1u << 5, 1 },
	{ "MFG4", MFG4_PWR_CON, 1u << 6, 1 },
	{ "MFG5", MFG5_PWR_CON, 1u << 7, 1 },
	{ "MFG6", MFG6_PWR_CON, 1u << 8, 1 },
};

/*
 * The bus protection between the GPU block and the rest of the chip: five steps, each its own
 * register pair, released from the last step to the first. The order is not what makes it work
 * -- each step is its own register and the chip acknowledges each on its own -- but this is the
 * order the one implementation written for this chip uses.
 */
static const struct mali_bus_step mali_bus_steps[5] = {
	{ 0x02a4, 0x0228, (1u << 21) | (1u << 22) },
	{ 0x0718, 0x0724, (1u << 7) },
	{ 0x0bbc, 0x0bc4, (1u << 17) | (1u << 19) },
	{ 0x02ac, 0x0258, (1u << 19) | (1u << 20) | (1u << 21) },
	{ 0x0718, 0x0724, (1u << 5) | (1u << 6) },
};

/* Up: power, then its second switch; then the clock in, the isolation down, the reset released,
 * and the SRAM woken. 0 means it never said: the step that stalled is in *stage. */
static int
mali_domain_on(const struct mali_domain * d, int * stage)
{
	uint64_t con = spm + d->con;

	*stage = 1;
	mali_set(con, PWR_ON);
	mali_set(con, PWR_ON_2ND);
	if (!mali_wait(spm + GPU_SPM_PWR_STATUS, d->status, d->status, 20) ||
	    !mali_wait(spm + GPU_GPU_SPM_PWR_STATUS_2ND, d->status, d->status, 20)) {
		return 0;
	}
	*stage = 2;
	mali_clear(con, PWR_CLK_DIS);
	mali_clear(con, PWR_ISO);
	mali_set(con, PWR_RST_B);
	if (d->sram) {
		*stage = 3;
		mali_clear(con, PWR_SRAM_PDN);
		if (!mali_wait(con, PWR_SRAM_PDN_ACK, 0, 20)) {
			return 0;
		}
	}
	*stage = 0;
	return 1;
}

/* Down, the way it came up but backwards. */
static int
mali_domain_off(const struct mali_domain * d)
{
	uint64_t con = spm + d->con;
	int answered = 1;

	if (d->sram) {
		mali_set(con, PWR_SRAM_PDN);
		answered = mali_wait(con, PWR_SRAM_PDN_ACK, PWR_SRAM_PDN_ACK, 20);
	}
	mali_set(con, PWR_ISO);
	mali_set(con, PWR_CLK_DIS);
	mali_clear(con, PWR_RST_B);
	mali_clear(con, PWR_ON);
	mali_clear(con, PWR_ON_2ND);
	return mali_wait(spm + GPU_SPM_PWR_STATUS, d->status, 0, 20) &&
	    mali_wait(spm + GPU_GPU_SPM_PWR_STATUS_2ND, d->status, 0, 20) && answered;
}

static int
mali_bus_open(void)
{
	for (int i = 0; i < (int)(sizeof(mali_bus_steps) / sizeof(mali_bus_steps[0])); i++) {
		wr(infracfg + mali_bus_steps[i].clr, mali_bus_steps[i].mask);
		if (!mali_wait(infracfg + mali_bus_steps[i].sta, mali_bus_steps[i].mask, 0, 20)) {
			return 0;
		}
	}
	return 1;
}

static void
mali_bus_close(void)
{
	for (int i = 0; i < (int)(sizeof(mali_bus_steps) / sizeof(mali_bus_steps[0])); i++) {
		wr(infracfg + MALI_BUS_SET_OF(&mali_bus_steps[i]), mali_bus_steps[i].mask);
	}
}

/* ---- the GPU's own page tables ---- */

/*
 * A table of 512 entries at each of the four levels. The tables are made once and kept: a
 * buffer's page is entered into them, and the MMU is told about the change afterwards.
 */
static uint64_t * pt_top;
static uint64_t pt_top_phys;
static uint64_t * pt_levels[4][GPU_TABLE_ENTRIES];
static uint64_t pt_level_phys[4][GPU_TABLE_ENTRIES];
static uint32_t pt_level_used[4][GPU_TABLE_ENTRIES];

static uint64_t *
pt_page(uint64_t * phys)
{
	vm_offset_t page = 0;

	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return NULL;
	}
	*phys = kvtophys(page);
	FlushPoC_DcacheRegion(page, PAGE_SIZE);
	return (uint64_t *)page;
}

/*
 * The table below `entry` at `level`, made if there is none yet. The count of the table's
 * valid entries goes in bits 55:58 of the first three words of the table above it, which is
 * how the driver says how much of it there is.
 */
static uint64_t *
pt_below(int level, uint32_t index, uint64_t * entry)
{
	uint64_t phys;
	uint64_t * table;

	if (*entry & GPU_ENTRY_TYPE_MASK) {
		return (uint64_t *)phystokv(*entry & 0x0000fffffffff000ull);
	}
	table = pt_page(&phys);
	if (table == NULL) {
		return NULL;
	}
	*entry = phys | GPU_ENTRY_TABLE | GPU_ENTRY_AF;
	pt_levels[level][index] = table;
	pt_level_phys[level][index] = phys;
	pt_level_used[level][index] = 0;
	FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
	return table;
}

/* The count of a table's valid entries: it goes in bits 55:58 of words 0, 1 and 2. */
static void
pt_set_count(uint64_t * table, uint32_t count)
{
	volatile uint64_t * words = (volatile uint64_t *)table;

	words[0] = (words[0] & ~GPU_ENTRY_COUNT) | (count ? GPU_ENTRY_COUNT : 0);
	words[1] = (words[1] & ~GPU_ENTRY_COUNT) | (count ? GPU_ENTRY_COUNT : 0);
	words[2] = (words[2] & ~GPU_ENTRY_COUNT) | (count ? GPU_ENTRY_COUNT : 0);
}

/* One 4 KB page of physical memory at GPU address `gpu`. */
static bool
map_page(uint64_t gpu, uint64_t phys)
{
	uint64_t * l1, * l2, * l3;
	uint32_t i0 = (gpu >> 39) & (GPU_TABLE_ENTRIES - 1);
	uint32_t i1 = (gpu >> 30) & (GPU_TABLE_ENTRIES - 1);
	uint32_t i2 = (gpu >> 21) & (GPU_TABLE_ENTRIES - 1);
	uint32_t i3 = (gpu >> 12) & (GPU_TABLE_ENTRIES - 1);

	if (pt_top == NULL && (pt_top = pt_page(&pt_top_phys)) == NULL) {
		return false;
	}
	l1 = pt_below(0, i0, &pt_top[i0]);
	if (l1 == NULL) {
		return false;
	}
	l2 = pt_below(1, i1, &l1[i1]);
	if (l2 == NULL) {
		return false;
	}
	l3 = pt_below(2, i2, &l2[i2]);
	if (l3 == NULL) {
		return false;
	}
	l3[i3] = (phys & ~0xfffull) | GPU_ENTRY_LEAF | GPU_ENTRY_AF | GPU_ENTRY_NX;
	FlushPoC_DcacheRegion((vm_offset_t)&l3[i3], sizeof(l3[i3]));
	pt_level_used[2][i2] = 1;
	return true;
}

/* The MMU told about the tables, in the driver's order: the address mode, the root table and the
 * memory attributes, then the update that makes them live, then the flush of the page table
 * cache. The update is confirmed by the MMU going idle again (kbase's wait_ready()). */
static void
as_update(uint64_t top_phys)
{
	uint64_t as = mali + AS_BASE(GPU_AS_NUMBER);

	if (!mali_wait(as + AS_STATUS, AS_STATUS_AS_ACTIVE, 0, 100)) {
		printf("mali: the MMU is busy\n");
		return;
	}
	wr(as + AS_TRANSCFG_LO, AS_TRANSCFG_ADRMODE_AARCH64_4K |
	    AS_TRANSCFG_PTW_MEMATTR_WRITE_BACK | AS_TRANSCFG_PTW_R_ALLOCATE);
	wr(as + AS_TRANSCFG_HI, 0);
	wr(as + AS_TRANSTAB_LO, (uint32_t)(top_phys & 0xffffffffu));
	wr(as + AS_TRANSTAB_HI, (uint32_t)(top_phys >> 32));
	wr(as + AS_MEMATTR_LO, 0x888d8f88u);
	wr(as + AS_MEMATTR_HI, 0x00004c8du);
	wr(as + AS_COMMAND, AS_COMMAND_UPDATE);
	if (mali_wait(as + AS_STATUS, AS_STATUS_AS_ACTIVE, 0, 100)) {
		wr(as + AS_COMMAND, AS_COMMAND_FLUSH_PT);
	}
}

/* The counts of the tables, after the pages went in. */
static void
pt_counts(void)
{
	for (int level = 0; level < 3; level++) {
		for (uint32_t i = 0; i < GPU_TABLE_ENTRIES; i++) {
			uint64_t * table = pt_levels[level][i];

			if (table != NULL) {
				pt_set_count(table, pt_level_used[level][i] ? 1 : 0);
				FlushPoC_DcacheRegion((vm_offset_t)table, PAGE_SIZE);
			}
		}
	}
}

/* ---- the GPU addresses ---- */

static uint64_t next_address = GPU_ADDRESS_FIRST;

uint64_t
hakios_mali_address(uint64_t size)
{
	uint64_t address;

	lck_mtx_lock(&mali_lock);
	address = next_address;
	if (address + size > GPU_ADDRESS_END) {
		address = 0;
	} else {
		next_address += (size + 0xffff) & ~0xffffull;
	}
	lck_mtx_unlock(&mali_lock);
	return address;
}

/* One page of physical memory at GPU address `gpu`. */
bool
hakios_mali_map(uint64_t gpu, uint64_t phys)
{
	bool ok;

	lck_mtx_lock(&mali_lock);
	ok = map_page(gpu, phys);
	pt_counts();
	as_update(pt_top_phys);
	lck_mtx_unlock(&mali_lock);
	return ok;
}

/* A range of GPU addresses is no longer used. */
void
hakios_mali_unmap(uint64_t gpu, uint64_t size)
{
	(void)gpu;
	(void)size;
}

/* The tables have changed: the MMU is told. */
void
hakios_mali_mapped(void)
{
	lck_mtx_lock(&mali_lock);
	pt_counts();
	as_update(pt_top_phys);
	lck_mtx_unlock(&mali_lock);
}

/* ---- the parameters ---- */

bool
hakios_mali_ready(void)
{
	return reached >= 6;
}

/*
 * Linux's DRM_PANFROST_PARAM_*, in its order. The numbers are the ones the enum in Mesa's
 * include/drm-uapi/panfrost_drm.h gives; the values come out of the GPU's own registers.
 */
bool
hakios_mali_parameter(uint32_t which, uint64_t * value)
{
	if (!hakios_mali_ready()) {
		return false;
	}
	switch (which) {
	case 0:                                       /* GPU_PROD_ID */
		*value = (gpu_id >> 16) & 0xffff;
		return true;
	case 1:                                       /* GPU_REVISION */
		*value = (gpu_id >> 4) & 0xffff;
		return true;
	case 2:                                       /* SHADER_PRESENT */
	case 3:                                       /* TILER_PRESENT */
	case 4:                                       /* L2_PRESENT */
		*value = rd(mali + 0x100 + (which - 2) * 0x10) | ((uint64_t)rd(mali + 0x104 + (which - 2) * 0x10) << 32);
		return true;
	case 5:                                       /* STACK_PRESENT: none on this GPU */
		*value = 0;
		return true;
	case 6:                                       /* SHADER_READY */
	case 7:                                       /* TILER_READY */
	case 8:                                       /* L2_READY */
	case 9:                                       /* SHADER_PWRACTIVE */
	case 10:                                      /* TILER_PWRACTIVE */
	case 11:                                      /* L2_PWRACTIVE */
	case 12:                                      /* SHADER_PWRTRANS */
	case 13:                                      /* TILER_PWRTRANS */
	case 14:                                      /* L2_PWRTRANS */
	case 15:                                      /* SHADER_PWRON */
	case 16:                                      /* TILER_PWRON */
	case 17:                                      /* L2_PWRON */
		*value = rd(mali + 0x100 + (which - 6) * 0x10);
		return true;
	case 18:                                      /* SHADER_PWROFF */
	case 19:                                      /* TILER_PWROFF */
	case 20:                                      /* L2_PWROFF */
	case 21:                                      /* STACK_PRESENT_LO */
		*value = rd(mali + 0x1c0 + (which - 18) * 4);
		return true;
	case 22:                                      /* SHADER_FEATURES */
	case 23:                                      /* TILER_FEATURES */
	case 24:                                      /* L2_FEATURES */
	case 25:                                      /* MEM_FEATURES */
	case 26:                                      /* MMU_FEATURES */
	case 27:                                      /* REGISTER_FEATURES */
	case 28:                                      /* SHADER_ID */
	case 29:                                      /* L2_ID */
	case 30:                                      /* TILER_ID */
	case 31:                                      /* JASPER_ID */
	case 32:                                      /* SCISSOR_ID */
	case 33:                                      /* BRA_ID */
	case 34:                                      /* DMA_ID */
	case 35:                                      /* TILER_ID2 */
	case 36:                                      /* NODE_ID */
	case 37:                                      /* THREAD_COUNT */
		*value = rd(mali + 0x0c0 + (which - 22) * 4);
		return true;
	case 38:                                      /* NR_CORE_GROUPS */
		*value = 1;
		return true;
	case 39:                                      /* THREAD_TLS_ALLOC */
		*value = rd(mali + 0x310);
		if (*value < rd(mali + 0x0a0)) {
			*value = rd(mali + 0x0a0);
		}
		return true;
	case 40:                                      /* AFBC_FEATURES */
		*value = 0;
		return true;
	default:
		return false;
	}
}

/* ---- the jobs ---- */

/*
 * One job chain, from GPU address `chain`, run to its end before this returns. 0 when it
 * finished; otherwise the slot's status, or 0xff when it did not answer within a second.
 */
uint32_t
hakios_mali_submit(uint64_t chain, bool fragment)
{
	uint32_t slot = fragment ? 0 : 1;
	uint64_t js = mali + JS_BASE(slot);
	uint32_t raw, status = 0;
	bool finished;

	lck_mtx_lock(&mali_job_lock);
	lck_mtx_lock(&mali_lock);
	wr(mali + JOB_IRQ_CLEAR, JOB_IRQ_DONE(slot) | JOB_IRQ_FAILED(slot));
	wr(js + JS_HEAD_NEXT_LO, (uint32_t)chain);
	wr(js + JS_HEAD_NEXT_HI, (uint32_t)(chain >> 32));
	wr(js + JS_AFFINITY_NEXT_LO, shader_mask);
	wr(js + JS_AFFINITY_NEXT_HI, 0);
	wr(js + JS_CONFIG_NEXT, JS_CONFIG_DISABLE_DESCRIPTOR_WR_BK | GPU_AS_NUMBER);
	while (rd(js + JS_COMMAND_NEXT) != 0) {
		/* the slot is busy with an older job */
	}
	wr(js + JS_COMMAND_NEXT, JS_COMMAND_START);
	lck_mtx_unlock(&mali_lock);

	finished = mali_wait(mali + JOB_IRQ_RAWSTAT, JOB_IRQ_DONE(slot) | JOB_IRQ_FAILED(slot),
	    JOB_IRQ_DONE(slot), 1000);
	lck_mtx_lock(&mali_lock);
	raw = rd(mali + JOB_IRQ_RAWSTAT);
	if (!finished) {
		status = (raw & JOB_IRQ_FAILED(slot)) ? rd(js + JS_STATUS) : 0xff;
		printf("mali: a job on slot %u did not finish: interrupts %#x, status %#x, MMU %#x fault %#x at %#x\n",
		    slot, raw, rd(js + JS_STATUS), rd(mali + MMU_IRQ_RAWSTAT),
		    rd(mali + AS_BASE(GPU_AS_NUMBER) + AS_FAULTSTATUS),
		    rd(mali + AS_BASE(GPU_AS_NUMBER) + AS_FAULTADDRESS_LO));
		gMaliFailed++;
	} else if (raw & JOB_IRQ_FAILED(slot)) {
		status = rd(js + JS_STATUS);
		gMaliFailed++;
	}
	wr(mali + JOB_IRQ_CLEAR, raw);
	lck_mtx_unlock(&mali_lock);
	lck_mtx_unlock(&mali_job_lock);
	return status;
}

/* ---- the bring-up ---- */

/*
 * The GPU is read: what it is, and how much of it there is.
 */
static void
gpu_read(void)
{
	uint32_t id = rd(mali + GPU_ID);

	gpu_id = id;
	shader_mask = rd(mali + SHADER_PRESENT_LO);
	printf("mali: GPU_ID %#x", id);
	if ((id & GPU_ID_ARCH_MAJOR) == (9u << 28)) {
		printf(": Mali-G77 r0p1 (Valhall)");
	} else {
		printf(": not the G77 this driver is for");
	}
	printf(", %u shader cores (present %#x), tiler %#x, L2 %#x\n", __builtin_popcount(shader_mask),
	    shader_mask, rd(mali + TILER_PRESENT_LO), rd(mali + L2_PRESENT_LO));
}

/*
 * The GPU's cores, tiler and L2 are powered, and it is reset.
 */
static int
gpu_start(void)
{
	wr(mali + GPU_IRQ_CLEAR, 0xffffffffu);
	wr(mali + GPU_COMMAND, GPU_COMMAND_SOFT_RESET);
	if (!mali_wait(mali + GPU_IRQ_RAWSTAT, GPU_IRQ_RESET_COMPLETED, GPU_IRQ_RESET_COMPLETED, 100)) {
		printf("mali: the GPU did not come out of its reset\n");
		return 0;
	}
	wr(mali + SHADER_PWRON_LO, rd(mali + SHADER_PRESENT_LO));
	wr(mali + TILER_PWRON_LO, rd(mali + TILER_PRESENT_LO));
	wr(mali + L2_PWRON_LO, rd(mali + L2_PRESENT_LO));
	wr(mali + STACK_PWRON_LO, rd(mali + STACK_READY_LO));
	if (!mali_wait(mali + SHADER_READY_LO, rd(mali + SHADER_PRESENT_LO), rd(mali + SHADER_PRESENT_LO), 50)) {
		printf("mali: the shader cores did not come up\n");
		return 0;
	}
	if (!mali_wait(mali + TILER_READY_LO, rd(mali + TILER_PRESENT_LO), rd(mali + TILER_PRESENT_LO), 50)) {
		printf("mali: the tiler did not come up\n");
		return 0;
	}
	if (!mali_wait(mali + L2_READY_LO, rd(mali + L2_PRESENT_LO), rd(mali + L2_PRESENT_LO), 50)) {
		printf("mali: the L2 did not come up\n");
		return 0;
	}
	return 1;
}

/*
 * A line from the kernel, kept and said again every minute (the tree's own convention).
 */
static uint32_t kept_next;
static char kept_lines[96][200];
static lck_mtx_t kept_lock;

void
hakios_mali_keep(const char * format, ...) __printflike(1, 2);

void
hakios_mali_keep(const char * format, ...)
{
	va_list ap;
	char line[200];

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	printf("%s\n", line);
	lck_mtx_lock(&kept_lock);
	strlcpy(kept_lines[kept_next % 96], line, sizeof(kept_lines[0]));
	kept_next++;
	lck_mtx_unlock(&kept_lock);
}

/*
 * The GPU comes up: the clock, the power, the bus, the block's own gate, then the GPU itself.
 */
void
hakios_mali_init(void)
{
	uint32_t clk_cfg5;
	uint32_t stage;
	int up;

	if (!PE_parse_boot_argn("mali", &last_step, sizeof(last_step))) {
		return;                               /* (mali is not asked for: the GPU is left alone) */
	}
	if (last_step == 0 || last_step > 6) {
		return;
	}
	lck_mtx_init(&mali_lock, &mali_lck_grp, LCK_ATTR_NULL);
	lck_mtx_init(&mali_job_lock, &mali_lck_grp, LCK_ATTR_NULL);
	lck_mtx_init(&kept_lock, &mali_lck_grp, LCK_ATTR_NULL);

	topckgen = ml_io_map(TOPCKGEN_BASE, 0x1000);
	infracfg = ml_io_map(INFRACFG_BASE, 0x1000);
	spm = ml_io_map(SCPSYS_BASE, 0x1000);
	mfgcfg = ml_io_map(MFGCFG_BASE, 0x1000);
	mali = ml_io_map(MALI_BASE, 0x10000);
	if (topckgen == 0 || infracfg == 0 || spm == 0 || mfgcfg == 0 || mali == 0) {
		printf("mali: the GPU's registers are not there to be read\n");
		return;
	}
	clk_cfg5 = rd(topckgen + CLK_CFG_5);

	/* the clock mux, to the main PLL's */
	wr(topckgen + CLK_CFG_5_CLR,
	    ((3u << MFG_SEL_LSB) & ((1u << MFG_SEL_WIDTH) - 1)) | MFG_SEL_PDN);
	wr(topckgen + CLK_CFG_5_SET, MFG_SEL_MAINPLL_D5_D2 << MFG_SEL_LSB);
	wr(topckgen + CLK_CFG_UPDATE, rd(topckgen + CLK_CFG_UPDATE) | CLK_CFG_UPDATE_MFG);
	reached = 1;
	if (last_step < 2) {
		goto done;
	}

	/* the seven power domains */
	for (up = 0; up < 7; up++) {
		const struct mali_domain * d = &mali_domains[up];

		if (!mali_domain_on(d, &stage)) {
			printf("mali: %s stalled at step %d; its control %#x, status %#x\n", d->name, stage,
			    rd(spm + d->con), rd(spm + GPU_SPM_PWR_STATUS));
			reached = 0;
			goto off;
		}
		reached = 2;
		if (up == 1) {
			if (!mali_bus_open()) {
				printf("mali: the bus protection did not open\n");
				reached = 0;
				goto off;
			}
			reached = 3;
		}
	}
	if (last_step < 3) {
		goto done;
	}

	/* the block's own clock gate: a set bit in the status means the clock is held off */
	wr(mfgcfg + MFG_CG_CLR, MFG_CG_BG3D);
	reached = 4;
	if (last_step < 4) {
		goto done;
	}

	if (!gpu_start()) {
		reached = 0;
		goto off;
	}
	reached = 5;
	gpu_read();
	if (last_step < 5) {
		goto done;
	}

	/*
	 * The address space: the tables are made, and the MMU is told about them, so a job can be
	 * run through them. This is what the loader proved on the phone.
	 */
	if (pt_top == NULL) {
		pt_top = pt_page(&pt_top_phys);
	}
	if (pt_top == NULL) {
		printf("mali: no memory for the GPU's page tables\n");
		reached = 0;
		goto off;
	}
	pt_set_count(pt_top, 1);
	FlushPoC_DcacheRegion((vm_offset_t)pt_top, PAGE_SIZE);
	as_update(pt_top_phys);
	reached = 6;

done:
	printf("mali: the GPU is up; the service HakiosPanfrostGPU is for programs to draw with\n");
	HakiosMaliStart();
	return;

off:
	/* everything that came up goes down again, the cores first and the bridge last */
	for (int i = up - 1; i >= 0; i--) {
		(void)mali_domain_off(&mali_domains[i]);
	}
	wr(mfgcfg + MFG_CG_SET, MFG_CG_BG3D);
	mali_bus_close();
	wr(topckgen + CLK_CFG_5_CLR,
	    ((3u << MFG_SEL_LSB) & ((1u << MFG_SEL_WIDTH) - 1)) | MFG_SEL_PDN);
	wr(topckgen + CLK_CFG_5_SET, clk_cfg5 & (((1u << MFG_SEL_WIDTH) - 1) << MFG_SEL_LSB));
	printf("mali: the GPU did not come up; the system draws with its software renderer\n");
}
