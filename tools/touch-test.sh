#!/bin/bash
# One XNU run on the X6739 that proves the touch driver is talking to the panel, and prints what
# the panel said.
#
# It is tools/xnu-cycle.sh with two differences: the kernel is the one given in XNU_KERNEL (the
# source-built one with the FT3681 driver -- the tree's prebuilt has no touch driver at all), and
# the kernel's command line asks for the per-sample log (touch_lat=1), so every sample the driver
# posts is a line in the log.
#
# What to look for in the log:
#   ft3681: chip id 5662, firmware 44    the chip answered on I2C0
#   ft3681: ready; a report is read every 5000 us
#   ft3681: N reads, N reports (N idle), N points, N failed
#   tl: p 1 0 X Y at US                  a sample posted (type 1 = down, 2 = move, 3 = up)
#
# "points" is the count of points the panel reported; while nobody touches the screen it stays 0
# and every report is idle. It is the user's finger on the glass that makes it grow.
#
# usage: HAKIOS_SERIAL=... XNU_KERNEL=../refs/hakios-kernel tools/touch-test.sh [seconds]
set -e
S=${HAKIOS_SERIAL:?set HAKIOS_SERIAL to the serial of your phone, as adb devices shows it}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SECS=${1:-${XNU_SECS:-90}}
OUT=${XNU_LOG_OUT:-$ROOT/logs/touch-last.log}

# touch_lat=1 is what makes the kernel log every sample it posts.
export XNU_ARGS="cpus=1 touchscreen=1 touch_lat=1"
export XNU_SECS=$SECS
export XNU_LOG_OUT=$OUT
export XNU_KERNEL=${XNU_KERNEL:-../refs/hakios-kernel}

[ -f "$XNU_KERNEL" ] || { echo "touch-test: no kernel at $XNU_KERNEL" >&2; exit 1; }
echo "touch-test: kernel $XNU_KERNEL ($(stat -c%s "$XNU_KERNEL") bytes)"

bash "$ROOT/tools/xnu-cycle.sh"

echo "--- the driver ---"
grep -E "^ft3681:|^ft3681" "$OUT" || echo "  (no ft3681 lines: the kernel has no touch driver, or it never started)"
echo "--- the samples it posted ---"
grep -E "^tl: p " "$OUT" | tail -20 || echo "  (none)"
echo "log: $OUT"
