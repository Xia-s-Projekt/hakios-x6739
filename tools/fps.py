#!/usr/bin/env python3
"""
fps.py RUN.log: the frame rate the window server actually reached, from its own trace.

The window server's library says, for each frame it draws, how long the frame took and when it
began (the "hakios-gl: T N whole US us" lines, each with the kernel's timestamp). The time
between two of those beginnings is the time between two frames: the rate the screen was
updated at, measured and not estimated.

The window server's own heartbeat counts something else ("ign=..., rend=..."), and the display
driver's five-second line counts the frames it put on the panel. This reads the frame trace,
which is what the rate of the drawn frames is in.

usage: fps.py RUN.log [--all]

  --all   every interval, not only the ones that came at a steady rate (the first and the
          last of a run are slow: the first is the start, the last the end)

Columns:
  t      seconds into the run, as the kernel counts them
  frame  the frame's number
  ms     milliseconds between this frame and the one before
  fps    frames a second in that interval

Then a summary: the frames counted, the median interval, the rate that interval means, and
the share of the intervals that were within a tenth of a millisecond of it.
"""
import re
import sys

# "[  117.397198]: inject_report: [backboardd] hakios-gl: T 60 whole 45119 us = ..."
FRAME = re.compile(r"\[\s*(\d+)\.(\d+)\]: .*hakios-gl: T (\d+) whole (\d+) us")
# the display driver's own line: "MiniIOMobileFramebuffer: 57 frames in 57965 ms (0 a second)"
DRIVER = re.compile(r"MiniIOMobileFramebuffer: (\d+) frames in (\d+) ms")


def frames(lines):
    out = []
    for line in lines:
        m = FRAME.search(line)
        if m:
            t = int(m.group(1)) + int(m.group(2)) / 1e6
            out.append((t, int(m.group(3)), int(m.group(4))))
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    every = "--all" in sys.argv
    if not args:
        print(__doc__)
        return 1

    with open(args[0], errors="replace") as f:
        lines = f.readlines()

    fs = frames(lines)
    if len(fs) < 2:
        print("fps.py: the log has no frame trace in it")
        return 1

    print(f"# {args[0]}: {len(fs)} frames traced")
    intervals = []
    for (t0, n0, cost0), (t1, n1, cost1) in zip(fs, fs[1:]):
        dt = t1 - t0
        if dt <= 0:
            continue
        intervals.append((t1, n1, dt))

    # the frames that came at a steady rate: the run's first and last frames are slow
    steady = intervals[1:-1] if len(intervals) > 4 and not every else intervals
    for t, n, dt in intervals:
        if not every and steady and (t, n, dt) not in steady:
            continue
        print(f"  t={t:9.3f}  frame {n:6d}  {dt * 1000:8.2f} ms  {1.0 / dt:7.1f} FPS")

    ms = sorted(dt * 1000 for _, _, dt in steady)
    if not ms:
        print("fps.py: no intervals")
        return 1
    median = ms[len(ms) // 2]
    within = sum(1 for v in ms if abs(v - median) < 0.1)
    print(f"# the steady rate: {len(ms)} intervals, median {median:.2f} ms = {1000.0 / median:.1f} FPS")
    print(f"#   {within} of {len(ms)} within 0.1 ms of it ({100.0 * within / len(ms):.0f}%)")

    # what the display driver says it put on the panel
    for line in lines:
        m = DRIVER.search(line)
        if m:
            n, ms_total = int(m.group(1)), int(m.group(2))
            print(f"# the display driver: {n} frames on the panel in {ms_total} ms"
                  f" = {1000.0 * n / ms_total:.1f} FPS" if ms_total else "")
    return 0


if __name__ == "__main__":
    sys.exit(main())
