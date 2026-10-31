#!/usr/bin/env python3
"""Print the MT6893 facts a HakiOS port needs, read out of the phone's own device tree.

usage: tools/mtk-soc-facts.py DEVICE-TREE.dtb

Only numbers and names are read out; no part of the tree is copied (the same rule as the
Qualcomm tools/soc-args.py in the common tree).
"""
import struct
import sys


def nodes_of(blob):
    if blob[:4] != b'\xd0\x0d\xfe\xed':
        sys.exit('mtk-soc-facts.py: not a flattened device tree')
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
            sys.exit(f'mtk-soc-facts.py: unexpected token {token}')


def u32(value, index=0):
    return struct.unpack_from('>I', value, index * 4)[0]


def s(value):
    return value.split(b'\0')[0].decode(errors='replace')


def reg_pairs(props, root=None):
    """A reg property as (address, size) pairs, honouring the cell counts.

    The root's #address-cells and #size-cells apply to the whole tree; a node may override
    them. MediaTek's tree uses 2 address cells and 2 size cells at the root (64-bit addresses).
    """
    reg = props.get('reg')
    if reg is None:
        return []
    root = root or {}
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


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    blob = open(sys.argv[1], 'rb').read()
    nodes = nodes_of(blob)
    root = nodes.get('', {})

    print('# the SoC')
    for key in ('model', 'compatible'):
        if key in root:
            print(f'{key} = {s(root[key])}')

    print()
    print('# memory')
    for path, props in nodes.items():
        if path == '/memory' or path.endswith('/memory'):
            for addr, size in reg_pairs(props, root):
                print(f'{path} reg = {addr:#x} size {size:#x} ({size >> 20} MiB)')

    print()
    print('# cpus')
    for path, props in sorted(nodes.items()):
        if '/cpus/cpu@' in path:
            for addr, size in reg_pairs(props, root):
                print(f'{path} = {addr:#x}')
            if 'enable-method' in props:
                print(f'    enable-method = {s(props["enable-method"])}')
            if 'compatible' in props:
                print(f'    compatible = {s(props["compatible"])}')

    print()
    print('# interrupt controller')
    for path, props in nodes.items():
        if path == '/interrupt-controller' or path.endswith('/interrupt-controller'):
            for addr, size in reg_pairs(props, root):
                print(f'{path} reg = {addr:#x} size {size:#x}')
            for key in ('compatible', 'interrupt-controller', '#interrupt-cells', 'arm,gic-version'):
                if key in props:
                    raw = props[key]
                    print(f'    {key} = {u32(raw) if key == "#interrupt-cells" else s(raw)}')

    print()
    print('# psci')
    for path, props in nodes.items():
        if path.endswith('/psci'):
            for key in ('compatible', 'method', '#address-cells'):
                if key in props:
                    raw = props[key]
                    print(f'{path} {key} = {u32(raw) if key == "#address-cells" else s(raw)}')

    print()
    print('# ufs / storage')
    for path, props in nodes.items():
        if 'ufs' in path.lower() or 'ufshcd' in path.lower():
            for addr, size in reg_pairs(props, root):
                print(f'{path} reg = {addr:#x} size {size:#x}')
            if 'compatible' in props:
                print(f'    compatible = {s(props["compatible"])}')

    print()
    print('# display')
    for path, props in nodes.items():
        if 'dsi' in path.lower() or 'panel' in path.lower() or 'disp' in path.lower():
            if 'compatible' in props:
                print(f'{path} compatible = {s(props["compatible"])}')

    print()
    print('# the SoC\'s own nodes (top level)')
    for path, props in sorted(nodes.items()):
        if path.count('/') != 1 or not path:
            continue
        if 'compatible' in props:
            print(f'{path:44s} {s(props["compatible"])}')


if __name__ == '__main__':
    main()
