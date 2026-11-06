#!/usr/bin/env python3
"""symcache.py: put names on the unslid addresses of the iOS shared cache.

The kernel's `ustacks=` sampler prints the program counters and stack return addresses of a
process's threads, and the slide of the shared cache that process runs from. Subtracting the slide
gives the unslid address, which this looks up in the Darwin shared cache: which image it is in,
and how far into that image.

usage: symcache.py ADDRESS...        (unslid hex addresses)
       symcache.py --file LIST       (a file of hex addresses, one per line)

The images' path names come out of the cache's own header, so nothing of Apple's is needed but the
cache itself (which is not in the repository: it is in the common tree's sysimage/).
"""
import os
import struct
import sys

CACHE = os.environ.get("HAKIOS_SHARED_CACHE", os.path.join(
    os.path.dirname(os.path.abspath(__file__)), '..', '..', 'refs', 'hakios-xnu-kernel', 'sysimage',
    '20F66__iPhone10,3_6', 'dyld_shared_cache_arm64'))

CACHE_LOW, CACHE_HIGH = 0x180000000, 0x280000000        # the unslid addresses of the shared cache


def images(path):
    """The images in the cache: (address, name), sorted by address."""
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
    if not os.path.exists(CACHE):
        print("symcache.py: no shared cache at %s" % CACHE, file=sys.stderr)
        return 2
    table = images(CACHE)
    starts = [address for address, _ in table]

    import bisect

    words = []
    if len(sys.argv) > 1 and sys.argv[1] == '--file':
        for line in open(sys.argv[2], errors='replace'):
            for word in line.split():
                words.append(word)
    else:
        words = sys.argv[1:]

    for word in words:
        try:
            address = int(word, 16)
        except ValueError:
            continue
        if not CACHE_LOW <= address < CACHE_HIGH:
            print("%#x (not in the shared cache)" % address)
            continue
        i = bisect.bisect_right(starts, address) - 1
        if i < 0:
            print("%#x (before the first image)" % address)
            continue
        base, path = table[i]
        print("%#x %s+0x%x" % (address, os.path.basename(path), address - base))
    return 0


if __name__ == '__main__':
    sys.exit(main())
