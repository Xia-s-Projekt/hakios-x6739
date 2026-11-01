# The X6739 port, step by step

What has to happen for HakiOS to run on the X6739, in the order that gets to a boot with nothing
of the phone's, and then to the phone's own hardware one piece at a time.

The reference for the *shape* of every step is `hakios-mt6768` (the first MediaTek phone of the
project, Infinix Hot 20 X6826B, MT6768) — the only port whose approach this one follows. The
reference for the *numbers* is this phone's own device tree.

## 0. What is already done

- the phone's device tree is read and its facts are written down
  ([docs/mt6893-facts.md](mt6893-facts.md));
- the tools that read a device tree without `dtc` are in `tools/`;
- the loader's platform constants are in `mtkloader/mt6893.h`, each with where its number came
  from;
- **the loader itself is forked, adapted, and proven on hardware**: `mtkloader/mtkloader.c` builds,
  chains to XNU, and hands control back to Android cleanly;
- **the first boot with nothing of the phone's succeeded**: XNU booted to userland, ran Process 1
  (`/sbin/launchd -> /sbin/init`), passed all 35 userland checks with 0 failures, verified the
  framebuffer console display, and returned to Android with full pstore logging;
- the run tools (`tools/xnu-test.sh`, `tools/xnu-cycle.sh`, `boot-xnu.sh`, `chainboot.py`, `gpt-carve.py`, …) and
  `pull-firmware.sh`.

### What the forked loader already does, and what is still the reference board's

Carried over unchanged (the same work on every board): the payload parsing, the XNU symbol
patching (the PL011 console, the GIC EOImode, the return stub), the panic log, the chain back to
Android, the boot arguments' shape.

Changed for this board:

| | reference (MT6768) | here (MT6893) |
|---|---|---|
| XNU's window in DRAM | `0xc0000000`, 0xfe000000 | high in this board's 8 GiB (`mt6893.h`) |
| the core order | `{0, 7, 6, 1, 2, 3, 4, 5}` | `core_mpidr` in `mt6893.h`: the prime core first, then the A78s, then the A55s |
| the GIC | v2 layout, CPU window 0x100000 | **GIC-v3**: distributor `0x0c000000`+`0x40000`, CPU `0x0c040000`+`0x200000` |
| the display | LK's framebuffer at `0x787b0000`, pitch 2944 | the OVL0 the phone's preloader leaves, **read out of the hardware** and checked against the 1080×2400 panel |
| the storage | the loader reads the system image over MSDC0 | the kernel reads it (`ufs_root=1`); the loader's eMMC path is compiled out |
| the big cores' clock | 1087 → 1176 MHz at the same 800 mV | **nothing is reprogrammed** — the tree's own frequencies are used and the kernel's driver sets the clock |
| the touch | the OmniVision TD4160 (TCM) over SPI2 | **not the reference's controller**; the path is disabled until this phone's touch is found |
| the console UART | `0x11002000` | `0x11002000` — the same (the tree's `serial@11002000`) |
| the boot request | `DARWIN-REDWOOD:BOOT-XNU` in the pmsg zone | `DARWIN-X6739:BOOT-XNU`, and the zone is scanned with **no header** (this phone's pmsg zone is Android's own format) |
| the console log | a header of ours ("DBGC") at the front of the zone | **no header**: this phone's console zone is plain text and belongs to Android's pstore driver |
| ramoops | console `0x4d09f000`, pmsg `0x4d0e0000` | console **`0x480ef000`**, pmsg **`0x48130000`** (from `/proc/iomem`) |
| the boot partition | 32 MiB | **64 MiB** (`boot_a`) |

Still to do in the loader:

- the exact top of the free DRAM (the window's size and base are right in shape, not yet
  confirmed on the phone);
- the framebuffer the phone's preloader leaves (the loader reads the OVL0 layer's address and
  pitch, but the layer layout of this engine has to be confirmed);
- the display engine's registers (the offsets inside OVL are the reference's until they are
  confirmed on this board);
- the UFS controller's state at handover to the kernel.

## 1. The loader (the platform layer)

The loader is bare-metal code that runs before XNU, with the MMU off, chained in front of
Android's kernel in `boot_a`. It brings up what XNU cannot, builds XNU's device tree and boot
arguments, starts the kernel, and brings the phone back to Android afterwards.

