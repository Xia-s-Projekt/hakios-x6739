/*
 * The X6739's SoC (MT6893), as its own device tree says.
 *
 * Every address here was read out of the phone's own device tree (/sys/firmware/fdt) or its own
 * kernel command line. The tools that read it are in tools/; the facts and what they mean are in
 * docs/mt6893-facts.md.
 *
 * Device memory with the MMU off: 32-bit accesses only, no SIMD, strict alignment (build.sh).
 */
#ifndef MT6893_H
#define MT6893_H

#include <stdint.h>

/* ---- memory ---- */

/*
 * The DRAM, as the tree's /memory node says: base 0x40000000, size 8 GiB (0x2_0000_0000).
 *
 * The loader runs before XNU and Linux take their view of DRAM, so it needs a window of its own
 * that nothing else uses: the preloader (LK), Android's kernel, the TEE, ATF and the framebuffer
 * all live somewhere in here. The reference MT6768 port put XNU's window at 0xc0000000 on a board
 * whose DRAM ended at 0x40000000; on this board the DRAM starts at 0x40000000 and the phone has
 * 8 GiB, so the same offset would land in the middle of DRAM where the running system may have
 * its own data.
 *
 * The window is therefore placed high in DRAM, clear of this phone's own reservations. What the
 * phone reserves (the secure world, the SCP, the modem) is not in this tree's /memory node, so
 * the exact top has to be confirmed on the phone before anything is built -- see
 * docs/port-plan.md, "The open questions".
 */
#define DRAM_BASE           0x40000000ULL
#define DRAM_SIZE           0x200000000ULL

/* XNU's window: one contiguous range, high in DRAM, clear of the phone's own work. */
#define XNU_PHYS_BASE       0xC0000000ULL
#define XNU_MEM_SIZE        0x2E000000ULL       /* to 0xee000000; NOT yet confirmed on the phone */

/* Where the system image is put, clear of the kernel, as xnuloader does it (RESIDENT_RD_OFFSET). */
#define SYSTEM_PA           (XNU_PHYS_BASE + 0x08000000ULL)
#define SYSTEM_PART         "xnusys"

/* The panic log region (the device tree's "pram"): DRAM outside XNU's window, kept across a reset. */
#define PANIC_LOG_BASE      0x80100000ULL
#define PANIC_LOG_SIZE      0x80000u

/* Run state, kept in DRAM across the watchdog reset (three copies: a bit may flip). */
#define MARKER_PA           0x80000000ULL
#define MARK_RUNNING        0x4e52584eu /* "NXRN" */
#define MARK_DONE           0x4e44584eu /* "NXDN" */

/* ---- the interrupt controller ---- */

/*
 * GIC-v3, as the tree's /interrupt-controller node says: arm,gic-v3, #interrupt-cells = 4, with
 * the distributor at 0x0c000000 (size 0x40000) and the CPU interface at 0x0c040000 (size
 * 0x200000).
 *
 * The tree gives the CPU interface a 2 MiB window, which covers the redistributor's per-CPU
 * frames. The reference MT6768 port told the kernel 0x100000 there, which is one frame per CPU
 * and is what Linux's gic-v3 driver expects for a redistributor without the extra frames; the
 * tree's own number is the one to use.
 */
#define GIC_DIST_BASE       0x0c000000ULL
#define GIC_DIST_SIZE       0x40000ULL
#define GIC_CPU_BASE        0x0c040000ULL
#define GIC_CPU_SIZE        0x200000ULL

/* The window the "arm-io" node maps, so the addresses above can be given to the kernel. */
#define ARMIO_BASE          0x0c000000ULL
#define ARMIO_SIZE          0x14000000ULL

/* ---- the CPUs ---- */

/*
 * Eight cores: four Cortex-A55 and four Cortex-A78, all started through PSCI.
 *
 * The tree numbers them cpu@000 to cpu@003 (the A55s), cpu@100 to cpu@102 (three A78s) and
 * cpu@200 (the prime core, the fastest one). The MPIDR of a core is reg shifted left by 8, so
 * the big cores are 0x400, 0x500, 0x600 and 0x700 -- the numbering skips 103, so the index in
 * the tree is not the MPIDR.
 *
 * The loader runs on the core LK leaves it on (core 0) and takes the *big* cores next, the way
 * the reference port does: "cpus=3" then already gives iOS the fast ones.
 */
#define CPU_COUNT           8

static const uint32_t core_mpidr[CPU_COUNT] = {
	0x000,                              /* the little one the loader runs on */
	0x700,                              /* the prime core (2.711 GHz) */
	0x600, 0x500,                       /* the other A78s (2.600 GHz) */
	0x400,                              /* the last A78 */
	0x300, 0x200, 0x100,                /* the rest of the A55s (2.000 GHz) */
};

