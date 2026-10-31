#!/usr/bin/env python3
"""Read a flattened device tree without dtc: the nodes and their properties.

Used by the tools next to it; import it rather than running it.

Only numbers and names are read out; no part of the tree is copied (the same rule as the
Qualcomm tools/soc-args.py in the common tree).
"""
import struct


def nodes_of(blob):
    """The tree as {path: {property: bytes}}."""
    if blob[:4] != b'\xd0\x0d\xfe\xed':
        raise ValueError('not a flattened device tree')
    off_struct, off_strings = struct.unpack('>II', blob[8:16])
    size_strings, = struct.unpack('>I', blob[32:36])
    strings = blob[off_strings:off_strings + size_strings]
    out, path, at = {}, [], off_struct
    while True:
        token, = struct.unpack('>I', blob[at:at + 4])
        at += 4
        if token == 1:                                   # FDT_BEGIN_NODE
            end = blob.index(b'\0', at)
            path.append(blob[at:end].decode())
            at = (end + 4) & ~3
            out['/'.join(path)] = {}
        elif token == 2:                                 # FDT_END_NODE
            path.pop()
        elif token == 3:                                 # FDT_PROP
            length, name_off = struct.unpack('>II', blob[at:at + 8])
            at += 8
            name = strings[name_off:strings.index(b'\0', name_off)].decode()
            out['/'.join(path)][name] = blob[at:at + length]
            at = (at + length + 3) & ~3
        elif token == 9:                                 # FDT_END
            return out
        elif token != 4:                                 # 4 = FDT_NOP
            raise ValueError(f'unexpected token {token}')


def u32(value, index=0):
    return struct.unpack_from('>I', value, index * 4)[0]


def s(value):
    """A string property, up to its first NUL."""
    return value.split(b'\0')[0].decode(errors='replace')


def reg_pairs(props, root):
    """A reg property as (address, size) pairs, honouring the cell counts.

    The root's #address-cells and #size-cells apply to the whole tree; a node may override
    them. MediaTek's tree uses 2 address cells and 2 size cells at the root (64-bit addresses).
    """
    reg = props.get('reg')
    if reg is None:
        return []
    ac = u32(props['#address-cells']) if '#address-cells' in props else u32(root.get('#address-cells', b'\0\0\0\0'))
    sc = u32(props['#size-cells']) if '#size-cells' in props else u32(root.get('#size-cells', b'\0\0\0\0'))
    cells = [u32(reg, i) for i in range(len(reg) // 4)]
    out, i = [], 0
    while i + ac + sc <= len(cells):
        addr = 0
        for k in range(ac):                             # a 64-bit address: high word first
            addr = (addr << 32) | cells[i + k]
        size = 0
        for k in range(sc):
            size = (size << 32) | cells[i + ac + k]
        out.append((addr, size))
        i += ac + sc
    return out


def load(path):
    """The tree of the device tree file at `path`."""
    with open(path, 'rb') as f:
        return nodes_of(f.read())
