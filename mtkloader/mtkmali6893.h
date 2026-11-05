/*
 * The GPU of the X6739 (MT6893): a Mali-G77 MC9, and what has to happen before it answers.
 *
 * This is NOT the reference board's GPU (a Bifrost Mali-G52 at 0x13040000 with four SPM domains
 * in the MT6768). The G77 is a Valhall-class part (Arm's DDK calls it "TTRx"): the register map,
 * the power domains and the clocks are all different, and every number below comes from this
 * phone's own device tree or from MediaTek's own MT6893 kernel sources, named at each place.
 *
 * What the phone says about itself (read on the device, under Android):
 *
 *   /sys/devices/platform/soc/13000000.mali/gpuinfo
 *       "Mali-G77 9 cores r0p1 0x9080"
 *   the driver: /vendor/lib/modules/mali_kbase_mt6893.ko, ARM's kbase, version r32p1-01eac0
 *       (UK version 11.38), built from "mali_valhall/mali-r32p1", driving the GPU in the legacy
 *       JOB-MANAGER mode. The module has kbase_jm_* symbols and no kbase_csf_* driver symbols,
 *       and the phone has no CSF firmware: JM is the only path here.
 *
 * The bring-up order is MediaTek's own, in the order its clk-mt6885-pg.c (which the MT6893 uses)
 * and clk-mt6893-mfgcfg do it:
 *
 *   1  the "mfg_sel" clock mux in topckgen, to the main PLL's D5/D2 (218.4 MHz): the whole GPU
 *      block's clock; needed running before any of its power domains is switched on
 *   2  seven power domains in the power controller (scpsys): MFG0 (the async bridge to the rest
 *      of the chip), MFG1 (the GPU block), and MFG2..MFG6 (the nine shader cores, grouped
 *      1+2+2+2+2 by the tree's own clock names). Each has its own SRAM, and the bus protection
 *      between the GPU block and the rest of the chip is released once MFG1 is up.
 *   3  the block's own clock gate (BG3D in mfgcfg at 0x13fbf000)
 *   4  the GPU's own registers: which GPU it is, what it has, and whether its shader cores,
 *      tiler and L2 are up.
 *
 * The GPU's rails (VGPU on an MT6315 buck, VSRAM_GPU on a PMIC LDO) are the preloader's business
 * on this board: this file does not touch them.
 *
 * THE FIRST JOB. Arm's job descriptor format for this GPU is in no source tree we have: the
 * kernel DDK treats a job chain as an opaque 64-bit GPU address, and Arm's user-space driver
 * (libmali) is not here. We do not need it to get the GPU to execute something, because the
 * dummy-job workaround blob this revision requires *is* a G77 job chain: 384 bytes of job data
 * at the fixed GPU address 0x100000000. See GPU_WA_* below, and the plan in
 * docs/gpu-port-mt6893.md.
 *
 * Sources:
 *   the phone's own device tree: /soc/mali@13000000 (compatible "mediatek,mali","arm,mali-valhall",
 *      reg 0x13000000 + 0x4000, five interrupts GIC_SPI 362..366 named GPU/MMU/JOB/EVENT/PWR),
 *      /soc/mfgcfg_clk (syscon@13fbf000, "mediatek,mt6893-mfgcfg"), /soc/gpufreq (the tree's
 *      clock names for the seven MFG domains), /soc/scpsys@10006000 (the "mt6893-scpsys-clk" node
 *      with the three windows: scpsys itself, infracfg and topckgen), /soc/syscon@10000000
 *      (topckgen), /soc/syscon@10001000 (infracfg).
 *   MediaTek's MT6893 clock and power sources: clk-mt6893.c (CLK_CFG_5, the "mfg_sel" mux),
 *      clk-mt6893-mfgcfg.c (the BG3D gate), clk-mt6885-pg.c and mtk-scpsys-mt6893 (the MFG0..MFG6
 *      power sequences, the bus protection and the domain table), as vendored in the phone's own
 *      module sources.
 *   the GPU's own register map: ARM's kbase for this GPU, the valhall r32p1 sources, whose
 *      mali_kbase_gpu_regmap.h and backend/mali_kbase_gpu_regmap_jm.h give the block bases and
 *      offsets used below (GPU control 0x0000, job control 0x1000, memory management 0x2000).
 */
