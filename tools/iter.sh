#!/bin/bash
# iter.sh: one short XNU run on the X6739, to see what a change does.
#
# The window is 120 seconds, not 300: a run that works reaches the home screen in well under a
# minute, and the frame rate can be read from the first seconds of it. A run that does not work
# shows nothing in the log either way, and 300 seconds of it is 180 seconds of waiting.
#
#   HAKIOS_SERIAL=... tools/iter.sh [extra boot-args...]
#
# The boot-args are the ones the frame rate work uses; anything given here is added to them.
# The log is logs/xnu-iter.log and tools/repaint.py reads the frame rate out of it.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT="$(cd "$HERE/.." && pwd)"
SECS=${XNU_SECS:-120}
LOG=${XNU_LOG_OUT:-$ROOT/logs/xnu-iter.log}
BASE="cpus=4 touchscreen=1 mali=6 mali_clk=2 ca_force=1 ca_vbl=1 fb_hz=120 fb_ca_hz=120 \
fbscale=2 fbfit=3 fb_queue=1 fb_flip=1 touch_lat=1 caenv=CA_FORCE_120HZ=1 \
caenv=CA_ENABLE_REDUCED_SERVER_VSYNC_RENDER=0"
ARGS="$BASE $*"

echo "iter.sh: ${SECS} s, the log is $LOG"
HAKIOS_SERIAL=${HAKIOS_SERIAL:?set HAKIOS_SERIAL to the serial of your phone, as adb devices shows it} \
XNU_SECS=$SECS XNU_LOG_OUT=$LOG XNU_ARGS="$ARGS" bash "$HERE/xnu-cycle.sh"

echo
echo "== the frame rate (tools/repaint.py)"
python3 "$ROOT/tools/repaint.py" "$LOG" 2>/dev/null || echo "   (no frames in the log)"
echo
echo "== the window server's own heartbeat"
grep -o "frames=[0-9]* ([0-9.]* FPS, rend [0-9.]* FPS)" "$LOG" 2>/dev/null | tail -3
echo
echo "== the display driver's report"
grep -o "MiniIOMobileFramebuffer: [0-9]* frames in [0-9]* ms ([0-9]* a second)" "$LOG" 2>/dev/null | tail -4
echo
echo "== what the window server believes about the screen"
grep -h "hakios: the window server's display\|hakios:   " "$LOG" 2>/dev/null | head -12
