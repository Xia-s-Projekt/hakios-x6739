#!/bin/bash
# Write userland/system.apfs into the phone's xnusys partition.
# Needs Android up with root for the shell. LK's fastboot cannot take a file this large.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
S="${HAKIOS_SERIAL:-$(adb devices | awk '/\tdevice$/ { print $1; exit }')}"
[ -n "$S" ] || { echo "no adb device found"; exit 1; }
IMG="${1:-$ROOT/../refs/hakios-xnu-kernel/userland/system.apfs}"
[ -f "$IMG" ] || IMG="$ROOT/userland/system.apfs"
[ -f "$IMG" ] || { echo "no such image: $IMG"; exit 1; }

adb -s "$S" shell su -c 'ls -l /dev/block/by-name/xnusys' || {
	echo "xnusys not there: reboot once so the kernel rereads the partition table"; exit 1; }

TMP=/data/local/tmp/system.apfs
echo "sending $(du -h "$IMG" | cut -f1) to the phone ($S)..."
adb -s "$S" push "$IMG" "$TMP"

echo "writing it to xnusys..."
adb -s "$S" shell su -c "dd if=$TMP of=/dev/block/by-name/xnusys bs=4M conv=fsync"
adb -s "$S" shell su -c 'sync'
adb -s "$S" shell su -c "rm -f $TMP"

echo -n "checking magic: "
adb -s "$S" shell su -c 'dd if=/dev/block/by-name/xnusys bs=4096 count=1 status=none' | dd bs=1 skip=32 count=4 status=none
echo " <- an APFS container begins with NXSB at offset 32"
