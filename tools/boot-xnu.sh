#!/bin/bash
# Reboot the X6739 into Darwin/XNU once: mtkloader starts Android unless it finds this request
# (kept across the reboot by ramoops' pmsg zone); the boot after XNU is Android again.
set -e
S=${HAKIOS_SERIAL:?set HAKIOS_SERIAL to the serial of your phone, as adb devices shows it}
adb -s $S shell 'su -c "printf DARWIN-X6739:BOOT-XNU > /dev/pmsg0"'
adb -s $S reboot
