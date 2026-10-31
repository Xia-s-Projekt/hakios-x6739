#!/bin/bash
# Read the phone's storage back, to see what the preloader left and whether it answers.
#
# MT6893: the storage is UFS (ufshci@11270000), not an eMMC on MSDC0. The reference port's read
# test (MSDC_TEST, CMD17/CMD18) is for an eMMC card and does not apply: a UFS link has to be
# negotiated before it will return a block, and the descriptor is behind the phone's secure
# world.
#
# What this does instead: read the GPT off the phone's own block device under Android, which
# tells us the partition layout the loader and the kernel's UFS driver have to work with.
set -e
S=${HAKIOS_SERIAL:?set HAKIOS_SERIAL to the serial of your phone, as adb devices shows it}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

echo "the phone's block devices:"
adb -s $S shell ls -l /dev/block/by-name/ 2>/dev/null || adb -s $S shell 'ls /sys/class/block/'

echo
echo "the UFS controller and its state:"
adb -s $S shell su -c 'cat /proc/device-tree/soc/ufshci@11270000/compatible' 2>/dev/null | tr '\0' ' '
echo
adb -s $S shell 'ls /sys/class/scsi_host/ /sys/class/ufs/ 2>/dev/null' || true

echo
echo "the partitions (GPT):"
adb -s $S shell su -c 'sgdisk -p /dev/block/sda' 2>/dev/null \
	|| adb -s $S shell su -c 'parted /dev/block/sda print' 2>/dev/null \
	|| echo "neither sgdisk nor parted is on the phone; read the GPT from the host instead"

echo
echo "the sizes of the ones that matter:"
for p in boot_a boot_b super userdata; do
	printf '%-10s ' "$p"
	adb -s $S shell "ls -l /dev/block/by-name/$p 2>/dev/null | sed 's/.*-> //'" 2>/dev/null || echo "(none)"
done
