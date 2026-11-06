#!/usr/bin/env python3
"""cadence.py: the frame rate as it changed over one run, from the display driver's own five-second report.

The log is a ring buffer, and lines from several processes interleave in it; this reads the kernel's
timestamps (`[ 123.456789]:`) and sorts, so the order printed is the order the kernel saw. Three lines
are used: the frame line, the memory/refresh line that follows it, and each pause line.

usage: cadence.py LOG [--every SECONDS]

Columns:
  t         seconds into the run, as the kernel counts them
  frames    frames put on the panel in that interval
  fps       frames a second in that interval
  drv_us    microseconds the driver spent on each frame
  free      pages free at the end of the interval (a page is 16 KB on this kernel)
  d_free    how many pages free changed in the interval (negative: used up)
  told      refreshes told to the window server in the interval
  lost      of those, dropped (the client's queue was full)
  longest   the longest gap between two frames when it was under 90 ms (a pause is 90 ms or more)
  pauses    gaps of 90 ms or more: nothing was moving then

Then the pauses themselves, by length, with the memory free when each ended.

`fb_ca_hz` and `ca_force` decide whether any of this compares with another run; print them too:
they are in the boot-args line at the head of the log.
"""
import re
import sys

STAMP = re.compile(r"^\[\s*(\d+)\.(\d+)\]:\s?(.*)$")
FRAMES = re.compile(
    r"MiniIOMobileFramebuffer: (\d+) frames in (\d+) ms \((\d+) a second\), (\d+) us each in the driver")
MEMORY = re.compile(
    r"MiniIOMobileFramebuffer: (\d+) refreshes told \((\d+) dropped\) in that time; (\d+) pages free \((-?\d+) since\), (\d+) inactive")
GAPS = re.compile(
    r"from one frame to the next: .*\(the longest (\d+) ms\); half under (\d+) ms, 95 in 100 under (\d+) ms; (\d+) pauses")
PAUSE = re.compile(r"MiniIOMobileFramebuffer: pause (\d+) ms, (\d+) pages free, (\d+) inactive")
BOOTARGS = re.compile(r"boot-args:(.*)$")


def median(values):
    values = sorted(values)

    return values[len(values) // 2] if values else 0


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    path = sys.argv[1]
    every = 0.0
    if "--every" in sys.argv:
        every = float(sys.argv[sys.argv.index("--every") + 1])

    events = []                 # (time, kind, match)
    bootargs = None
    with open(path, "r", errors="replace") as log:
        for line in log:
            if bootargs is None:
                found = BOOTARGS.search(line)
                if found:
                    bootargs = found.group(1).strip()
            match = STAMP.match(line.strip())
            if not match:
                continue
            when = int(match.group(1)) + int(match.group(2).ljust(6, "0")[:6]) / 1e6
            body = match.group(3)
            for kind, pattern in (("frames", FRAMES), ("memory", MEMORY), ("gaps", GAPS), ("pause", PAUSE)):
                found = pattern.search(body)
                if found:
                    events.append((when, kind, found))
                    break
    events.sort(key=lambda event: event[0])

    if bootargs:
        told = [word for word in bootargs.split() if word.startswith(("fb_", "ca_")) or word.startswith("gl")]
        print("boot-args: " + " ".join(told))

    print("%8s %7s %6s %7s %8s %8s %6s %5s %8s %7s" % (
        "t", "frames", "fps", "drv_us", "free", "d_free", "told", "lost", "longest", "pauses"))

    last = None
    greyed = 0
    pauses = []                 # (ms, pages free)
    for when, kind, found in events:
        if kind == "pause":
            pauses.append((int(found.group(1)), int(found.group(2))))
            continue
        if kind == "frames":
            last = {"t": when, "frames": int(found.group(1)), "ms": int(found.group(2)),
                    "fps": int(found.group(3)), "drv": int(found.group(4))}
            continue
        if kind == "memory" and last is not None:
            last["told"] = int(found.group(1))
            last["lost"] = int(found.group(2))
            last["free"] = int(found.group(3))
            last["d_free"] = int(found.group(4))
            continue
        if kind == "gaps" and last is not None:
            last["longest"] = int(found.group(1))
            last["pauses"] = int(found.group(4))
            if every and last["t"] - greyed < every:
                last = None
                continue
            greyed = last["t"]

            def cell(name, sign=""):
                if name not in last:
                    return "%8s" % "-"
                return ("%" + sign + "8d") % last[name]

            print("%8.1f %7d %6d %7d %s %s %s %s %8d %7d" % (
                last["t"], last["frames"], last["fps"], last["drv"], cell("free"), cell("d_free", "+"),
                cell("told"), cell("lost"), last["longest"], last["pauses"]))
        last = None

    if pauses:
        buckets = [(90, 150), (150, 250), (250, 400), (400, 700), (700, 1 << 30)]
        print()
        print("pauses: %d, median %d ms, longest %d ms; free pages then: median %d, fewest %d" % (
            len(pauses), median([ms for ms, _ in pauses]), max(ms for ms, _ in pauses),
            median([free for _, free in pauses]), min(free for _, free in pauses)))
        for low, high in buckets:
            inside = [free for ms, free in pauses if low <= ms < high]
            if inside:
                name = "%d-%d ms" % (low, high - 1) if high < (1 << 30) else "%d ms and longer" % low
                print("  %-18s %3d pauses, free pages median %4d, fewest %4d" % (
                    name, len(inside), median(inside), min(inside)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
