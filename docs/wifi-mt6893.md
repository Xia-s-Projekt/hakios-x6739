# Wi-Fi on the X6739 (MT6893): what the port has, what has to be redone, and in what order

Written offline, 1 November 2026, from the phone's own device tree (`refs/fdt-x6739.dtb`),
the phone's own Android log (`logs/android-dmesg.txt`), the firmware already pulled into
`fw-connsys/`, the reference MediaTek port (`refs/hakios-mt6768-reference`), and MediaTek's
own conninfra platform source for this SoC. No phone was touched.

## 1. The verdict

**Feasible, but not by porting the reference's Wi-Fi driver.** The reference port's
CONNSYS work (power, firmware, BTIF, STP, WMT) was for the MT6768's "connac1x"
generation, where the Wi-Fi firmware ran on a microcontroller inside the SoC and was talked
to over a UART-like bridge (BTIF). The MT6893 is a different generation: its Wi-Fi is
"conninfra" (MediaTek's own per-SoC directory for it is
`conninfra/conn_drv/connv2/platform/mt6893/`), the radio's die is reached over AXI and
SPI, and the firmware images are a different set (`soc3_0_*` where the reference's were
`soc1_0_*`). The reference's `hakios.mtk_connsys.c` cannot be adapted; it has to be
replaced.

**A full 802.11 MAC would require porting MediaTek's WLAN driver** (`wlan_drv_gen4m`,
the gen4m "connac2x" driver, tens of thousands of lines of vendor code) into XNU, or
writing an 802.11 MAC from scratch, which is not feasible for this port. That is the
honest size of the job, and it is the same wall the reference port hit and did not climb:
its `device.conf` says, still today, `status wifi partial CONNSYS up, WMT channel over
BTIF/STP; no network yet`.

**What is achievable, in order:**

| milestone | what it gives | size |
|---|---|---|
| A1 | the conn infra's domain powered up and its chip id read (`0x20010101`) | days |
| A2 | the conn infra's firmware in its memory and running | a week |
| A3 | the WMT command channel to it | a week or two |
| A4 | scan and join, through the vendor WLAN driver | months |

A1 to A3 are what the reference port reached and what this port should reach too. A4 is
where the port has to decide whether to port the vendor driver or stop.

## 2. What the port already has

### 2.1 The firmware (`hakios-x6739/fw-connsys/`)

Seven files, all present, all from the phone's `/vendor/firmware`:

| file | bytes | load address in the header | what it is |
|---|---|---|---|
| `soc3_0_ram_mcu_1a_1_hdr.bin` | 165 376 | `0xf0000000` | the conn infra's own RAM code |
| `soc3_0_ram_mcu_1_1_hdr.bin` | 165 376 | `0xf0000000` | the same, an earlier build (2023-11-17) |
| `soc3_0_ram_wmmcu_1a_1_hdr.bin` | 49 868 | `0xf0230000` | the multimedia MCU's RAM code |
| `soc3_0_ram_wifi_1a_1_hdr.bin` | 296 916 | `0xf02b0000` | the Wi-Fi's RAM code |
| `soc3_0_ram_bt_1a_1_hdr.bin` | 671 980 | `0xf0100011` | the Bluetooth's RAM code |
| `soc3_0_ram_bt_1_1_hdr.bin` | 672 276 | `0xf0100011` | the same, an earlier build (2023-11-17) |
| `soc3_0_patch_wmmcu_1a_1_hdr.bin` | 5 726 | `0x00100000` | the patch for the multimedia MCU |

Each is a 16-byte build timestamp, the string `ALPS`, a version pair (`8a10 8a10`, or
`8a00 8a00` for the older Bluetooth build), and then, at offset `0x18`, the address the
image is to be loaded at. The `0xf0......` values are the conn infra's own address space
(`0xf0000000` is the start of it, as on the reference board). The patch file has the same
header shape but its address field reads `0x00100000`, and it is the only file of the set
that is not a RAM image.

**The generation matters.** The reference board's files were `soc1_0_*`; these are
`soc3_0_*`. The same names with a different generation number, and the generation is what
the code has to match.

**Missing, and needed:** `WIFI_RAM_CODE_soc3_0_1a_1.bin`, `WMT_SOC.cfg`, `wifi.cfg`,
`BT_FW.cfg` and `conninfra.cfg` are listed in `docs/mt6893-facts.md` as being on the
phone, but they are **not** in `fw-connsys/`. `pull-firmware.sh` lists them, so the last
pull either skipped them or they were not readable. The first three are the Wi-Fi
firmware proper (the `WIFI_RAM_CODE` file is what runs the 802.11 MAC) and the `.cfg`
files are the configuration the vendor driver reads. **This is the first thing to fix, and
it needs the phone.**

