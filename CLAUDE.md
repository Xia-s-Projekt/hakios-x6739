# CLAUDE.md

This file provides guidance to Coding CLI (the coding CLI) when working with code in this repository.

## What this repository is

**The X6739 device repository.** Everything here is about this one phone — MediaTek **MT6893**
(MT6893Z_T/CZA, Dimensity 8050), Mali-G77 MC9, **UFS**, 8 GiB RAM, 1080×2400 @ 120 Hz.

It holds the bare-metal loader (`mtkloader/`), the run/flash/probe scripts (`tools/`), this phone's
XNU driver files (`kernel/`), the docs, and `device.conf`.

The kernel tree, userland, APFS tooling and the common loader live in the **common tree**
`../refs/hakios-xnu-kernel` (override with `HAKIOS_COMMON`). `../refs/` is reference material —
read it, never commit it into this repository.

`AGENTS.md` and `README.md` here hold the project rules (no Apple/vendor material in the tree, MIT
vs APSL licensing, flash only `boot_a`, `HAKIOS_SERIAL` for the serial). Read them; they are not
repeated here.

## The state of the port — read this first

**The initial bare-metal boot and userland execution on this phone are WORKING.**
The loader has been flashed to `boot_a` on the attached X6739 device, successfully chained
to Darwin/XNU, brought up display console and GICv3 virtual timer, reached Mach/BSD userland,
executed Process 1 (`/sbin/launchd -> /sbin/init`), passed all 35 userland checks with 0 failures,
and cleanly returned to Android with full pstore console logs.

What is working:
- **Loader & Chaining**: `mtkloader` builds and chains to XNU; safe handover back to Android.
- **Ramoops / Pstore Logging**: 12-byte `ramoops_hdr` preserved across boots; pstore logs captured.
- **Display**: LK framebuffer scanned on `OVL0_2L` (`0x14001000`, `0x76a10000`, pitch 4352, 1080×2400); text console verified rendered and read back.
- **GICv3 & Timers**: GICv3 redistributor layout, `EOImode` cleared (`0x8400`), PPI 27 virtual timer running.
- **Userland Init**: Mach VM, devfs, APFS ramdisk root (`md0`), Process 1 userland checks (35/35 passed).

Next steps:
- the **kernel's UFS driver does not exist** — the largest single piece of new code in the port;
  until it does, the small root (`root.apfs`, 4 MB) is the root (`-DXNU_SMALL_ROOT=1`) and the big
  2.5 GB system image stays on the UFS where nothing can read it yet;
- the **system partition (`xnusys`) has not been carved** out of `userdata` (226 GiB) — there is no
  free space at the end of the disk, so `userdata` has to be shrunk (`tools/gpt-carve.py`);
- **Multi-core**: Bring up the 4×A78 + 4×A55 SMP cores via PSCI `CPU_ON`.
- **Drivers**: Buttons, touch (FocalTech), UFS, Mali-G77 GPU.

## How to boot it

Every run is: build the loader → pack it in front of Android's kernel in `boot_a` → flash
`boot_a` → Android boots → ask for **one** XNU boot → XNU runs → it comes back by itself → read
the log out of pstore. Slot b is never touched.

```sh
export HAKIOS_SERIAL=...                     # adb devices

./pull-firmware.sh                           # the phone's vendor firmware into fw-*/ (read only)
XNU_SECS=300 XNU_ARGS="cpus=1" tools/xnu-test.sh
```

`tools/xnu-test.sh` is the full run (loader flags go to the compiler; `XNU_NO_FLASH=1` skips the
flash so the image can be inspected). `tools/xnu-cycle.sh` is the same cycle with its own flag
set — but note it builds **without** `-DXNU_SMALL_ROOT=1` and reads only `console-ramoops-0`;
`xnu-test.sh` copies the whole pstore directory out, which is what you want when the loader's
own text is what you are hunting for.

The pieces, individually:

