#!/usr/bin/env python3
"""
Take the AVB structures out of a boot image, so that a bootloader will not refuse it.

  strip-avb.py <boot.img> <out.img>

What it does, and why:

The X6739's boot_a carries Android's verified-boot structures. The image ends with an AvbFooter
("AVBf") that points at a small vbmeta (768 bytes) inside the image, and that vbmeta holds a hash
descriptor for the partition "boot": the sha256 of the image up to the footer's
original_image_size. Change the image and the hash no longer matches.

There is a second layer: the phone's vbmeta_a partition is a top-level vbmeta with three chain
descriptors, and one of them covers "boot" too. That one lives on the phone, not in the image, so
this script cannot touch it.

The footer is 64 bytes at the very end of the partition, and the vbmeta it points at is inside the
image. Both are removed: the footer is overwritten with zeros and the vbmeta's bytes are zeroed,
so the image has no hash to fail. The vbmeta_a partition on the phone still has to be dealt with
separately -- either its flags are set to disable verification, or it is blanked.

usage: strip-avb.py BOOT.img OUT.img
"""
import struct
import sys

FOOTER_MAGIC = b'AVBf'
VBMETA_MAGIC = b'AVB0'


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__.strip())
    src, out = sys.argv[1], sys.argv[2]
    d = bytearray(open(src, 'rb').read())

    # the footer: the last "AVBf" in the image, at the very end of the partition
    at = d.rfind(FOOTER_MAGIC)
    if at < 0:
        raise SystemExit('no AVB footer in this image: nothing to strip')
    original_image_size, vbmeta_offset, vbmeta_size = struct.unpack_from('>3Q', d, at + 12)
    print('footer at %d: the image is %d bytes, its vbmeta %d bytes at %d'
          % (at, original_image_size, vbmeta_size, vbmeta_offset))

    if d[vbmeta_offset:vbmeta_offset + 4] != VBMETA_MAGIC:
        # the footer points at a vbmeta that is not there: the image was repacked and the
        # offset is stale. The vbmeta is the last "AVB0" before the footer.
        found = d.rfind(VBMETA_MAGIC, 0, at)
        if found < 0:
            raise SystemExit('the vbmeta the footer points at is not one, and there is none '
                             'before the footer either')
        vbmeta_offset = found
        print('  the offset is stale; the vbmeta is at %d' % vbmeta_offset)

    # the vbmeta is inside the image: zeroing it leaves no hash to fail
    d[vbmeta_offset:vbmeta_offset + vbmeta_size] = b'\0' * vbmeta_size
    # the footer itself: zeroed, so nothing points at the vbmeta any more
    d[at:at + 64] = b'\0' * 64

    open(out, 'wb').write(d)
    print('%s: %d bytes, the vbmeta and the footer are gone' % (out, len(d)))


if __name__ == '__main__':
    main()
