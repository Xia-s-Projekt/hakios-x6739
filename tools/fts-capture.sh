#!/bin/bash
# Capture raw FT3681 reports (sysfs) and Android's evdev stream while a finger touches.
set -u
SERIAL="${HAKIOS_SERIAL:?set HAKIOS_SERIAL to the serial shown by adb devices}"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="${1:-$ROOT/logs/fts-capture}"
mkdir -p "$OUT"
BASE=/sys/devices/platform/soc/11015000.i2c/i2c-0/0-0038
DUR="${DUR:-180}"

adb -s "$SERIAL" shell "su -c 'for i in \$(seq 1 $((DUR*20))); do printf \"T %s \" \$(date +%s.%N); cat $BASE/fts_touch_point | tr -d \"\\n\"; echo; sleep 0.05; done'" > "$OUT/fts_touch_point.log" 2>&1 &
RAW=$!

adb -s "$SERIAL" shell "su -c 'getevent -lt /dev/input/event4'" > "$OUT/event4.log" 2>&1 &
EV=$!

wait $RAW $EV
echo "done"
