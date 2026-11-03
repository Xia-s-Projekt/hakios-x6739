# The X6739 bring-up checklist

The goal: **a stable iOS firmware running natively on the Infinix X6739 (MT6893)**, with the
phone's own hardware working — sensors, storage, camera, GPU, radios — the way it works
under Android.

This is the state of the port, area by area, with what is proven and what is not. It is
written from the run logs and the source, not from intention. A line that says *missing*
means it has never been seen to work on the phone.

Last updated 2026-11-03. Read `docs/ios-screen-state.md` for the screen and `docs/port-plan.md` for the plan.

---

## 0. The shape of the port

| | |
|---|---|
| Device | Infinix X6739 (GT 10 Pro), MediaTek **MT6893** (Dimensity 8050), 8 GiB UFS |
| Target | Darwin/XNU (iOS 16.5 era image: Darwin 22.5.0, `iPhone10,3` identity) |
| Kernel | XNU 8796.121.2, cross-built on this Linux host, with HakiOS board drivers |
| Boot chain | `boot_a` → `mtkloader` (bare metal) → XNU kernel + ramdisk → `launchd.ios` |
| Root today | a **~5 MB small root** ramdisk, built by `tools/mk-root.sh` |
| Root wanted | the full Darwin volume from flash (`root_apfs=ufs_darwin`), as the reference tree's phone uses |
| Reference | `refs/hakios-xnu-kernel` — the working MT6768 port (upstream, read-only) |

**The distinction that decides everything below:** the reference tree's phone boots a *full
Darwin image from flash*. This port boots a *small ramdisk root*. Every difference in
behaviour between them comes from that, not from the hardware.

---

## 1. Where each area stands

Legend: **works** = seen working on the phone. **partial** = the path below the hardware
works, but the top is not proven. **missing** = never seen working.

### 1.1 Boot and platform

| Area | State | Evidence |
|---|---|---|
| Loader → XNU handover | **works** | Apple's `launchd` runs as PID 1; `backboardd` and `SpringBoard` start; the log comes back to the loader in pstore |
| GIC-v3, SMP | **works** | 4 A55 + 4 A78 online, `cpus=4` runs |
| CPU frequency policy | **partial** | the big cores run at the clock the loader left (1790 MHz); per-cluster DVFS (apmixed + an OPP table per cluster) is not implemented — the plan puts it first |
| All 8 cores | **partial** | the tree's DT numbers the cores skipping 103; the kernel is asked for `cpus=N`, and the phone's own numbering has to be matched |
| Reboot / panic recovery | **partial** | the log is in pstore; a panic signature has never been captured |
| Interrupt controller | **works** | GIC-v3 at `0x0c000000`, EOImode handled by the loader |
| PSCI | **works** | `method = smc`, CPU_ON used by the loader |

### 1.2 Storage

| Area | State | Evidence |
|---|---|---|
| UFS controller | **works** | native UFS LUN 2, controller `0x11270000`, APFS root mounted |
| Partition table | **works** | the phone's GPT read; `userdata` 226 GiB runs to the end, `flashinfo` is last, nothing free |
| System image on flash | **missing** | not carved yet; needs `userdata` shrunk and a partition made |
| The iOS volume as root | **missing** | the small root is used instead; the full Darwin volume (`root_apfs=ufs_darwin`) is the target |
| Data volume (`/private/var`) | **missing** | `/private/var` is an empty ramfs each boot, so nothing persists: no home-screen layout, no app data, no first-boot state |
| Writes to flash | **missing** | `CONFIG_UFS_WRITE` and `CONFIG_VAR_STORE` are off; the loader has **no block-write service**, and the kernel's UFS driver refuses host-to-device writes. The reference tree has both |
| SD card | **missing** | no MT6893 SD host-controller driver exists |
| Files app | **partial** | the "On My iPhone" provider exists but points at the empty ramfs |

### 1.3 Screen and graphics