#ifndef MTKMALI6893_H
#define MTKMALI6893_H

#include <stdint.h>

/* ---- the blocks ---- */

#define TOPCKGEN_BASE       0x10000000ULL
#define INFRACFG_BASE       0x10001000ULL
#define SCPSYS_BASE         0x10006000ULL      /* the power controller */
#define MFGCFG_BASE         0x13fbf000ULL      /* the GPU block's own clock gate */
#define MALI_BASE           0x13000000ULL      /* the GPU's own registers */

/* ---- the clock mux (topckgen) ---- */

/*
 * The "mfg_sel" mux: CLK_CFG_5 bits 9:8 choose the parent, bit 15 is the gate ("pdn"), and the
 * change is published by setting bit 21 of CLK_CFG_UPDATE. The parents are 0 tck_26m_mx9_ck,
 * 1 mfgpll_ck (1150 MHz), 2 univpll_d6, 3 mainpll_d5_d2 (the tree's own comment gives it as
 * 218.4 MHz) -- the tree says so beside the gpufreq node's clocks, and clk-mt6893.c's
 * mfg_parents[] says the same order.
 */
#define CLK_CFG_UPDATE      0x0004
#define CLK_CFG_UPDATE_MFG  (1u << 21)
#define CLK_CFG_5           0x0060
#define CLK_CFG_5_SET       0x0064
#define CLK_CFG_5_CLR       0x0068
#define MFG_SEL_LSB         8
#define MFG_SEL_WIDTH       2
#define MFG_SEL_PDN         (1u << 15)
#define MFG_SEL_MAINPLL_D5_D2 3u             /* the parent this file picks: the main PLL's */

/* ---- the power domains (scpsys) ---- */

#define GPU_SPM_PWR_STATUS      0x016c
#define GPU_GPU_SPM_PWR_STATUS_2ND  0x0170

/* The control register of each domain, and its bit in both status registers. */
#define MFG0_PWR_CON        0x0308
#define MFG1_PWR_CON        0x030c
#define MFG2_PWR_CON        0x0310
#define MFG3_PWR_CON        0x0314
#define MFG4_PWR_CON        0x0318
#define MFG5_PWR_CON        0x031c
#define MFG6_PWR_CON        0x0320

#define PWR_RST_B            (1u << 0)
#define PWR_ISO              (1u << 1)
#define PWR_ON               (1u << 2)
#define PWR_ON_2ND           (1u << 3)
#define PWR_CLK_DIS          (1u << 4)
#define PWR_SRAM_PDN         (1u << 8)
#define PWR_SRAM_PDN_ACK     (1u << 12)

/*
 * The bus protection between the GPU block and the rest of the chip: five steps, each its own
 * register pair, released from the last step to the first (that is the order
 * mtk-scpsys-ext.c clears them in). mtk-scpsys-mt6893's table for MFG1 gives the registers and
 * the bits; the status register is read until the mask is clear.
 *
 * The five steps, in the order MediaTek's own MT6893 code releases them
 * (clk-mt6885-pg.c:1631-1656), each with the register it is cleared in and the bits it clears:
 *
 *   step2_0  0x02a4   21, 22          (PROT_EN)
 *   step2_1  0x0718    7              (PROT_EN_2, second)
 *   step2_2  0x0bbc   17, 19          (SUB_INFRA_VDNR)
 *   step1_0  0x02ac   19, 20, 21      (PROT_EN_1)
 *   step1_1  0x0718    5,  6          (PROT_EN_2)
 *
 * The order is not what makes it work -- each step is its own register with its own status bit
 * and the chip acknowledges each one on its own -- but this is the order the one implementation
 * written for this chip uses, so it is the order used here. Note that bit 21 is in two of the
 * masks (step2_0 and step1_0); MediaTek's code relies on the per-register acknowledgement to
 * keep that straight, so each step is waited for on its own status register before the next.
 *
 * The set register of a step is four bytes below its clear register.
 */
