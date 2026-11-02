#!/bin/bash
# build-graphics.sh: the graphics library the window server draws with.
#
# The tree draws CoreAnimation with OpenGL ES (the tree's own docs/ru/metal.md section 4 argues
# that Metal is out of reach here and that the OpenGL ES path is the one to keep working). That
# path is two libraries:
#
#   libhakios.gl.dylib   the tree's own bridge (userland/src/eagl.c, userland/src/gl_osmesa.c):
#                         CoreAnimation's OpenGL ES calls, handed to Mesa
#   libOSMesa.8.dylib    Mesa itself, built by the tree's tools/mk-mesa.sh
#
# Without them the window server has no renderer at all, and the phone's log says so:
#
#   CoreAnimation: Failed to initialize Metal or OpenGLES contexts!
#
# and the screen shows no wallpaper (the tree's own UI is what it draws instead).
#
# This phone has no Adreno, so the tree's default Mesa build (softpipe + freedreno) is the wrong
# one: softpipe is a rasteriser in plain C and needs nothing of the machine but its processor, and
# freedreno is the Adreno's driver. This builds softpipe only.
#
# Where the two land:
#   $COMMON/userland/obj/libhakios.gl.dylib
#   $COMMON/xnu-deps/build/mesa/out/libOSMesa.8.dylib
#
# and tools/mk-root.sh copies them into the small root the phone carries (where the phone sees
# them as /redwood/lib).
#
#   tools/build-graphics.sh
#
# Needs: meson, ninja, bison, flex, python3, llvm-strip, and the tree's own toolchain (the
# cross compiler under xnu-deps/host/bin and the theos SDK under $THEOS). Nothing is written
# under the home directory.
set -e -o pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
COMMON=${HAKIOS_COMMON:-$HERE/../refs/hakios-xnu-kernel}
export THEOS=${THEOS:-../refs/theos}

# ninja is not in the distribution's packages here; a copy is kept beside this repository's cache
NINJA_DIR=${HAKIOS_NINJA_DIR:-$HOME/.cache/hakios-scratch/cache/scratch/bin}
[ -x "$NINJA_DIR/ninja" ] && PATH="$NINJA_DIR:$PATH"
command -v ninja >/dev/null || { echo "no ninja (HAKIOS_NINJA_DIR, or install it)" >&2; exit 1; }
command -v meson >/dev/null || { echo "no meson" >&2; exit 1; }

echo "== building Mesa (softpipe: a rasteriser that needs no GPU) =="
# the tree's script fetches Mesa, patches it and builds it; DRIVERS=softpipe leaves the Adreno's
# driver (freedreno) out, which is of no use on this phone
(cd "$COMMON" && DRIVERS=softpipe bash tools/mk-mesa.sh)

MESA=$COMMON/xnu-deps/build/mesa/out
echo "== building the tree's OpenGL ES bridge (libhakios.gl.dylib) =="
# the same two steps the tree's userland/build.sh does when it is given GL=1
cd "$COMMON/userland"
export PATH="$COMMON/xnu-deps/host/bin:$PATH"
./mkeagl.py src/eagl_slots.txt "$MESA/gl.xml" "$MESA/exported-gl.txt" src/eagl.c > obj/eagl_gen.h
xnu-clang -arch arm64 -isysroot "$THEOS/sdks/iPhoneOS16.5.sdk" -O1 \
	-dynamiclib -install_name /redwood/lib/libhakios.gl.dylib \
	-Iobj -framework CoreFoundation -framework IOKit -framework IOSurface -lobjc \
	"$MESA/libOSMesa.8.dylib" -o obj/libhakios.gl.dylib \
	src/eagl.c src/gl_osmesa.c

echo "built: $COMMON/userland/obj/libhakios.gl.dylib ($(stat -c %s obj/libhakios.gl.dylib) bytes)"
echo "built: $MESA/libOSMesa.8.dylib ($(stat -c %s "$MESA/libOSMesa.8.dylib") bytes)"
echo "the small root carries them now: tools/mk-root.sh"
