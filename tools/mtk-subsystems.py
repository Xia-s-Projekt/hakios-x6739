#!/usr/bin/env python3
"""Print the addresses of the subsystems a HakiOS loader needs, from the phone's own device tree.

usage: tools/mtk-subsystems.py DEVICE-TREE.dtb

Only numbers and names are read out; no part of the tree is copied.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mtk_fdt import load, reg_pairs, s  # noqa: E402


# What the loader and the drivers touch, and what MediaTek calls it. The names are the
# compatibles in the tree; the value is what a port has to change from the MT6768 board.
WANTED = [
    ('storage (UFS)', ('ufshci', 'ufs')),
    ('storage (eMMC/SD, MSDC)', ('msdc',)),
    ('SPI', ('spi',)),
    ('GPIO / pinctrl', ('pinctrl', 'gpio')),
    ('top clock generator', ('topckgen', 'topck')),
    ('infrastructure config', ('infracfg',)),
    ('peripheral config', ('pericfg',)),
    ('AP mixed clocks', ('apmixed',)),
    ('AP interrupt router', ('apirq',)),
    ('memory mux (MMSYS/EMI)', ('mmsram', 'mmsys', 'emi')),
    ('SCP / DSP', ('scp',)),
    ('audio', ('audio', 'aud', 'sound')),
    ('thermal', ('thermal', 'lvts', 'tsap')),
    ('watchdog', ('wdt', 'wdog')),
    ('timer', ('timer', 'tick')),
    ('mailbox', ('mbox',)),
    ('i2c', ('i2c',)),
    ('pmic', ('pmic', 'mt6360')),
    ('GPU', ('mali', 'gpu', 'g3d')),
]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    nodes = load(sys.argv[1])
    root = nodes.get('', {})

    for label, keys in WANTED:
        hits = []
        for path, props in sorted(nodes.items()):
            if not path:
                continue
            compat = s(props.get('compatible', b'')).lower()
            name = path.rsplit('/', 1)[-1].split('@', 1)[0].lower()
            if not any(k in compat or k == name for k in keys):
                continue
            addrs = reg_pairs(props, root)
            if not addrs:
                continue
            hits.append((path, compat, addrs))
        print(f'# {label}')
        if not hits:
            print('    (none)')
        for path, compat, addrs in hits[:12]:
            regs = ' '.join(f'{a:#x}+{sz:#x}' for a, sz in addrs)
            print(f'    {path:52s} {regs}   {compat}')
        print()


if __name__ == '__main__':
    main()
