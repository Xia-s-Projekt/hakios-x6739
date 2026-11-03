#!/bin/bash
# Build the phone-side programs this port adds: userland/hakios-cmd.
#
# The program runs on the phone (arm64, Darwin) and is put into the small root's
# bin by tools/mk-root.sh, so it is reachable as /redwood/bin/hakios-cmd once the
# phone's own iOS volume is the root.
#
# The cross compiler is the common tree's (xnu-deps/host/bin/xnu-clang) and the SDK
# is theos' (iPhoneOS16.5.sdk), the same pair the tree's own userland/build.sh and
# this port's tools/build-graphics.sh use.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
COMMON=${HAKIOS_COMMON:-$(cd "$ROOT/.." && pwd)/refs/hakios-xnu-kernel}
export THEOS=${THEOS:-/mnt/mainline/theos}

[ -x "$COMMON/xnu-deps/host/bin/xnu-clang" ] || {
	echo "build-userland.sh: no xnu-clang in $COMMON/xnu-deps/host/bin" >&2; exit 1; }
[ -d "$THEOS/sdks/iPhoneOS16.5.sdk" ] || {
	echo "build-userland.sh: no iPhoneOS16.5.sdk in $THEOS/sdks" >&2; exit 1; }

export PATH="$COMMON/xnu-deps/host/bin:$PATH"
mkdir -p "$ROOT/userland/obj"
for src in "$ROOT"/userland/*.c; do
	name=$(basename "$src" .c)
	echo "build-userland.sh: $name"
	xnu-clang -arch arm64 -isysroot "$THEOS/sdks/iPhoneOS16.5.sdk" -O1 \
		-o "$ROOT/userland/obj/$name" "$src"
done
xnu-clang -arch arm64 -isysroot "$THEOS/sdks/iPhoneOS16.5.sdk" -O1 \
	-fno-omit-frame-pointer -fno-optimize-sibling-calls -Wall -Wextra -Wno-frame-address \
	-dynamiclib -install_name /redwood/lib/libhakios.lsd.dylib -framework Foundation -lobjc \
	-o "$ROOT/userland/obj/libhakios.lsd.dylib" "$ROOT/userland/lsd-diagnostic.m"
xnu-clang -arch arm64 -isysroot "$THEOS/sdks/iPhoneOS16.5.sdk" -O1 \
	-fno-omit-frame-pointer -fno-optimize-sibling-calls -Wall -Wextra \
	-dynamiclib -install_name /redwood/lib/libhakios.sb-exit.dylib -framework Foundation -lobjc \
	-o "$ROOT/userland/obj/libhakios.sb-exit.dylib" "$ROOT/userland/springboard-exit.m"
ls -l "$ROOT/userland/obj"