/* The A55s: the first four in the tree's own order. The kernel is told which are the small ones. */
#define SMALL_CORE_COUNT   4

/* The clocks the kernel is told, in hertz (the tree's own clock-frequency values). */
#define CPU_CLOCK_LITTLE   2000000000ULL
#define CPU_CLOCK_BIG      2600000000ULL
#define CPU_CLOCK_PRIME    2711000000ULL

/* PSCI: the tree says arm,psci-1.0 with method = smc, so the calls go through ATF. */
#define PSCI_FN_0           0xC4000003u        /* CPU_ON */
#define PSCI_FN_2           0x84000009u        /* SYSTEM_RESET */
#define PSCI_METHOD_SMC     1

/* ---- the clock and the pins ---- */

/*
 * The top clock generator and the infrastructure config, at the same places as on the MT6768:
 * /soc/syscon@10000000 and /soc/syscon@10001000.
 */
#define TOPCKGEN_BASE       0x10000000ULL
#define INFRACFG_BASE       0x10001000ULL

/* The GPIO block and its function registers: /soc/pinctrl@10005000. */
#define GPIO_BASE           0x10005000ULL
#define GPIO_MODE(n)        (0x200u + 0x10u * (n))

/* ---- storage ---- */

/*
 * UFS, not eMMC.
 *
 * The tree's /soc/ufshci@11270000 (mediatek,mt8183-ufshci, reg 0x11270000 + 0x2300) is the only
 * storage controller in the tree: there is no msdc node for the phone's own storage.
 *
 * The reference MT6768 port read its eMMC through MSDC0 with plain CMD17/CMD18, because the
 * preloader had already set the card up. UFS is not like that: the controller has to be reset,
 * the link negotiated (power modes, gear, lanes) and the device's descriptor read out of its
 * own firmware before it will return a block. That is a driver of its own, and the descriptor
 * is behind the phone's secure world.
 *
 * So the loader does not read the system image here. The kernel's own UFS driver reads it, the
 * way the reference port's emmc_root=1 does: the root stays on the phone and the kernel reads it
 * as it needs it. The loader's job is to leave the controller in a state the kernel's driver can
 * take over, or to hand it the descriptor.
 */
#define UFSHCI_BASE         0x11270000ULL
#define UFSHCI_SIZE         0x2300ULL

/* ---- the display ---- */

/*
 * The display pipeline, as the tree has it:
 *   dispsys_config@14116000  mediatek,mt6893-disp
 *   disp_ovl0@14000000       mediatek,disp_ovl0      (a layer the phone's preloader may leave
 *                                                       scanning)
 *   disp_ovl1@14100000       mediatek,disp_ovl1
 *   dsi@1400e000             mediatek,dsi0
 *   dsi@1410e000             mediatek,dsi1
 *   the panel: nt37703 in the tree, a Tianma NT37705 in the phone's own configuration
 *
 * The reference port took the framebuffer LK left scanning on OVL0 layer 0 (0x787b0000, pitch
 * 2944 = 736 pixels) and switched per-pixel alpha off, because LK had enabled it and XNU's
 * console writes transparent pixels. On this board the panel is 1080x2400 and the layer
 * arrangement is different, so both the address and the pitch have to come out of the phone's own
 * tree and its preloader state -- not out of the reference port.
 *
 * Nothing here is set yet: the loader's display code has to be written for this pipeline.
 */
#define DISPSYS_BASE        0x14116000ULL
#define DISP_OVL0_BASE      0x14000000ULL
#define DISP_OVL1_BASE      0x14100000ULL
#define DSI0_BASE           0x1400e000ULL
#define DSI1_BASE           0x1410e000ULL

/*
 * The registers inside the OVL block and the display mutex.
 *
 * MT6893: the reference board's OVL0 was at 0x1400b000 and the mutex at 0x14001020; this board's
 * OVL0 is at 0x14000000 and its mutex node (disp_mutex@14117000) is at 0x14117000. The offsets
 * inside the block are MediaTek's own and are the same family on both engines, but they have to
 * be confirmed on the phone: the loader prints what it reads before it writes anything.
 */
#define OVL_SRC_CON         0x02c      /* a bit per layer the hardware composes */
#define OVL_L0_CON          0x030
#define OVL_L0_PITCH        0x044
#define OVL_L0_ADDR         0xf40

/* The OVL block the loader reads the layer from and writes the composition to. */
#define OVL0_BASE           DISP_OVL0_BASE

#define DISP_MUTEX_EN       0x14117020ULL
#define DISP_MUTEX_RUN      0x14117024ULL

/* ---- the console ---- */

/*
 * The UART the tree gives (mediatek,mt6577-uart at 0x11002000, as on the reference board).
 * The kernel is told about it as "uart0" and its driver is patched in the loader's run_xnu().
 */
#define UART0_BASE          0x11002000ULL

#endif /* MT6893_H */