### 2.2 The loader (`hakios-x6739/mtkloader/`)

- `mtkconnsys.h` — a byte-for-byte copy of the reference's, which only prints the device
  tree's CONNSYS-ish nodes. Superseded for this board by the two new files below; kept as
  the record of the reference's shape.
- `mtkconnsys6893.h` (new) — this board's CONNSYS constants, each with its source: the
  sixteen register blocks, the registers inside them, the radio's AXI window and
  interrupts, the reserved-memory sizes, and the firmware inventory above.
- `mtkconnsys_probe.h` (new) — step 1 of the bring-up, in the reference's shape: the
  phone's own tree is printed with each block named, and then the domain's power state is
  read from SPM. Read-only; nothing is written.

### 2.3 The kernel side

`hakios-x6739/kernel/` is empty: there is no CONNSYS driver for this board, and there
should not be one until the design below is decided. The reference's driver lives in the
common tree as `mtk/mt6768/drivers/wifi/hakios.mtk_connsys.c`; the equivalent for this
board would be `mtk/mt6893/drivers/wifi/hakios.mtk_connsys.c`.

The common tree's `main` branch Wi-Fi is the Qualcomm WCN6750 (ath11k over FreeBSD's
LinuxKPI) and does not carry over.

## 3. The addresses, and where each comes from

All of these are the phone's own; `tools/mtk-connsys-facts.py` prints them from
`refs/fdt-x6739.dtb` and checks them against MediaTek's `enum consys_base_addr_index`
for the MT6893.

The tree's `/soc/consys@18000000` (compatible `mediatek,mt6893-consys`) has **sixteen**
`reg` pairs, and MediaTek's enum for this SoC has **sixteen** entries, and they agree one
for one — the tree, the vendor source and the phone's own Android log are tied to each
other this way, which is why the numbers below can be relied on:

| index | block | address | size | what it is |
|---|---|---|---|---|
| 0 | `CONN_INFRA_RGU` | `0x18000000` | 4 KiB | the conn infra's reset generator |
| 1 | `CONN_INFRA_CFG` | `0x18001000` | 4 KiB | the chip id, the clock and oscillator |
| 2 | `CONN_HOST_CSR_TOP` | `0x18060000` | 64 KiB | wake-up, sleep protection, EMI remap |
| 3 | `INFRACFG_AO` | `0x10001000` | 4 KiB | the AXI bus protection |
| 4 | `TOPRGU` | `0x10007000` | 4 KiB | the software reset of the domain |
| 5 | `SPM` | `0x10006000` | 4 KiB | the power manager |
| 6 | `INFRACFG` | `0x1020e000` | 4 KiB | the ap2conn GALS clock |
| 7 | `CONN_WT_SLP_CTL` | `0x18005000` | 4 KiB | the wake timer |
| 8 | `CONN_AFE_CTL` | `0x18003000` | 4 KiB | the analog front end, the WBG, the PLLs |
| 9 | `CONN_INFRA_SYSRAM` | `0x18050000` | 64 KiB | the system RAM and the efuse shadow |
| 10 | `GPIO` | `0x10005000` | 4 KiB | the radio's pins |
| 11 | `CONN_RF_SPI_MST` | `0x18004000` | 4 KiB | the SPI master towards the radio's die |
| 12 | `CONN_SEMAPHORE` | `0x18070000` | 64 KiB | the semaphores |
| 13 | `CONN_TOP_THERM_CTL` | `0x18002000` | 4 KiB | the thermal calibration |
| 14 | `IOCFG_RT` | `0x11ea0000` | 4 KiB | the pin drive and pull |
| 15 | `CONN_DEBUG_CTRL` | `0x1800f000` | 4 KiB | the debug block |

The registers the bring-up needs, with the offsets inside their blocks (MediaTek's
`mt6893_consys_reg_offset.h`):

