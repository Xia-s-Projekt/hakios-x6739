# The X6739's SoC (MT6893), as its own device tree says

Everything here was read out of the phone's own device tree
(`/sys/firmware/fdt`, 339708 bytes, pulled with root). Only numbers and names were taken;
no part of the tree is copied.

The tree is MediaTek's `mediatek,mt6893` (MT6893Z_T/CZA), the Dimensity 8050. It is **not** the
MT6768 of the reference MediaTek port (`hakios-mt6768`, Infinix Hot 20 X6826B): different core
count and layout, different storage (UFS, not eMMC), different GPU, different display pipeline.
The loader's *shape* transfers; its addresses and its storage path do not.

## What the tree says

| Fact | Value |
|---|---|
| model | `MT6893Z_T/CZA` |
| compatible | `mediatek,mt6893` |
| DRAM | base `0x40000000`, size `0x2_0000_0000` (8 GiB) |
| root cell counts | `#address-cells` 2, `#size-cells` 2 (64-bit addresses throughout) |
| CPUs | 8: four `arm,cortex-a55` and four `arm,cortex-a78`, all `enable-method = psci` |
| the cores, as the tree numbers them | `cpu@000`–`cpu@003` = A55 (MPIDR 0x000–0x300), `cpu@100`–`cpu@102` = A78 (MPIDR 0x400–0x600), `cpu@200` = A78, the prime core (MPIDR 0x700) |
| clocks | A55 `clock-frequency` 0x77359400 = 2.000 GHz; A78 0x9af8da00 = 2.600 GHz; prime core 0xa1b51840 = 2.711 GHz. The prime core's table is not monotonic (an entry above 2.6 GHz is 1.593 GHz), so the maximum has to be read, not assumed to be the last one |
| clusters | `cpu-map` has three: cluster0 = the four A55s, cluster1 = three A78s, cluster2 = the prime core |
| interrupt controller | `/interrupt-controller`, `arm,gic-v3`, `#interrupt-cells = 4`, at `0x0c000000` + `0x40000` and `0x0c040000` + `0x200000` |
| PSCI | `/psci`, `arm,psci-1.0`, `method = smc` |
| storage | `/soc/ufshci@11270000`, `mediatek,mt8183-ufshci`, reg `0x11270000` + `0x2300` — **UFS**, and no eMMC path in the tree |
| display | `mediatek,mt6893-disp` at `0x14116000`; OVL0 `0x14000000`, OVL1 `0x14100000`; DSI0 `0x1400e000`, DSI1 `0x1410e000`; the panel node says `nt37703,fhdp,dsi,vdo,tm,panda,x6710` |
| other top-level | `arm,dsu-pmu`, `mediatek,mcucfg@0c530000`, `mediatek,mmsram@1f005000`, `mediatek,mt6893-lvts@1100b000`, `mediatek,mt6893-mdpm`, `mediatek,mtk-bp-thl`, `mediatek,goodix-fp` (fingerprint), `mediatek,mtk` GPIO/pinctrl at `0x10005000` |

## The subsystems the loader and the drivers want

Read out of `/soc` in the tree:

| what | node | compatible | address |
|---|---|---|---|
| console (UART) | `/soc/serial@11002000` | `mediatek,mt6577-uart` | `0x11002000` — **the same as the reference board** |
| a second UART | `/soc/serial@11003000` | `mediatek,mt6577-uart` | `0x11003000` |
| watchdog | `/soc/watchdog@10007000` | `mediatek,mt6893-wdt`, `mediatek,mt6589-wdt` | `0x10007000` |
| timer | `/soc/systimer@10017000` | `mediatek,mt6873-timer`, `mediatek,mt6765-timer` | `0x10017000` |
| SPI | `/soc/spi@1100a000`, `spi@11010000`, `spi@11012000`, `spi@11013000`, `spi@11018000`, `spi@11019000`, `spi@1101d000`, `spi@1101e000` | `mediatek,mt6765-spi` | `0x1100a000` … `0x1101e000` |
| SCP (sensor hub) | `/soc/scp@10700000` | `mediatek,scp` | `0x10700000` |
| CONNSYS (Wi-Fi/Bluetooth) | `/soc/consys@18000000` | `mediatek,mt6893-consys` | `0x18000000` |
| the audio DSP | `/soc/adsp_common@10800000`, `adsp_core0@10820000`, `adsp_core1@10850000` | | `0x10800000` |
| the GPU | `/soc/g3d_config@13fbf000`, `g3d_secure_reg@13fbc000`, `gpueb@13c00000` | | `0x13fbf000` |
| the AP mixed clocks | `/apmixed@1000c000` | `mediatek,apmixed` | `0x1000c000` |
| the AP interrupt router | `/soc/apirq@1000b000` | | `0x1000b000` |
| the DRAM controller | `/soc/dramc@10230000` | | `0x10230000` |
| the memory mux | `/soc/mmsram@1f005000` | `mediatek,mmsram` | `0x1f005000` |
| the I/O remap | `/soc/gateic@0` | | `0x0` |
| the USB | `/soc/usb0@11201000`, `usb-phy0@11e40000` | | `0x11201000` |
| the SPMi | `/soc/spmi@10027000` | | `0x10027000` |

