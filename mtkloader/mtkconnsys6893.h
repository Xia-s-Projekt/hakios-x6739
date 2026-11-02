/*
 * The X6739's CONNSYS (the MT6893's Wi-Fi/Bluetooth/GPS subsystem), as the phone's own
 * device tree and MediaTek's own conninfra platform source say.
 *
 * The shape is the reference port's (hakios-mt6768, kernel/bsd/dev/redwood_connsys.c): every
 * step is read-only until it says what it is about to write, and every wait is bounded. The
 * numbers are this board's, not the reference's.
 *
 * What the MT6893's CONNSYS is, from the tree and the phone's own Android log:
 *
 *   The MT6893 has no MT6631 and no BTIF path to the Wi-Fi firmware the way the MT6768 had.
 *   It has "conninfra": a separate microcontroller (the "conn infra") that owns the radio's
 *   die, the Wi-Fi firmware itself, and the memory the firmware runs in. The phone's own
 *   Android log (logs/android-dmesg.txt) shows the driver that Android runs, and it is not
 *   the reference's:
 *
 *       [consys_conninfra_wakeup_mt6893] Polling chip id success (0x20010101)
 *       conninfra@(consys_thermal_query:407) ...
 *
 *   0x20010101 is CONN_HW_VER in MediaTek's conninfra platform source for this SoC
 *   (conninfra/conn_drv/connv2/platform/mt6893/include/mt6893_consys_reg_offset.h), read at
 *   CONN_INFRA_CFG + 0. So the MT6893's conninfra is the "connac2x2, soc3_0" generation, and
 *   the reference port's WMT-over-BTIF work does not apply to it: the MT6768's radio spoke
 *   over a UART-like bridge (BTIF) and the reference driver spoke STP frames on it. The
 *   MT6893's radio is reached over AXI (the "mediatek,wifi" node) and the conninfra itself
 *   is a different piece of hardware with a different power sequence.
 *
 * The addresses below are the phone's own: they are the sixteen "reg" pairs of
 * /soc/consys@18000000 (compatible "mediatek,mt6893-consys"), in the order MediaTek's
 * enum consys_base_addr_index reads them. The tree and the enum agree one for one, which
 * tools/mtk-connsys-facts.py prints and checks.
 *
 * Only SPM (0x10006000) and the blocks that are always on are read here. The conn infra's
 * own registers hang the bus when the domain is off -- the reference port learned that on
 * the MT6768 -- so nothing below reads them unless the power status says they are alive.
 */

#ifndef MTKCONNSYS_MT6893_H
#define MTKCONNSYS_MT6893_H

#include <stdint.h>

/* ---- the blocks, in the tree's order ---- */

#define CONNSYS_INFRA_RGU_BASE      0x18000000ULL   /* the conn infra's reset generator */
#define CONNSYS_INFRA_CFG_BASE      0x18001000ULL   /* the chip id, the clock and oscillator */
#define CONNSYS_HOST_CSR_BASE       0x18060000ULL   /* wake-up, sleep protection, EMI remap */
#define CONNSYS_INFRACFG_AO_BASE    0x10001000ULL   /* the AXI bus protection */
#define CONNSYS_TOPRGU_BASE         0x10007000ULL   /* the software reset of the domain */
#define CONNSYS_SPM_BASE            0x10006000ULL   /* the power manager */
#define CONNSYS_INFRACFG_BASE       0x1020e000ULL   /* the ap2conn GALS clock */
#define CONNSYS_WT_SLP_CTL_BASE     0x18005000ULL   /* the wake timer */
#define CONNSYS_AFE_CTL_BASE        0x18003000ULL   /* the analog front end */
#define CONNSYS_INFRA_SYSRAM_BASE   0x18050000ULL   /* the system RAM and the efuse shadow */
#define CONNSYS_GPIO_BASE           0x10005000ULL   /* the radio's pins */
#define CONNSYS_RF_SPI_MST_BASE     0x18004000ULL   /* the SPI master towards the radio's die */
#define CONNSYS_SEMAPHORE_BASE      0x18070000ULL   /* the semaphores */
#define CONNSYS_TOP_THERM_CTL_BASE  0x18002000ULL   /* the thermal calibration */
#define CONNSYS_IOCFG_RT_BASE       0x11ea0000ULL   /* the pin drive and pull */
#define CONNSYS_DEBUG_CTRL_BASE     0x1800f000ULL   /* the debug block */