| address | block | offset | name |
|---|---|---|---|
| `0x18001000` | `CONN_INFRA_CFG` | `+0x0000` | `CONN_HW_VER` — the chip id, `0x20010101` |
| `0x18001004` | `CONN_INFRA_CFG` | `+0x0004` | `CONN_CFG_ID` |
| `0x1000616c` | `SPM` | `+0x016c` | `SPM_PWR_STATUS` |
| `0x10006170` | `SPM` | `+0x0170` | `SPM_PWR_STATUS_2ND` |
| `0x10006304` | `SPM` | `+0x0304` | `SPM_CONN_PWR_CON` — power, isolation, clock |
| `0x10007018` | `TOPRGU` | `+0x0018` | `WDT_SWSYSRST` — the software reset, bit 9 |
| `0x180601a0` | `CONN_HOST_CSR_TOP` | `+0x01a0` | `CONN_INFRA_WAKEPU_TOP` |
| `0x18060184` | `CONN_HOST_CSR_TOP` | `+0x0184` | `CONN_SLP_PROT_CTRL` — the sleep handshake |
| `0x1020e504` | `INFRACFG` | `+0x0504` | `INFRA_AP2MD_GALS_CTL` — the ap2conn clock |
| `0x10005450` | `GPIO` | `+0x0450` | `GPIO_MODE21` — the radio's pins 172..175 |
| `0x10005460` | `GPIO` | `+0x0460` | `GPIO_MODE22` — the radio's pins 176..182 |

**Which stack this is, confirmed by the vendor's own source tree.** MediaTek's
connectivity tree keeps a per-SoC directory for each generation, and this one has:

| what | where | what it holds |
|---|---|---|
| the conninfra's platform code | `conninfra/conn_drv/connv2/platform/mt6893/` | `mt6893.c`, `mt6893_pos.c` (the power sequence, the pinmux, the wake-up), `mt6893_pmic.c` (the supplies), `mt6893_emi.c` (the firmware memory), `mt6893_consys_reg.c` (the block table) |
| the WLAN driver, for this SoC | `wlan/core/gen4m/build/connac2x/6893/` | builds `wlan_drv_gen4m_6893`, `_CONNAC_VER := 2_0` |

The `common` stack the MT6768 used (`connectivity/common`, with `wmt_core.c` and
`stp_btif.c`) is a **different directory** in the same tree, and the MT6893's build is
under `connac2x`, not `connac1x`. So on the MT6893 the WMT driver is the **conninfra**
and the WLAN driver is `wlan_drv_gen4m_6893`; the reference's BTIF/STP work belongs to
the other branch of the tree entirely.

**The phone's own Android log shows the same generation, and shows Wi-Fi not powered.**
`logs/android-dmesg.txt` contains, from Android's own driver:

```
[consys_conninfra_wakeup_mt6893] Polling chip id success (0x20010101)
conninfra@(consys_thermal_query:407) ...
```

`0x20010101` is `CONN_HW_VER` in MediaTek's source, read at `CONN_INFRA_CFG + 0`. So the
phone runs the MT6893 conninfra platform, and the chip id read is the right one.

The same log says, repeatedly, `conninfra@(_status_dump:242) Connsys status: bt` and
`conninfra@(opfunc_pre_cal_check:1076) [pre_cal] bt=[1] wf=[0]` — on the phone, as it was
captured, **Bluetooth's domain was up and Wi-Fi's was not**, and the Wi-Fi driver's own
notifier said `driver is not ready`. That is Android's state on this phone, not a
measurement of the hardware, but it is the one piece of evidence about this board's radio
in the whole repository.

**Where the vendor source used here came from.** The block names, the register offsets and
the power sequence in this document are from MediaTek's own conninfra platform source for
the MT6893, read in the public OnePlusOSS tree
`android_kernel_modules_and_devicetree_oneplus_mt6993` (branch
`oneplus/mt6993_b_16.0_ace_6_ultra`), at
`vendor/mediatek/kernel_modules/connectivity/conninfra/conn_drv/connv2/platform/mt6893/`.
It is that tree's MT6893 platform directory, not a description of the X6739, so every
number in it has been checked against the phone's own device tree (the sixteen blocks
agree one for one) or against the phone's own Android log (the chip id), and the rest is
marked as such.

**The radio's own nodes** (from the tree):

- `/soc/wifi@18000000`, compatible `mediatek,wifi`: AXI window `0x18000000` + `0x700000`
  (7 MiB), interrupts GIC SPI **375** (INTID 407) and **378** (INTID 410), and a
  `memory-region` pointing at the reserved memory below. This is the node the vendor
  WLAN driver's HIF (`gen4m/os/linux/hif/axi/axi.c`) looks up by `compatible` — it does
  `of_find_compatible_node(NULL, NULL, "mediatek,wifi")` and maps its first `reg` region.
- `/soc/btif@1100c000`, compatible `mediatek,btif`: `0x1100c000` + 4 KiB, plus the DMA
  transmit at `0x11000d80` and the DMA receive at `0x11000e00`; interrupts GIC SPI 158
  (190), 152 (184), 153 (185); clocks `btifc`, `apdmc`. **The same address as the
  reference board's BTIF** (`0x1100c000`), which is worth knowing: the block survives, but
  on this SoC it carries Bluetooth, not the Wi-Fi command channel.