For the X6739 the loader has to be **written for MT6893**, not edited from the MT6768 one. The
parts:

| what | the reference does | what this phone needs |
|---|---|---|
| entry and chain to Android | `start.S`, `chain_stub` | the same shape; the chain offset and the image layout come from `tools/chainboot.py` |
| the XNU window in DRAM | `0xc0000000`, sized by hand | **placed high in this board's 8 GiB**, clear of what the phone's own system uses — the exact top is not yet confirmed on the phone |
| the core order | `{0, 7, 6, 1, 2, 3, 4, 5}` for 4×A53 + 3×A55 + 1×A72 | **4×A55 + 4×A78**, and the tree's own numbering (it skips 103) |
| the GIC | v2 layout, told to the kernel as 0x10000 + 0x100000 | **GIC-v3**: distributor `0x0c000000`+`0x40000`, CPU `0x0c040000`+`0x200000`, as the tree says |
| PSCI | SMC through ATF | the same (`arm,psci-1.0`, `method = smc`) |
| storage | eMMC through MSDC0, CMD17/CMD18 | **UFS** — no eMMC path exists on this board |
| the display | LK's framebuffer on OVL0 layer 0, 736 px pitch | **a different pipeline**: `mt6893-disp`, 1080×2400, DSI0+DSI1, a Tianma NT37705 panel |
| the console | UART at `0x11002000` | the same address; the tree's UART is the same compatible |

### 1a. The storage, decided first

This is the decision that shapes the rest, and it has to be made before the loader's storage code
is written.

The reference port reads the iOS system image (2.4 GB) out of the eMMC. It can, because the card
is a command/response device: the preloader left it set up, and CMD17/CMD18 are enough to read
blocks. **UFS is not like that.** It is a link to a device with its own firmware, its own
descriptor table and its own command queue; reading a block means driving a doorbell and waiting
for the device to answer.

Two ways out:

- **(A) the kernel owns the root.** The system image stays on the phone, in a partition, and the
  kernel's own UFS driver reads it as it needs it. This is the reference port's `emmc_root=1`
  shape, and the reference port already uses it for the system image — it is the cheaper path,
  because the negotiation is paid once, by the kernel, with a real driver, instead of by a
  bare-metal loader with no OS underneath it.
- **(B) the loader reads it.** The loader negotiates the link and reads the image into DRAM. This
  costs 2.4 GB of the phone's 8 GiB, three and a half minutes of every boot, and a bare-metal UFS
  driver — including getting hold of the descriptor, which on this platform sits behind the
  phone's secure world.

**Chosen: (A).** The loader does not read the system image; the kernel does. The loader's job is
to leave the UFS controller in a state the kernel's driver can take over, and to tell the kernel
where the image is.

That means the kernel needs a UFS driver for the MT6893 — which does not exist yet, in the common
tree or anywhere else in the project.

### 1b. The loader's own code

- `mtkloader/mt6893.h` — the platform constants (done, to be confirmed on the phone).
- `mtkloader/start.S` — the entry, the chain stub, the vectors, PSCI calls, the MMU (done).
- `mtkloader/mtkloader.c` — the main flow, the device tree the kernel gets, the run (done, to be
  completed).
