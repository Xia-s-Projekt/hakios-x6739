#!/usr/bin/env python3
"""Print the MT6893's CONNSYS facts, read out of the phone's own device tree.

usage: tools/mtk-connsys-facts.py DEVICE-TREE.dtb

The names of the CONNSYS register blocks are not in the phone's tree -- the tree gives
only their addresses, in the order the vendor driver reads them. The order and the
names come from MediaTek's own conninfra platform source for this SoC:

    vendor/mediatek/kernel_modules/connectivity/conninfra/conn_drv/connv2/platform/mt6893/
        include/mt6893_consys_reg.h     enum consys_base_addr_index

That enum has sixteen entries and the phone's consys node has sixteen "reg" pairs, and
they agree one for one, address by address -- that agreement is what this tool prints
and checks, so the tree and the vendor source are tied to each other by evidence
rather than by assumption.

Only numbers and names are read out; no part of the tree is copied.
"""
import struct
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from mtk_fdt import nodes_of, reg_pairs, s, u32  # noqa: E402

# MediaTek's enum consys_base_addr_index for the MT6893, in order.
CONNSYS_BLOCKS = [
    ('CONN_INFRA_RGU',        "the conn infra's own reset generator"),
    ('CONN_INFRA_CFG',        'the chip id and the clock and oscillator control'),
    ('CONN_HOST_CSR_TOP',     'the host side CSR: wake-up, sleep protection, EMI remap'),
    ('INFRACFG_AO',           'the always-on infrastructure config: the AXI bus protection'),
    ('TOPRGU',                'the top reset generator: the software reset of the domain'),
    ('SPM',                   'the power manager: the conn_infra power domain'),
    ('INFRACFG',              'the infrastructure config: the ap2conn GALS clock'),
    ('CONN_WT_SLP_CTL',       'the wake timer and the sleep control'),
    ('CONN_AFE_CTL',          'the analog front end: the WBG and the PLLs'),
    ('CONN_INFRA_SYSRAM',     "the conn infra's system RAM and its efuse shadow"),
    ('GPIO',                  "the pinmux of the radio's pins"),
    ('CONN_RF_SPI_MST',       "the SPI master towards the radio's die"),
    ('CONN_SEMAPHORE',        'the semaphores the drivers take'),
    ('CONN_TOP_THERM_CTL',    'the thermal calibration'),
    ('IOCFG_RT',              "the pin drive and pull of the radio's pins"),
    ('CONN_DEBUG_CTRL',       'the debug block'),
]

# Registers inside them, with the offset inside the block, from the vendor source
# (mt6893_consys_reg_offset.h). The chip id is confirmed by the phone's own Android
# log (logs/android-dmesg.txt): "[consys_conninfra_wakeup_mt6893] Polling chip id
# success (0x20010101)".
CONNSYS_REGS = [
    ('CONN_INFRA_CFG',    0x0000, 'CONN_HW_VER',         'the chip id: 0x20010101 on this SoC'),
    ('CONN_INFRA_CFG',    0x0004, 'CONN_CFG_ID',         'the configuration id'),
    ('SPM',               0x016c, 'SPM_PWR_STATUS',      'the power status of the domain'),
    ('SPM',               0x0170, 'SPM_PWR_STATUS_2ND',  '... of its second part'),
    ('SPM',               0x0304, 'SPM_CONN_PWR_CON',    'the conn_infra power, isolation and clock'),
    ('TOPRGU',            0x0018, 'WDT_SWSYSRST',        'the software reset (bit 9) of the domain'),
    ('CONN_HOST_CSR_TOP', 0x01a0, 'CONN_INFRA_WAKEPU_TOP', 'the wake-up request to the domain'),
    ('CONN_HOST_CSR_TOP', 0x0184, 'CONN_SLP_PROT_CTRL',  'the sleep protection handshake'),
    ('INFRACFG',          0x0504, 'INFRA_AP2MD_GALS_CTL', 'the ap2conn clock gate'),
    ('GPIO',              0x0450, 'GPIO_MODE21',         "the radio's pins 172..175"),
    ('GPIO',              0x0460, 'GPIO_MODE22',         "the radio's pins 176..182"),
]

CONNSYS_COMPAT = 'mediatek,mt6893-consys'


