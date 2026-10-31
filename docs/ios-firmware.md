# The iOS 16.5 firmware, and what the port needs out of it

HakiOS runs Apple's own user space: XNU 16.5 (xnu-8796.121.2) with the files of iOS 16.5. Those
files come out of Apple's update archive for the **iPhone X** (`iPhone10,3` / `iPhone10,6`,
build **20F66**). The phone's own model has nothing to do with it — the identity the loader hands
to XNU is the iPhone's, so that the user space believes it is on the phone its files were built
for.

## Where it comes from

Apple publishes the update as one ZIP64 file of 6.2 GB on its CDN:

```
https://updates.cdn-apple.com/2023SpringFCS/fullrestores/032-85130/
  6AB0D10A-8AF9-4F6C-8CF5-A1B315CABE88/
  iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw
```

The common tree's `sysimage/fetch-from-ipsw.py` reads the archive's directory with HTTP range
requests and streams out **one member at a time**, so the 6.2 GB file itself is never downloaded.
`refs/ios/ipsw-get.py` is the same thing for several members at once, and is what
`tools/fetch-ios.sh` calls.

The four members the port needs:

| member | what it is | what it is for |
|---|---|---|
| `Firmware/all_flash/DeviceTree.d22ap.im4p` | the iPhone X's device tree (34 KB) | the loader's `identity.h`: the properties iOS reads (through MobileGestalt) to decide which phone it is — the model, the screen class, the artwork scale, the haptics, the camera's flash |
| `096-04088-077.dmg` | the root filesystem, 4.7 GB | the system image: `sysimage/select-system.py` chooses the part of it that goes into memory |
| `096-04666-078.dmg` | the OS cryptex, 2.7 GB | the dyld shared cache (the framework code iOS runs) |
| `096-03536-083.dmg` | the App cryptex, 10 MB | Safari's daemons and its settings, which the root's links reach through `/System/Cryptexes/App` |

## Getting them

```sh
cd /mnt/project/Misc/hakios/hakios-x6739
bash tools/fetch-ios.sh identity     # the device tree and the loader's identity.h
bash tools/fetch-ios.sh system       # the three images, and the system image out of them
```

`identity` writes `refs/hakios-xnu-kernel/xnuloader/identity.h` through the common tree's
`xnuloader/mkidentity.sh`. That script needs an LZFSE decoder (the device tree inside the im4p is
an LZFSE stream); it builds one from `github.com/lzfse/lzfse` if the system has none. Note that
the `lzfse` upstream Makefile has no `lzfse` target (only `all` and `install`), so the build line
the script uses (`make lzfse`) does not work against current upstream: build it with `make all`
and put the binary in `xnu-deps/host/bin/`.

`system` runs the common tree's `sysimage/fetch.sh`, which unpacks the root filesystem and the
cryptexes and lays them out the way `userland/build.sh` expects. It needs **7z** (7-Zip 23 or
newer — it reads APFS) and `cc`. It looks for the images under the names it uses internally
(`rootfs.dmg`, `cryptex-os.dmg`, `cryptex-app.dmg`), so `fetch-ios.sh` hard-links them there
before calling it. The `.dmg` files are deleted afterwards unless `KEEP=1` is set.

**Nothing of Apple's is in the repository.** The files land in `refs/ios/`, which
`.gitignore` excludes. The same is true of the identity header.

## What has been fetched and verified

All four members, with the sizes the archive's own directory gives:

| member | bytes | verified |
|---|---|---|
| `Firmware/all_flash/DeviceTree.d22ap.im4p` | 33,694 | yes — the fetch printed "33694 bytes (expected 33694)" |
| `096-03536-083.dmg` (App cryptex) | 10,485,760 | yes |
| `096-04088-077.dmg` (root filesystem) | 4,728,546,953 | yes |
| `096-04666-078.dmg` (OS cryptex) | 2,726,297,600 | yes |

The identity header was made from the device tree: `mkidentity.sh` reported
*"xnuloader/identity.h made from the device tree (186040 bytes)"*, and the header holds the
iPhone X's own identity — `model` = `iPhone10,3`, `manufacturer` = `Apple Inc.`, `compatible` =
`AppleARM64` and `iPhone10,3`, `device_type` = `bootrom`, plus the `/product` node's 61
properties (the screen class, the artwork scale, the haptics, the camera).

The system image (`userland/system.apfs`) has **not** been built yet: the unpacking step was
still running when this was written.

## The loader's build

The loader (`mtkloader/build.sh`) needs two headers that are made at build time and never
committed:

- `identity.h`, from the iPhone X's device tree — described above.
- `font8x16.h`, the console's 8x16 text font. The reference port rasterizes it with
  `tools/mkfont8x16.swift` (CoreGraphics, Menlo Bold 13pt), which needs macOS. This port uses
  `tools/mkfont8x16.c` instead: FreeType renders the same glyphs from a monospaced bold font
  (Liberation Mono Bold, standing in for Menlo), and the result is the same 8x16 1-bit cell
  layout the console expects — one byte per row, the leftmost pixel in the top bit. It needs
  `cc` and FreeType's headers (`pkg-config freetype2`).

The touch firmware (`touchfw.h`) is the reference board's TCM controller's, and this board's
touch is not that controller, so the loader does not talk to it (see the comment on
`touch_bring_up` in `mtkloader.c`). `build.sh` writes an empty image in its place when the
phone's own firmware is not there, so the loader compiles without it.


## The two things the port actually needs from the iOS side

**The identity header.** The loader builds its own device tree for the hardware the X6739 really
has, but copies the iPhone's identity properties into it, so that the user space sees the phone
its files were built for. `identity.h` is that copy: a table of properties and their bytes.
Without it the loader can be built (`mkidentity.sh --placeholder`) and can boot the open kernel
under QEMU, but the iOS user space will not come up.

**The system image** (`userland/system.apfs`, about 2.5 GB): the chosen part of the root
filesystem, the dyld shared cache, the two cryptexes and the loader's own `stage0` as
`/sbin/launchd`. It is an APFS container, built by the common tree's `tools/apfs/mkapfs.py`.

## Where the system image goes on the phone

The X6739 has **no `xnusys` partition** (the MT6768 reference port has one, carved out of the end
of `userdata` with `tools/gpt-carve.py`). `userdata` here is 226 GiB, so there is room, but the
partition has to be carved out of this phone's GPT first — and that is one of the open questions in
`docs/port-plan.md`.

The port's storage decision (the kernel owns the root, `ufs_root=1`) means the loader does **not**
read the image: the kernel's own UFS driver does. That driver does not exist yet, in the common
tree or anywhere else in the project — it is the largest single piece of new code in this port.

## What has not been done

The system image (`userland/system.apfs`) has not been built yet, and nothing has been booted:
the loader has not been compiled, no image has been flashed, and the phone has never run
HakiOS. The X6739 has no `xnusys` partition either — see above.