```sh
./mtkloader/build.sh                         # -> mtkloader/mtkloader.bin (link address 0x40080000)
./mtkloader/build.sh -DXNU_CORES=4 -DXNU_WATCH_SECONDS=300   # extra args go to the compiler

python3 tools/mkpayload.py -o build-payload.bin \
    --kernel $COMMON/prebuilt/mach.development.bcm2837 \
    --ramdisk $COMMON/userland/root.apfs \
    --cmdline "rd=md0 ufs_root=1 cpus=1"
python3 tools/chainboot.py backup/boot_a.img mtkloader/mtkloader.bin out.img build-payload.bin
python3 tools/strip-avb.py in.img out.img     # when the repacked image is refused

tools/boot-xnu.sh                            # ask for one XNU boot, via /dev/pmsg0
```

`build.sh` generates two headers it does not keep in git (`mtkloader/font8x16.h` via
`tools/mkfont8x16.c` + FreeType; `mtkloader/touchfw.h` via `tools/mktouchfw.py` from
`fw-touch/`), links with zig's `ld.lld` from the common tree, and needs
`$COMMON/xnuloader/identity.h` (made by `tools/fetch-ios.sh identity`).

Reading the phone's device tree (pure Python, no `dtc`; the tree itself is the phone's and is
never committed — `../refs/fdt-x6739.dtb` is the pulled copy):

```sh
python3 tools/mtk-soc-facts.py ../refs/fdt-x6739.dtb        # SoC, memory, CPUs, GIC, PSCI, UFS
python3 tools/mtk-subsystems.py ../refs/fdt-x6739.dtb       # the subsystem addresses
python3 tools/mtk-prop.py ../refs/fdt-x6739.dtb /memory reg # one property, as words
```

Other tools: `tools/gpu-probe.sh` (GPU probe — **broken**: it calls `mtkloader/build_flashed.py`,
which does not exist here), `tools/storage-read.sh` (GPT + UFS state, read only),
`tools/system-flash.sh` (writes `xnusys`), `tools/gpt-carve.py` (carves the partition, on the
host), `tools/fbshot.py` and `tools/frames.py` (log helpers). The kernel itself is built in the
common tree: `make kernel` there.

## How a boot actually happens

`boot_a` holds a MediaTek boot image whose kernel section is a gzip of: our loader, padded to
`CHAIN_OFFSET` (0x40000), then Android's own uncompressed `Image`. LK decompresses that to
0x40080000 and enters it with the MMU and caches off, so our loader is the first thing that runs
at that address. The loader's arm64 header carries the Image's length (0x28) and the XNUP
payload's offset (0x30) so it can find both.

Inside `mtkloader/`:

- `start.S` — the entry, the exception vectors, `jump_to_kernel` (enters XNU at EL1h), the
  `chain_stub` (moves Android's `Image` back down to 0x40080000 and starts it), `xnu_return_stub`
  (how the kernel comes back), `watcher_entry` (the second core), `mmu_on`, `dcache_clean_range`.
- `mtkloader.c` — everything else: the payload parse, the XNU symbol patch (PL011 console, GIC
  EOImode), the device tree and the boot arguments it writes for XNU, the chain back to Android,
  the panic/backtrace printers, the watcher's watchdog.
- `mt6893.h` — this board's constants, each with the source it came from.
- `mtkdbg.h` — the framebuffer console and the pstore (ramoops) logging.

The loader ends one of two ways. `chain_android()` puts Android's `Image` back and starts it;
`run_xnu()` instead prepares XNU (device tree, boot args, root) and jumps to it.
`mtkloader_main()` chooses: a boot menu, a one-shot request Android left in the ramoops pmsg
zone (`DARWIN-X6739:BOOT-XNU`, written by `tools/boot-xnu.sh`), or the default. XNU comes back by
itself — the kernel returns through `xnu_return_stub` after `XNU_RETURN_AFTER` seconds, or the
watcher resets the board — so a run always ends back in Android.

**The log** goes to the framebuffer console and to the phone's ramoops zones (console
0x480ef000, pmsg 0x48130000, dmesg 0x48090000 — from the phone's own `/proc/iomem`, *not* the
sizes in the bootargs, which do not match the real layout). Android's pstore driver reads them
back as `/sys/fs/pstore/console-ramoops-0`; `tools/xnu-test.sh` copies the files out before
anything else can consume them (Android removes the file once it has been read). On this phone the
console zone holds **plain text with no header of ours** and the pmsg zone uses Android's own
record format ("l0\0", a length, the text) — that is why the loader writes from the start of the
zone, keeps its cursor in a variable, and finds the boot request by scanning for the flag.

