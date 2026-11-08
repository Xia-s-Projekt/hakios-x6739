#!/bin/bash
# test-120hz.sh: the 300-second window the user tests by hand.
#
# One XNU run on the X6739 with the settings that give the highest frame rate found so far, and
# the log of it kept. The phone is in Android when this is started; it is asked for one XNU boot,
# XNU runs for XNU_SECS (300 by default), and Android comes back by itself.
#
#   HAKIOS_SERIAL=... tools/test-120hz.sh
#
# The settings, and why each is here:
#
#   cpus=4 touchscreen=1     the four cores and the touch screen
#   mali=6 mali_clk=2        the GPU, and its clock at 416 MHz (the mux reads back 2)
#   ca_force=1               CoreAnimation renders every frame it is given
#   ca_vbl=1                 CoreAnimation takes the display driver's refresh notifications
#   fb_hz=120 fb_ca_hz=120   the panel is 120 Hz, and CoreAnimation is told so
#   fbscale=2 fbfit=3        the picture is scaled onto the panel, and fitted
#   fb_queue=1 fb_flip=1     one swap may wait for the panel, and the picture is flipped
#   caenv=CA_FORCE_120HZ=1   THE ONE THAT MATTERS: CoreAnimation's clock was at 10 Hz without it
#   caenv=CA_DISABLE_APT=1   the window server renders every 120 Hz refresh (bypasses tick divider)
#   touch_lat=1              a line for every frame (the "tl: g" lines tools/repaint.py reads)
#
# The result is in logs/xnu-120hz.log, and tools/repaint.py reads the frame rate out of it.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT="$(cd "$HERE/.." && pwd)"
SECS=${XNU_SECS:-300}
LOG=${XNU_LOG_OUT:-$ROOT/logs/xnu-120hz.log}
ARGS=${XNU_ARGS:-"cpus=4 touchscreen=1 mali=6 mali_clk=2 ca_force=1 ca_vbl=1 fb_hz=120 fb_ca_hz=120 \
fbscale=2 fbfit=3 fb_queue=1 fb_flip=1 touch_lat=1 caenv=CA_FORCE_120HZ=1 \
caenv=CA_DISABLE_APT=1 \
caenv=CA_ENABLE_ALL_FRAME_RATES=1 \
caenv=CA_HEARTBEAT_RATE=120 \
caenv=CA_MINIMUM_FRAME_DURATION=0p0083333 \
caenv=CA_ENABLE_HIGH_FRAME_RATE_RESTRICTION=0"}

echo "test-120hz.sh: a ${SECS} s run; the log is $LOG"
HAKIOS_SERIAL=${HAKIOS_SERIAL:?set HAKIOS_SERIAL to the serial of your phone, as adb devices shows it} \
XNU_SECS=$SECS XNU_LOG_OUT=$LOG XNU_ARGS="$ARGS" bash "$HERE/xnu-cycle.sh"

echo
echo "== the frame rate (tools/repaint.py)"
python3 "$ROOT/tools/repaint.py" "$LOG" || true
echo
echo "== the driver's own five-second report"
grep -o "MiniIOMobileFramebuffer: [0-9]* frames in [0-9]* ms ([0-9]* a second)" "$LOG" | tail -8 || true
echo
echo "== the window server's own heartbeat"
grep -o "frames=[0-9]* ([0-9.]* FPS, rend [0-9.]* FPS)" "$LOG" | tail -5 || true
