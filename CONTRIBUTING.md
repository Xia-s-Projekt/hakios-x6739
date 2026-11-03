# Contributing to the X6739 port

This is an experimental device port, not a ready-to-flash distribution. The home screen and touch were tested on one X6739; graphics still use Mesa softpipe, and exact source reproduction of the private tested kernel is unfinished.

## Development disclosure

The maintainer reports that device-port implementation, debugging investigations and documentation were produced with agentic AI. Their role was code review and device observations/bug reports. Existing upstream work remains attributed to its original authors. Contributions from people and AI-assisted workflows are welcome, but their evidence and licenses must be reviewable.

## Start here

- `docs/host-build.md`: external toolchain/common-tree requirements.
- `docs/mt6893-facts.md`: device-derived addresses and identity.
- `LEGAL.md`, `LICENSE`, `LICENSE.APSL`: distribution boundaries and licenses.

The common kernel/userland repository is a separate dependency. Scripts default to `../refs/hakios-xnu-kernel`; set `HAKIOS_COMMON` for a different location. Obtain the common tree, SDK, iOS archive and device firmware separately under their respective licenses. This repository does not redistribute them or promise a self-contained build.

## Useful work

1. Reproduce the privately tested kernel from public source and versioned patches; eliminate undocumented binary drift.
2. Profile CoreAnimation/softpipe during reproducible animations. Idle clock updates are not a benchmark of maximum FPS.
3. Implement the MT6893 Mali-G77/Valhall acceleration path and IOSurface sharing. Reference MT6768/G52 and Qualcomm/Adreno numbers are not valid for this SoC.
4. Investigate display tearing/flicker and frame scheduling independently of rendering cost.
5. Improve memory use and persistent data-volume support without writing unapproved partitions.

## Before submitting

```sh
python3 tools/test-capture.py
bash -n tools/xnu-cycle.sh tools/mk-root.sh tools/build-userland.sh
```

Include exact source changes, build commands, observed results and limitations. Label untested hypotheses. For device runs, state CPU count, duration, boot arguments, job set, kernel hash and the relevant redacted log lines. Do not submit raw logs without reviewing them for personal or proprietary material.

Use four CPUs and 300-second runs unless a test specifically needs otherwise. Only the device owner should authorize flashing. Existing scripts target `boot_a` after checking active slot `a`; do not touch slot B or userdata. Keep the device's SELinux mode unchanged. Never use another SoC's register addresses.

## What may enter this repository

Original source with clear provenance, appropriately licensed open-source modifications, build scripts, configuration, register/address facts, short redacted diagnostics and written test observations. Preserve upstream copyright notices and applicable licenses. Kernel code and XNU patch material are under APSL 2.0; other original port code is MIT unless a file says otherwise.

Do not upload iOS executables, dyld caches, IPSW members, root filesystems, SDKs, Apple fonts/shaders/assets/screenshots, vendor firmware/modules, partition images, phone device-tree dumps, private keys, tokens, serial numbers, account identifiers, or private host paths. Locally generated kernel and loader binaries are excluded from this source-only public release. A different artifact's open-source license may permit distribution with obligations; obtain a separate provenance/license review before adding it.

Keep local-only files in ignored build/output directories. Never attach the pre-public private Git bundle or publish it as a branch, tag, release asset, artifact or issue attachment.
