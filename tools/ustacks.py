#!/usr/bin/env python3
"""ustacks.py: the kernel's `ustacks=` output, with names on the addresses.

The kernel's sampler (xnu/files/osfmk/arm64/user_exception_trace.c) prints, for a process, the
shared cache's slide and then every thread's program counter, link register and stack return
addresses. Subtracting the slide gives the unslid address, which tools/symcache.py looks up in the
iOS shared cache.

usage: ustacks.py LOG [PROCESS]

Only the threads of PROCESS are shown (the one the log's `ustacks:` lines name), and each stack is
printed with the image and offset of every address.
"""
import bisect
import os
import re
import struct
import sys

CACHE = os.environ.get("HAKIOS_SHARED_CACHE", os.path.join(
    os.path.dirname(os.path.abspath(__file__)), '..', '..', 'refs', 'hakios-xnu-kernel', 'sysimage',
    '20F66__iPhone10,3_6', 'dyld_shared_cache_arm64'))
CACHE_LOW, CACHE_HIGH = 0x180000000, 0x280000000


def images(path):
    with open(path, 'rb') as cache:
        header = cache.read(0x200)
        images_offset, images_count = struct.unpack_from('<II', header, 0x1c0)
        cache.seek(images_offset)
        entries = [struct.unpack('<QQQII', cache.read(32)) for _ in range(images_count)]
        result = []
        for address, _, _, path_offset, _ in entries:
            cache.seek(path_offset)
            result.append((address, cache.read(256).split(b'\0')[0].decode('utf-8', 'replace')))
    result.sort()
    return result


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    table = images(CACHE) if os.path.exists(CACHE) else []
    starts = [address for address, _ in table]

    def name(address, slide):
        unslid = address - slide
        if not CACHE_LOW <= unslid < CACHE_HIGH:
            return "%#x" % address
        i = bisect.bisect_right(starts, unslid) - 1
        if i < 0:
            return "%#x" % address
        base, path = table[i]
        return "%s+0x%x" % (os.path.basename(path), unslid - base)

    slide = 0
    wanted = sys.argv[2] if len(sys.argv) > 2 else None
    for line in open(sys.argv[1], errors='replace'):
        line = line.rstrip()
        m = re.search(r'ustacks: (\S+)\[(\d+)\] \d+ threads, shared cache slide (0x[0-9a-f]+)', line)
        if m:
            slide = int(m.group(3), 16)
            if wanted is not None and m.group(1) != wanted:
                continue
            print("\n%s" % line)
            continue
        m = re.match(r'ustacks:\s+thread (0x[0-9a-f]+)(.*)', line)
        if m and (wanted is None or wanted in line):
            rest = m.group(2)
            addresses = re.findall(r'0x[0-9a-f]+', rest)
            running = '(running)' in rest
            named = re.search(r'"([^"]*)"', rest)
            parts = [name(int(a, 16), slide) for a in addresses]
            print("  thread %s%s%s: %s" % (
                m.group(1), ' "%s"' % named.group(1) if named else '', ' (running)' if running else '',
                '  '.join(parts)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