struct mali_bus_step {
	uint32_t clr;        /* write the mask here to release this step's protection */
	uint32_t sta;        /* read the mask here until it is clear */
	uint32_t mask;
};

static const struct mali_bus_step mali_bus_steps[5] = {
	{ 0x02a4, 0x0228, (1u << 21) | (1u << 22) },
	{ 0x0718, 0x0724, (1u << 7) },
	{ 0x0bbc, 0x0bc4, (1u << 17) | (1u << 19) },
	{ 0x02ac, 0x0258, (1u << 19) | (1u << 20) | (1u << 21) },
	{ 0x0718, 0x0724, (1u << 5) | (1u << 6) },
};

/* The set register of a step is four bytes below its clear register. */
#define MALI_BUS_SET_OF(step)  ((step)->clr - 4)

/* ---- the clock gate (mfgcfg) ---- */

/*
 * The block's clock gate. A set bit in the status register means the clock is HELD OFF, so the
 * gate is opened by clearing the bit -- writing to the clear register, not the set one. MediaTek
 * names these the other way round for other gates; this one is as it is.
 */
#define MFG_CG_STA           0x000
#define MFG_CG_SET           0x004
#define MFG_CG_CLR           0x008
#define MFG_CG_BG3D          (1u << 0)

/* ---- the GPU's own registers (ARM's kbase map for this GPU) ---- */

/*
 * One identity register: GPU_ID at 0x000 holds the version (status bits 3:0, minor 11:4,
 * major 15:12) and, in the product bits, the model: product major 19:16, arch rev 23:20, arch
 * minor 27:24, arch major 31:28. There is no separate GPU_ID2 register; the driver applies
 * this layout to this one register.
 *
 * This G77 reads 0x90800011: arch major 9 (Valhall), product major 0, arch rev 0, and the
 * version minor 1 (bits 7:4) with major 0 -- the phone's own "r0p1".
 */
#define GPU_ID                0x000
#define GPU_ID_MAJOR         (1u << 12)
#define GPU_ID_MINOR         (1u << 4)

/*
 * The product, in this hardware's layout: arch major in bits 31:28, arch minor 27:24, arch
 * revision 23:20, product major 19:16. Arm's own driver compares only the arch major and the
 * product major (GPU_ID2_PRODUCT_MODEL), so the model of a GPU is
 *
 *     ((id >> 16) & 0xf00f)
 *
 * and the rest of the bits (the revision, 8 on this phone) are not part of the model. The
 * phone reports 0x9080, which masked is 0x9000: arch major 9, product major 0 -- the G77.
 */
#define GPU_ID_MODEL         0xf00f0000u
#define GPU_ID_ARCH_MAJOR    (0xfu << 28)
#define GPU_ID_PRODUCT_MAJOR (0xfu << 16)

#define GPU_IRQ_RAWSTAT       0x020
#define GPU_IRQ_CLEAR         0x024
#define GPU_IRQ_MASK          0x028
#define GPU_IRQ_STATUS        0x02c
#define GPU_COMMAND           0x030

/* The MMU's own features: bits 7:0 the width of the addresses it takes, 15:8 the physical ones. */
#define GPU_MMU_FEATURES      0x014

/* What the GPU says in its interrupts. */
#define GPU_IRQ_FAULT               (1u << 0)
#define GPU_IRQ_MULTIPLE_FAULTS     (1u << 7)
#define GPU_IRQ_RESET_COMPLETED     (1u << 8)
#define GPU_IRQ_POWER_CHANGED_SINGLE (1u << 9)
#define GPU_IRQ_POWER_CHANGED_ALL   (1u << 10)
#define GPU_IRQ_PRFCNT_SAMPLE_DONE  (1u << 16)
#define GPU_IRQ_CLEAN_CACHES_DONE   (1u << 17)

