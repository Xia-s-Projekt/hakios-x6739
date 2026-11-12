# AGENTS.md

Notes for AI coding agents (and people) working in this repository: what it is, how it is laid
out, and the rules that are not obvious from the code. Read it before changing anything.

## What this is

HakiOS for the **X6739**: MediaTek **MT6893** (MT6893Z_T/CZA, Dimensity 8050), Mali-G77 MC9,
**UFS** storage, 8 GB of RAM, 1080×2400 @ 120 Hz.

This is the device repository. It follows the shape of `hakios-mt6768` (the first MediaTek phone
of the project, Infinix Hot 20 X6826B, MT6768): a bare-metal loader that chains in front of
Android's kernel in `boot_a`, brings up what XNU cannot, builds XNU's device tree and boot
arguments, starts the kernel, and brings the phone back to Android afterwards. The common kernel,
user space and Metal driver live in the organisation's common repositories (`hakios-xnu-kernel`,
`hakios-metal`).

**The port boots to the iOS home screen.** What works and what does not is in `device.conf`.

## The one thing to know before writing any code

This is **a different SoC** from the reference MediaTek port, not a variant of it. The reference's
*shape* transfers; its *numbers* do not. The differences that decide the work:

| | reference (MT6768) | this phone (MT6893) |
|---|---|---|
| cores | 4×A53 + 3×A55 + 1×A72 | 4×A55 + 4×A78, and the tree's numbering skips 103 |
| storage | eMMC through MSDC0 (`0x11230000`), read with CMD17/CMD18 | **UFS** (`ufshci@11270000`) — there is no eMMC path |
| interrupt controller | GIC v2 | **GIC-v3** |
| GPU | Mali-G52 MC2 | Mali-G77 MC9 |
| display | LK's framebuffer, 720×1640 | `mt6893-disp` pipeline, 1080×2400, DSI0+DSI1, `nt37703` panel |

Every number this port uses comes out of the phone's own device tree, read with the tools in
`tools/`. **Do not carry an address over from the reference port.** The tree is the truth.

## Layout

| path | what | license |
|---|---|---|
| `mtkloader/` | bare-metal loader, runs with the MMU off before XNU | MIT |
| `kernel/` | this phone's XNU drivers, as paths of the kernel tree (`kernel/README.md`) | APSL 2.0 |
| `tools/` | one run (`xnu-cycle.sh`), flashing, probes, log readers, the device-tree readers | MIT |
| `docs/` | how each part was found, run by run | MIT |
| `pull-firmware.sh` | the phone's firmware out of the user's own phone into `fw-*/` | MIT |
| `device.conf` | the phone and each component's state (`works` / `partial` / `missing`) | |

How a boot goes: the loader is chained in front of Android's kernel in `boot_a`
(`tools/chainboot.py`, `mtkloader/build_flashed.py`). Android boots normally; `tools/boot-xnu.sh`
asks it for one XNU boot; the loader brings up what XNU cannot (touch firmware over SPI, PMIC,
clocks), builds XNU's device tree and boot-args, and starts the kernel. The kernel's root is the
iOS system image in the `xnusys` partition, read off the UFS as needed (`emmc_root=1`'s shape,
for UFS), with `rd=md0` kept for libignition. A run ends after `XNU_SECS`, the watchdog returns
the phone to Android, and the log is read back from pstore
(`/sys/fs/pstore/console-ramoops-0`).

Every hardware step is behind a boot-arg and does nothing without it (`mali=1`, `touchscreen=2`,
`buttons=2`, `mtk_cpufreq=3`, `scp_step=N`, `connsys_step=N`, `emmc_root=1`). New hardware work
follows the same pattern: numbered steps, reading only first, each step's result said in the log.

## The plan

[docs/port-plan.md](docs/port-plan.md) is the order the work happens in, and what each step
needs. [docs/mt6893-facts.md](docs/mt6893-facts.md) is what the tree says.

## Rules

1. **Nothing of Apple's in the tree**: no iOS binary, image, cache, shader library, font, picture
   or screenshot, and no header derived from iOS firmware. Describe what the screen showed in
   words.
2. **Nothing of the vendor's**: no firmware (touch, CONNSYS, SCP, GPU), no partition image, no
   device tree taken off a phone. `pull-firmware.sh` fetches them into `fw-*/`, which git ignores.
   The device tree is read with the tools in `tools/` and its numbers are quoted, but the tree
   itself is not committed.
