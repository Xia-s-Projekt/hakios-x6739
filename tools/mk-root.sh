#!/bin/bash
# Build the small root image the payload carries: the common tree's userland/root, prepared for
# the iOS-root flow.
#
# The kernel's root for a first run is this image (the loader's XNU_SMALL_ROOT=1). With
# boot-arg root_apfs=ufs_xnusys our init mounts the phone's own iOS volume on /flash and then
# makes it the root (userland/src/init.c, flash_root). The mount point is a directory in the
# kernel's root, so it has to be in this image: without it the mount fails with ENOENT before
# the APFS driver is reached.
#
# The rest of the preparation is what the iOS 16.5 handover needs:
#   - sbin/launchd and sbin/launchd.ios point at init: process 1 is init, and init hands over
#     to the real iOS launchd through etc/rc
#   - launchd's stdout+stderr go to /dev/null when etc/quiet is there (otherwise launchd floods
#     the console): userland/src/init.c does that itself, so there is no patch of sbin/init's
#     binary any more (there was one, at fixed offsets, and it broke when init was rebuilt)
#   - redwood -> . : the tree's own name for this image, and the path init's rc jobs use
#   - Cryptexes/OS and Cryptexes/App: empty mount points for the cryptexes
#   - etc/quiet, the rewritten etc/rc and etc/rc.jobs, and the two plist edits
#
# This is the one copy of that recipe: tools/xnu-test.sh calls this too, so the two paths cannot
# drift apart again (they did once, and the run that lost the init patches booted to a halt).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
COMMON=${HAKIOS_COMMON:-$(cd "$ROOT/.." && pwd)/refs/hakios-xnu-kernel}
# The graphics library the window server draws with, and the Mesa it is built on. They live in the
# tree's own build (tools/build-graphics.sh makes them); without them the window server has no
# renderer at all: "CoreAnimation: Failed to initialize Metal or OpenGLES contexts!"
GL_DIR=${HAKIOS_GL_DIR:-$COMMON/userland/obj}
MESA_DIR=${HAKIOS_MESA_DIR:-$COMMON/xnu-deps/build/mesa/out}

rm -rf "$ROOT/build-root"
cp -a "$COMMON/userland/root" "$ROOT/build-root"
# init is the prebuilt one, not the one this tree builds: the prebuilt is the binary the phone
# was brought up with, and init's own code and the etc/rc below name the same handover
# ("/sbin/launchd.ios"). A rebuilt init knows the name the tree has since renamed it to, and
# the two must agree or the handover does not happen and nothing runs.
if [ -f "$COMMON/prebuilt/userland/init" ]; then
	cp "$COMMON/prebuilt/userland/init" "$ROOT/build-root/sbin/init"
fi
if [ -n "${HAKIOS_RC_JOBS:-}" ] && [ ! -f "$COMMON/userland/$HAKIOS_RC_JOBS" ]; then
	echo "mk-root.sh: no userland/$HAKIOS_RC_JOBS" >&2
	exit 1
fi
ln -sf init "$ROOT/build-root/sbin/launchd"
ln -sf init "$ROOT/build-root/sbin/launchd.ios"
ln -sf private/var/tmp "$ROOT/build-root/tmp"
mkdir -p "$ROOT/build-root/flash" "$ROOT/build-root/Cryptexes/OS" "$ROOT/build-root/Cryptexes/App"
ln -sf . "$ROOT/build-root/redwood"
if [ "${HAKIOS_LSD_DIAGNOSTIC:-0}" = 1 ]; then
	cp "$ROOT/userland/obj/libhakios.lsd.dylib" "$ROOT/build-root/lib/"
	printf '%s\n' '/redwood/lib/libhakios.lsd.dylib lsd' >> "$ROOT/build-root/etc/inject.conf"
fi
if [ "${HAKIOS_SB_EXIT_DIAGNOSTIC:-0}" = 1 ]; then
	cp "$ROOT/userland/obj/libhakios.sb-exit.dylib" "$ROOT/build-root/lib/"
	printf '%s\n' '/redwood/lib/libhakios.sb-exit.dylib SpringBoard' >> "$ROOT/build-root/etc/inject.conf"