/* What to ask of the GPU. */
#define GPU_COMMAND_NOP             0x00
#define GPU_COMMAND_SOFT_RESET      0x01
#define GPU_COMMAND_HARD_RESET      0x02
#define GPU_COMMAND_PRFCNT_CLEAR    0x03
#define GPU_COMMAND_PRFCNT_SAMPLE   0x04
#define GPU_COMMAND_CLEAN_CACHES    0x07
#define GPU_COMMAND_CLEAN_INV_CACHES 0x08
#define GPU_STATUS            0x034
#define L2_CONFIG             0x048
#define PWR_KEY               0x050
#define PWR_OVERRIDE0         0x054
#define PWR_OVERRIDE1         0x058
#define AS_PRESENT            0x018
#define JS_PRESENT            0x01c
#define GPU_FEATURES_LO       0x060   /* (RO) */
#define GPU_FEATURES_HI       0x064

#define SHADER_PRESENT_LO     0x100
#define TILER_PRESENT_LO      0x110
#define L2_PRESENT_LO         0x120
#define SHADER_READY_LO       0x140
#define TILER_READY_LO        0x150
#define L2_READY_LO           0x160
#define SHADER_PWRON_LO       0x180   /* (WO) */
#define TILER_PWRON_LO        0x190
#define L2_PWRON_LO           0x1a0
#define SHADER_PWROFF_LO      0x1c0
#define SHADER_PWRTRANS_LO    0x200
#define SHADER_PWRACTIVE_LO   0x240
#define STACK_READY_LO        0xe10
#define STACK_PWRON_LO        0xe20

/* The job manager: a block at 0x1000, slot n at 0x1800 + n * 0x80. */
#define JOB_CONTROL_BASE      0x1000
#define JOB_IRQ_RAWSTAT       (JOB_CONTROL_BASE + 0x000)
#define JOB_IRQ_CLEAR         (JOB_CONTROL_BASE + 0x004)
#define JOB_IRQ_MASK          (JOB_CONTROL_BASE + 0x008)
#define JOB_IRQ_STATUS        (JOB_CONTROL_BASE + 0x00c)

/* Bit i of the job interrupts: slot i finished. Bit 16+i: slot i failed. */
#define JOB_IRQ_DONE(n)       (1u << (n))
#define JOB_IRQ_FAILED(n)     (1u << (16 + (n)))

#define JS_BASE(n)            (0x1800u + (n) * 0x80u)
#define JS_HEAD_LO            0x00     /* (RO) the chain the slot is on */
#define JS_HEAD_HI            0x04
#define JS_TAIL_LO            0x08
#define JS_TAIL_HI            0x0c
#define JS_AFFINITY_LO        0x10
#define JS_AFFINITY_HI        0x14
#define JS_CONFIG             0x18
#define JS_XAFFINITY          0x1c
#define JS_COMMAND            0x20
#define JS_STATUS             0x24
#define JS_HEAD_NEXT_LO       0x40     /* (RW) the chain to run next */
#define JS_HEAD_NEXT_HI       0x44
#define JS_AFFINITY_NEXT_LO   0x50
#define JS_AFFINITY_NEXT_HI   0x54
#define JS_CONFIG_NEXT        0x58
#define JS_XAFFINITY_NEXT     0x5c
#define JS_COMMAND_NEXT       0x60
#define JS_FLUSH_ID_NEXT      0x70

#define JS_COMMAND_NOP        0x00
#define JS_COMMAND_START      0x01

