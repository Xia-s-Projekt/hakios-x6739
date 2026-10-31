#!/bin/bash
# One XNU run on the X6739, and what the phone says about it.
#
#   tools/xnu-test.sh [loader flags...]
#
# The loader is built with the flags this passes, the payload is made, the image is packed into
# the phone's own boot_a, and boot_a is flashed. Then the phone is asked for one XNU boot
# (tools/boot-xnu.sh), and when it comes back the loader's console log is taken out of ramoops.
#
# Slot b is never touched. The phone keeps booting Android if XNU does not come back: the loader
# starts Android unless it is asked for XNU, and the request is used once.
#
# What it needs: the phone's boot image in backup/ (pulled off the phone with dd), the loader's
# identity header (tools/fetch-ios.sh identity), and the XNU payload's parts in the common tree
# (the prebuilt kernel and userland/root.apfs).
set -e
S=${HAKIOS_SERIAL:?set HAKIOS_SERIAL to the serial of your phone, as adb devices shows it}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORKSPACE="$(cd "$ROOT/.." && pwd)"
COMMON=${HAKIOS_COMMON:-$WORKSPACE/refs/hakios-xnu-kernel}
OUT=${XNU_LOG_OUT:-$ROOT/logs/xnu-last.log}
IMG=${XNU_IMAGE:-$ROOT/xnu-flashed-slot_a.img}
BOOT=${XNU_BOOT_IMAGE:-$ROOT/backup/boot_a.img}

# how long the kernel runs before it hands the log back
SECS=${XNU_SECS:-45}
# the kernel's own command line; the clock comes from this machine
XNU_ARGS=${XNU_ARGS:-cpus=1}
# the device tree offers the kernel exactly this many cores; "cpus=" in XNU_ARGS should match
CORES=${XNU_CORES:-$(expr "$XNU_ARGS" : '.*cpus=\([0-9]*\)' || echo 1)}
[ -n "$CORES" ] || CORES=1
WATCH=${XNU_WATCH:-$((SECS + 240))}

[ -f "$BOOT" ] || { echo "no boot image at $BOOT -- pull it off the phone:"; exit 1; }
mkdir -p "$ROOT/logs"

# The loader. The small root is the kernel's root here: the big system image stays on the UFS and
# the kernel's own UFS driver would read it, but that driver does not exist yet, so there would be
# no root at all. XNU_SMALL_ROOT=0 is for the day it does.
echo "== building the loader =="
"$ROOT/mtkloader/build.sh" -DXNU_WDT_SECONDS=0 -DXNU_CORES=$CORES -DXNU_WATCH_SECONDS=$WATCH \
	"-DXNU_RETURN_AFTER=\"$SECS\"" "-DXNU_EXTRA_ARGS=\"$XNU_ARGS epoch=$(date +%s)\"" \
	-DXNU_SMALL_ROOT=1 "$@" >/dev/null

echo "== making the payload =="
# The payload's ramdisk is the small root (root.apfs), which stage0 mounts and hands over to.
python3 "$ROOT/tools/mkpayload.py" -o "$ROOT/build-payload.bin" \
	--kernel "$COMMON/prebuilt/mach.development.bcm2837" \
	--ramdisk "$COMMON/userland/root.apfs" \
	--cmdline="rd=md0 ufs_root=1 $XNU_ARGS"

echo "== packing the boot image =="
python3 "$ROOT/tools/chainboot.py" "$BOOT" "$ROOT/mtkloader/mtkloader.bin" "$IMG" \
	"$ROOT/build-payload.bin"

echo "== flashing boot_a =="
[ "$(adb -s $S get-state 2>/dev/null)" = device ] && adb -s $S reboot bootloader
for i in $(seq 1 120); do fastboot devices | grep -q $S && break; sleep 1; done
# (fastboot -s waits for ever for a phone that is not there: a run would hang for hours)
fastboot devices | grep -q $S || { echo "the phone did not come up in fastboot"; exit 1; }
[ "$(fastboot -s $S getvar current-slot 2>&1 | sed -n 's/^current-slot: //p')" = a ] || { echo "current slot is not a"; exit 1; }
fastboot -s $S flash boot_a "$IMG" 2>&1 | tail -2
fastboot -s $S reboot >/dev/null 2>&1

echo "== waiting for Android, to ask it for one XNU boot =="
for i in $(seq 1 100); do [ "$(adb -s $S get-state 2>/dev/null)" = device ] && break; sleep 3; done
sleep 4
"$ROOT/tools/boot-xnu.sh"

echo "== XNU is running; Android comes back by itself =="
WAIT_ITERS=$(( (SECS + 300) / 3 ))
for i in $(seq 1 $WAIT_ITERS); do [ "$(adb -s $S get-state 2>/dev/null)" = device ] && break; sleep 3; done
sleep 3

echo "== taking the log out of ramoops =="
rm -f "$OUT"
for i in $(seq 1 10); do
	adb -s $S shell 'su -c "cat /sys/fs/pstore/console-ramoops-0"' > "$OUT" 2>/dev/null
	[ -s "$OUT" ] && break
	sleep 2
done
echo "log: $OUT ($(wc -c < "$OUT") bytes)"