- `/soc/gps@18C00000`, compatible `mediatek,mt6885-gps`: `0x18000000` + 1 MiB and
  `0x18c00000` + 1 MiB, six interrupts (SPI 383..389).

**The memory:**

| node | size | alignment | may be placed at |
|---|---|---|---|
| `consys-reserve-memory` | `0x450000` (4.5 MiB) | 16 MiB | `0x50000000`..`0x90000000` |
| `wifi-reserve-memory` | `0xf20000` (15.1 MiB), a shared DMA pool | 16 MiB | `0x40000000`..`0xc0000000` |
| `gps-reserve-memory` | `0x100000` (1 MiB) | 1 MiB | `0x40000000`..`0xc0000000` |

**The supplies:** the tree's regulator nodes, under the MT6359P PMIC
(`/pwrap@10026000/mt6359p/mt6359pregulator/`), say `vcn18` 1.80 V fixed, `vcn13`
0.90..1.30 V, `vcn33_1_bt` and `vcn33_1_wifi` 2.80..3.50 V, `vcn33_2_bt` and
`vcn33_2_wifi` 2.80..3.50 V, `vefuse` 1.70..2.00 V. **Note the PMIC is the MT6359P at
`0x10026000`, not the MT6358 at `0x1000d000` the reference used** — MediaTek's
`mt6893_pmic.h` has the register numbers (`MT6359_LDO_VCN33_1_CON0` `0x1be2`,
`MT6359_LDO_VCN33_2_CON0` `0x1c08`, `MT6359_LDO_VCN18_CON0` `0x1c2e`,
`MT6359_LDO_VCN13_CON0` `0x1c1c`).

## 4. What the reference port's work gives us, and what does not

The reference port's `hakios.mtk_connsys.c` (776 lines, in the common tree as
`mtk/mt6768/drivers/wifi/hakios.mtk_connsys.c`) is ten steps behind one boot-arg
(`connsys_step=1..10`), each step saying what it is about to touch before it touches it.

| reference step | carries over? | why |
|---|---|---|
| 1: read the tree, read the domain's state | **yes, in shape** | the MT6893's blocks and registers are different (table above) but the discipline is the same |
| 2: read the PMIC's VCN supplies | **no** | MT6358 → MT6359P; the register numbers differ; the phone's supplies are also a different set (`vcn13` is new here) |
| 3: turn the supplies on | **no** | as above |
| 4: power the domain up (SPM, TOP_RGU, isolation, bus protection) | **in shape only** | the MT6893's sequence is `SPM_CONN_PWR_CON` bits 0..4, then `TOP_RGU+0x18` bit 9, then the AXI bus protection at `INFRACFG_AO+0x2a4`/`+0x718`, then `INFRA_AP2MD_GALS_CTL` bit 0 — a different order and different registers from the MT6768's `TOP1_PWR_CTRL` sequence |
| 5: the firmware into the conn infra's memory | **in shape only** | the addresses come from the images' own headers, as on the reference, but the remap register is `CONN_HOST_CSR_TOP + 0x01c4` (`CONN2AP_REMAP_MCU_EMI_BASE_ADDR_OFFSET`) here, not the reference's `INFRA+0x380` |
| 6: BTIF | **no** | BTIF exists at the same address but carries Bluetooth; the Wi-Fi command channel is not on it |
| 7, 8: the WMT command channel over STP | **no, not as written** | the MT6893's conninfra has no STP-over-BTIF; the vendor's `common/common_main/core/wmt_core.c` is for the older `common` stack, and the MT6893's WMT lives in the conninfra generation with its own transport |
| 9, 10: the MCU's patch over PDA | **no** | the MT6893's patch is `soc3_0_patch_wmmcu_1a_1_hdr.bin` for the multimedia MCU, and it goes over the conninfra's own path |
| the WLAN driver (gen4m) | **no** | a different generation again: `wlan_drv_gen4m_6893`, built for `connac2x` (`_CONNAC_VER := 2_0`) |

So the reference gives the port its **method** (one step per boot-arg, read before write,
every wait bounded, everything said in the log) and nothing of its **content**.

**How far the reference actually got:** CONNSYS up, the WMT channel answering over
BTIF/STP in full mode, the firmware in memory and running (`MCU+0x600 = 0x1D1E`). **No
802.11 MAC, no network.** Its `device.conf` says so. The gap between "the command
channel works" and "the radio is a network interface" is where the port stopped, and on
this board that gap is bigger, because there the command channel's other half (the WLAN
driver) has to be ported too.

