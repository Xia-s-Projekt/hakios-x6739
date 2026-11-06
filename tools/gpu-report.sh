#!/bin/bash
# gpu-report.sh: the GPU and frame numbers out of one run's log, side by side.
#
# Reads the log tools/xnu-cycle.sh left and prints the four things that decide where a frame's
# time went, in the order they have to be read:
#
#   1  the clock        what the mfg_sel mux was asked for and what it reads back
#   2  the submits      microseconds a submit cost, and how much of that was the GPU busy
#   3  the driver       the framebuffer's own 5-second report: driver time and refresh wait
#   4  the drawing      eagl's per-frame split (handing on / waiting / copying out)
#
# Usage: tools/gpu-report.sh [log]        (default: logs/xnu-last.log)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
LOG=${1:-$HERE/../logs/xnu-last.log}
[ -f "$LOG" ] || { echo "no such log: $LOG" >&2; exit 1; }

echo "== log: $LOG"

echo
echo "== 1. the clock"
grep -o "mali: mfg_sel asked for.*" "$LOG" | head -2 || echo "   (no mux readback: the kernel predates it)"

echo
echo "== 2. the submits"
echo "   submits: $(grep -o 'submit #[0-9]*' "$LOG" | tail -1)"
grep -oE "gpu_submits=[0-9]+, gpu_fails=[0-9]+, gpu_us=[0-9.]*, submit_us=[0-9.]*" "$LOG" | tail -4 |

	sed 's/^/   /'

echo
echo "== 3. the driver (per frame, from MiniIOMobileFramebuffer)"
grep -oE "[0-9]+ frames in [0-9]+ ms \([0-9]+ a second\), [0-9]+ us each in the driver.*refresh" "$LOG" | tail -4 |
	sed 's/^/   /'

echo
echo "== 4. the drawing (per frame, from eagl)"
if grep -q "hakios-gl: T " "$LOG"; then
	grep -oE "hakios-gl: T [0-9]+ us = .*" "$LOG" | tail -4 | sed 's/^/   /'
else
	echo "   (no compact T lines in this log: the library predates them; the long frame lines say the"
	echo "    same thing at the end, but this console cuts a line at about 200 bytes and cuts exactly"
	echo "    that part off. Rebuild the userland with the T line to compare runs.)"
	grep -oE "after [0-9]+ frames: [0-9]+ calls.*us a frame" "$LOG" | tail -3 | cut -c1-200 | sed 's/^/   /'
fi