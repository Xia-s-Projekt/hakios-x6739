#!/usr/bin/env python3
"""Print the raw words of one device tree property, for a node the port is reading.

usage: tools/mtk-prop.py DEVICE-TREE.dtb NODE PROPERTY

Only numbers are read out; no part of the tree is copied.
"""
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from mtk_fdt import nodes_of  # noqa: E402


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    nodes = nodes_of(open(sys.argv[1], 'rb').read())
    path, prop = sys.argv[2], sys.argv[3]
    props = nodes.get(path)
    if props is None:
        sys.exit(f'mtk-prop.py: no node {path}')
    if prop not in props:
        sys.exit(f'mtk-prop.py: {path} has no {prop}')
    value = props[prop]
    print(f'{path} {prop} = {len(value)} bytes')
    print(' '.join(hex(int.from_bytes(value[i:i + 4], 'big')) for i in range(0, len(value) - len(value) % 4, 4)))
    if len(value) % 4:
        print('tail bytes:', value[len(value) - len(value) % 4:].hex())


if __name__ == '__main__':
    main()
