#!/usr/bin/env python3
"""repaint.py: what a run's frame rate is made of.

With `touch_lat=1` the drawing thread prints, for every frame, when it began, when it ended, how
many draws it had and which processor it ended on:

    tl: g FRAME began US ended US DRAWS cpu N

Those lines say what the frame rate is made of. This sorts them by time (the console interleaves
lines from several processes and repeats them, so each frame is counted once), and groups the
frames by how many draws they had -- because the frames are not alike: on this port a light frame
and a full repaint differ by two orders of magnitude in cost, and the average of the two says
nothing about either.

usage: repaint.py LOG [--all]

Prints, for each group: how many frames, how long the drawing took (median and worst), and the gap
to the next frame. The gap is the time the drawing thread was not drawing -- which is where the
frame rate is actually lost.
"""
import re
import statistics
import sys

FRAME = re.compile(r"tl: g (\d+) began (\d+) ended (\d+) (\d+) cpu (\d+)")
BOOTARGS = re.compile(r"boot-args:(.*)$")

GROUPS = (("few (<40 draws)", 40), ("many (40-79 draws)", 80), ("full (80+ draws)", 0))


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    path = sys.argv[1]
    show_all = "--all" in sys.argv

    seen = {}
    bootargs = None
    with open(path, "r", errors="replace") as log:
        for line in log:
            if bootargs is None:
                found = BOOTARGS.search(line)
                if found:
                    bootargs = found.group(1).strip()
            found = FRAME.search(line)
            if found:
                number = int(found.group(1))
                if number in seen:
                    continue               # the console repeats lines; count each frame once
                seen[number] = (int(found.group(2)), int(found.group(3)), int(found.group(4)),
                                int(found.group(5)))

    if not seen:
        print("no tl: g lines in %s (the run needs touch_lat=1)" % path)
        return 1
    numbers = sorted(seen)
    rows = [seen[number] for number in numbers]

    if bootargs:
        told = [word for word in bootargs.split() if word.startswith(("fb_", "ca_", "touch"))]
        print("boot-args: " + " ".join(told))
    print("%d frames timed\n" % len(rows))

    groups = {}
    for index, (began, ended, draws, cpu) in enumerate(rows):
        gap = (began - rows[index - 1][1]) / 1000.0 if index else 0
        name = "few (<40 draws)" if draws < 40 else ("many (40-79 draws)" if draws < 80 else "full (80+ draws)")
        groups.setdefault(name, []).append(((ended - began) / 1000.0, gap, draws, cpu))

    print("%-18s %5s %10s %11s %10s %11s" % ("frames", "n", "draw med", "work med", "work max", "gap med"))
    for name, _ in GROUPS:
        entries = groups.get(name)
        if not entries:
            continue
        work = [entry[0] for entry in entries]
        gap = [entry[1] for entry in entries]
        draws = [entry[2] for entry in entries]
        print("%-18s %5d %10d %10.1f ms %9.1f ms %10.1f ms" % (
            name, len(entries), statistics.median(draws), statistics.median(work), max(work),
            statistics.median(gap)))
    print()
    print("%-18s %s" % ("", "the rate each group alone would give:"))
    for name, _ in GROUPS:
        entries = groups.get(name)
        if not entries:
            continue
        work = statistics.median([entry[0] for entry in entries])
        gap = statistics.median([entry[1] for entry in entries])
        print("%-18s %8.1f FPS  (work %.1f ms + gap %.1f ms)" % (name, 1000.0 / (work + gap), work, gap))

    fulls = sorted(row[0] for row in rows if row[2] >= 80)
    if len(fulls) > 1:
        intervals = [(fulls[i + 1] - fulls[i]) / 1e6 for i in range(len(fulls) - 1)]
        print()
        print("full repaints: %d, every %.2f s (median), the longest gap between them %.1f s" % (
            len(fulls), statistics.median(intervals), max(intervals)))

    if show_all:
        print()
        print("%6s %12s %10s %10s %8s %5s" % ("frame", "began us", "work ms", "gap ms", "draws", "cpu"))
        for index, (began, ended, draws, cpu) in enumerate(rows):
            gap = (began - rows[index - 1][1]) / 1000.0 if index else 0
            print("%6d %12d %10.1f %10.1f %8d %5d" % (
                numbers[index], began, (ended - began) / 1000.0, gap, draws, cpu))
    return 0


if __name__ == "__main__":
    sys.exit(main())