- `mtkloader/mtkdbg.h` — the console and the ramoops output (done for this board's layout).
- `mtkloader/link.ld`, `mtkloader/build.sh` — the build (done). The build makes the console's
  8x16 font from a system font (`tools/mkfont8x16.c`, FreeType) and takes the loader's identity
  from the iPhone X's device tree (`tools/fetch-ios.sh identity`). The loader compiles and links
  to `mtkloader/mtkloader.bin`.

## 2. The kernel's UFS driver

A new driver in the common tree's `drivers/storage/`, reading the phone's UFS: reset the
controller, negotiate the link, read the device descriptor, and expose the partition holding the
system image as a block device and as the root. It is the largest single piece of new code in this
port.

The phone has the vendor's own driver to read against: `/vendor/lib/modules/mali_kbase_mt6893.ko`
is for the GPU, but the UFS driver is in the vendor kernel, not in `/vendor/lib/modules`. What
the tree says about the controller (`mediatek,mt8183-ufshci` at `0x11270000`, and the
`mcucfg`/`mmsram`/`dvfsp` nodes around it) is the starting point.

## 3. The first boot, with nothing of the phone's

The loader alone: DRAM, cores, the GIC, the framebuffer, XNU started with a minimal device tree,
launchd reached, the run ends by itself and the phone comes back to Android. **No phone driver
is in the kernel at this point.**

## 4. The drivers, one at a time

The reference port's order, each step read-only first, each step's result said in the log:

| order | driver | the X6739's difference |
|---|---|---|
| 1 | CPU frequency | the MT6893's DVFS is a different mechanism (apmixed, three clusters, and an operating-points table per cluster) |
| 2 | buttons, then touch | the reference board's touch is an OmniVision TD4160 over SPI2; this phone's is a **FocalTech** (`focaltech_ts_fw_00.bin`) and the tree's `/touch` is a GPIO interrupt with no controller behind it |
| 3 | storage | UFS, not eMMC (step 2) |
| 4 | GPU | the reference is Mali-G52 MC2; this is **Mali-G77 MC9** (`valhall-1691526.wa`) — a newer generation, so the bring-up sequence differs |
| 5 | audio and battery | both come through the audio DSP on the reference board; this board's audio is its own subsystem (`adsp_common@10800000`, `mt6885-afe-pcm@11210000`) |
| 6 | Wi-Fi | the reference drives CONNSYS over BTIF/STP; this board's is the **soc3_0** generation, not soc1_0 |

## 5. Record and publish

`status` lines in `device.conf` as each area is seen to work on the phone → the documentation's
device pages → the device repository of its own.

## The open questions

1. **Where does the phone's own system live in DRAM?** *Answered.* DRAM base `0x40000000` (8 GiB). Loader links at `0x40080000`, small root ramdisk at `0x48000000`, framebuffer at `0x76a10000`.
2. **What does the preloader leave running?** *Answered.* Display engine `OVL0_2L` is scanning at 1080x2400 (pitch 4352). GIC-v3 is initialized in EOImode=1 (loader clears to 0). UFS host controller is active and handed over.
3. **What is the touch controller, and on which pins?** *Answered.* FocalTech FT3681 (`TM_FT3681_00`) on I2C0 (`0x11015000`) at slave `0x38`. Pins: SCL0=GPIO 204, SDA0=GPIO 205, Reset=GPIO 20, IRQ=GPIO 21 (GIC SPI 245 = GIC interrupt 277). Full architecture in `docs/touchscreen-ft3681.md`, and the state of the work in `docs/handoff-touch-i2c0.md`.
4. **How is the I2C0 bus driven?** *Answered and implemented.* The controller uses the **v2**
   register table with the slave address at `0x04` (the phone's own module carries `mt6873_compat`,
   and that profile has `slave_addr_ver` unset). The clock parent is `i2c_sel` in TOPCKGEN at
   124.8 MHz (`univpll_d5_d4`); the gates are `impc_ap_i2c0_ro` (0x11008000 + 0xe04 bit 0),
   `ifrao_i2c0` (0x84 bit 11), `ifrao_ap_dma_ps` (0x8c bit 18) and `ifrao_apdma` (0xd4 bit 31). The
   timings for 400 kHz are TIMING 0x0419, LTIMING 0x0123, EXT_CONF 0x5801, SDA_TIMING 0x100a
   (`tools/i2c-timing.py`). SCL clock-stretch hang resolved by dropping `I2C_CTL_CLK_EXT_EN`. The
   hardware controller completes bus cycles cleanly (`DEBUGSTAT = 0x700`, `stat = 0x3`). Slave `0x38`
   NACKs because MT6360 LDO2 (`vtp`, 3.3V) on I2C5 must be enabled. Next step: power LDO2 and read
   FT3681 Chip ID. Full details in `docs/handoff-touch-i2c0.md`.
5. **Where is the system's partition, and how is it read?** *Half answered.* The phone's GPT has
   been read off the phone (see `docs/ios-firmware.md`): `userdata` is 226 GiB and runs to the end
   of the disk, with `flashinfo` the last partition and nothing free. The system's partition has
   to be carved out of `userdata`, which means shrinking it, and the image is read by the
   kernel's own UFS driver — which does not exist yet.
