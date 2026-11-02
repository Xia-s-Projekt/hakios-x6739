#!/bin/bash
# One XNU run on the X6739, and what the loader said about it.
#
#   tools/xnu-test.sh [loader flags...]
#
# The loader is built with the flags this passes, the payload is made, the image is packed into
# the phone's own boot_a, and boot_a is flashed. Then the phone is asked for one XNU boot
# (tools/boot-xnu.sh), and the loader's text is taken out of the phone.
#
# The text is in the ramoops pmsg zone, which Android's pstore driver does not clear -- but it
# does remove the file once it has been read, so the files are copied out as soon as adb is up,
# before anything else can read them.
#
# Slot b is never touched. The phone keeps booting Android if XNU does not come back: the loader
# starts Android unless it is asked for XNU, and the request is used once.
set -e
S=${HAKIOS_SERIAL:?set HAKIOS_SERIAL to the serial of your phone, as adb devices shows it}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORKSPACE="$(cd "$ROOT/.." && pwd)"
COMMON=${HAKIOS_COMMON:-$WORKSPACE/refs/hakios-xnu-kernel}
OUT=${XNU_LOG_OUT:-$ROOT/logs/xnu-last.log}
IMG=${XNU_IMAGE:-$ROOT/xnu-flashed-slot_a.img}
BOOT=${XNU_BOOT_IMAGE:-$ROOT/backup/boot_a.img}
KERNEL=${XNU_KERNEL:-$ROOT/mach.development.mt6893}
[ -f "$KERNEL" ] || KERNEL="$COMMON/prebuilt/mach.development.bcm2837"

# how long the kernel runs before it hands the log back
SECS=${XNU_SECS:-120}
# the kernel's own command line; the clock comes from this machine
# touchscreen=1: the kernel's touch driver. The tree's driver for the phone's FocalTech FT3681
# (kernel/bsd/dev/hakios.ft3681.c) starts only with this argument.
XNU_ARGS=${XNU_ARGS:-"cpus=4 fb_diff=1 touchscreen=1 gl=1"}
# the device tree offers the kernel exactly this many cores; "cpus=" in XNU_ARGS should match
CORES=${XNU_CORES:-$(expr "$XNU_ARGS" : '.*cpus=\([0-9]*\)' || echo 1)}
[ -n "$CORES" ] || CORES=1
WATCH=${XNU_WATCH:-$((SECS + 240))}
WDT=${XNU_WDT:-0}

[ -f "$BOOT" ] || { echo "no boot image at $BOOT -- pull it off the phone:"; exit 1; }
mkdir -p "$ROOT/logs"

echo "== building the loader =="
# The small root is the kernel's root here: the big system image stays on the UFS and the
# kernel's own UFS driver would read it, but that driver does not exist yet, so there would be
# no root at all. XNU_SMALL_ROOT=0 is for the day it does.
"$ROOT/mtkloader/build.sh" -DXNU_WDT_SECONDS=$WDT -DXNU_CORES=$CORES -DXNU_WATCH_SECONDS=$WATCH \
	"-DXNU_RETURN_AFTER=\"$SECS\"" "-DXNU_EXTRA_ARGS=\"$XNU_ARGS epoch=$(date +%s)\"" \
	-DXNU_SMALL_ROOT=1 "$@" >/dev/null

echo "== making the payload =="
# The payload's ramdisk is the small root (root.apfs). When mounted directly as / (XNU_SMALL_ROOT=1),
# XNU's load_init_program looks for /sbin/launchd, and init's writable test checks /tmp -> private/var/tmp.
bash "$ROOT/tools/mk-root.sh"
python3 "$ROOT/tools/mkpayload.py" -o "$ROOT/build-payload.bin" \
	--kernel "$KERNEL" \
	--ramdisk "$ROOT/build-root.apfs" \
	--cmdline="rd=md0 ufs_root=1 $XNU_ARGS"

echo "== packing the boot image =="
python3 "$ROOT/tools/chainboot.py" "$BOOT" "$ROOT/mtkloader/mtkloader.bin" "$IMG" \
	"$ROOT/build-payload.bin"

if [ "${XNU_NO_FLASH}" != 1 ]; then
	echo "== flashing boot_a =="
	"$ROOT/tools/fastboot-bootloader.sh" "$S"
	[ "$(fastboot -s $S getvar current-slot 2>&1 | sed -n 's/^current-slot: //p')" = a ] || { echo "current slot is not a"; exit 1; }
	fastboot -s $S flash boot_a "$IMG" 2>&1 | tail -2
	fastboot -s $S reboot >/dev/null 2>&1

	echo "== waiting for Android, to ask it for one XNU boot =="
	for i in $(seq 1 100); do [ "$(adb -s $S get-state 2>/dev/null)" = device ] && break; sleep 3; done
	sleep 4
fi

echo "== asking for one XNU boot =="
"$ROOT/tools/boot-xnu.sh"

echo "== waiting for phone to leave Android and run XNU =="
for i in $(seq 1 30); do [ "$(adb -s $S get-state 2>/dev/null)" != device ] && break; sleep 1; done

echo "== XNU is running; Android comes back by itself =="
WAIT_ITERS=$(( (SECS + 300) / 3 ))
for i in $(seq 1 $WAIT_ITERS); do [ "$(adb -s $S get-state 2>/dev/null)" = device ] && break; sleep 3; done
sleep 3

echo "== taking the log out =="
rm -rf "$ROOT/logs/pstore"
mkdir -p "$ROOT/logs/pstore"
adb -s $S shell 'su -c "ls -la /sys/fs/pstore"' || true
for i in $(seq 1 10); do
	adb -s $S shell 'su -c "cat /sys/fs/pstore/console-ramoops-0"' > "$ROOT/logs/pstore/console-ramoops-0" 2>/dev/null || true
	[ -s "$ROOT/logs/pstore/console-ramoops-0" ] && break
	adb -s $S shell 'su -c "cat /data/persist_log/pstore/pstore_last_kmsg"' > "$ROOT/logs/pstore/console-ramoops-0" 2>/dev/null || true
	[ -s "$ROOT/logs/pstore/console-ramoops-0" ] && break
	sleep 2
done
adb -s $S shell 'su -c "cat /sys/fs/pstore/pmsg-ramoops-0"' > "$ROOT/logs/pstore/pmsg-ramoops-0" 2>/dev/null || true
adb -s $S shell 'su -c "cat /sys/fs/pstore/dmesg-ramoops-0"' > "$ROOT/logs/pstore/dmesg-ramoops-0" 2>/dev/null || true
cp -f "$ROOT/logs/pstore/console-ramoops-0" "$OUT" 2>/dev/null || true
echo "log: $OUT ($(wc -c < "$OUT") bytes)"
