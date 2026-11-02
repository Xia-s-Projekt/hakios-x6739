#!/bin/bash
# Wait for the phone to come back, then take the picture of the panel at once.
#
# The phone goes off the host's USB entirely while iOS runs, and comes back to
# Android only when the run ends. When a run's end is not reached the phone stays
# off USB; when it does, the phone is back for a while and then off again (its own
# init-retry loop). So: wait, and the moment adb answers, run the loader that
# photographs the panel.
#
# usage: tools/phone-shot.sh [SECONDS] [OUT.png]
set -e
S=${HAKIOS_SERIAL:-102562537K104189}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WAIT=${1:-900}
OUT=${2:-$ROOT/logs/panel.png}
LOG=$HOME/.hermes/cache/scratch/shot-panel.log

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