Three of these matter for the port:

- **The SPI is the same kind.** `mediatek,mt6765-spi` is what the reference port's touch driver
  was written against, so the SPI code there carries over — the *pins* are the question, not the
  controller.
- **The touch is not on the SPI.** The tree's `/touch` node says only `mediatek,touch` and has no
  `reg`, so it is a *GPIO* interrupt, not a controller behind an SPI bus. The reference port's
  OmniVision TD4160 over SPI2 is not this phone's touch.
- **The console UART is the same address**, so the reference loader's UART code carries over.

## The phone's firmware

Read out of `/vendor/firmware` on the phone (the shell user cannot list it; root can):

| what | file | for |
|---|---|---|
| the touch controller's firmware | `focaltech_ts_fw_00.bin` | the loader carries it (the reference board's was the OmniVision's) |
| the GPU's firmware | `valhall-1691526.wa` | the Mali-G77, the "valhall" generation |
| CONNSYS (Wi-Fi/Bluetooth) | `soc3_0_patch_mcu_1a_1_hdr.bin`, `soc3_0_ram_mcu_1a_1_hdr.bin`, `soc3_0_ram_wifi_1a_1_hdr.bin`, `soc3_0_ram_bt_1a_1_hdr.bin`, `soc3_0_ram_mcu_e1_hdr.bin`, `soc3_0_ram_wmmcu_1a_1_hdr.bin`, `WIFI_RAM_CODE_soc3_0_1a_1.bin`, `WMT_SOC.cfg`, `wifi.cfg`, `BT_FW.cfg`, `conninfra.cfg` | Wi-Fi and Bluetooth |
| the FM radio | `mt6635_fm_v1_coeff.bin`, `mt6635_fm_v1_patch.bin` | |
| the haptics | `aw8622x_haptic.bin`, `aw8624_haptic.bin` | |
| the charger | `fastchg` | |

The CONNSYS files are the **soc3_0** generation, where the reference board's were soc1_0 — the
same names with a different generation number, and the generation is what the code has to match.

Also on the phone, and worth knowing:

| what | where | why it matters |
|---|---|---|
| the Mali kernel driver | `/vendor/lib/modules/mali_kbase_mt6893.ko` (plus `mali_mgm_mt6893.ko`, `mali_prot_alloc_mt6893.ko`) | this is the MT6893's Mali driver; the reference port's GPU bring-up was written for the G52 and the reference board's driver |
| the SCP driver | `/vendor/lib/modules/scp.ko`, `ccci_fsm_scp.ko` | the sensor hub |
| the panel | `/vendor/etc/cust_silky_brightness_22823_Tianma_NT37705_Tianma4095.xml` | the panel is a **Tianma NT37705**, and the tree's panel compatible says `nt37703` |
| the SCP firmware | `/vendor/firmware/remoteproc_scp` — a 32-byte file, not a directory | the SCP's firmware is not a plain file here; it is loaded by the `scp.ko` driver from somewhere else, and where it is has to be found before the sensor hub can be brought up |

The panel's identity matters: the tree's panel node says `nt37703,fhdp,dsi,vdo,tm,panda,x6710`
and the phone's own configuration files say **NT37705**. Those are different panel generations on
the same controller family, and the display timing has to come from the phone's own side (the
timing the preloader programs), not from the name.

## The phone's partitions