| Area | State | Evidence |
|---|---|---|
| Panel | **works** | 1080×2400 on `OVL0_2L` (`0x14001000`), framebuffer `0x76a10000`, pitch 4352 |
| Interface scale | **works** | 375×812 points at scale 3 (`display-scale=3`), verified at runtime |
| Renderer | **works** | Mesa software GLES (`gl=1`): 190 `hakios-gl` lines, 1000+ frames, an IOSurface at 1125×2436, presented (`swap 1 layer 0: surface 4 (1125x2436 'BGRA')`) |
| Metal / Mali-G77 | **missing** | Mesa's software GLES stands in; the Mali-G77 MC9 (`valhall-1691526.wa`) path is not brought up. The user wants METAL forked onto Mali and named **METAL-Mali** |
| Home screen / wallpaper | **missing** | the picture on the panel is Darwin's **boot UI** (`BKBootUIPresenter`, `BKDisplayRenderOverlayAppleLogo`) — a 192×240 Apple logo at `@3x`, sustained, not late. The standard still wallpapers are already on the phone at `/Library/Wallpaper` and the software renderer can draw them; what is missing is the first-boot state that dismisses the overlay |
| Lock screen, Notification Center, Control Center | **missing** | never tested interactively; all behind the overlay |
| Setup Assistant | **partial** | deliberately bypassed by injection; `SetupDone = True` is installed every boot (the skeleton's 8 preference files); but it cannot persist without `/private/var`, and activation fails on Secure Enclave (`0xe00002c7`) |

### 1.4 Input

| Area | State | Evidence |
|---|---|---|
| Touch hardware (loader) | **works** | the loader reads the FT3681 on I2C0 (`chip_id=0x5662`, fw `0x44`, vendor `0x8d`) and a **real finger** (`event=0x0 x=9482 y=30656 area=16` → 593×1916) |
| Touch driver in the kernel | **partial** | `kernel/bsd/dev/hakios.ft3681.c` runs (58,800 reads, 0 failures) but every report is byte-identical and matches the panel's chip-information block, not a touch frame; one sample posted, x saturated at 65535 |
| Touch path below the hardware | **works** | FT3681 → `/dev/hakios-touch` → `libhakios.dylib` → backboardd's virtual HID; backboardd counts the digitizer's events and the unlock notification is posted |
| Buttons (power, volume) | **missing** | the PMIC arbiter is mapped but nothing is read from it |
| Touch IRQ | **missing** | GPIO 21 / GIC SPI 245 is known but the driver polls every 5 ms instead |

### 1.5 Power, audio, radio

| Area | State | Evidence |
|---|---|---|
| Battery gauge | **works** | the loader publishes the phone's own gauge reading at `0x80240000`; the kernel polls it every 5 s and the sysctls carry SOC, mV, mA, °C |
| Charging | **missing** | no charger HAL |
| Thermal | **missing** | no thermal driver |
| Sensors | **missing** | accelerometer, gyro, magnetometer, proximity, light — none |
| Haptic | **missing** | — |
| Audio (speaker, mic, headset) | **missing** | the board's audio is its own subsystem (`adsp_common@10800000`, `mt6885-afe-pcm@11210000`); nothing driven |
| Wi-Fi | **missing** | this board is the **conninfra** generation (`conninfra`, `wlan_drv_gen4m`); the reference tree's BTIF/STP path does not carry over. Facts and plan in `docs/wifi-mt6893.md` |
| Bluetooth | **missing** | — |
| Cellular | **missing** | — |
| Camera | **missing** | — |
| Torch | **missing** | — |
| Network (Ethernet over USB) | **missing** | — |

### 1.6 Host channel and tooling

| Area | State | Evidence |
|---|---|---|
| Host → iOS command channel | **partial** | the transport is proven (`extra=cmd=...` reaches `kern.bootargs`) but the output path has not come back end to end |
| USB | **missing** | the phone leaves the host's USB entirely while iOS runs; there is no iOS USB gadget stack |
| Panel photograph | **works** | the loader photographs the panel twice — before XNU and when it comes back (`shot_panel`) — and `tools/fbshot.py` makes a PNG of it |
| Build from source | **works** | `tools/build-kernel-linux.sh`; the kext symbol-set steps always fail, which is expected |
| Reproducible historical kernel | **missing** | `tools/patch-kernel-mt6893.py` reproduces the old prebuilt byte for byte, but the source-level rebuild of it does not exist (`tools/patch-kernel-mt6893-rebuilt.py`, `docs/host-build.md`) |

---

## 2. The critical path

Ordered by what unblocks the most.

### 2.1 Make the source kernel complete — *in progress*

The kernel being flashed is now the **source-built** one. The source tree carries only
**2 of the 13 byte-level patches** the prebuilt had. Those 13 are the specification of
what this phone needs, and 11 are still to be converted to source:

- `proc_set_task_ruse_cpu`: the rusage printf is a nop
- `ufs_native_map`: the LUN list is always read
- **`hc_uefi_slot0`: no slot to look at** ← *this one is the current blocker*
- `ramfs_mount` and `ramfs_store`: the root is filled by the loader, not by the kernel's writer
- `IOSurfaceRootUserClient`: one flag
- `IOMobileFramebuffer::show`: the check this phone does not do

`hc_uefi_slot0` is why **the storage dies 48 s into every run**: the native UFS driver
refuses ("UEFI's command descriptor is not where it was"), storage falls back to asking the
loader, and our loader has **no request service** to answer it. The reference tree's loader
(`xnuloader/loader.c`) has one; ours does not.

*Done so far:* the UFS controller's address (`0x1d84000`, the reference board's, → the
phone's `0x11270000`), the LUN mask and LUN search, and the code-signing page shift.

### 2.2 Touch

Two candidate causes, both testable in one 120-second run each:

1. The kernel's driver never resets the chip or powers its rail. The loader's sequence is
   assert reset → power MT6360 LDO2 → release reset → read. The kernel does none of it.
2. The cache handling of the RX buffer differs from the loader's proven path (the kernel
   invalidates where the loader only cleans), and the kernel's RX buffer is a page split at
   `+256` where the loader's are separate aligned arrays.

The panel's report is byte-identical to the **chip-information block** the loader read
earlier in the same boot, which points at the transfer path rather than at the panel's
state.

