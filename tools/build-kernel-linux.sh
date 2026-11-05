#!/bin/bash
# build-kernel-linux.sh: build the HakiOS XNU kernel on this Linux machine.
#
# The common tree (../refs/hakios-xnu-kernel) needs a Mach-O toolchain and the theos SDK, and
# Set THEOS to a Theos installation with cctools and the iPhoneOS16.5 SDK. XNU_SRC defaults to
# the common tree's applied XNU source, and KERNEL_OUT defaults to this repository's ignored
# mach.development.mt6893.
#
# Usage: tools/build-kernel-linux.sh [-jN] [extra make args]
#
# The exit status is 2 on this configuration: the last steps of XNU's own build make the kernel
# extensions' symbol sets (config/*.symbolset) and run check_all_exports, and they fail for
# names this kernel does not have (DriverKit's IOService methods, Apple silicon's _t8110dart_*).
# Nothing here loads a kext, and tools/make-kernel.sh --judge says the build is good.
set -e -o pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
COMMON=${HAKIOS_COMMON:-$HERE/../refs/hakios-xnu-kernel}

export THEOS=${THEOS:?set THEOS to a Theos installation containing cctools and the iPhoneOS16.5 SDK}
export XNU_SRC=${XNU_SRC:-$COMMON/xnu-src/xnu-8796.121.2}
KERNEL_OUT=${KERNEL_OUT:-$HERE/mach.development.mt6893}
JOBS=16
[ "$1" = "-j" ] && JOBS=$2 && shift 2
[ "${1#-j}" != "$1" ] && JOBS=${1#-j} && shift

# the cctools and the SDK have to be there before anything else
[ -x "$THEOS/toolchain/linux/iphone/bin/ld" ] || { echo "no cctools in $THEOS/toolchain/linux/iphone/bin" >&2; exit 1; }
[ -d "$THEOS/sdks/iPhoneOS16.5.sdk/usr/include" ] || { echo "no iPhoneOS16.5.sdk in $THEOS/sdks" >&2; exit 1; }

# stage 1 (sources, host tools, the SDK), the kernel tree, stage 2 (the two static libraries)
if [ ! -x "$COMMON/xnu-deps/host/bin/ld64" ]; then
	(cd "$COMMON" && bash xnu-deps/setup.sh --stage1)
fi
if [ ! -f "$XNU_SRC/makedefs/MakeInc.def" ]; then
	(cd "$COMMON" && bash xnu/apply.sh)
	(cd "$COMMON" && bash xnu-deps/setup.sh --stage2)
fi

log=${KERNEL_LOG:-$HERE/logs/kernel-build.log}
mkdir -p "$(dirname "$log")"

# The drivers this phone's kernel is built with: this repository's own choice
# (configs/x6739.config -- the tree's touch driver is for the POCO's SPI chip, not this phone's
# I2C0 FocalTech FT3681). Without this the tree builds every driver it knows, goodix included.
CONFIG=${HAKIOS_CONFIG:-$HERE/configs/x6739.config}
(cd "$COMMON" && python3 tools/hakios-config.py kernel --tree "$XNU_SRC" --config "$CONFIG")
(cd "$COMMON" && bash xnu/sync-drivers.sh "$XNU_SRC")

# make exits 2 on this configuration (the kext symbol-set steps, see the head of this file):
# the kernel is built anyway, so the exit status is not what decides here
(cd "$COMMON" && bash xnu-build.sh -k -j$JOBS "$@") >"$log" 2>&1 || true
tail -5 "$log"
kernel=$XNU_SRC/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
[ -f "$kernel" ] || { echo "no kernel was produced: $kernel (log: $log)" >&2; exit 1; }
# and the build step that failed must be one of the symbol-set steps, not a compile or a link
(cd "$COMMON" && KERNEL="$kernel" bash tools/make-kernel.sh --judge "$log") || exit 1
cp -p "$kernel" "$KERNEL_OUT"
echo "built: $KERNEL_OUT ($kernel, $(stat -c %s "$kernel") bytes)"
echo "log: $log"
exit 0
