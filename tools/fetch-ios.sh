#!/bin/bash
# Get the iOS 16.5 files (iPhone X, 20F66) that the X6739 port needs, and make what the loader and
# the kernel need out of them.
#
# Nothing of Apple's is committed: the files land in refs/ios/, which the repository ignores.
#
#   refs/ios/DeviceTree.d22ap.im4p  the iPhone X's device tree, out of which the loader's
#                                    identity.h is made (xnuloader/mkidentity.sh)
#   refs/ios/096-04088-077.dmg      the root filesystem, out of which the system image is chosen
#   refs/ios/096-04666-078.dmg      the OS cryptex: the dyld shared cache
#   refs/ios/096-03536-083.dmg      the App cryptex
#
# usage: tools/fetch-ios.sh [all|identity|system]
#
# Needs: python3, and for the system image 7z (7-Zip 23 or newer: it reads APFS) and cc. About
# 8 GB of disk while it runs.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
# the workspace: the common tree and the references live beside this repository, not in it
WORKSPACE=$(cd "$ROOT/.." && pwd)
COMMON=$WORKSPACE/refs/hakios-xnu-kernel
OUT=$ROOT/refs/ios
# the scratch directory: never /tmp, which is a RAM-backed tmpfs on this host
export TMPDIR=$WORKSPACE/refs/.tmp
mkdir -p "$OUT" "$TMPDIR"

[ -d "$COMMON" ] || { echo "fetch-ios.sh: no common tree at $COMMON"; exit 1; }

what=${1:-all}

if [ "$what" = all ] || [ "$what" = identity ]; then
	echo "== the iPhone X's device tree =="
	(cd "$OUT" && python3 "$HERE/ipsw-get.py" Firmware/all_flash/DeviceTree.d22ap.im4p)
	# the loader's identity.h: the properties iOS reads to tell which iPhone it is
	bash "$COMMON/xnuloader/mkidentity.sh" "$OUT/DeviceTree.d22ap.im4p"
fi

if [ "$what" = all ] || [ "$what" = system ]; then
	echo "== the root filesystem and the cryptexes =="
	(cd "$OUT" && python3 "$HERE/ipsw-get.py" \
		096-04088-077.dmg 096-04666-078.dmg 096-03536-083.dmg)
	# The common tree's sysimage/fetch.sh unpacks the images and lays them out the way
	# userland/build.sh expects. It looks for them by the names it uses internally, so they
	# are hard-linked there under those names (no copy: the images stay in refs/ios/).
	# The link is made only when the target is not already the same file: re-making it while
	# the fetch is running would truncate the download through the link.
	link_image() { # link_image SOURCE TARGET
		[ "$(stat -c %i "$1")" = "$(stat -c %i "$2" 2>/dev/null)" ] && return 0
		rm -f "$2"
		ln "$1" "$2"
	}
	link_image "$OUT/096-04088-077.dmg" "$COMMON/sysimage/rootfs.dmg"
	link_image "$OUT/096-04666-078.dmg" "$COMMON/sysimage/cryptex-os.dmg"
	link_image "$OUT/096-03536-083.dmg" "$COMMON/sysimage/cryptex-app.dmg"
	(cd "$COMMON/sysimage" && KEEP=1 bash fetch.sh)
fi

echo "done:"
/bin/ls -la "$OUT"
