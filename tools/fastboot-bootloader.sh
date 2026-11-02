#!/bin/bash
# Put the phone into bootloader fastboot mode, ready for "fastboot flash".
#
# Two things go wrong without this:
#   - the phone can come up in fastbootd (the recovery's userspace fastboot, 18d1:4ee0). That
#     mode cannot write boot_a: it is a bootloader partition. fastboot lists the phone there,
#     so a wait for "fastboot devices" is satisfied by a phone that cannot be flashed.
#   - the phone is dark for a couple of minutes after a reboot (its init-retry loop): it is
#     neither in adb nor in fastboot, and a short wait gives up on it.
#
# So: bring the phone back to Android if it is in fastbootd, ask Android for the bootloader,
# and wait long enough for the phone to come back.
#
#   tools/fastboot-bootloader.sh [serial]
set -e
S=${1:-${HAKIOS_SERIAL:?set HAKIOS_SERIAL to the serial of your phone, as adb devices shows it}}

state() {
	# "fastboot" (the bootloader), "fastbootd" (the recovery's), "adb" (Android) or "gone"
	if fastboot devices | rg -q "^$S\s+fastboot$"; then echo fastboot; return; fi
	if fastboot devices | rg -q "^$S\s+fastbootd$"; then echo fastbootd; return; fi
	if [ "$(adb -s "$S" get-state 2>/dev/null)" = device ]; then echo adb; return; fi
	echo gone
}

echo "== the phone is in $(state) =="
if [ "$(state)" = fastbootd ]; then
	echo "== fastbootd cannot flash boot_a: back to Android first =="
	fastboot -s "$S" reboot >/dev/null 2>&1 || true
fi

# wait for Android
for i in $(seq 1 200); do
	[ "$(state)" = adb ] && break
	sleep 3
done
if [ "$(state)" != adb ]; then
	echo "the phone did not come back to Android" >&2
	exit 1
fi

echo "== asking for the bootloader =="
adb -s "$S" reboot bootloader
for i in $(seq 1 200); do
	[ "$(state)" = fastboot ] && break
	sleep 3
done
if [ "$(state)" != fastboot ]; then
	echo "the phone did not come up in fastboot (it is $(state))" >&2
	exit 1
fi
echo "== the phone is in fastboot =="
