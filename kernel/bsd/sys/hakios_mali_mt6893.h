/*
 * The registers of the Mali-G77 of the Infinix GT 10 Pro (X6739, MT6893).
 *
 * Every number here is out of the loader's own header (mtkloader/mtkmali6893.h), which is where
 * the sources are cited: ARM's kbase r32p1 for the GPU's own registers, MediaTek's clk-mt6893,
 * mtk-scpsys-mt6893 and clk-mt6885-pg.c for the clock and the power. The loader proved this
 * register map on the phone: it brought the GPU up, read its id, and ran a job through its MMU.
 *
 * The G77 is a Valhall GPU of architecture version 9, driven through the job manager (not CSF),
 * which is what ARM's kbase does on this phone as well.
 */

#ifndef HAKIOS_MALI_MT6893_H
#define HAKIOS_MALI_MT6893_H

/* ---- MediaTek's clock and power controllers ---- */

#define TOPCKGEN_BASE       0x10000000ULL
#define INFRACFG_BASE       0x10001000ULL
#define SCPSYS_BASE         0x10006000ULL      /* the power controller */
#define MFGCFG_BASE         0x13fbf000ULL      /* the GPU block's own clock gate */
#define MALI_BASE           0x13000000ULL      /* the GPU's own registers */

/* The "mfg_sel" mux in CLK_CFG_5: bits 9:8 the parent, bit 15 the gate. */
#define CLK_CFG_5           0x0060
#define CLK_CFG_5_SET       0x0064
#define CLK_CFG_5_CLR       0x0068
#define CLK_CFG_UPDATE      0x00b0
#define CLK_CFG_UPDATE_MFG  (1u << 13)
#define MFG_SEL_LSB         8
#define MFG_SEL_WIDTH       2
#define MFG_SEL_PDN         (1u << 15)
#define MFG_SEL_MAINPLL_D5_D2 3u

/* The power controller's status registers for the GPU. */
#define GPU_SPM_PWR_STATUS      0x016c
#define GPU_GPU_SPM_PWR_STATUS_2ND 0x0170

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
 * register pair, released from the last step to the first (that is the order MediaTek's own
 * code clears them in). The set register of a step is four bytes below its clear register.
 */
struct mali_bus_step {
	uint32_t clr;        /* write the mask here to release this step's protection */
	uint32_t sta;        /* read the mask here until it is clear */
	uint32_t mask;
};

#define MALI_BUS_SET_OF(step)  ((step)->clr - 4)

/* The block's clock gate: a set bit in the status means the clock is HELD OFF. */
#define MFG_CG_STA           0x000
#define MFG_CG_SET           0x004
#define MFG_CG_CLR           0x008
#define MFG_CG_BG3D          (1u << 0)

/* ---- the GPU's own registers ---- */

/*
 * One identity register: GPU_ID holds the version and the model. The model is
 * ((id >> 16) & 0xf00f): arch major in bits 31:28, product major in bits 19:16. This phone
 * reads 0x90800011: arch major 9 (Valhall), product major 0 -- the G77.
 */
#define GPU_ID                0x000
#define GPU_ID_MODEL         0xf00f0000u
#define GPU_ID_ARCH_MAJOR    (0xfu << 28)
#define GPU_ID_PRODUCT_MAJOR (0xfu << 16)

#define GPU_IRQ_RAWSTAT       0x020
#define GPU_IRQ_CLEAR         0x024
#define GPU_IRQ_MASK          0x028
#define GPU_IRQ_STATUS        0x02c
#define GPU_COMMAND           0x030
#define GPU_STATUS            0x034

/* The MMU's own features: bits 7:0 the width of the addresses it takes. */
#define GPU_MMU_FEATURES      0x014

#define GPU_IRQ_FAULT               (1u << 0)
#define GPU_IRQ_MULTIPLE_FAULTS     (1u << 7)
#define GPU_IRQ_RESET_COMPLETED     (1u << 8)
#define GPU_IRQ_POWER_CHANGED_SINGLE (1u << 9)
#define GPU_IRQ_POWER_CHANGED_ALL   (1u << 10)
#define GPU_IRQ_PRFCNT_SAMPLE_DONE  (1u << 16)
#define GPU_IRQ_CLEAN_CACHES_DONE   (1u << 17)

#define GPU_COMMAND_NOP             0x00
#define GPU_COMMAND_SOFT_RESET      0x01
#define GPU_COMMAND_HARD_RESET      0x02
#define GPU_COMMAND_CLEAN_CACHES    0x07
#define GPU_COMMAND_CLEAN_INV_CACHES 0x08

#define AS_PRESENT            0x018
#define JS_PRESENT            0x01c
#define GPU_FEATURES_LO       0x060
#define GPU_FEATURES_HI       0x064

