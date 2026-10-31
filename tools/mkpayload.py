#!/usr/bin/env python3
"""
Build the "XNUP" payload the loader reads out of the boot image.

  mkpayload.py -o payload.bin

The payload is a header (the magic "XNUP" and the size of what follows) and then entries: a
name, the size, and the bytes, each padded to sixteen. It holds:

  kernel    the XNU kernel (the prebuilt mach.development.bcm2837 of the common tree)
  ramdisk   the small root image (root.apfs), which the loader puts in memory as the second
            disk, where stage0 mounts it
  cmdline   the kernel's command line

The system image (system.apfs, 2.5 GB) is not in the payload: it does not fit in a boot image.
On this board the kernel reads it off the UFS instead (ufs_root=1), so the loader's own storage
path is compiled out and the payload carries only the small root.
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
COMMON = os.environ.get('HAKIOS_COMMON', os.path.join(ROOT, '..', 'refs', 'hakios-xnu-kernel'))


def add(entries, name, data):
    entries.append((name, data))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-o', '--output', required=True)
    ap.add_argument('--kernel', default=os.path.join(COMMON, 'prebuilt', 'mach.development.bcm2837'))
    ap.add_argument('--ramdisk', default=os.path.join(COMMON, 'userland', 'root.apfs'))
    ap.add_argument('--cmdline', default='rd=md0 ufs_root=1')
    args = ap.parse_args()

    items = []
    for name, path in (('kernel', args.kernel), ('ramdisk', args.ramdisk)):
        if not os.path.isfile(path):
            raise SystemExit('mkpayload: no %s at %s' % (name, path))
        add(items, name, open(path, 'rb').read())
    add(items, 'cmdline', args.cmdline.encode() + b'\0')

    body = b''
    for name, data in items:
        body += struct.pack('<16sII', name.encode(), len(data), 0)
        body += data + b'\0' * (-len(data) % 16)
    body += b'\0' * 24

    open(args.output, 'wb').write(struct.pack('<II', 0x50554e58, len(body)) + body)
    print('%s: %d bytes' % (args.output, len(body) + 8))
    for name, data in items:
        print('  %-8s %d bytes' % (name, len(data)))


if __name__ == '__main__':
    main()
