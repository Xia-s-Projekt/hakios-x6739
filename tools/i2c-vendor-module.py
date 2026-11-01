#!/usr/bin/env python3
"""What the phone's own I2C module says about this SoC's controller.

The vendor's `i2c-mt65xx.ko` on the phone carries one `struct mtk_i2c_compatible` per supported
SoC. Which one the phone's I2C0 gets is decided by the DT compatible (`mediatek,mt6873-i2c`), and
that struct says which register table (v1 or v2), whether the slave address goes to SLAVE_ADDR or
SLAVE_ADDR1, and the flags the driver uses. This script reads the module's symbol table and
prints all of it, so the answer does not have to be guessed.

Usage: i2c-vendor-module.py [path/to/i2c-mt65xx.ko]
"""
import re
import struct
import sys

DEFAULT = ('/mnt/project/Misc/recovery_tree/android_recovery_device_x6739/'
           'recovery/root/lib/modules/i2c-mt65xx.ko')

# struct mtk_i2c_compatible, after the two pointers: the bit fields are packed into bytes
# exactly as the compiler lays them out (little endian, seven one-bit fields in the first byte).
FLAGS = (
    ('pmic_i2c', 0, 1),
    ('dcm', 1, 1),
    ('auto_restart', 2, 1),
    ('aux_len_reg', 3, 1),
    ('timing_adjust', 4, 1),
    ('dma_sync', 5, 1),
    ('ltiming_adjust', 6, 1),
)


def sections(blob):
    e_shoff, = struct.unpack_from('<Q', blob, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', blob, 0x3a)
    secs = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        name, typ, flags, addr, off, size, link, info = struct.unpack_from('<IIQQQQII', blob, o)
        secs.append(dict(name=name, typ=typ, addr=addr, off=off, size=size, link=link, info=info))
    shstr = secs[e_shstrndx]

    def sname(x):
        e = blob.index(b'\0', shstr['off'] + x)
        return blob[shstr['off'] + x:e].decode()

    for s in secs:
        s['nm'] = sname(s['name'])
    return secs


def symbols(blob, secs):
    symtab = next(s for s in secs if s['nm'] == '.symtab')
    strtab = secs[symtab['link']]
    out = {}
    for i in range(symtab['size'] // 24):
        o = symtab['off'] + i * 24
        name, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', blob, o)
        e = blob.index(b'\0', strtab['off'] + name)
        out[blob[strtab['off'] + name:e].decode(errors='replace')] = \
            dict(value=value, size=size, shndx=shndx)
    return out


def symbol_bytes(blob, secs, sym):
    sh = secs[sym['shndx']]
    off = sh['off'] + (sym['value'] - sh['addr'])
    return blob[off:off + sym['size']]


def relocations_for(blob, secs, target):
    """The RELA relocations that apply to `target` (a section name): {offset: addend}.

    A module's relocations live in their own section (`.rela.rodata.mtk_i2c_of_match`); the
    section they patch is that section's `sh_info`.
    """
    index = next((i for i, s in enumerate(secs) if s['nm'] == target), None)
    if index is None:
        return {}
    out = {}
    for s in secs:
        if s['typ'] != 4 or s['info'] != index:
            continue
        for i in range(s['size'] // 24):
            o = s['off'] + i * 24
            r_off, r_info, r_add = struct.unpack_from('<QQq', blob, o)
            out[r_off] = r_add
    return out


def decode_flags(raw):
    """The bit fields of struct mtk_i2c_compatible, from the bytes after the two pointers."""
    b = raw[16:]
    out = {}
    for name, bit, width in FLAGS:
        out[name] = (b[0] >> bit) & ((1 << width) - 1)
    out['dma_ver'] = b[1]
    out['apdma_sync'] = b[2] & 1
    out['speed_ver'] = (b[2] >> 1) & 1
    out['max_dma_support'] = b[3]
    out['slave_addr_ver'] = b[4]
    out['fifo_size'] = b[5]
    return out


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT
    blob = open(path, 'rb').read()
    secs = sections(blob)
    syms = symbols(blob, secs)
    rodata = next(s for s in secs if s['nm'] == '.rodata')
    rel = relocations_for(blob, secs, '.rodata')

    print(f'{path}\n')

    print('the register tables:')
    for nm in ('mt_i2c_regs_v1', 'mt_i2c_regs_v2'):
        if nm not in syms:
            print(f'  {nm}: absent')
            continue
        b = symbol_bytes(blob, secs, syms[nm])
        vals = [struct.unpack_from('<H', b, 2 * i)[0] for i in range(len(b) // 2)]
        print(f'  {nm} ({syms[nm]["size"]} bytes):')
        print('    ' + ' '.join(f'{v:#06x}' for v in vals))

    table_by_addr = {syms[nm]['value']: nm for nm in ('mt_i2c_regs_v1', 'mt_i2c_regs_v2')
                     if nm in syms}

    print('\nthe compatible profiles the module carries:')
    for nm in sorted(syms):
        if not nm.endswith('_compat'):
            continue
        sym = syms[nm]
        raw = symbol_bytes(blob, secs, sym)
        regs = rel.get(sym['value'] + 8)
        if regs is None:
            regs = struct.unpack_from('<Q', raw, 8)[0]
        flags = decode_flags(raw)
        print(f'  {nm}: regs = {table_by_addr.get(regs, hex(regs))}, '
              + ', '.join(f'{k}={v}' for k, v in flags.items()))

    print('\nthe compatibles the module matches on:')
    blob_rodata = blob[rodata['off']:rodata['off'] + rodata['size']]
    for m in re.finditer(rb'mediatek,[a-z0-9,_-]*-i2c[a-z0-9,_-]*', blob_rodata):
        print('  ' + m.group().decode())

if __name__ == '__main__':
    main()