Read out of `/dev/block/by-name` on the phone. A MediaTek A/B layout, everything on UFS LUN 0
(`/dev/block/sdc*`), with the slots side by side:

| partition | device | what |
|---|---|---|
| `boot_a` / `boot_b` | `sdc36` / `sdc56` | the boot image the loader is chained into — **64 MiB** (`blockdev --getsize64`), twice the reference board's 32 MiB |
| `lk_a` / `lk_b` | `sdc34` / `sdc54` | the phone's own preloader (LK) |
| `init_boot_a` / `_b` | `sdc38` / `sdc58` | the initramfs |
| `vendor_boot_a` / `_b` | `sdc37` / `sdc57` | the vendor's boot image |
| `dtbo_a` / `_b` | `sdc39` / `sdc59` | the device tree overlay |
| `vbmeta*`, `tee_a`/`_b`, `spmfw*`, `sspm*`, `mcupm*`, `md1img*`, `pi_img*`, `dpm*`, `gz_*`, `logo_*` | | the secure world and the other pieces the phone boots with |
| `super` | `sdc61` | the dynamic partitions (system, vendor, product) |
| `userdata` | `sdc63` | **226 GiB** (`blockdev --getsize64`) |
| `tranfs` | `sdc62` | the phone's own data |
| `expdb` | `sdc5` | 128 MB on this phone — normal for it, not an anomaly |

Two of these matter for the port:

- **`boot_a` is 64 MiB**, so the boot image has room: the reference port had to build the loader
  `-Os` because the image was close to its 32 MiB partition. Here it does not.
- **There is no `xnusys` partition.** The iOS system image (2.4 GB) needs a partition of its own,
  and the reference port carved one out of the end of `userdata`. `userdata` here is 226 GiB, so
  there is room; `tools/gpt-carve.py` is the reference port's script for it and has to be checked
  against this phone's GPT before it is used.

## The phone's own memory map

From `/proc/cmdline` on the phone:

```
ramoops.mem_address=0x48090000
ramoops.mem_size=0xe0000
ramoops.pmsg_size=0x40000
ramoops.console_size=0x40000
```

But the **actual layout** is in `/proc/iomem`, and it is not what the sizes alone suggest — the
zones are laid out in a fixed order, not packed from the end:

| zone | range | size |
|---|---|---|
| dmesg (95 zones of 4 KB) | `0x48090000`–`0x480eefff` | 0x5f000 |
| **console** | `0x480ef000`–`0x4812efff` | 0x40000 |
| ftrace (8 zones) | `0x4812f000`–`0x4812ffff` | 0x1000 |
| **pmsg** | `0x48130000`–`0x4816ffff` | 0x40000 |

So the loader's console zone is at **`0x480ef000`** and the pmsg zone at **`0x48130000`**. The
reference port had console at `0x4d09f000` and pmsg at `0x4d0e0000` on its own board.

Two things follow, and both change the loader's code:

- **The console zone holds plain text, not the reference's own header.** What
  `/sys/fs/pstore/console-ramoops-0` shows on this phone begins with the *kernel's* log
  ("0 returned 1 after 1251 usecs", the probe results), with nothing of ours in front of it.
  The reference port wrote a header of its own ("DBGC", start, size) at the front of the zone and
  kept its text after it, so it could find where it had left off; on this board the zone belongs
  to Android's pstore driver, which expects the text to start at the beginning. So the loader
  writes from the start and keeps its position in a variable, and clears the zone when it starts
  (otherwise the loader's text and the previous kernel's would be one run of text).
- **The pmsg zone has the same property.** What `/sys/fs/pstore/pmsg-ramoops-0` shows is
  Android's own record format ("l0\0", a length, the text), so the loader's boot request is
  written by `/dev/pmsg0` and found by scanning the zone for the flag, with no header to check.

The loader's staging area and its panic log are placed above the ramoops, and `/dev/pmsg0`
exists on this phone, so the boot-request mechanism works.

## What this means for the port

### The storage path is new work

The reference MT6768 port reads its root filesystem out of the **eMMC** through MSDC0
(`0x11230000`), with the controller already set up by the preloader: it only issues CMD17/CMD18
(`mtkloader/msdc.h`). The X6739 has **UFS** — the tree has no `msdc` node for the storage, only
`ufshci@11270000`. UFS is a much harder controller than an eMMC card:

