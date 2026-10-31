#!/usr/bin/env python3
"""Download the members of Apple's iOS 16.5 archive (iPhone X) that the HakiOS port needs.

Apple publishes the update as a ZIP64 file on its CDN. This reads the archive's directory with
HTTP range requests and streams out only the members asked for, so that the 6.2 GB file itself is
never downloaded.

usage: ipsw-get.py MEMBER...        (each member is written into the current directory)

The members, and what they are for:

  Firmware/all_flash/DeviceTree.d22ap.im4p   the iPhone X's device tree; the loader's identity.h
                                             is made out of it (xnuloader/mkidentity.sh)

The three disk images (the root filesystem and the two cryptexes) are fetched by the common
tree's own sysimage/fetch.sh, which unpacks them as well.

Nothing of Apple's is committed: the files land in refs/ios/, which the repository ignores.
"""
import os
import struct
import sys
import time
import urllib.request
import zlib

URL = ('https://updates.cdn-apple.com/2023SpringFCS/fullrestores/032-85130/'
       '6AB0D10A-8AF9-4F6C-8CF5-A1B315CABE88/'
       'iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw')
SIZE = 6165698198
ATTEMPTS = 20


def fetch(start, end):
    request = urllib.request.Request(URL, headers={'Range': 'bytes=%d-%d' % (start, end)})
    return urllib.request.urlopen(request, timeout=120)


def directory():
    """The archive's member list: (name, compression method, compressed size, size, offset)."""
    tail = fetch(SIZE - 65536, SIZE - 1).read()
    at = tail.rfind(b'PK\x06\x06')
    cd_size, cd_offset = struct.unpack('<QQ', tail[at + 40:at + 56])
    cd = fetch(cd_offset, cd_offset + cd_size - 1).read()
    p = 0
    while cd[p:p + 4] == b'PK\x01\x02':
        method, = struct.unpack('<H', cd[p + 10:p + 12])
        csize, usize, nl, el, cl = struct.unpack('<IIHHH', cd[p + 20:p + 34])
        offset, = struct.unpack('<I', cd[p + 42:p + 46])
        name = cd[p + 46:p + 46 + nl].decode()
        extra = cd[p + 46 + nl:p + 46 + nl + el]
        q = 0
        while q + 4 <= len(extra):
            tag, length = struct.unpack('<HH', extra[q:q + 4])
            if tag == 1:
                values = list(struct.unpack('<' + 'Q' * (length // 8),
                                           extra[q + 4:q + 4 + length // 8 * 8]))
                if usize == 0xFFFFFFFF:
                    usize = values.pop(0)
                if csize == 0xFFFFFFFF:
                    csize = values.pop(0)
                if offset == 0xFFFFFFFF:
                    offset = values.pop(0)
            q += 4 + length
        yield name, method, csize, usize, offset
        p += 46 + nl + el + cl


def pull(name, method, csize, usize, offset):
    """One member, into the current directory under its own name."""
    out_path = os.path.basename(name)
    have = os.path.getsize(out_path) if os.path.exists(out_path) else 0
    if have == usize:
        print('%s -> %s: %d bytes (expected %d): already there'
              % (name, out_path, have, usize))
        return True
    header = fetch(offset, offset + 29).read()
    nl, el = struct.unpack('<HH', header[26:30])
    data = offset + 30 + nl + el
    written = -1
    for attempt in range(1, ATTEMPTS + 1):
        try:
            if method == 0:
                # stored, not compressed: a broken download continues where it stopped
                have = os.path.getsize(out_path) if os.path.exists(out_path) else 0
                if have > csize:
                    have = 0
                if have < csize:
                    with fetch(data + have, data + csize - 1) as stream, \
                            open(out_path, 'ab' if have else 'wb') as out:
                        while True:
                            chunk = stream.read(1 << 20)
                            if not chunk:
                                break
                            out.write(chunk)
                written = os.path.getsize(out_path)
            else:
                inflater = zlib.decompressobj(-15)
                written = 0
                with fetch(data, data + csize - 1) as stream, open(out_path, 'wb') as out:
                    while True:
                        chunk = stream.read(1 << 20)
                        if not chunk:
                            break
                        chunk = inflater.decompress(chunk)
                        out.write(chunk)
                        written += len(chunk)
                    tail = inflater.flush()
                    out.write(tail)
                    written += len(tail)
        except OSError as error:
            print('%s: %s' % (name, error), file=sys.stderr)
        if written == usize:
            break
        print('%s: the download broke off, trying again (%d of %d)'
              % (name, attempt, ATTEMPTS), file=sys.stderr)
        time.sleep(5)
    print('%s -> %s: %d bytes (expected %d)' % (name, out_path, written, usize))
    return written == usize


def main():
    wanted = set(sys.argv[1:])
    if not wanted:
        print(__doc__, file=sys.stderr)
        return 2
    ok = True
    for name, method, csize, usize, offset in directory():
        if name in wanted:
            if method not in (0, 8):
                print('%s: compression method %d is not supported' % (name, method),
                      file=sys.stderr)
                ok = False
            elif not pull(name, method, csize, usize, offset):
                ok = False
            wanted.discard(name)
    if wanted:
        print('not in the archive: %s' % ', '.join(sorted(wanted)), file=sys.stderr)
        ok = False
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
