#!/bin/bash
# Wait for the phone to come back, then take the picture of the panel at once.
#
# The phone goes off the host's USB entirely while iOS runs, and comes back to
# Android only when the run ends and the loader chains to it. When a run's end was
# not reached -- the loader was never able to get back, or the watchdog reset into
# a loop -- the phone stays off USB for good: `adb devices` empty AND `lsusb`
# showing no MediaTek device at all (not even 18d1:4ee0, which is what a fastbootd
# phone looks like). That is the case that needs the user's hands: hold the power
# button and reseat the USB. Do not keep polling it for ten minutes when `lsusb`
# shows nothing at all -- the zygote-restart case still enumerates, this one does
# not. Before asking for a power cycle, check that no `fastboot`/`adb` process of
# yours is still running: a flashing command left alive holds the device and
# looks exactly like a dead phone.
#
# usage: tools/phone-shot.sh [SECONDS] [OUT.png]
set -e
S=${HAKIOS_SERIAL:-DEVICE_SERIAL}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WAIT=${1:-900}
OUT=${2:-$ROOT/logs/panel.png}
LOG=$HOME/.cache/hakios-scratch/cache/scratch/shot-panel.log

echo "waiting up to ${WAIT}s for the phone"
for i in $(seq 1 $((WAIT / 5))); do
	if [ "$(adb -s $S get-state 2>/dev/null)" = device ]; then
		echo "the phone is back after $((i * 5))s"
		break
	fi
	sleep 5
done
if [ "$(adb -s $S get-state 2>/dev/null)" != device ]; then
	echo "the phone did not come back" >&2
	exit 1
fi

# the run that photographs the panel: the loader writes the picture into the log
# when XNU comes back, and the host turns it into a PNG.
HAKIOS_SERIAL=$S \
XNU_ARGS="cpus=1 touchscreen=1 gl=1" \
XNU_SECS=${XNU_SECS:-90} \
XNU_LOG_OUT=$LOG \
	bash "$ROOT/tools/xnu-cycle.sh" || true

echo "=== the panel ==="
python3 "$ROOT/tools/fbshot.py" "$LOG" "$OUT" || echo "no whole picture in the log"
echo "log: $LOG"
