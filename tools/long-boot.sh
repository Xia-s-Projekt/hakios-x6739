#!/bin/bash
# A long run, with more cores, to let the boot get past the Apple logo.
#
# The tree says what a boot looks like on the way (docs/getting-started/qemu.md):
# black for the first minute, the Apple logo at about one minute, the home
# screen at about five. Our runs ended at 70 s of kernel time, which is about
# when the logo comes up -- so the runs were too short to see what comes after.
#
# This phone draws in software (gl=1: Mesa's softpipe) on one core, and a frame
# takes 28461 us; the tree's reference phone has a GPU and eight cores and is at
# the home screen in under a minute. So: more cores, and much more time.
#
# The loader photographs the panel twice -- once before XNU runs and once when it
# comes back (mtkloader/mtkloader.c, shot_panel) -- and tools/fbshot.py makes a
# PNG of each. The two say whether the window server's drawing reached the panel.
#
# usage: tools/long-boot.sh [SECONDS] [CORES]
set -e
S=${HAKIOS_SERIAL:-DEVICE_SERIAL}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SECS=${1:-420}
CORES=${2:-4}
LOG=$HOME/.cache/hakios-scratch/cache/scratch/long-boot.log
OUT=$HOME/.cache/hakios-scratch/cache/scratch/panel.png

export HAKIOS_SERIAL=$S
export XNU_ARGS="cpus=$CORES touchscreen=1 gl=1"
export XNU_SECS=$SECS
export XNU_LOG_OUT=$LOG

echo "the run: $SECS s, $CORES cores"
bash "$ROOT/tools/xnu-cycle.sh" || true

echo "=== the panel after XNU ==="
python3 "$ROOT/tools/fbshot.py" "$LOG" "$OUT" 2 || echo "no second picture in the log"
echo "=== the panel before XNU ==="
python3 "$ROOT/tools/fbshot.py" "$LOG" "$HOME/.cache/hakios-scratch/cache/scratch/panel-before.png" 1 || \
	echo "no first picture in the log"
echo "log: $LOG"