/* What JS_STATUS says when a slot did not finish cleanly. */
#define JS_STATUS_NO_EXCEPTION      0x00
#define JS_STATUS_INTERRUPTED       0x02
#define JS_STATUS_STOPPED           0x03
#define JS_STATUS_TERMINATED        0x04
#define JS_STATUS_FAULT_BASE        0x40
#define JS_STATUS_CONFIG_FAULT      0x40
#define JS_STATUS_POWER_FAULT       0x41
#define JS_STATUS_READ_FAULT        0x42
#define JS_STATUS_WRITE_FAULT       0x43
#define JS_STATUS_AFFINITY_FAULT    0x44
#define JS_STATUS_BUS_FAULT         0x48
#define JS_STATUS_INSTRUCTION_FAULT_BASE 0x50
#define JS_STATUS_MEMORY_FAULT_BASE 0x60
#define JS_STATUS_OUT_OF_MEMORY     0x60
#define JS_STATUS_UNKNOWN           0x7f

/*
 * JS_CONFIG: bits 9:8 the flush before the job, bit 10 flush the MMU, bit 11 the chain flag,
 * bits 13:12 the flush after it, bit 14 flush reduction, bit 15 no descriptor write-back,
 * bits 19:16 the thread priority -- and, in the low nibble, the ADDRESS SPACE the job runs in.
 */
#define JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE (3u << 8)
#define JS_CONFIG_START_MMU                    (1u << 10)
#define JS_CONFIG_JOB_CHAIN_FLAG               (1u << 11)
#define JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE   (3u << 12)
#define JS_CONFIG_ENABLE_FLUSH_REDUCTION       (1u << 14)
#define JS_CONFIG_DISABLE_DESCRIPTOR_WR_BK     (1u << 15)
#define JS_CONFIG_THREAD_PRI(n)                ((n) << 16)

/* The MMU: a block at 0x2000, address space n at 0x2400 + n * 0x40. */
#define MEMORY_MANAGEMENT_BASE 0x2000
#define MMU_IRQ_RAWSTAT        (MEMORY_MANAGEMENT_BASE + 0x000)
#define MMU_IRQ_CLEAR          (MEMORY_MANAGEMENT_BASE + 0x004)
#define MMU_IRQ_MASK           (MEMORY_MANAGEMENT_BASE + 0x008)
#define MMU_IRQ_STATUS         (MEMORY_MANAGEMENT_BASE + 0x00c)

/* Bit n of the MMU interrupts: a page fault in address space n. Bit 16+n: a bus error. */
#define MMU_PAGE_FAULT(n)      (1u << (n))
#define MMU_BUS_ERROR(n)       (1u << (16 + (n)))

/* Address space n of the MMU: the block's base, then 0x400 for the first space, 0x40 each. */
#define AS_BASE(n)            (MEMORY_MANAGEMENT_BASE + 0x400u + (n) * 0x40u)
#define AS_TRANSTAB_LO        0x00
#define AS_TRANSTAB_HI        0x04
#define AS_MEMATTR_LO         0x08
#define AS_MEMATTR_HI         0x0c
#define AS_LOCKADDR_LO        0x10
#define AS_LOCKADDR_HI        0x14
#define AS_COMMAND            0x18
#define AS_FAULTSTATUS        0x1c
#define AS_FAULTADDRESS_LO    0x20
#define AS_FAULTADDRESS_HI    0x24
#define AS_STATUS             0x28
#define AS_TRANSCFG_LO        0x30
#define AS_TRANSCFG_HI        0x34
#define AS_FAULTEXTRA_LO      0x38   /* the second fault address, if there was one */
#define AS_FAULTEXTRA_HI      0x3c

#define AS_COMMAND_UPDATE     0x01
#define AS_COMMAND_LOCK       0x02
#define AS_COMMAND_UNLOCK     0x03
#define AS_COMMAND_FLUSH_PT   0x04
#define AS_STATUS_AS_ACTIVE   (1u << 0)

