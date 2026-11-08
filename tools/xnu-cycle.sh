#!/bin/bash
# One XNU run on the X6739: build mtkloader (extra args go to the compiler, e.g.
# -DXNU_CHECKPOINT=17), pack it with Android and the XNU payload, flash boot_a, let XNU run and
# Android come back, then print what XNU left in pstore. Slot b is never touched.
set -e
S=${HAKIOS_SERIAL:?set HAKIOS_SERIAL to the serial of your phone, as adb devices shows it}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${XNU_LOG_OUT:-$ROOT/logs/xnu-last.log}
# test settings: XNU picked by default, watchdog and watcher on, XNU comes back by itself
# XNU_SECS: how long the kernel runs before it hands the log back (the watcher waits a bit longer)
SECS=${XNU_SECS:-45}
# XNU_ARGS: the kernel's own command line (cores and so on); the clock comes from this machine
# touchscreen=1: the kernel's touch driver (kernel/bsd/dev/hakios.ft3681.c) starts only with it
DEFAULT_ARGS="cpus=4 touchscreen=1 mali=6 mali_clk=2 ca_force=1 ca_vbl=1 fb_hz=120 fb_ca_hz=120 \
fbscale=2 fbfit=3 fb_queue=1 fb_flip=1 touch_lat=1 caenv=CA_FORCE_120HZ=1 \
caenv=CA_DISABLE_APT=1 \
caenv=CA_ENABLE_ALL_FRAME_RATES=1 \
caenv=CA_HEARTBEAT_RATE=120 \
caenv=CA_MINIMUM_FRAME_DURATION=0p0083333 \
caenv=CA_ENABLE_HIGH_FRAME_RATE_RESTRICTION=0"
XNU_ARGS=${XNU_ARGS:-$DEFAULT_ARGS}
# the device tree offers the kernel exactly this many cores; "cpus=" in XNU_ARGS should match
CORES=${XNU_CORES:-$(expr "$XNU_ARGS" : '.*cpus=\([0-9]*\)' || echo 1)}
[ -n "$CORES" ] || CORES=1
WATCH=${XNU_WATCH:-$((SECS + 240))}
# No MENU_DEFAULT_XNU: the image boots Android, and XNU is asked for once (tools/boot-xnu.sh), so
# a kernel that hangs costs a power cycle and not a rescue through BROM.
TEST_FLAGS=(-DXNU_WDT_SECONDS=0 -DXNU_CORES=$CORES -DXNU_WATCH_SECONDS=$WATCH
	"-DXNU_RETURN_AFTER=\"$SECS\"" "-DXNU_EXTRA_ARGS=\"$XNU_ARGS epoch=$(date +%s)\"")
# The XNU log does not fit in the 256 KB the ramoops console keeps, so the loader saves the head
# of the boot, a window from the middle, and the end of the run. XNU_LOG_OFFSET is where the
# window starts and XNU_LOG_LEN how long it is; both are byte offsets into XNU's own log, which
# is about 1 MB for a 120 s run. 0 turns the window off (head and tail only).
LOG_OFFSET=${XNU_LOG_OFFSET:-0}
LOG_LEN=${XNU_LOG_LEN:-262144}
if [ "$LOG_OFFSET" -gt 0 ] 2>/dev/null; then
	TEST_FLAGS+=("-DXNU_LOG_OFFSET=$LOG_OFFSET" "-DXNU_LOG_LEN=$LOG_LEN")
fi
"$ROOT/mtkloader/build.sh" "${TEST_FLAGS[@]}" "$@" >/dev/null
# the payload (the XNU kernel and the small root) and the flashable image
COMMON=${HAKIOS_COMMON:-$(cd "$ROOT/.." && pwd)/refs/hakios-xnu-kernel}
bash "$ROOT/tools/mk-root.sh"
python3 "$ROOT/tools/mkpayload.py" -o "$ROOT/build-payload.bin" \
	--kernel "${XNU_KERNEL:-$ROOT/mach.development.mt6893}" \
	--ramdisk "$ROOT/build-root.apfs" >/dev/null
python3 "$ROOT/tools/chainboot.py" "$ROOT/backup/boot_a.img" "$ROOT/mtkloader/mtkloader.bin" \
	"$ROOT/xnu-flashed-slot_a.img" "$ROOT/build-payload.bin" >/dev/null
"$ROOT/tools/fastboot-bootloader.sh" "$S"
[ "$(fastboot -s $S getvar current-slot 2>&1 | sed -n 's/^current-slot: //p')" = a ] || { echo "current slot is not a"; exit 1; }
fastboot -s $S flash boot_a "$ROOT/xnu-flashed-slot_a.img" >/dev/null 2>&1
fastboot -s $S reboot >/dev/null 2>&1
echo "waiting for Android, to ask it for one XNU boot..."
for i in $(seq 1 100); do [ "$(adb -s $S get-state 2>/dev/null)" = device ] && break; sleep 3; done
sleep 4
"$ROOT/tools/boot-xnu.sh"
for i in $(seq 1 30); do [ "$(adb -s $S get-state 2>/dev/null)" != device ] && break; sleep 1; done
echo "XNU is running; Android comes back by itself"
WAIT_ITERS=$(( (SECS + 300) / 3 ))
for i in $(seq 1 $WAIT_ITERS); do [ "$(adb -s $S get-state 2>/dev/null)" = device ] && break; sleep 3; done
sleep 3
for i in $(seq 1 10); do adb -s $S shell 'su -c "cat /sys/fs/pstore/console-ramoops-0"' > "$OUT" 2>/dev/null && [ -s "$OUT" ] && break; sleep 2; done
echo "log: $OUT ($(wc -c < "$OUT") bytes)"