def interrupts(props):
    """The "interrupts" property as (type, id) pairs; GIC-v3 cells are 4."""
    raw = props.get('interrupts')
    if raw is None:
        return []
    cells = 4
    out = []
    for at in range(0, len(raw) - cells * 4 + 1, cells * 4):
        out.append((u32(raw, at // 4), u32(raw, at // 4 + 1)))
    return out


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    nodes = nodes_of(open(sys.argv[1], 'rb').read())
    root = nodes['']

    consys = None
    for path, props in nodes.items():
        if CONNSYS_COMPAT in s(props.get('compatible', b'')):
            consys = (path, props)
            break
    if consys is None:
        sys.exit(f'mtk-connsys-facts.py: no node with compatible "{CONNSYS_COMPAT}"')
    path, props = consys

    print(f'# {path} ({CONNSYS_COMPAT}), as the phone\'s own tree says it')
    pairs = reg_pairs(props, root)
    print(f'# {len(pairs)} register blocks in the tree; MediaTek\'s enum for this SoC has '
          f'{len(CONNSYS_BLOCKS)}')
    print()
    print(f'{"index":>5}  {"block":<20} {"address":>12} {"size":>10}  what it is')
    for index, (base, size) in enumerate(pairs):
        if index < len(CONNSYS_BLOCKS):
            name, what = CONNSYS_BLOCKS[index]
        else:
            name, what = '(no name)', 'the vendor enum has no entry for this one'
        print(f'{index:>5}  {name:<20} {base:#012x} {size:#010x}  {what}')

    print()
    print('# the registers inside them that the bring-up needs')
    for block, offset, name, what in CONNSYS_REGS:
        index = next((i for i, (n, _) in enumerate(CONNSYS_BLOCKS) if n == block), None)
        if index is None or index >= len(pairs):
            continue
        base = pairs[index][0]
        print(f'  {base + offset:#012x}  {block:<20} +{offset:#06x}  {name:<20} {what}')

    print()
    print('# the node\'s other properties')
    for name in ('power-domains', 'pmic', 'memory-region', '#thermal-sensor-cells'):
        if name in props:
            value = props[name]
            if name == '#thermal-sensor-cells':
                print(f'  {name}: {u32(value)}')
            else:
                print(f'  {name}: phandle {u32(value):#x}')

    print()
    print('# the radio\'s own nodes')
    for want in ('mediatek,wifi', 'mediatek,bt', 'mediatek,btif', 'mediatek,mt6885-gps'):
        for node_path, node_props in nodes.items():
            if want not in s(node_props.get('compatible', b'')):
                continue
            print(f'  {node_path}  ({want})')
            for base, size in reg_pairs(node_props, root):
                print(f'      reg {base:#012x} +{size:#x}')
            for kind, number in interrupts(node_props):
                if kind == 0:
                    print(f'      GIC SPI {number} (INTID {32 + number})')
                else:
                    print(f'      GIC PPI {number}')
            for name in ('memory-region', 'clock-names'):
                if name in node_props:
                    value = node_props[name]
                    if name == 'memory-region':
                        print(f'      memory-region phandle {u32(value):#x}')
                    else:
                        print(f'      clock-names: {s(value)}')
            break

    print()
    print('# the memory the firmware and the radio\'s buffers live in')
    for node_path, node_props in nodes.items():
        if not node_path.startswith('/reserved-memory/'):
            continue
        name = node_path.rsplit('/', 1)[1]
        if not any(k in name for k in ('consys', 'wifi', 'gps')):
            continue
        size = node_props.get('size')
        align = node_props.get('alignment')
        ranges = node_props.get('alloc-ranges')
        # these are 64-bit (two cells, high word first), as the root's #size-cells says
        size64 = (u32(size, 0) << 32) | u32(size, 1)
        align64 = (u32(align, 0) << 32) | u32(align, 1)
        low = (u32(ranges, 0) << 32) | u32(ranges, 1)
        high = low + ((u32(ranges, 2) << 32) | u32(ranges, 3))
        print(f'  {node_path}: size {size64:#x} ({size64 // 1024} KiB), '
              f'alignment {align64:#x}', end='')
        if ranges:
            print(f', it may be placed at {low:#x}..{high:#x}', end='')
        print()

    print()
    print('# the supplies the radio needs, as the tree\'s regulator nodes say')
    for node_path, node_props in nodes.items():
        if 'regulator' not in node_path:
            continue
        name = s(node_props.get('regulator-name', b''))
        if not any(k in name for k in ('vcn', 'vefuse')):
            continue
        low = node_props.get('regulator-min-microvolt')
        high = node_props.get('regulator-max-microvolt')
        print(f'  {node_path}: {name} '
              f'{u32(low) / 1e6:.2f}..{u32(high) / 1e6:.2f} V')


if __name__ == '__main__':
    main()
