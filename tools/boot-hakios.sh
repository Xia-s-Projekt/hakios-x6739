#!/bin/bash
# Boot the configured HakiOS image on the X6739 (MT6893).
# Supports passing cpu count, kernel run duration (secs), and kernel command line arguments.
# If rebuilding/re-flashing is needed, it rebuilds with the specified parameters.
# Otherwise, it arms the one-shot pmsg flag and reboots into HakiOS directly.
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

# Defaults
CPUS="${XNU_CPUS:-}"
SECS="${XNU_SECS:-}"
ARGS="${XNU_ARGS:-}"
FLASH=0

usage() {
	echo "Usage: $0 [options] [extra_xnu_args...]"
	echo ""
	echo "Options:"
	echo "  -c, --cpus <n>      Number of CPUs (e.g. 1, 2, 4)"
	echo "  -s, --secs <n>      Execution duration in seconds before return (e.g. 30, 60, 300)"
	echo "  -a, --args <str>    Kernel boot arguments string"
	echo "  -f, --flash         Rebuild loader and payload with args and flash boot_a first"
	echo "  -h, --help          Show this help message"
	echo ""
	echo "Examples:"
	echo "  $0 -c 1 -s 30"
	echo "  $0 --cpus 2 --secs 60 -f"
	echo "  $0 -c 1 -s 30 \"touchscreen=1\""
	exit 0
}

EXTRA_POS_ARGS=()
while [ $# -gt 0 ]; do
	case "$1" in
		-c|--cpus|--cpu)
			CPUS="$2"
			shift 2
			;;
		-s|--secs|--sec)
			SECS="$2"
			shift 2
			;;
		-a|--args)
			ARGS="$2"
			shift 2
			;;
		-f|--flash)
			FLASH=1
			shift
			;;
		-h|--help)
			usage
			;;
		*)
			EXTRA_POS_ARGS+=("$1")
			shift
			;;
	esac
done

if [ ${#EXTRA_POS_ARGS[@]} -gt 0 ]; then
	if [ -n "$ARGS" ]; then
		ARGS="$ARGS ${EXTRA_POS_ARGS[*]}"
	else
		ARGS="${EXTRA_POS_ARGS[*]}"
	fi
fi

# If cpus was specified, ensure it is in ARGS unless already present
if [ -n "$CPUS" ]; then
	if [[ "$ARGS" =~ cpus= ]]; then
		ARGS=$(echo "$ARGS" | sed -E "s/cpus=[0-9]+/cpus=$CPUS/")
	else
		ARGS="cpus=$CPUS $ARGS"
	fi
fi
ARGS=$(echo "$ARGS" | xargs)

# Rebuild and flash if requested or if parameters require custom compilation
IMG="$ROOT/xnu-flashed-slot_a.img"
REBUILD=0

if [ "$FLASH" -eq 1 ] || [ ! -f "$IMG" ]; then
	REBUILD=1
elif [ -n "$CPUS" ] || [ -n "$SECS" ] || [ -n "$ARGS" ]; then
	REBUILD=1
fi

if [ "$REBUILD" -eq 1 ]; then
	echo "Rebuilding HakiOS payload and loader..."
	[ -n "$CPUS" ] && echo "  CPUs: $CPUS"
	[ -n "$SECS" ] && echo "  Duration: ${SECS}s"
	[ -n "$ARGS" ] && echo "  XNU Args: $ARGS"

	export HAKIOS_SERIAL="$SERIAL"
	[ -n "$SECS" ] && export XNU_SECS="$SECS"
	[ -n "$CPUS" ] && export XNU_CORES="$CPUS"
	[ -n "$ARGS" ] && export XNU_ARGS="$ARGS"

	XNU_NO_FLASH=1 "$ROOT/tools/xnu-test.sh"

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