fi
# The graphics library the window server draws with, and the Mesa under it. The phone sees this
# image's /lib as /redwood/lib, and the window server's library (libhakios.dylib, injected into
# backboardd) opens /redwood/lib/libhakios.gl.dylib from there.
if [ -f "$GL_DIR/libhakios.gl.dylib" ] && [ -f "$MESA_DIR/libOSMesa.8.dylib" ]; then
	rm -f "$ROOT/build-root/lib/libOSMesa.8.dylib" "$ROOT/build-root/lib/libhakios.gl.dylib"
	cp "$GL_DIR/libhakios.gl.dylib" "$ROOT/build-root/lib/"
	cp "$MESA_DIR/libOSMesa.8.dylib" "$ROOT/build-root/lib/"
else
	echo "mk-root.sh: no graphics library ($GL_DIR/libhakios.gl.dylib, $MESA_DIR/libOSMesa.8.dylib):" >&2
	echo "mk-root.sh: the window server will have no renderer (tools/build-graphics.sh makes them)" >&2
fi
if [ -f "$COMMON/userland/obj/gl_test" ]; then
	cp "$COMMON/userland/obj/gl_test" "$ROOT/build-root/bin/"
fi
if [ -f "$COMMON/userland/obj/fps_info" ]; then
	cp "$COMMON/userland/obj/fps_info" "$ROOT/build-root/bin/"
