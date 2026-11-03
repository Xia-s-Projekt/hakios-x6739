# Building the kernel on Linux

This repository contains device sources and patch records, not a kernel binary, SDK, iOS files or vendor firmware. The tested home-screen kernel remains a private local build; its exact source-to-binary reproduction is unfinished. Do not flash a newly built image without reviewing its source and validating the remaining patch drift.

## Dependencies

Obtain the HakiOS common tree separately and set `HAKIOS_COMMON` to its location. By default, scripts expect `../refs/hakios-xnu-kernel` relative to this repository. Its own license and build instructions apply.

Set `THEOS` to a separately obtained toolchain installation containing:

- `toolchain/linux/iphone/bin/ld` and the other cctools;
- `sdks/iPhoneOS16.5.sdk/usr/include`.

These dependencies are not redistributed here. Contributors must obtain and use them under their applicable licenses.

`XNU_SRC` defaults to `$HAKIOS_COMMON/xnu-src/xnu-8796.121.2`. The common tree's `xnu/apply.sh` prepares that source tree; `xnu-deps/setup.sh` prepares host tools and the kernel SDK.

From this repository:

```sh
export HAKIOS_COMMON=/path/to/hakios-xnu-kernel
export THEOS=/path/to/theos
export TMPDIR="$PWD/build-host"
mkdir -p "$TMPDIR" logs
tools/build-kernel-linux.sh -j4
```

`KERNEL_OUT` defaults to the ignored local file `mach.development.mt6893`; `KERNEL_LOG` defaults to `logs/kernel-build.log`. Building does not flash a device.

## Machine configuration and patch status

The existing build uses `DEVELOPMENT_ARM64_BCM2837` as its generic ARM64 configuration, not a dedicated `MT6893` machine configuration. The device's addresses and drivers are applied separately.

The older `tools/patch-kernel-mt6893.py` does not reproduce the latest privately tested kernel. Do not treat its output, or a newly compiled kernel, as device-tested merely because compilation succeeds. The public source tree still needs source-level integration and reproducibility work for all device-specific fixes.

The private tested kernel includes a 192 MiB ramfs quota for the current 512 MiB XNU memory window and the FT3681 polling-register fix. Preserve runtime page-size calculations when porting the quota change; a fixed page count previously reduced capacity to 48 MiB.

## Build failures

A previous local configuration linked a kernel but exited 2 in the kext symbol-set checks. `tools/build-kernel-linux.sh` delegates acceptance of that failure to the common tree's `tools/make-kernel.sh --judge`; a compile or link failure is not success. Check the build log and verify source changes are present before device testing.
