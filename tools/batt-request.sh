#!/bin/bash
# Ask for one XNU boot, carrying the phone's own battery reading.
#
# The loader cannot read the gauge itself: the MT6893's fuel gauge is behind the PMIC's
# firmware, and the only reader on this phone is Android's. So the reading is taken under
# Android, here, and carried in the request the loader finds in the pmsg zone: the loader
# publishes it in the page the kernel's battery driver polls.
#
# The request is "DARWIN-X6739:BOOT-XNU" plus ":batt=soc,mv,ma,tc,status,online":
#   soc     the capacity in percent, as the phone reports it
#   mv      the voltage in millivolts
#   ma      the current in milliamps, positive while the battery takes charge
#   tc      the temperature in Celsius
#   status  the power supply class's status (1 charging, 2 discharging, 3 not charging, 4 full)
#   online  1 while a charger is attached
#
# HAKIOS_REQUEST_EXTRA adds parameters of its own to the request, after the
# battery's: the loader reads them from the same line (tools/ios-cmd.sh adds
# "secs=N:extra=ARGS" that way). It must not contain a colon.
#
# The signs follow the kernel's own conventions (drivers/power/hakios.battery.c): the current
# is positive while the cell is charging, and the temperature is in Celsius, not tenths.
set -e
S=${HAKIOS_SERIAL:-$(adb devices | awk 'NR>1 && $2=="device" {print $1; exit}')}
[ -n "$S" ] || { echo "no phone on adb"; exit 1; }

read_one() {
	# The power supply class's uevent: one "POWER_SUPPLY_NAME=value" per line.
	adb -s "$S" shell "cat /sys/class/power_supply/$1/uevent 2>/dev/null" 2>/dev/null |
		sed -n "s/^POWER_SUPPLY_$2=//p" | head -1
}

BATT=$(read_one battery CAPACITY)
MV=$(read_one battery VOLTAGE_NOW)
UA=$(read_one battery CURRENT_NOW)
AVG=$(read_one battery CURRENT_AVG)
TEMP=$(read_one battery TEMP)
STATUS=$(read_one battery STATUS)
ONLINE=$(read_one charger ONLINE)

if [ -z "$BATT" ]; then
	echo "no battery reading from the phone; asking for the boot without one"
	adb -s "$S" shell 'su -c "printf DARWIN-X6739:BOOT-XNU > /dev/pmsg0"'
	exit 0
fi

# The status as the power supply class numbers it.
case "$STATUS" in
Charging)     ST=1 ;;
Discharging)  ST=2 ;;
"Not charging") ST=3 ;;
Full)         ST=4 ;;
*)            ST=0 ;;
esac

# The current: the gauge's own reading, with the sign the kernel wants: positive while the
# cell takes charge. The power supply class counts in microamps; the kernel in milliamps.
CUR=${UA:-$AVG}
CUR=${CUR#-}
[ -n "$CUR" ] || CUR=0
CUR=$(( CUR / 1000 ))
if [ "$ST" = 2 ]; then
	CUR="-$CUR"
fi

# The temperature is in tenths of a degree.
TC=$(( ${TEMP:-0} / 10 ))

# The voltage is in microvolts.
MV_MV=$(( ${MV:-0} / 1000 ))

REQ="DARWIN-X6739:BOOT-XNU:batt=$BATT,$MV_MV,$CUR,$TC,$ST,${ONLINE:-0}"
[ -n "$HAKIOS_REQUEST_EXTRA" ] && REQ="$REQ:$HAKIOS_REQUEST_EXTRA"
echo "request: $REQ"
adb -s "$S" shell "su -c \"printf '$REQ\n' > /dev/pmsg0\""