/* How the MMU walks memory. */
#define AS_TRANSCFG_ADRMODE_IDENTITY    2u
#define AS_TRANSCFG_ADRMODE_AARCH64_4K  6u
#define AS_TRANSCFG_ADRMODE_MASK        0xfu
#define AS_TRANSCFG_PTW_MEMATTR_WRITE_BACK (2ull << 24)
#define AS_TRANSCFG_PTW_SH_OS             (2ull << 28)
#define AS_TRANSCFG_PTW_R_ALLOCATE        (1ull << 30)
#define AS_TRANSTAB_MASK      0xfffffffff0ull   /* 40 bits, low 4 clear */

/* ---- the dummy-job workaround blob ---- */

/*
 * This revision (r0p1) of the G77 has hardware issue TTRX_3485, and Arm's driver refuses to
 * open /dev/mali without the workaround blob: "valhall-1691526.wa", 440 bytes, loaded as
 * firmware. We keep our own copy in fw-gpu/; it is the same file the phone ships.
 *
 * Its layout, read out of the file and out of mali_kbase_dummy_job_wa.c:
 *
 *   0x000  header   { u16 signature 0x4157 'WA', u16 version 2, u32 info_offset 0x1a0 }
 *   0x008  payload  384 bytes: the job chain itself
 *   0x1a0  info     { u64 jc 0x100000000, u32 js 0, u32 blob_offset 0x188, u64 flags 0x6 }
 *   0x188  blob     { u64 base 0x100000000, u32 size 384, u32 map_flags 6,
 *                     u32 payload_offset 8, u32 blob_offset 0 }
 *
 * So the payload is copied to the fixed GPU address 0x100000000, one page, and the chain is
 * submitted from slot 0. The flags are 0x6 = wait for power-up + logical shader power.
 *
 * The payload IS a valid G77 job chain, so pointing a job slot's HEAD_NEXT at 0x100000000 runs
 * it. Arm's own driver does exactly that on every shader core after power-up, with the address
 * space in JS_CONFIG's low bits, and waits for the slot's JOB_IRQ_RAWSTAT bit. We therefore do
 * not need Arm's proprietary descriptor format to get the GPU to execute something.
 */
#define GPU_WA_FILE          "valhall-1691526.wa"
#define GPU_WA_SIGNATURE     0x4157        /* 'WA' */
#define GPU_WA_VERSION       2
#define GPU_WA_INFO_OFFSET   0x1a0
#define GPU_WA_BLOB_OFFSET   0x188
#define GPU_WA_PAYLOAD_OFF   0x008         /* where the job data sits in the file */
#define GPU_WA_PAYLOAD_SIZE  384
#define GPU_WA_JC            0x100000000ull /* the chain's GPU address */
#define GPU_WA_SLOT          0
/* The address space the job runs in: JS_CONFIG's low bits carry it (the driver writes
 * kctx->as_nr there, unshifted), and the loader programs that same space's registers. */
#define GPU_AS_NUMBER        0
#define GPU_WA_FLAGS         0x6           /* wait for power-up, logical shader power */
#define GPU_WA_FLAG_SERIALIZE        (1ull << 0)
#define GPU_WA_FLAG_WAIT_POWERUP     (1ull << 1)
#define GPU_WA_FLAG_LOGICAL_POWER    (1ull << 2)

/*
 * The seven domains, in the order they come up (they go down in the other). The tree's own clock
 * names say what each one is: MFG0 is the async bridge, MFG1 the block itself, MFG2 the first
 * shader core and MFG3..MFG6 the eight others in groups of two.
 */