- it is a link to a device that has to be reset, negotiated (power modes, gear shifts, lane
  count), and given its own descriptor table before it reads a single block;
- it does not answer CMD17/CMD18 — it has a SCSI-like command queue and a doorbell;
- the descriptor comes from the phone's own firmware, which on this platform is behind the
  `secure world`/SCP arrangement and is not simply readable.

So the reference port's `msdc.h` does not carry over. The options are: a UFS driver in the loader
(paying the negotiation cost at every boot), or letting the phone's own bootloader set UFS up and
handing the loader a working block device, or reading the system image with the kernel's own UFS
driver rather than from the loader (the `emmc_root=1` shape, but for UFS: the root stays on the
phone and the kernel reads it as it needs it — this is the path the reference port already uses
for the system image, and it is the cheaper one).

**Chosen: the kernel owns the root.** The loader does not read the system image; the kernel does.
The loader's job is to leave the UFS controller in a state the kernel's driver can take over.

### The interrupt controller is GIC-v3

The reference loader runs with the MMU off and takes its interrupts through the GIC that the
preloader left configured. MT6893 has GIC-v3 (the MT6768 had the older GIC v2), so the
distributor/system registers and the LPIs are different, and the affinity is set differently.
The tree's own numbers are used: distributor `0x0c000000`+`0x40000`, CPU `0x0c040000`+`0x200000`.

### The CPUs are different

Eight cores, 4×A55 + 4×A78, where the reference board has 4×A53 + 3×A55 + 1×A72. The loader's
`core_order` table (`{0, 7, 6, 1, 2, 3, 4, 5}`) and the "big cores first" rule have to be
rewritten for this layout, and the watchdog core has to be picked from the *new* order. The tree
also skips 103 in its numbering, so a core's index is not its MPIDR.

### The clocks differ

The reference loader switches SPI2's bit clock through `CLK_CFG_2` at TOPCKGEN `0x10000000` and
INFRACFG `0x10001000`, and pokes the GPIO function register at `0x10005300`. On MT6893 the
topckgen/infracfg pair is still at `0x10000000`/`0x10001000` and the GPIO block is still at
`0x10005000`, but the mux bit positions and the SPI function number are per-chip: they have to be
read out of this board's own tree and its pin table, not copied. The big cores' clock is left
alone entirely: the tree's own frequencies are used, and the kernel's driver sets it.

### The display is a different pipeline

The reference port takes the framebuffer that LK left scanning (OVL0 layer 0, `0x787b0000`, pitch
2944 = 736 pixels) and switches per-pixel alpha off. On MT6893 the display pipeline is
`disp_ovl0`/`disp_ovl1` with DSI0 and DSI1 and a merge block, the panel is 1080×2400 and the
phone's own configuration files say it is a Tianma NT37705 — so the framebuffer is a different
size, a different pitch and a different layer arrangement, and the timing has to come from the
phone rather than from the panel's name.

### The touch is a different chip

The reference port's touch is an OmniVision TD4160, a TCM controller on SPI2, whose firmware the
loader carries. This phone's is a FocalTech (its firmware is `focaltech_ts_fw_00.bin`), and the
tree's `/touch` node is a GPIO interrupt with no controller behind it. The SPI driver is the same
kind (`mediatek,mt6765-spi`), so the bus code carries over; the controller, the firmware format
and the pins do not.

## What has to be written

In the order that gets to a boot with nothing of the phone's:

1. **The loader** (`mtkloader/`): the platform bring-up for MT6893 — DRAM map, GIC-v3, the core
   order and PSCI, the clock gate for the storage, the framebuffer.
2. **The storage**: UFS, not eMMC. The kernel owns the root — see above.
3. **The device tree the kernel sees**: the loader builds XNU's device tree itself; the tree the
   kernel gets is the loader's, not the phone's.
4. **Then the drivers**, in the reference port's order: CPU freq → buttons and touch → storage →
   GPU → audio and battery → Wi-Fi.

## What is not here yet

- No `pull-firmware.sh` run against the phone (the script is written; the firmware has not been
  pulled).
- No loader image has been built, and nothing has been flashed: **nothing on this phone has been
  booted with HakiOS**.
- The kernel sources of the reference port (`kernel/bsd/dev/redwood_*.c`) are the MT6768 board's;
  every address in them is that board's.
