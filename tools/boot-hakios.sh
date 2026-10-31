#!/bin/bash
# Boot the configured HakiOS image on the X6739 (MT6893).
# If the device is connected via ADB, it requests the one-shot XNU boot flag
# and reboots directly into HakiOS without rebuilding or re-flashing.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SERIAL="${HAKIOS_SERIAL}"

if [ -z "$SERIAL" ]; then
	SERIAL=$(adb devices | awk 'NR>1 && $2=="device" {print $1; exit}')
fi

if [ -z "$SERIAL" ]; then
	echo "Error: no active adb device detected and HAKIOS_SERIAL is unset."
	echo "Connect the device or set HAKIOS_SERIAL=<serial>."
	exit 1
fi

echo "Target device: $SERIAL"

# Ensure boot_a is flashed with the latest built image if requested or missing
IMG="$ROOT/xnu-flashed-slot_a.img"
if [ "$1" = "--flash" ] || [ "$1" = "-f" ]; then
	if [ ! -f "$IMG" ]; then
		echo "Image $IMG not found. Building first via tools/xnu-test.sh..."
		XNU_NO_FLASH=1 "$ROOT/tools/xnu-test.sh"
	fi
	echo "Rebooting to bootloader to flash boot_a..."
	adb -s "$SERIAL" reboot bootloader
	for i in $(seq 1 120); do fastboot devices | rg -q "$SERIAL" && break; sleep 1; done
	fastboot -s "$SERIAL" flash boot_a "$IMG"
	fastboot -s "$SERIAL" reboot
	echo "Waiting for Android to come back up..."
	for i in $(seq 1 100); do [ "$(adb -s "$SERIAL" get-state 2>/dev/null)" = device ] && break; sleep 3; done
	sleep 4
fi

echo "Arming one-shot HakiOS boot flag..."
adb -s "$SERIAL" shell 'su -c "printf DARWIN-X6739:BOOT-XNU > /dev/pmsg0"'
echo "Rebooting into HakiOS..."
adb -s "$SERIAL" reboot
echo "Device is rebooting into HakiOS (iOS 16.5)."
