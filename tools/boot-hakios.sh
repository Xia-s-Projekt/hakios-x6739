#!/bin/bash
# Boot the configured HakiOS image on the X6739 (MT6893).
# Supports passing cpu count, kernel run duration (secs), and kernel command line arguments.
# If rebuilding/re-flashing is needed, it rebuilds with the specified parameters.
# Otherwise, it arms the one-shot pmsg flag and reboots into HakiOS directly.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

usage() {
	echo "Usage: $0 [options] [extra_xnu_args...]"
	echo ""
	echo "Options:"
	echo "  -i, --instant, --skip-flash  Skip build and flash, reboot directly into HakiOS with current boot_a"
	echo "  -c, --cpus <n>               Number of CPUs (e.g. 1, 2, 4)"
	echo "  -s, --secs <n>               Execution duration in seconds before return (e.g. 30, 60, 300)"
	echo "  -a, --args <str>             Kernel boot arguments string"
	echo "  -f, --flash                  Rebuild loader and payload with args and flash boot_a first"
	echo "  -h, --help                   Show this help message"
	echo ""
	echo "Examples:"
	echo "  $0 -i"
	echo "  $0 -c 1 -s 30"
	echo "  $0 --cpus 2 --secs 60 -f"
	echo "  $0 -c 1 -s 30 \"touchscreen=1\""
	exit 0
}

# Quick check for help before adb device probing
for arg in "$@"; do
	if [ "$arg" = "-h" ] || [ "$arg" = "--help" ]; then
		usage
	fi
done

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
INSTANT=0

EXTRA_POS_ARGS=()
while [ $# -gt 0 ]; do
	case "$1" in
		-i|--instant|--skip-flash|--skip-build)
			INSTANT=1
			shift
			;;
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

if [ "$INSTANT" -eq 1 ]; then
	REBUILD=0
elif [ "$FLASH" -eq 1 ] || [ ! -f "$IMG" ]; then
	REBUILD=1
elif [ -n "$CPUS" ] || [ -n "$SECS" ] || [ -n "$ARGS" ]; then
	REBUILD=1
fi

if [ "$REBUILD" -eq 1 ]; then
	echo "Building HakiOS image..."
	[ -n "$CPUS" ] && echo "  CPUs: $CPUS"
	[ -n "$SECS" ] && echo "  Duration: ${SECS}s"
	[ -n "$ARGS" ] && echo "  XNU Args: $ARGS"

	# Build mtkloader
	WATCH=$(( (SECS > 0 ? SECS : 120) + 240 ))
	"$ROOT/mtkloader/build.sh" -DXNU_WDT_SECONDS=0 -DXNU_CORES="${CPUS:-4}" \
		-DXNU_WATCH_SECONDS="$WATCH" "-DXNU_RETURN_AFTER=\"${SECS:-120}\"" \
		"-DXNU_EXTRA_ARGS=\"${ARGS:-cpus=4 fb_diff=1} epoch=$(date +%s)\"" \
		-DXNU_SMALL_ROOT=1 >/dev/null

	# Build payload if missing or when args/cmdline change
	python3 "$ROOT/tools/mkpayload.py" -o "$ROOT/build-payload.bin" \
		--kernel "${XNU_KERNEL:-$ROOT/mach.development.mt6893}" \
		--ramdisk "$ROOT/build-root.apfs" \
		--cmdline="rd=md0 ufs_root=1 ${ARGS:-cpus=4 fb_diff=1}" >/dev/null

	# Pack boot image directly
	python3 "$ROOT/tools/chainboot.py" "$ROOT/backup/boot_a.img" "$ROOT/mtkloader/mtkloader.bin" "$IMG" "$ROOT/build-payload.bin" >/dev/null

	echo "Arming one-shot HakiOS boot flag..."
	"$ROOT/tools/batt-request.sh"

	echo "Flashing boot_a and booting HakiOS directly..."
	adb -s "$SERIAL" reboot bootloader
	for i in $(seq 1 120); do fastboot devices | rg -q "$SERIAL" && break; sleep 1; done
	fastboot devices | rg -q "$SERIAL" || { echo "Device did not enter fastboot"; exit 1; }
	fastboot -s "$SERIAL" flash boot_a "$IMG"
	fastboot -s "$SERIAL" reboot
	echo "Device rebooted directly into HakiOS."
	exit 0
fi

echo "Arming one-shot HakiOS boot flag..."
"$ROOT/tools/batt-request.sh"
echo "Rebooting into HakiOS..."
adb -s "$SERIAL" reboot
echo "Device is rebooting into HakiOS (iOS 16.5)."