## What to know about the X6739

From the phone's own device tree and command line; `docs/mt6893-facts.md` has each of these with
its evidence, `docs/ios-firmware.md` has the firmware and the boot image.

| | |
|---|---|
| DRAM | base `0x40000000`, 8 GiB; the loader's XNU window is placed high in it (`0xc0000000` + `0x2e000000` in `mt6893.h`) — **not yet confirmed on the phone** |
| CPUs | 8: 4×A55 (MPIDR 0x000–0x300) + 4×A78, the prime core at MPIDR 0x700; the tree's numbering **skips 103**, so an index is not an MPIDR |
| GIC | **v3**: distributor `0x0c000000`+`0x40000`, CPU `0x0c040000`+`0x200000` |
| PSCI | `arm,psci-1.0`, SMC through ATF |
| storage | **UFS** at `ufshci@11270000` — there is no eMMC path in the tree at all; the reference port's `msdc.h` does not apply |
| console UART | `0x11002000` — the same address as the reference board |
| display | `mt6893-disp`, OVL0 `0x14000000` / OVL1 `0x14100000`, DSI0 `0x1400e000` / DSI1 `0x1410e000`, 1080×2400, a Tianma NT37705 panel (the tree's panel node says `nt37703`) |
| touch | **FocalTech** (`focaltech_ts_fw_00.bin`), and the tree's `/touch` is a GPIO interrupt with no controller behind it — the reference's OmniVision TD4160 over SPI2 is not this phone's touch |
| GPU | Mali-G77 MC9, firmware `valhall-1691526.wa` (the "valhall" generation) |
| CONNSYS | the **soc3_0** generation of firmware, not the reference board's soc1_0 |
| boot partition | `boot_a` / `boot_b`, **64 MiB** each — the reference's was 32 MiB, so the loader does not have to be `-Os` to fit |
| expdb | 128 MB — normal for this phone, and it holds **only** preloader/LK logs, never the loader's or the kernel's output |

**Never carry an address over from the reference port** (`../refs/hakios-mt6768-reference` is the
Infinix Hot 20 X6826B, MT6768). Its *shape* transfers — the loader that chains in front of
Android's kernel, the run scripts, the log in pstore — but every number is MT6768's. This is a
different SoC, not a variant: GIC-v3 not v2, UFS not eMMC, Mali-G77 not G52, FocalTech not TCM.

## The kernel drivers

`kernel/` holds this phone's XNU drivers as files of the kernel tree (APSL 2.0). They are
currently **placeholders** — the real work, in the reference port's order, is: the UFS driver
(largest), the touch, the GPU bring-up for Mali-G77, the buttons and the big cores' clock,
CONNSYS/SCP, audio and battery. They hook into the common kernel (its `bsd/conf/files`, the input
device `/dev/hakios-touch`, the display driver, `IOFindBSDRoot`), which lives in the common tree.

Every hardware step is behind a boot-arg and does nothing without it (`mali=1`, `touchscreen=2`,
`buttons=2`, `mtk_cpufreq=3`, `scp_step=N`, `connsys_step=N`, `ufs_root=1`). New hardware work
follows the same pattern: numbered steps, read-only first, each step's result said in the log.

## Evidence rules

Every claim about the phone comes from the device or from its device tree — never from the
reference port's addresses and never from documentation written for a different SoC.

Never put the phone's current ROM name, its brand strings, or any property name from `getprop`
into code or docs: they are spoofed and change between flashes. Use the DT-derived identity
(`X6739`, `mediatek,mt6893`) only.
