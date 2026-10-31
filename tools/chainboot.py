#!/usr/bin/env python3
"""
Put a bare-metal program in front of Android's kernel in a boot image.

  chainboot.py <android-boot.img> <program.bin> <out.img> [payload.bin]

LK decompresses the kernel to 0x40080000 and enters it there. The new kernel is gzip of:
program.bin, padded to CHAIN_OFFSET, then Android's own (uncompressed) Image. The program runs
first and then moves the Image down to 0x40080000 and starts it. An optional payload (the
loader's "XNUP": the XNU kernel and its ramdisk) follows the Image, 16-aligned. The program's own
arm64 header gets the Image's length in res2 (0x28), the payload's offset in res3 (0x30) and a
covering image_size.

Everything else - the header, the ramdisk - is copied from the Android image.

MT6893: the X6739's boot image is a MediaTek one, not the AOSP one the reference port's script
knew. Its header says version 4 but keeps only three meaningful fields: the kernel's size, a
kernel address that is not an address (0x303a99) and a ramdisk size in 512-byte units
(0x200001aa = 1 GiB), which describes the ramdisk the vendor built, not the bytes in the file.
The page size is not in the header either; it is 4096, and the layout says so: the kernel starts
at 4096 and the ramdisk starts at the next 4096-byte boundary after the kernel, which is exactly
where the ramdisk's own "MEDIATEK" magic sits. So the sections are found by that, and the header
is copied with only the kernel's size changed.
"""
import gzip
import hashlib
import struct
import sys

CHAIN_OFFSET = 0x40000
# boot_a on the X6739 is 64 MB (the reference board's X6826B had 32 MB): an image any larger is
# refused by fastboot, and a script that sends fastboot's output nowhere would carry on as if it
# had been flashed.
BOOT_PARTITION = 64 << 20

# MediaTek's ramdisk header, at the start of the ramdisk section
MTK_RAMDISK_MAGIC = b'\x02\x21\x4c\x18'


def pad(b, n):
    return b + b'\0' * (-len(b) % n)


def sections(d):
    """The image's sections: the header, the kernel, the ramdisk.

    The header is one page; the kernel is a gzip stream of the given size; the ramdisk is what
    is left, starting at the first page boundary after the kernel.
    """
    assert d[:8] == b'ANDROID!', 'not an Android boot image'
    ksize, = struct.unpack_from('<I', d, 8)
    hver, = struct.unpack_from('<I', d, 40)

    page = 4096
    if hver >= 2:
        # a standard AOSP header says its page size; MediaTek's leaves it at zero
        p, = struct.unpack_from('<I', d, 36)
        if p in (2048, 4096, 8192, 16384):
            page = p

    def pages(n):
        return (n + page - 1) // page * page

    header = d[:page]
    kernel = d[page:page + ksize]
    ramdisk = d[page + pages(ksize):]

    # the ramdisk's own magic says where it starts: that is the check that the layout is right
    assert ramdisk[:4] == MTK_RAMDISK_MAGIC, \
        'the ramdisk is not where the header says it is (no MediaTek ramdisk magic at %d)' \
        % (page + pages(ksize))

    # The ramdisk section runs to the end of the partition, but only its first few megabytes hold
    # anything: the rest is the padding the vendor shipped. Keeping it would leave no room for
    # the payload.
    #
    # The last bytes of the partition are Android's verified-boot footer ("AVBf"); it is not the
    # ramdisk's, and it has to stay the last thing in the partition. It is cut off first, so the
    # padding before it can be found.
    footer = b''
    at = ramdisk.rfind(b'AVBf')
    if at >= 0 and len(ramdisk) - at <= 64:
        footer = ramdisk[at:]
        ramdisk = ramdisk[:at]
    # the padding is all zeros: the ramdisk ends at the last byte that is not one
    ramdisk = ramdisk[:len(ramdisk.rstrip(b'\0'))]
    return page, header, kernel, ramdisk, footer

def main():
    src, prog_path, out = sys.argv[1:4]
    payload = open(sys.argv[4], 'rb').read() if len(sys.argv) > 4 else b''
    d = open(src, 'rb').read()

    page, header, kernel, ramdisk, footer = sections(d)

    image = gzip.decompress(kernel) if kernel[:2] == b'\x1f\x8b' else kernel
    assert image[0x38:0x3c] == b'ARM\x64', 'Android kernel is not an arm64 Image'
    linux_image_size = struct.unpack_from('<Q', image, 0x10)[0]

    prog = bytearray(open(prog_path, 'rb').read())
    assert prog[0x38:0x3c] == b'ARM\x64', 'program has no arm64 header'
    assert len(prog) <= CHAIN_OFFSET, 'program too large'
    payload_off = CHAIN_OFFSET + len(pad(image, 16)) if payload else 0
    total = max(CHAIN_OFFSET + linux_image_size, payload_off + len(payload))
    struct.pack_into('<Q', prog, 0x10, (total + 0xfffff) & ~0xfffff)
    struct.pack_into('<Q', prog, 0x28, len(image))
    struct.pack_into('<Q', prog, 0x30, payload_off)
    blob = bytes(prog) + b'\0' * (CHAIN_OFFSET - len(prog)) + image
    if payload:
        blob = pad(blob, 16) + payload
    new_kernel = gzip.compress(blob, 9, mtime=0)

    hdr = bytearray(header)
    struct.pack_into('<I', hdr, 8, len(new_kernel))
    sha = hashlib.sha1()
    for part in (new_kernel, ramdisk):
        sha.update(part)
        sha.update(struct.pack('<I', len(part)))
    hdr[576:608] = pad(sha.digest(), 32)

    # The image fills the partition exactly: the header, the new kernel, the ramdisk, and then
    # the padding. Android's verified-boot footer ("AVBf") has to be the last thing in the
    # partition, so it is put back at the end.
    body = bytes(hdr) + pad(new_kernel, page) + ramdisk
    if len(body) + len(footer) > BOOT_PARTITION:
        raise SystemExit(f'{out}: {len(body) + len(footer)} bytes, more than the {BOOT_PARTITION} of the '
                         f'boot partition ({len(body) + len(footer) - BOOT_PARTITION} too many): the '
                         f'payload has to shrink')
    img = body + b'\0' * (BOOT_PARTITION - len(body) - len(footer)) + footer
    open(out, 'wb').write(img)
    print(f'{out}: {len(img)} bytes (program {len(prog)}, Image {len(image)}, payload {len(payload)}, '
          f'kernel gz {len(new_kernel)}, ramdisk {len(ramdisk)})')


if __name__ == '__main__':
    main()
