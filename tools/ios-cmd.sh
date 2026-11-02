#!/bin/bash
# Run a command on the phone's iOS side, and show what it printed.
#
# usage: ios-cmd.sh "PROGRAM [ARGUMENTS...]"
#
# The phone has no network and no USB while iOS runs, so the command goes the way
# the tree's own runtime parameters go: the request is written into the phone's pmsg
# zone under Android (tools/batt-request.sh writes that zone), the loader reads it
# at the next boot and puts "cmd=..." on the kernel's command line, and the program
# userland/hakios-cmd.c runs it there. What it prints lands in the console and so in
# pstore, where this script reads it back.
#
# The command cannot contain a colon: the loader's parameters are separated by
# colons. Its output is in the log after the "hakios-cmd: running:" line.
set -e
S=${HAKIOS_SERIAL:?set HAKIOS_SERIAL to the serial of your phone, as adb devices shows it}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
CMD=${1:?usage: ios-cmd.sh "PROGRAM [ARGUMENTS...]"}
case "$CMD" in
*:*) echo "ios-cmd.sh: the command cannot contain a colon"; exit 1;;
esac

# how long the phone stays in iOS: the command runs when the launchd job list starts,
# which is some half a minute into the run, and the loader takes the phone back to
# Android when the time is up.
SECS=${XNU_SECS:-90}

# the request carries the phone's own battery reading as well (the loader cannot read
# the gauge itself): batt-request.sh writes the zone, with these parameters added.
HAKIOS_REQUEST_EXTRA="secs=$SECS:extra=cmd=$CMD" \
	HAKIOS_SERIAL=$S "$HERE/batt-request.sh"

echo "waiting for the phone to come back..."
adb -s $S reboot
for i in $(seq 1 200); do
	[ "$(adb -s $S get-state 2>/dev/null)" = device ] && break
	sleep 3
done
sleep 3
LOG=${XNU_LOG_OUT:-$ROOT/logs/ios-cmd.log}
for i in $(seq 1 15); do
	adb -s $S shell 'su -c "cat /sys/fs/pstore/console-ramoops-0"' > "$LOG" 2>/dev/null && [ -s "$LOG" ] && break
	sleep 2
done
echo "--- the phone's output ---"
sed -n '/hakios-cmd: running:/,$p' "$LOG" | head -100