#define SHADER_PRESENT_LO     0x100
#define TILER_PRESENT_LO      0x110
#define L2_PRESENT_LO         0x120
#define SHADER_READY_LO       0x140
#define TILER_READY_LO        0x150
#define L2_READY_LO           0x160
#define SHADER_PWRON_LO       0x180
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

#define JOB_IRQ_DONE(n)       (1u << (n))
#define JOB_IRQ_FAILED(n)     (1u << (16 + (n)))

#define JS_BASE(n)            (0x1800u + (n) * 0x80u)
#define JS_HEAD_LO            0x00
#define JS_HEAD_HI            0x04
#define JS_TAIL_LO            0x08
#define JS_TAIL_HI            0x0c
#define JS_AFFINITY_LO        0x10
#define JS_AFFINITY_HI        0x14
#define JS_CONFIG             0x18
#define JS_XAFFINITY          0x1c
#define JS_COMMAND            0x20
#define JS_STATUS             0x24
#define JS_HEAD_NEXT_LO       0x40
#define JS_HEAD_NEXT_HI       0x44
#define JS_AFFINITY_NEXT_LO   0x50
#define JS_AFFINITY_NEXT_HI   0x54
#define JS_CONFIG_NEXT        0x58
#define JS_COMMAND_NEXT       0x60

#define JS_COMMAND_NOP        0x00
#define JS_COMMAND_START      0x01

#define JS_STATUS_NO_EXCEPTION      0x00
#define JS_STATUS_INTERRUPTED       0x02
#define JS_STATUS_STOPPED           0x03
#define JS_STATUS_TERMINATED        0x04
#define JS_STATUS_CONFIG_FAULT      0x40
#define JS_STATUS_POWER_FAULT       0x41
#define JS_STATUS_READ_FAULT        0x42
#define JS_STATUS_WRITE_FAULT       0x43
#define JS_STATUS_AFFINITY_FAULT    0x44
#define JS_STATUS_BUS_FAULT         0x48
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

#define MMU_PAGE_FAULT(n)      (1u << (n))
#define MMU_BUS_ERROR(n)       (1u << (16 + (n)))

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
#define AS_FAULTEXTRA_LO      0x38
#define AS_FAULTEXTRA_HI      0x3c

#define AS_COMMAND_UPDATE     0x01
#define AS_COMMAND_LOCK       0x02
#define AS_COMMAND_UNLOCK     0x03
#define AS_COMMAND_FLUSH_PT   0x04
#define AS_COMMAND_FLUSH_MEM  0x05
#define AS_STATUS_AS_ACTIVE   (1u << 0)

#define AS_TRANSCFG_ADRMODE_AARCH64_4K  6u
#define AS_TRANSCFG_ADRMODE_MASK        0xfu
#define AS_TRANSCFG_PTW_MEMATTR_WRITE_BACK (2ull << 24)
#define AS_TRANSCFG_PTW_SH_OS             (2ull << 28)
#define AS_TRANSCFG_PTW_R_ALLOCATE        (1ull << 30)
#define AS_TRANSTAB_MASK      0xfffffffff0ull

/*
 * The G77's page tables: kbase's aarch64 mode. Four levels of 512 entries; an entry's type in
 * bits 1:0, the access flag in bit 10, the AP field in bits 6:7, the NX flag in bit 54, and the
 * count of the table's valid entries in bits 55:58 of its first three words.
 *
 * The loader wrote exactly this and the GPU walked it (mtkloader/mtkloader.c, gpu_job()), so it
 * is what the driver uses. It is NOT the Bifrost LPAE format the MT6768 reference port uses.
 */
#define GPU_ENTRY_TYPE_MASK   3ull
#define GPU_ENTRY_TABLE       3ull        /* a table at level 0..2 */
#define GPU_ENTRY_LEAF        3ull        /* a page at level 3 */
#define GPU_ENTRY_AF          (1ull << 10)
#define GPU_ENTRY_AP_RO       (1ull << 6) /* bits 6:7: 01 is read-only */
#define GPU_ENTRY_NX          (1ull << 54)
#define GPU_ENTRY_COUNT       (1ull << 55) /* how many of this table's entries are valid */

#define GPU_PAGE_SIZE         4096
#define GPU_TABLE_ENTRIES     512

/*
 * The address space the jobs run in: JS_CONFIG's low bits carry it, and the driver's own tables
 * are put in that space. One space for everything, as in the loader.
 */
#define GPU_AS_NUMBER         0

/* The GPU addresses handed out: from Mesa's PAN_VA_USER_START upwards, never given out twice. */
#define GPU_ADDRESS_FIRST     0x02000000ull
#define GPU_ADDRESS_END       (1ull << 35)

#endif /* HAKIOS_MALI_MT6893_H */