### 2.3 The full Darwin volume as root

This is what makes the rest possible. It needs, in order:

1. A `darwin` partition carved out of `userdata` (`tools/userdata-carve.py`,
   `tools/mkdatavol.py` in the reference tree; the phone's GPT backup is in
   `backup/sdc-gpt-primary.bin`).
2. The loader's **block-write service** — ours has none; the reference's
   `xnuloader/loader.c` does.
3. `CONFIG_UFS_WRITE=y` and `CONFIG_VAR_STORE=y`, and bootargs
   `ufs_write=1 var_hfs=ufs_darwin_data`.
4. `root_apfs=ufs_darwin ufs=2` instead of `root_apfs=ufs_xnusys`.
5. The cryptexes and the first-boot applications staged on flash, and the APFS container
   that the reference tree's phone has.

### 2.4 The screen

With the full volume and a persistent `/private/var`, the first-boot state that dismisses
the boot UI should appear, and with it the home screen and the standard wallpapers. Until
then the overlay covers everything. The renderer, the scale and the wallpapers themselves
are already proven or already present.

### 2.5 The radios and the rest

Wi-Fi first (it is the most used, and the facts are already gathered in
`docs/wifi-mt6893.md`), then the camera and the audio subsystem, then the sensors, then
bluetooth and cellular.

---

## 3. The board's facts, so nobody has to rediscover them

From the phone's own device tree (`refs/fdt-x6739.dtb`), not from the reference board:

| | |
|---|---|
| SoC | `MT6893Z_T/CZA`, `compatible = mediatek,mt6893` |
| Memory | base `0x40000000`, 8 GiB |
| CPUs | 4× `arm,cortex-a55` + 4× `arm,cortex-a78`, `enable-method = psci` |
| GIC | `arm,gic-v3`, `0x0c000000` + `0x40000`, `0x0c040000` + `0x200000` |
| PSCI | `arm,psci-1.0`, `method = smc` |
| Storage | `/soc/ufshci@11270000`, `mediatek,mt8183-ufshci` — **UFS** |
| Display | `mediatek,mt6893-disp` @ `0x14116000`; DSI0 `0x1400e000`, DSI1 `0x1410e000`; panel `nt37703,fhdp,dsi,vdo` |
| Touch | FocalTech FT3681 on I2C0 (`0x11015000`), slave `0x38`; SCL0 = GPIO 204, SDA0 = GPIO 205, Reset = GPIO 20, IRQ = GPIO 21 (GIC SPI 245) |
| Touch rail | MT6360 LDO2 (`vtp`, 3.3 V) on I2C5 — must be enabled or the chip NACKs |
| I2C0 | v2 register table, slave address at `0x04`, parent `i2c_sel` 124.8 MHz; 400 kHz timings TIMING `0x0419`, LTIMING `0x0123`, EXT_CONF `0x5801`, SDA_TIMING `0x100a` |
| GPU | Mali-G77 MC9 (`valhall-1691526.wa`) |
| Framebuffer | `OVL0_2L` @ `0x14001000`, `L0_ADDR 0x76a10000` |
| Battery | gauge page PA `0x80240000`, magic `0x2154544142574452`, version 1, page `0x4000` |
| CPU ranges | A55 500–2000 MHz, A78 437–2600 MHz, prime 659–3000 MHz |

---

## 4. The rules that hold

* Every claim comes from the phone or from the phone's device tree. Never from the
  reference port's addresses, never from documentation written for another SoC.
* Nothing is "working" until it has been seen on the panel or in a log. `device.conf` says
  `missing` for every area that has not.
* Do not put the phone's current ROM name, its brand strings, or any `getprop` property
  into these documents or into code: they are spoofed and change between flashes. Use the
  DT-derived identity (`X6739`, `mediatek,mt6893`) only.
* One root shell command per `adb` invocation — never `&&` or `;` inside one quoted
  `su -c` string; it silently drops root for everything after the first command.
* Keep scratch and build output on persistent storage, not a RAM-backed temporary filesystem.
* Do not commit to `refs/hakios-xnu-kernel` — it is upstream and read-only for device work.
* The phone leaves USB entirely for 2–4 min during an init-retry loop; poll for ~10 min
  before calling it dead. A dark screen after that needs a power cycle and a USB reseat.
* A successful I²C or chip-ID read is **not** proof of a real touch report.
* Successful build-time or loader-level success is **not** proof of a working feature.

---

## 5. Documents

| | |
|---|---|
| `docs/ios-screen-state.md` | the boot UI overlay, the wallpaper, the panel measurements |
| `docs/port-plan.md` | the port, step by step |
| `docs/mt6893-facts.md` | the evidence for every number above |
| `docs/touchscreen-ft3681.md` | the FT3681's architecture |
| `docs/wifi-mt6893.md` | the conninfra Wi-Fi facts and plan |
| `docs/ios-firmware.md` | the iOS image: where it comes from, how it is carved |
| `docs/kernel-source-patches/` | the source-level patches, and the reasoning |
| `device.conf` | the per-area status, in the common tree's format |