struct mali_domain {
	const char *name;
	uint32_t con;           /* its control register in the power controller */
	uint32_t status;        /* its bit in PWR_STATUS and PWR_STATUS_2ND */
	int sram;               /* it has SRAM to wake and put to sleep */
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

/* ---- waiting, with a limit on it ---- */

/* Until (register & mask) == want, for at most `ms`. 1 if it got there. */
static int mali_wait(uint64_t reg, uint32_t mask, uint32_t want, uint32_t ms)
{
	uint64_t end = dbg_ticks() + dbg_freq() / 1000 * ms;

	do {
		if ((rd32(reg) & mask) == want) {
			return 1;
		}
	} while (dbg_ticks() < end);
	return 0;
}

static void mali_set(uint64_t reg, uint32_t bits)
{
	wr32(reg, rd32(reg) | bits);
}

static void mali_clear(uint64_t reg, uint32_t bits)
{
	wr32(reg, rd32(reg) & ~bits);
}

/* ---- one domain up, or down ---- */

/*
 * Up: power, and its second switch; wait until the controller says both are on; then let the
 * clock in, drop the isolation and release the reset; and wake the SRAM, waiting until the SRAM
 * says it is awake. 0 means it never said: the step that stalled is in *stage.
 */
static int mali_domain_on(const struct mali_domain *d, int *stage)
{
	uint64_t con = SCPSYS_BASE + d->con;

	*stage = 1;
	mali_set(con, PWR_ON);
	mali_set(con, PWR_ON_2ND);
	if (!mali_wait(SCPSYS_BASE + GPU_SPM_PWR_STATUS, d->status, d->status, 20) ||
	    !mali_wait(SCPSYS_BASE + GPU_GPU_SPM_PWR_STATUS_2ND, d->status, d->status, 20)) {
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

/*
 * Down, the way it came up but backwards: SRAM to sleep, isolate, clock off, reset, power off. It
 * goes all the way even when a step does not answer -- a domain that only half came up has to be
 * switched off too -- and says afterwards whether every step did.
 */
static int mali_domain_off(const struct mali_domain *d)
{
	uint64_t con = SCPSYS_BASE + d->con;
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
	return mali_wait(SCPSYS_BASE + GPU_SPM_PWR_STATUS, d->status, 0, 20) &&
	    mali_wait(SCPSYS_BASE + GPU_GPU_SPM_PWR_STATUS_2ND, d->status, 0, 20) && answered;
}

/* The GPU block's side of the bus: protection off once MFG1 has power, on again before it goes. */
static int mali_bus_open(void)
{
	int i;

	for (i = 0; i < (int)(sizeof(mali_bus_steps) / sizeof(mali_bus_steps[0])); i++) {
		wr32(INFRACFG_BASE + mali_bus_steps[i].clr, mali_bus_steps[i].mask);
		if (!mali_wait(INFRACFG_BASE + mali_bus_steps[i].sta, mali_bus_steps[i].mask, 0, 20)) {
			return 0;
		}
	}
	__asm__ volatile("dsb sy");
	return 1;
}

static int mali_bus_close(void)
{
	int i;

	for (i = 0; i < (int)(sizeof(mali_bus_steps) / sizeof(mali_bus_steps[0])); i++) {
		wr32(INFRACFG_BASE + MALI_BUS_SET_OF(&mali_bus_steps[i]), mali_bus_steps[i].mask);
	}
	__asm__ volatile("dsb sy");
	return 1;
}

/*
 * What the product number of GPU_ID says, for the ones this phone could plausibly have. The
 * values are Arm's GPU_ID2_MODEL_MAKE(arch major, product major), shifted down by 16 to sit in
 * the same place as the masked id: the G77 is (9,0), the G57 on this chip is (9,1) -- the other
 * G57, (9,3), belongs to a different family of MediaTek parts -- the G78 is (9,2), the G68 is
 * (9,4), the G710 is (10,2) and the G610 is (10,7).
 */
static const char *mali_name(uint32_t id)
{
	switch ((id >> 16) & 0xf00fu) {
	case 0x9000: return "Mali-G77";
	case 0x9001: return "Mali-G57";
	case 0x9002: return "Mali-G78";
	case 0x9004: return "Mali-G68";
	case 0x9005: return "Mali-G78AE";
	case 0xa002: return "Mali-G710";
	case 0xa003: return "Mali-G510";
	case 0xa004: return "Mali-G310";
	case 0xa007: return "Mali-G610";
	default:     return "an unfamiliar Mali";
	}
}

#endif /* MTKMALI6893_H */
