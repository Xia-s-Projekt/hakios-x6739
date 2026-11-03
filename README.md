# hakios-x6739

HakiOS for the **X6739**: MediaTek **MT6893** (MT6893Z_T/CZA, Dimensity 8050), Mali-G77 MC9,
**UFS** storage, 8 GB of RAM, 1080×2400 @ 120 Hz.

This is the device repository. It follows the shape of `hakios-mt6768` (the first MediaTek phone
of the project, Infinix Hot 20 X6826B, MT6768): a bare-metal loader that chains in front of the
phone's own Android kernel, brings up what XNU cannot, builds XNU's device tree and boot
arguments, starts the kernel, and brings the phone back to Android afterwards.

**The port boots to the iOS 16.5 home screen.** Native UFS, four CPUs, display, battery and touch
work. Graphics still use Mesa softpipe on the CPUs rather than the Mali-G77, so interaction is
slow; Wi-Fi, audio and the remaining hardware drivers are still missing.

## The difference from the reference port

The X6739 is a **different SoC** from the reference MediaTek port, not a variant of it. What carries
over is the *shape* — the loader, the chain-in-front-of-Android boot, the run scripts, the log in
pstore. What does not carry over:

| | reference (MT6768) | this phone (MT6893) |
|---|---|---|
| cores | 4×A53 + 3×A55 + 1×A72 | 4×A55 + 4×A78, and the tree's numbering skips 103 |
| storage | eMMC through MSDC0 (`0x11230000`), read with CMD17/CMD18 | **UFS** (`ufshci@11270000`) — there is no eMMC path |
| interrupt controller | GIC v2 | **GIC-v3** |
| GPU | Mali-G52 MC2 | Mali-G77 MC9 (`valhall`) |
| display | LK's framebuffer, 720×1640 | `mt6893-disp` pipeline, 1080×2400, DSI0+DSI1, a Tianma NT37705 panel |
| touch | OmniVision TD4160 over SPI2 | **FocalTech FT3681 over I2C0** (`0x11015000`, slave `0x38`; SCL0=GPIO 204, SDA0=GPIO 205, reset=GPIO 20, IRQ=GPIO 21) |
| CONNSYS | the MT6768's "connac1x": a microcontroller in the SoC, spoken to over BTIF/STP | the **conninfra**: a separate conn-infra microcontroller, the radio over AXI and SPI, and the `soc3_0` firmware — see [docs/wifi-mt6893.md](docs/wifi-mt6893.md) |
| boot partition | 32 MiB | 64 MiB |
| ramoops | console `0x4d09f000`, pmsg `0x4d0e0000` | console `0x480ef000`, pmsg `0x48130000` |

The details, and every number they rest on, are in [docs/mt6893-facts.md](docs/mt6893-facts.md) —
all of it read out of the phone's own device tree and its own kernel command line.

## Layout

| path | what |
|---|---|
| `mtkloader/` | the bare-metal loader: MMU off, the SoC's bring-up, XNU's device tree, the boot arguments |
| `kernel/` | this phone's drivers in XNU, as paths of the kernel tree (`kernel/README.md`) |
| `tools/` | one run (`xnu-cycle.sh`), flashing, probes, log readers, the device-tree readers, the iOS firmware fetcher |
| `docs/` | how each part was found, run by run; the plan (`docs/port-plan.md`); the iOS firmware ([docs/ios-firmware.md](docs/ios-firmware.md)); the touchscreen ([docs/touchscreen-ft3681.md](docs/touchscreen-ft3681.md)) and its handoff ([docs/handoff-touch-i2c0.md](docs/handoff-touch-i2c0.md)) |
| `device.conf` | the phone and each component's state (`works` / `partial` / `missing`) |
| `pull-firmware.sh` | the phone's firmware out of the user's own phone into `fw-*/` |

## What it needs

- this phone with an unlocked bootloader, and rooted Android;
- the common HakiOS tree (`hakios-xnu-kernel`): the kernel build, the user space, and the loader's
  `identity.h` made from the user's own IPSW;
- clang, plus the zig `ld.lld` the common tree provides;
- `adb` and `fastboot`.

The iOS files themselves come out of Apple's archive for the iPhone X (iOS 16.5, 20F66) with
`tools/fetch-ios.sh`; see [docs/ios-firmware.md](docs/ios-firmware.md).

## The rules

1. **Nothing of Apple's in the tree**: no iOS binary, image, cache, shader library, font, picture
   or screenshot, and no header derived from iOS firmware. Describe what the screen showed in
   words.
2. **Nothing of the vendor's**: no firmware (touch, CONNSYS, SCP, GPU), no partition image, no
   device tree taken off a phone. `pull-firmware.sh` fetches them into `fw-*/`, which git ignores.
   The device tree is read with the tools in `tools/` and its numbers are quoted, but the tree
   itself is not committed.
3. **No personal data**: no device serial numbers (the scripts read `HAKIOS_SERIAL`), no
   home-directory paths, no tokens, no Wi-Fi names or passwords.
4. **Licenses**: `kernel/` is APSL 2.0; everything else is MIT. Do not relabel XNU-derived code as
   MIT. MediaTek's and Linux's GPL sources may be read for register addresses and hardware order
   and named where used, never copied.
5. **Names**: new code uses `hakios_*`, `kern.hakios.*`, `/dev/hakios-*`.
6. **Commit messages say what the phone did** in the run that proved the change (a log line, a
   number), not only what the code does.

## Working on the phone: safety

- Flash **only `boot_a`**. Never write slot B, and no partition other than `boot_a` and the
  system-image partition.
- Always pass the serial (`adb -s`, `fastboot -s`); a second phone may be connected.
- Keep the user's stock `boot_a` image; it is how the phone is restored.
- Runs are short (200-600 s) and one at a time. Do not raise CPU or GPU voltages.
- The storage driver is read only by construction; keep it so unless the owner asks otherwise.

## Tools

`tools/mtk_fdt.py`, `tools/mtk-prop.py`, `tools/mtk-soc-facts.py` and `tools/mtk-subsystems.py`
read the phone's own device tree and print the numbers a port needs. They are pure Python and
need no `dtc`:

```sh
python3 tools/mtk-soc-facts.py device-tree.dtb     # the SoC, memory, CPUs, GIC, PSCI, UFS
python3 tools/mtk-subsystems.py device-tree.dtb    # the addresses of the subsystems
python3 tools/mtk-prop.py device-tree.dtb /memory reg   # one property, as words
```

`tools/storage-read.sh` reads the phone's partition layout and UFS state off the phone (read
only).

To get the tree off the phone (it is readable only as root):

```sh
adb shell su -c 'cp /sys/firmware/fdt /data/local/tmp/fdt.dtb'
adb shell su -c 'chmod 644 /data/local/tmp/fdt.dtb'
adb pull /data/local/tmp/fdt.dtb
```

The device tree is the phone's own and is **not** committed.