fi
# Ad-hoc sign all dylibs and binaries in the image so dyld treats them as signed platform libraries
LDID_BIN=${LDID:-/mnt/mainline/theos/toolchain/linux/iphone/bin/ldid}
if [ -x "$LDID_BIN" ]; then
	for dylib in "$ROOT/build-root/lib"/*.dylib; do
		[ -f "$dylib" ] && "$LDID_BIN" -S "$dylib" 2>/dev/null || true
	done
	for bin in "$ROOT/build-root/bin"/*; do
		[ -f "$bin" ] && "$LDID_BIN" -S "$bin" 2>/dev/null || true
	done
fi
# Assets for SpringBoard: wallpaper image
WP_SRC="$COMMON/sysimage/rootfs/System/Library/PrivateFrameworks/Settings/DisplayAndBrightnessSettings.framework/ContinuousExposeLayout-Dark-LTR.ca/assets/SydneyDarkWallpaper.jpg"
if [ -f "$WP_SRC" ]; then
	mkdir -p "$ROOT/build-root/share" "$ROOT/build-root/var/mobile/Library"
	cp "$WP_SRC" "$ROOT/build-root/share/wallpaper.jpg"
	cp "$WP_SRC" "$ROOT/build-root/var/mobile/Library/wallpaper.jpg"
fi
# Mobile directories and preference caches for widgets and apps:
# WeatherWidget and other apps crash immediately if $HOME/Documents is missing
# (Swift force-unwraps urls(for: .documentDirectory, in: .userDomainMask).first!)
mkdir -p "$ROOT/build-root/var/mobile/Documents" \
	"$ROOT/build-root/var/mobile/Library/Weather" \
	"$ROOT/build-root/var/mobile/Library/Preferences" \
	"$ROOT/build-root/var/mobile/Containers/Shared/AppGroup"
printf 'd\t755\t501\t501\t/private/var/mobile/Documents\n' >> "$ROOT/build-root/etc/var.skel"
printf 'd\t755\t501\t501\t/private/var/mobile/Library/Weather\n' >> "$ROOT/build-root/etc/var.skel"
printf 'd\t755\t501\t501\t/private/var/mobile/Containers/Shared/AppGroup\n' >> "$ROOT/build-root/etc/var.skel"
python3 -c "
import plistlib
p = '$ROOT/build-root/var/mobile/Library/Preferences/com.apple.weather.plist'
plistlib.dump({
    'Cities': [{
        'Name': 'Cupertino',
        'State': 'CA',
        'Lat': 37.33182,
        'Lon': -122.03118,
        'Timezone': 'America/Los_Angeles',
    }],
    'LastViewedCity': 0,
    'Celsius': False,
}, open(p, 'wb'))
"
printf 'f\t644\t501\t501\t/private/var/mobile/Library/Preferences/com.apple.weather.plist\t/var/mobile/Library/Preferences/com.apple.weather.plist\n' >> "$ROOT/build-root/etc/var.skel"
if [ -f "$COMMON/userland/var/mobile/Library/Preferences/com.apple.UIKit.plist" ]; then
	cp "$COMMON/userland/var/mobile/Library/Preferences/com.apple.UIKit.plist" "$ROOT/build-root/var/mobile/Library/Preferences/com.apple.UIKit.plist"
	printf 'f\t644\t501\t501\t/private/var/mobile/Library/Preferences/com.apple.UIKit.plist\t/var/mobile/Library/Preferences/com.apple.UIKit.plist\n' >> "$ROOT/build-root/etc/var.skel"
fi
# Hook WeatherWidget with libhakios.app.dylib for diagnostic output on /dev/console
if [ -f "$ROOT/build-root/etc/inject.conf" ]; then
	sed -i -e 's|/redwood/lib/libhakios.app.dylib Sileo|/redwood/lib/libhakios.app.dylib Sileo WeatherWidget|' "$ROOT/build-root/etc/inject.conf"
fi
# The jobs the phone's launchd starts. The tree's own list (userland/root/LaunchDaemons) is what
# the tree built; the wallpaper needs more of them: PosterBoard, which draws the wallpaper live in
# its own process, never starts without the daemons it waits for. The tree's notes (docs/ru/notes.md)
# name the stall exactly: cloudd asks com.apple.nsurlsessiond thousands of times a session, and
# nsurlsessiond is not in the image, so the home screen never appears and no widget or poster does
# either. With nsurlsessiond and cloudd in, the same run reaches 112 processes and 15 widget and
# poster processes, PosterBoard among them.
if [ "${HAKIOS_QUIET_JOBS:-0}" != 1 ] && [ -x "$(command -v python3)" ] && [ -d "$COMMON/sysimage/rootfs" ]; then
	# --rest adds the launch angels: the applications without an icon that draw parts of the
	# system's own interface in a process of their own -- the wallpaper (PosterBoard, PaperBoard),
	# the keyboard, the now playing controls. They are reached through a Mach service like a
	# daemon, and only when a program asks for one, so the tree takes out what would start them
	# by itself (RunAtLoad, KeepAlive). Without them there is no wallpaper: the phone's log says
	# "CoreAnimation: Failed to initialize Metal or OpenGLES contexts!" and the screen shows the
	# dock over nothing.
	#
	# The daemons the home screen waits for are named in the tree's notes (docs/ru/notes.md):
	# cloudd asks com.apple.nsurlsessiond thousands of times a session, nsurlsessiond is not in
	# the image, and the home screen and every widget and poster never come up. ALLOW lets those
	# through the tree's own DENY list.
	#
	# OSLOG=com.apple.backboardd: the window server's own os_log output goes to the console.
	# That is where it says what it believes the screen is ("initial main screen size:{1125,
	# 2436} scale:2"), which is the one place the scale of the display can be read from.
	#
	# HAKIOS_QUIET_JOBS=1 leaves the tree's own smaller set of jobs in the image instead. The
	# phone's console drops lines when there is a lot of output on it, and the window server's
	# own line about the screen is early in the run: with the whole job set going, it is gone
	# before the log comes back.
	(cd "$COMMON/userland" && ALLOW="cloudd nsurlsessiond appstored chronod mediaremote weatherd" \
		OSLOG="com.apple.backboardd" \
		python3 mkjobs.py --rest ../sysimage/rootfs "$ROOT/build-root/LaunchDaemons" \
		com.apple.notifyd com.apple.logd com.apple.containermanagerd com.apple.containermanagerd.system \
		com.apple.runningboardd com.apple.backboardd com.apple.mobile.usermanagerd com.apple.lsd \
		com.apple.iconservices.iconservicesagent com.apple.pluginkit.pkd com.apple.SpringBoard \
		com.apple.nsurlsessiond com.apple.cloudd com.apple.appstored \
		com.apple.chronod com.apple.mediaremoted com.apple.distnoted.xpc.daemon \
		com.apple.weatherd) >/dev/null 2>&1
	python3 -c "
import plistlib, os
for label in ('com.apple.chronod', 'com.apple.mediaremoted', 'com.apple.weatherd'):
    p = os.path.join('$ROOT/build-root/LaunchDaemons', f'{label}.plist')
    if os.path.isfile(p):
        d = plistlib.load(open(p, 'rb'))
        d['RunAtLoad'] = True
        d['KeepAlive'] = True
        plistlib.dump(d, open(p, 'wb'))
"
fi
# The patch of init's binary: launchd's standard output and standard error go to /dev/null when
# /etc/quiet is there (otherwise launchd floods the console). The offsets are the addresses of
# the instructions in one particular build of init, so a rebuild of init moves them; the patch
# then does nothing rather than stopping the build, and the console is loud but the phone boots.
python3 -c "
import struct
p = '$ROOT/build-root/sbin/init'
b = bytearray(open(p, 'rb').read())
try:
    assert struct.unpack('<Q', b[0x8000:0x8008])[0] == 0x100005d92
    struct.pack_into('<Q', b, 0x8000, 0x100005d17)
    assert b[0x5a0:0x5a4] == bytes.fromhex('a0000035')
    b[0x5a0:0x5a4] = bytes.fromhex('1f2003d5')

    assert b[0x3684:0x3688] == bytes.fromhex('80010035')
    assert b[0x369c:0x36a0] == bytes.fromhex('c000f837')
    assert b[0x36a4:0x36a8] == bytes.fromhex('41008052')
    assert b[0x36c8:0x36cc] == bytes.fromhex('a1050054')

    b[0x3684:0x3688] = bytes.fromhex('e0010035')
    b[0x369c:0x36a0] = bytes.fromhex('2001f837')
    b[0x36a4:0x36c8] = bytes.fromhex('21008052a7050094e00319aa41008052a4050094e00319aa8d050094e06f40f92e000014')
    open(p, 'wb').write(b)
except (AssertionError, IndexError):
    print('mk-root.sh: init was rebuilt and its binary patch no longer fits: the console will be loud')
"
# SpringBoard's own os_log output goes to the console only when asked for: it says a megabyte
# in half a minute, and the phone's console drops lines, so it pushes everything else out of
# the log that comes back -- the window server's own line about the screen among it.
if [ "${HAKIOS_SB_OSLOG:-0}" = 1 ]; then
	python3 -c "
import plistlib
p = '$ROOT/build-root/LaunchDaemons/com.apple.SpringBoard.plist'
pl = plistlib.load(open(p, 'rb'))
env = pl.setdefault('EnvironmentVariables', {})
env['OS_ACTIVITY_DT_MODE'] = 'YES'
env['OS_ACTIVITY_MODE'] = 'debug'
plistlib.dump(pl, open(p, 'wb'))
"
fi
touch "$ROOT/build-root/etc/quiet"
# The command the host asked for, if any (tools/ios-cmd.sh). It is a launch job of its own:
# the job runner (etc/rc.jobs) cannot show its output, because init's patch sends its own
# standard output to /dev/null when etc/quiet is there.
if [ -x "$ROOT/userland/obj/hakios-cmd" ]; then
	cp "$ROOT/userland/obj/hakios-cmd" "$ROOT/build-root/bin/"
	python3 -c "
import plistlib
plistlib.dump({
    'Label': 'com.hakios.cmd',
    'ProgramArguments': ['/redwood/bin/hakios-cmd'],
    'RunAtLoad': True,
    'AbandonProcessGroup': True,
    'ProcessType': 'Interactive',
    'LimitLoadToSessionType': 'System',
    'StandardOutPath': '/dev/console',
    'StandardErrorPath': '/dev/console',
}, open('$ROOT/build-root/LaunchDaemons/com.hakios.cmd.plist', 'wb'))
"
fi
cat > "$ROOT/build-root/etc/rc" << 'EOF'
# Hand process 1 over to the real launchd. The name is the one init's own code knows: it
# compares argv[1] with "/sbin/launchd.ios" and rewrites it to "/sbin/launchd", which is the
# phone's own launchd on the volume that is the root by then.
exec /sbin/launchd.ios
EOF
python3 -c "
import plistlib
p = '$ROOT/build-root/LaunchDaemons/com.hakios.demo.plist'
pl = plistlib.load(open(p, 'rb'))
pl['LimitLoadToSessionType'] = 'System'
plistlib.dump(pl, open(p, 'wb'))
"
cat > "$ROOT/build-root/etc/rc.jobs" << 'EOF'
# Commands run by the launchd job com.hakios.demo (see com.hakios.demo.plist).
/redwood/bin/hello_dyn started by launchd
/redwood/bin/svc_test
/redwood/bin/surf_test
# what the system believes the screen is: the scale and the size in points
/redwood/bin/mg_test main-screen-scale main-screen-width main-screen-height main-screen-class DeviceSupports120HzRefreshRate MaxRefreshRate main-screen-refresh-rate DeviceSupportsProMotion
/redwood/bin/fps_info
/bin/ps -A
EOF
if [ -n "${HAKIOS_RC_JOBS:-}" ]; then
	cp "$COMMON/userland/$HAKIOS_RC_JOBS" "$ROOT/build-root/etc/rc.jobs"
fi
python3 "$COMMON/tools/apfs/mkapfs.py" -z --plain fw -o "$ROOT/build-root.apfs" "redwood=$ROOT/build-root" >/dev/null
rm -rf "$ROOT/build-root"
echo "small root: $ROOT/build-root.apfs ($(stat -c %s "$ROOT/build-root.apfs") bytes)"
