#!/bin/bash
# Flash the GPU probe to boot_a, ask for it once, and print what it found.
#
# The probe is a loader build with GPU_PROBE=1, which chains the loader in front of Android's
# kernel and then, after the probe, goes straight back to Android: it never boots XNU, so the
# image carries no XNU payload. The request is a line in ramoops' pmsg zone, which the loader
# takes (and wipes) as it comes up: the probe runs on the boot after the request was written.
# A probe that hangs the phone costs one reboot, after which it is Android again.
#
# Slot b is never touched, and only boot_a is written.
set -e
S=${HAKIOS_SERIAL:?set HAKIOS_SERIAL to the serial of your phone, as adb devices shows it}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/logs"
"$ROOT/mtkloader/build.sh" -DGPU_PROBE=1 "$@" >/dev/null
python3 "$ROOT/tools/chainboot.py" "$ROOT/backup/boot_a.img" "$ROOT/mtkloader/mtkloader.bin" \
	"$ROOT/xnu-flashed-slot_a.img"
[ "$(adb -s $S get-state 2>/dev/null)" = device ] && adb -s $S reboot bootloader
for i in $(seq 1 60); do fastboot devices | grep -q $S && break; sleep 1; done
[ "$(fastboot -s $S getvar current-slot 2>&1 | sed -n 's/^current-slot: //p')" = a ] || { echo "current slot is not a"; exit 1; }
fastboot -s $S flash boot_a "$ROOT/xnu-flashed-slot_a.img" >/dev/null 2>&1
fastboot -s $S reboot >/dev/null 2>&1
echo "waiting for Android, to ask it for the probe..."
for i in $(seq 1 100); do [ "$(adb -s $S get-state 2>/dev/null)" = device ] && break; sleep 3; done
sleep 5
adb -s $S shell 'su -c "printf DARWIN-X6739:GPU-PROBE > /dev/pmsg0"'
adb -s $S reboot
sleep 15
for i in $(seq 1 100); do [ "$(adb -s $S get-state 2>/dev/null)" = device ] && break; sleep 3; done
sleep 3
adb -s $S shell 'su -c "cat /sys/fs/pstore/console-ramoops-0"' > "$ROOT/logs/gpu-probe.log" 2>/dev/null
sed -n '/GPU probe ==/,/GPU probe done/p' "$ROOT/logs/gpu-probe.log"