/* ---- the registers inside them ---- */

/* CONN_INFRA_CFG */
#define CONNSYS_HW_VER              0x0000u         /* the chip id: 0x20010101 on this SoC */
#define CONNSYS_CFG_ID              0x0004u
#define CONNSYS_HW_VER_VALUE        0x20010101u

/* SPM */
#define SPM_PWR_STATUS              0x016cu         /* the power status of the domain */
#define SPM_PWR_STATUS_2ND          0x0170u
#define SPM_CONN_PWR_CON            0x0304u         /* the power, isolation and clock of it */
#define SPM_POWERON_CONFIG_EN       0x0000u         /* the write key: 0x0b160001 */

/* TOPRGU */
#define TOPRGU_WDT_SWSYSRST         0x0018u         /* bit 9: the software reset of the domain */

/* CONN_HOST_CSR_TOP */
#define HOST_CSR_SLP_PROT_CTRL      0x0184u         /* bit 5: the sleep protection handshake */
#define HOST_CSR_INFRA_WAKEPU_TOP   0x01a0u         /* the wake-up request to the domain */
#define HOST_CSR_DEADFEED_EN_CR     0x0124u         /* bit 0: the deadfeed workaround */

/* INFRACFG */
#define INFRA_AP2MD_GALS_CTL        0x0504u         /* bit 0: the ap2conn clock gate */

/* INFRACFG_AO */
#define INFRA_TOPAXI_PROTECTEN_STA1 0x0228u
#define INFRA_TOPAXI_PROTECTEN_CLR  0x02a4u
#define INFRA_TOPAXI_PROTECTEN2_CLR 0x0718u

/* GPIO: the radio's pins, 172..182 */
#define GPIO_MODE21                 0x0450u
#define GPIO_MODE22                 0x0460u

/* ---- the tree's own numbers ---- */

/* the radio's AXI window (the "mediatek,wifi" node) and its two interrupts */
#define WIFI_AXI_BASE               0x18000000ULL
#define WIFI_AXI_SIZE               0x700000ULL
#define WIFI_IRQ_SPI0               375u            /* GIC INTID 407 */
#define WIFI_IRQ_SPI1               378u            /* GIC INTID 410 */

/* the memory the conn infra's firmware and the radio's buffers live in */
#define CONNSYS_RESERVE_SIZE        0x450000ULL     /* 4.5 MiB, 16 MiB aligned */
#define WIFI_RESERVE_SIZE           0xf20000ULL     /* 15.1 MiB, 16 MiB aligned */

/*
 * The conn infra's firmware images are not in the phone's tree but in the phone's
 * /vendor/firmware, and they are the "soc3_0" generation (the reference board's were
 * "soc1_0"): the same names with a different generation number. Each is a 16-byte
 * timestamp, the string "ALPS", a version pair, and then the address the image is to be
 * loaded at -- the image's own header, read at offset 0x18, which is where the conn infra
 * will see it (0xf0000000 being the start of its memory).
 *
 *   file                             size     load address   what it is
 *   soc3_0_ram_mcu_1a_1_hdr.bin    165376    0xf0000000      the conn infra's own RAM code
 *   soc3_0_ram_mcu_1_1_hdr.bin     165376    0xf0000000      the same, an earlier build
 *   soc3_0_ram_wmmcu_1a_1_hdr.bin   49868    0xf0230000      the multimedia MCU's RAM code
 *   soc3_0_ram_wifi_1a_1_hdr.bin   296916    0xf02b0000      the Wi-Fi's RAM code
 *   soc3_0_ram_bt_1a_1_hdr.bin     671980    0xf0100011      the Bluetooth's RAM code
 *   soc3_0_ram_bt_1_1_hdr.bin      672276    0xf0100011      the same, an earlier build
 *   soc3_0_patch_wmmcu_1a_1_hdr.bin  5726    0x00100000      the patch for the multimedia MCU
 *
 * The "1a_1" and "1_1" pairs are two builds of the same thing; which one Android picks is
 * in its own configuration, and the phone's Android log says nothing about it yet.
 */

#endif /* MTKCONNSYS_MT6893_H */
