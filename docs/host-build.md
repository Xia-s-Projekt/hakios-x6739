# Building the kernel on this Linux machine

The kernel is the common tree's XNU (Apple 8796.121.2) with HakiOS's patches. It builds on Linux
with cctools (the Mach-O linker) and the theos iPhoneOS16.5 SDK. Nothing goes into the home
directory: the toolchain, the SDK and the kernel tree live under `/mnt/mainline`.

    tools/build-kernel-linux.sh -j16        # the build; the kernel lands in /mnt/mainline/hakios-kernel

What it uses, and where each piece came from:

| what | where | how |
| --- | --- | --- |
| cctools (ld64, nm, otool, strip, lipo, libtool, nmedit, ctf_insert, codesign_allocate, install_name_tool) | `/mnt/mainline/theos/toolchain/linux/iphone/bin` | the Linux iOS toolchain of L1ghtmann/llvm-project (`iOSToolchain-x86_64.tar.xz`, ld64-956.6), unpacked under `theos/toolchain/` |
| the theos SDK | `/mnt/mainline/theos/sdks/iPhoneOS16.5.sdk` | sparse clone of github.com/theos/sdks (`git clone --depth 1 --filter=blob:none --sparse`) |
| the kernel tree | `/mnt/mainline/xnu-src/xnu-8796.121.2` | `xnu/apply.sh` in the common tree (Apple's XNU + `xnu/hakios.patch` + the drivers) |
| the host tools (mig, migcom, tcsh, unifdef) | `xnu-deps/host/bin` | `xnu-deps/setup.sh --stage1` |
| the SDK the kernel is compiled against | `xnu-deps/sdk/iPhoneOS16.5.xnu.sdk` | `xnu-deps/setup.sh --stage1` (stage 2 adds the two static libraries) |

`THEOS` must point at `/mnt/mainline/theos` (xnu-env.sh defaults it to `~/theos`, which is not
wanted here) and `XNU_SRC` at `/mnt/mainline/xnu-src/xnu-8796.121.2`.

## The machine configuration

`MACHINE=MT6893` does not exist: the tree's ARM64 machine configurations are
`BCM2837 T6000 T8101 T8103 VMAPPLE` (`makedefs/MakeInc.def`), and `xnu-build.sh` uses `BCM2837`
by default. The phone's own kernel (`mach.development.mt6893`) is the same build: its version
string says `BUILD/obj/DEVELOPMENT_ARM64_BCM2837`, and the name `mach.development.mt6893` is the
copy the device repository keeps.

## What the phone's kernel is, exactly

`mach.development.mt6893` is the common tree's `prebuilt/hakios-kernel` with thirteen
instruction patches (`tools/patch-kernel-mt6893.py`): the UFS controller's address
(0x11270000 instead of the tree's 0x1d84000), the LUN mask (0-2), a code-signing check that
accepts a 16 KB page shift, the ramfs as the loader fills it, and two framebuffer flags.

The patch script is verified against the shipped kernel: applying it to `prebuilt/hakios-kernel`
reproduces `mach.development.mt6893` byte for byte (13 patches, 77 bytes differ).

The byte patterns are specific to the revision the prebuilt was built from. A kernel built from
the tree's `main` today does not match them (the sources say "Darwin" where the prebuilt says
"iOS", since the rename in 45df619), so the patches must be re-derived for a fresh build.

## The build's exit status

`make` exits 2 on this configuration: the last steps make the kernel extensions' symbol sets
(`config/*.symbolset`) and run `check_all_exports`, which fail for names this kernel does not
have (DriverKit's IOService methods, Apple silicon's `_t8110dart_*`). Nothing here loads a kext.
`tools/make-kernel.sh --judge <log>` accepts exactly those failures and rejects any other; it
says the build is good.
