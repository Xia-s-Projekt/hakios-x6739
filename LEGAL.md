# LEGAL.md

What this repository does and does not contain, and why.

## What is here

- **The loader** (`mtkloader/`): our own bare-metal code, MIT. It contains no hardware firmware
  and no code taken from the phone's vendor.
- **The drivers** (`kernel/`): our own code for the phone, APSL 2.0, because it is built into
  Apple's XNU kernel. Until they are written, this directory holds only its README.
- **The tools** (`tools/`): our own scripts. The device-tree readers are pure Python and read
  only numbers and names out of the phone's own device tree.

## What is not here, by design

- **No file of iOS**: no binary, image, cache, shader library, font, picture or screenshot, and no
  header derived from iOS firmware. What the screen showed is described in words.
- **No file of the vendor's**: no touch firmware, no CONNSYS firmware, no SCP firmware, no GPU
  firmware, no partition image, and no device tree taken off the phone. `pull-firmware.sh` fetches
  the firmware into `fw-*/`, which git ignores; the device tree is read with the tools in `tools/`
  and its numbers are quoted in the source, but the tree itself is never committed.
- **No personal data**: no serial numbers, no home-directory paths, no tokens, no Wi-Fi names or
  passwords. The serial iOS is told is the placeholder `XNU_SERIAL`.
- **No screenshot**: `tools/fbshot.py` makes pictures of the screen for the owner's own checks;
  they are never committed.

## The licenses

- `kernel/` — Apple Public Source License 2.0 (`LICENSE.APSL`), as code built into XNU.
- everything else — MIT (`LICENSE`).

XNU-derived code is not relabelled as MIT. MediaTek's and Linux's GPL sources may be read for
register addresses and hardware order and named where used, never copied.

## The device tree

The phone's own device tree (`/sys/firmware/fdt`) is the phone's. This repository quotes numbers
from it — addresses, sizes, compatibles — because a port needs them, and that is the same rule
the Qualcomm `tools/soc-args.py` in the common tree follows. The tree itself is not distributed
here.