## 5. The gap list for the MT6893

1. **The missing firmware and configuration files** (`WIFI_RAM_CODE_soc3_0_1a_1.bin`,
   `WMT_SOC.cfg`, `wifi.cfg`, `BT_FW.cfg`, `conninfra.cfg`). Needs the phone.
2. **The conninfra's own driver.** MediaTek's `conninfra/conn_drv/connv2` is a Linux
   kernel driver with its own message threads, semaphores, work queues and power
   management. It is not portable to XNU as it stands; it has to be rewritten against
   XNU's equivalents, or replaced by a much smaller XNU-native driver that does what the
   bring-up needs.
3. **The power sequence** for the conninfra domain (A1 above), in the MT6893's registers.
   Small, and the reference's shape applies.
4. **The firmware load** into the conninfra's memory (A2), with the MT6893's remap
   register.
5. **The WMT command channel** (A3), over the MT6893's transport. This is the largest
   unknown: the transport has to be established from the vendor's conninfra source, and
   it is not BTIF.
6. **The WLAN driver** (A4): `wlan_drv_gen4m_6893` and its dependencies (`conninfra`,
   `connfem`, `wlan_page_pool`, `connadp`). Ported into XNU, or not at all.
7. **The upper half** — `bsd/dev/hakios.apple80211.c`, `hakios.sta.c`, `hakios.net.h`,
   `wifi/lx/src/lx_wpa.c` — carries over unchanged, because it is radio-agnostic. It is
   already in the common tree.

## 6. The order of work

Each step is one boot-arg, as in the reference port, and each says in the log what it
found before it does anything.

**Step 0 — offline (done).** The tree's facts, the vendor's block names, the firmware
inventory: `tools/mtk-connsys-facts.py`, `mtkloader/mtkconnsys6893.h`,
`mtkloader/mtkconnsys_probe.h`. The loader prints the tree's CONNSYS nodes with each
block named and reads the domain's power state; nothing is written.

**Step 1 — the phone.** Pull the five missing firmware and `.cfg` files, and the vendor's
modules from `/vendor/lib/modules` (`conninfra.ko`, `connadp.ko`, `connfem.ko`,
`wlan_drv_gen4m_6893.ko`), to read them and to see what Android's own driver does with
them. **Without these the port cannot go past step 3.**

**Step 2 — the domain's power (A1).** A new kernel driver,
`mtk/mt6893/drivers/wifi/hakios.mtk_connsys.c`, `connsys_step=1`: read SPM's power
status, and report. `=2`: turn the supplies on (MT6359P over `PWRAP` at `0x10026000`),
the pinmux (GPIO `0x450`/`0x460`, IOCFG_RT), the clock buffer, then the power sequence
(`SPM_CONN_PWR_CON`, `TOP_RGU+0x18`, `INFRACFG_AO+0x2a4`/`+0x718`,
`INFRA_AP2MD_GALS_CTL+0x504`), then read `CONN_INFRA_CFG+0` and expect `0x20010101`.
The reference's step 4 is the model, with the MT6893's registers.

**Step 3 — the firmware (A2).** `=3`: put the RAM images into the conn infra's memory at
the addresses in their own headers, set the remap register
(`CONN_HOST_CSR_TOP + 0x01c4`), and see whether the firmware runs. The reference's step 5
is the model.

**Step 4 — the command channel (A3).** Here the vendor's conninfra source has to be read
for what the transport is. Until that is settled, this step cannot be written.

**Step 5 — the MAC (A4).** A decision, not a task: port the vendor WLAN driver into XNU
(the way the reference port's `wifi/lx` carries ath11k over FreeBSD's LinuxKPI), or stop
at step 4 and report the radio as "command channel only".

## 7. What was written

In `hakios-x6739/`, in the working tree, nothing committed:

- `tools/mtk-connsys-facts.py` — prints the CONNSYS facts from the phone's own tree and
  checks them against MediaTek's block names.
- `mtkloader/mtkconnsys6893.h` — this board's CONNSYS constants, each with its source.
- `mtkloader/mtkconnsys_probe.h` — step 1 of the bring-up: the tree printed with the
  blocks named, and the domain's power state read. Read-only.
- `mtkloader/mtkloader.c` — includes the new probe in place of the reference's
  `mtkconnsys.h`, and calls it at the same point in the boot.
- this document.

The loader builds (`mtkloader/build.sh`, with the common tree's `xnuloader/identity.h`).
The probe's parsing was checked offline against the phone's real device tree with a host
harness (in the scratch directory, not committed): it prints the same sixteen blocks, with
the same addresses, as the Python tool.