3. **No personal data**: no device serial numbers (scripts read `HAKIOS_SERIAL`), no home-directory
   paths, no tokens, no Wi-Fi names or passwords. The serial given to iOS is the placeholder
   `XNU_SERIAL`.
4. **Licenses**: `kernel/` is APSL 2.0; everything else is MIT. Do not relabel XNU-derived code as
   MIT. MediaTek's and Linux's GPL sources may be read for register addresses and hardware order
   and named where used, never copied.
5. **Names**: new code uses `hakios_*`, `kern.hakios.*`, `/dev/hakios-*`. Files copied from the
   reference port keep the former `redwood_*` prefix until they are merged; do not rename them
   piecemeal.
   The **image** is `zenith`: that is the name the project calls itself, and it is what the
   identity strings say (the audio plug-in's manufacturer, the HID service's manufacturer, the
   test programs' keys, the input thread's name).
   The **mount point** is still `/redwood`, and that is not an oversight: the directory the
   image is mounted on is inside the *prebuilt kernel's* own root filesystem
   (`mach.development.mt6893`, whose root has the directory `redwood` in it), so the path can
   only change by rebuilding the kernel from source. `redwood` is also the name of the
   *reference phone* (the POCO X5 Pro, SM7325) and of the files copied from the reference port:
   those keep it, and only those.
6. **Commit messages say what the phone did** in the run that proved the change (a log line, a
   number), not only what the code does.

## Working on the phone: safety

- Flash **only `boot_a`**. Never write slot B (`boot_b`, `system_b`, ...) or any partition other
  than `boot_a` and `xnusys`; `xnusys` is written only by `tools/system-flash.sh`.
- Always pass the serial (`adb -s`, `fastboot -s`); a second phone may be connected.
- Keep the user's stock `boot_a` image; it is how the phone is restored.
- Runs are short (200-600 s) and one at a time. Do not raise CPU or GPU voltages.
- The storage driver is read only by construction; keep it so unless the owner asks otherwise.
- Nothing here needs, or should touch, Apple's servers or accounts.

## Reading the phone's device tree

The tools are pure Python and need no `dtc`:

```sh
python3 tools/mtk-soc-facts.py device-tree.dtb     # the SoC, memory, CPUs, GIC, PSCI, UFS
python3 tools/mtk-subsystems.py device-tree.dtb    # the addresses of the subsystems
python3 tools/mtk-prop.py device-tree.dtb /memory reg   # one property, as words
```

To get the tree off the phone (it is readable only as root):

```sh
adb shell su -c 'cp /sys/firmware/fdt /data/local/tmp/fdt.dtb'
adb shell su -c 'chmod 644 /data/local/tmp/fdt.dtb'
adb pull /data/local/tmp/fdt.dtb
```

## Building and running

What it needs:

- the common HakiOS tree: kernel build, user space, `xnuloader/identity.h` made from the user's
  own IPSW;
- clang, plus the zig `ld.lld` that the common tree provides;
- `adb` and `fastboot`;
- this phone with an unlocked bootloader, rooted Android.

```sh
export HAKIOS_SERIAL=...
./pull-firmware.sh
XNU_SECS=300 XNU_ARGS="cpus=4 touchscreen=2 buttons=2 mali=1 mtk_cpufreq=3 metal=1 camtl=1" tools/xnu-cycle.sh
```

The boot partition is 33,554,432 bytes and the image is close to it: check the size line that
`build_flashed.py` prints; the loader is built `-Os` for that reason.

The scripts still assume the layout of the tree they came from (`mtkloader/` and `tools/` at the
top of the common tree). Moving them under the common repository's `devices/<name>/` is pending.

## Reading a run's log

- Kept lines (`kept:` / `!`) are a 96-line ring the kernel repeats every minute: put results
  there.
- `tools/fbshot.py` turns `fbshot=` lines into a picture of the screen (for your own checks only:
  never commit it).
- `tools/frames.py` gives frame timing; `tools/cache-name.py` names shared-cache addresses.
- `kern.hakios.ustack` and SpringBoard's `sb_prof=` sampler find what a process spends its CPU
  on.

## State, briefly

What works and what does not is in `device.conf` and `README.md`.
