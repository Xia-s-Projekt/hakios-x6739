#!/bin/bash
# Reboot the X6739 into Darwin/XNU once: mtkloader starts Android unless it finds this request
# (kept across the reboot by ramoops' pmsg zone); the boot after XNU is Android again.
#
# The request carries the phone's own battery reading (tools/batt-request.sh): the loader cannot
# read the gauge itself, and without a reading in the request it publishes no page, so the
# kernel's battery driver has nothing to poll and the phone shows no battery at all.
set -e
S=${HAKIOS_SERIAL:?set HAKIOS_SERIAL to the serial of your phone, as adb devices shows it}
HAKIOS_SERIAL=$S "$(dirname "$0")/batt-request.sh" 2>/dev/null || true
adb -s $S reboot
