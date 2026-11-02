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
#   - the binary patches to sbin/init: the stage0 launcher execs /sbin/launchd.ios directly as
#     process 1, and launchd's stdout+stderr go to /dev/null when etc/quiet is there (otherwise
#     launchd floods the console)
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

rm -rf "$ROOT/build-root"
cp -a "$COMMON/userland/root" "$ROOT/build-root"
ln -sf init "$ROOT/build-root/sbin/launchd"
ln -sf init "$ROOT/build-root/sbin/launchd.ios"
ln -sf private/var/tmp "$ROOT/build-root/tmp"
mkdir -p "$ROOT/build-root/flash" "$ROOT/build-root/Cryptexes/OS" "$ROOT/build-root/Cryptexes/App"
ln -sf . "$ROOT/build-root/redwood"
python3 -c "
import struct
p = '$ROOT/build-root/sbin/init'
b = bytearray(open(p, 'rb').read())
assert struct.unpack('<Q', b[0x8000:0x8008])[0] == 0x100005d92
struct.pack_into('<Q', b, 0x8000, 0x100005d17)
assert b[0x5a0:0x5a4] == bytes.fromhex('a0000035')
b[0x5a0:0x5a4] = bytes.fromhex('1f2003d5')

assert b[0x3684:0x3688] == bytes.fromhex('80010035')
assert b[0x369c:0x36a0] == bytes.fromhex('c000f837')
assert b[0x36a4:0x36a8] == bytes.fromhex('41008052')
assert b[0x36c8:0x36cc] == bytes.fromhex('a1050054')

# Silence launchd stdout+stderr to /dev/null when /etc/quiet is present, bypassing launchd log floods
b[0x3684:0x3688] = bytes.fromhex('e0010035')
b[0x369c:0x36a0] = bytes.fromhex('2001f837')
b[0x36a4:0x36c8] = bytes.fromhex('21008052a7050094e00319aa41008052a4050094e00319aa8d050094e06f40f92e000014')
open(p, 'wb').write(b)
"
python3 -c "
import plistlib
p = '$ROOT/build-root/LaunchDaemons/com.apple.SpringBoard.plist'
pl = plistlib.load(open(p, 'rb'))
env = pl.setdefault('EnvironmentVariables', {})
env['OS_ACTIVITY_DT_MODE'] = 'YES'
env['OS_ACTIVITY_MODE'] = 'debug'
plistlib.dump(pl, open(p, 'wb'))
"
touch "$ROOT/build-root/etc/quiet"
cat > "$ROOT/build-root/etc/rc" << 'EOF'
# Hand process 1 over to the real iOS launchd
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
/bin/ps -A
/redwood/bin/rwtouch wait 35 tap 0.5 0.5 wait 1 swipe 0.5 0.99 0.5 0.35 250 wait 1 notify com.hakios.unlock &
/redwood/bin/rwtouch wait 50 tap 0.5 0.5 wait 1 swipe 0.5 0.99 0.5 0.35 250 wait 1 notify com.hakios.unlock &
/redwood/bin/rwtouch wait 70 tap 0.5 0.5 wait 1 swipe 0.5 0.99 0.5 0.35 250 wait 1 notify com.hakios.unlock &
EOF
python3 "$COMMON/tools/apfs/mkapfs.py" -z --plain fw -o "$ROOT/build-root.apfs" "redwood=$ROOT/build-root" >/dev/null
rm -rf "$ROOT/build-root"
echo "small root: $ROOT/build-root.apfs ($(stat -c %s "$ROOT/build-root.apfs") bytes)"
