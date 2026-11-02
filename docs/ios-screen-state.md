# What the iOS side shows on this phone, and what is missing

Findings from the tree (`/mnt/project/Misc/hakios/refs/hakios-xnu-kernel`) and from
the runs on the phone. Every claim here is file:line or log evidence.

## What the tree's own words say

`userland/src/var_store.c`, the tree's own description of the data volume:

> /private/var is a memory filesystem (bsd/miscfs/ramfs), mounted empty by init at every
> boot ... so nothing Darwin keeps there outlives the boot that wrote it: no home screen
> layout, no application data, no baked wallpaper.

That is the wallpaper's blocker, in the tree's own words.

## The picture on the panel is the Apple boot logo

The window server draws and presents: with `gl=1` the Mesa software GLES renderer
is up (190 `hakios-gl` lines, 1000+ frames) and the swap puts a 1125x2436 BGRA
surface on the panel (`MiniIOMobileFramebuffer: swap 1 layer 0: surface 4`).

But the loader's own reading of the panel (8x16 text cells) finds only a shape
centred on it, and no cell of it matches XNU's console font.

That shape is the Apple boot logo, and the sizes say so beyond doubt. The image
the progress UI shows is
`System/Library/PrivateFrameworks/ProgressUI.framework/apple-logo@3x~iphone.png`,
194 x 237 pixels (a CgBI PNG; `file` reports "PNG image data (CgBI), 194 x 237,
8-bit/color RGBA"). The window server draws into a picture 1125 wide and the
panel is 1080, so the logo lands on the panel 194 * 1080/1125 = 186 pixels wide
and 237 * 1080/1125 = 228 high, centred -- left at x = 447, top at y = 1086.

The loader's own reading of the panel says: the shape spans columns 56 to 80 of
its 135 (x = 448 to 640) and rows 65 to 79 of its 150 (y = 1040 to 1280) -- 192
by 240 pixels, centred at x = 544 against the panel's 540. Predicted 186 by 228;
measured 192 by 240. The difference is the 8-pixel granularity of the reading.

The root filesystem carries the logo for the progress UI as well:
`sysimage/rootfs/usr/share/progressui/applelogo@3x.tga`.

The picture on the panel is not the same in every run, which says the boot UI is
live and being redrawn: the 70-second run (`glq.log`) shows a small shape of 20
lit cells in 3 rows, and the 100-second runs (`wp2.log`, `gl-test.log`) show the
full logo -- 260 lit cells in 14 rows, 23 cells wide.

It is not the kernel's or the loader's: it is Darwin's own boot UI overlay, drawn
by `backboardd` once user space is up. `usr/libexec/backboardd` has the strings
`Current bootUI is an Apple Logo`, `Dismissing boot logo (pid:%d)`,
`Sustaining the boot logo: %{public}@`, `Cannot dismiss nonexistent boot logo
(pid:%d)` and the class `BKDisplayRenderOverlayAppleLogo`, and it links
`ProgressUI.framework/ProgressUI`.

The overlay is sustained while a system app has it up (`Not dismissing because
overlay has been sustained by the system app`), and the system app here is
SpringBoard. `BKBootUIPresenter` has `didFinishFirstBoot`, and `backboardd` logs
`First boot is finished` when it happens.

The overlay is a render overlay, above the window server's frames:
`BKDisplayRenderOverlayAppleLogo` and `BKDisplayBootUIRenderOverlay` are drawn by
`BKDisplayRenderOverlayManager`, which has
`dismissOverlayWithAnimationSettings:requstedByPID:`. It goes away when it is
explicitly dismissed or when the startup has finished -- the log line is
`updateOverlay: pid:%d explicitlyDismissed:%{BOOL}u finishedStartup:%{BOOL}u`.

The kernel's own console is not the culprit: `gc_pause()` sets
`vc_progress_enable = FALSE` on the graphics-mode path, and the display driver
takes that path with the first picture
(`PE_initialize_console(NULL, kPEGraphicsMode)`).

What is missing is the dismissal. The overlay goes away when it is explicitly
dismissed or when the startup has finished, and the strings say what stops it:
"Not dismissing because overlay has been sustained by the system app". So
something sustains it, and it is not the clock.

The one thing on this phone that the tree's reference board does not have is the
injected library in SpringBoard. `init` announces the injection ("library
insertion: .../redwood/lib/libhakios.sb.dylib SpringBoard"), and the library is
in the image -- the tree's own `userland/root/lib/libhakios.sb.dylib` carries the
unlock hook (`SBLockScreenManager`, `unlockUIFromSource:withOptions:`). But
neither the hook's output nor its summary line appears in any log: the tree's
`inject_sb.c` prints one line per replaced method ("found %#lx, expected %#lx;
left alone") and a summary of what it hooked, and there is not one of them in
the 70-second log, which kept its head.

That is a lead, not a conclusion: the log's beginning was evicted in the
100-second run, and the tree's own hooks may simply not print on a clean load.
The tree's `unlock()` is the thing that dismisses the lock screen, ten seconds
after SpringBoard's run loop starts.

The first boot is not the reason, though, and this is worth saying plainly because
it is the obvious guess: QEMU has no persistent storage either -- `/private/var` is
a memory filesystem there too -- and it reaches the home screen all the same.

## The Apple logo is the normal middle of a boot

The tree says what to expect, for its own QEMU (`docs/getting-started/qemu.md`):

> the CPU is emulated, so it is slow, and **the window is black at first**: that is
> normal. On a fast desktop: black for the first minute, the Apple logo at about
> one minute, the home screen at about five.

So the logo is what a boot looks like on the way, and what is missing is the time
after it. The tree's reference phone brings the home screen in under a minute
(`docs/u-boot.md`: "brings the home screen in under a minute (the screenshot at
60 s), with touch"), but that phone has a GPU and eight cores; this one draws in
software on one core.

That is the cheap thing to try first, and it costs nothing: the run has to be long
enough. `loader_return_after=70` ends the run at 70 s of kernel time, and the last
log line is at 92.9 s. The tree's own numbers say how much longer: on QEMU, one
core takes about two and a half minutes to the lock screen and four cores about a
minute and a half; and after SpringBoard appears the system is still busy
registering applications for another 30 to 40 seconds. So a run wants minutes, not
seconds, and it wants more than one core.

But time alone is not the whole answer, and the 100-second run says so. In that
run (`wp2.log`) SpringBoard was up and registering services ("Service stub
created for com.apple.NTKFaceSnapshotService" and a dozen more), the window
server was answering backboardd's calls, and the boot UI was still on the panel
when the run ended at 125.9 s of kernel time -- the logo, 260 lit cells in 14
rows, in exactly the place the logo was in the 70-second run.

So the boot UI is not simply late. It is up, and it is being sustained, while the
system around it is fully running.

What is genuinely slow here, and is worth fixing for its own sake: the software
renderer draws a frame in 28461 us (114 frames in 5008 ms, 22 a second), and the
run uses one core (`cpus=1`). The tree's notes say the software renderer manages
15 to 40 frames a second on four cores.

## The lock screen is a different thing, and the tree already handles it

`userland/src/inject_sb.c` dismisses the lock screen forcibly ten seconds after
SpringBoard's run loop starts (`unlock()`, at `inject_sb.c:1581`), unless
`autounlock=0` is among the kernel's arguments. So the lock screen is not what
is missing; it is behind the boot logo.

## What the log keeps, and what it drops

The loader keeps the first 256 KB and the last megabyte of XNU's log and drops
the middle, saying so in one line ("the log between the first 256 KB and the
last 1 MB was dropped"). The tree's own measurements, on the same set of 225
modules:

| window | log size | drop markers | modules in the log |
| --- | --- | --- | --- |
| 300 s | 1 466 579 | 1 | 2 |
| 120 s | 1 466 582 | 1 | 28 |
| 30 s | 1 185 596 | 0 | 225 |

So a long run keeps the *end* -- which is the part that says whether the home
screen came -- and loses the beginning, which is where the boot's own story is.
Two things make the difference, and neither is enough alone: a short window and
`QUIET=1` at build time, which sends launchd's own thousands of lines nowhere
and leaves the daemons their output.

Our run's log starts at 25.9 s of kernel time, which is exactly this: the
beginning was evicted. Two ways to see the early boot are a short run with
`HAKIOS_QUIET_JOBS=1` (the quiet jobs are already on by default), and the
loader photographs the panel before XNU runs, so what was on it before the boot
is not lost either.

## The wallpaper cannot be drawn, because there is nowhere to keep it

The live wallpaper is drawn by PosterBoard, in its own process, and handed over
as a placed layer; SpringBoard has no raster of its own and nobody bakes one
(`userland/src/inject_sb.c`, the poster switcher comment).

PosterBoard and PaperBoard are launch angels
(`sysimage/rootfs/System/Library/LaunchAngels/`), reached through a Mach service
and only when something asks for one, so they must be in the image as jobs:
`com.apple.PosterBoard.plist` and `com.apple.PaperBoard.plist`, both with
`MachServices` and `_ExperimentalNonLaunching`. `tools/mk-root.sh` builds the job
set with `mkjobs.py --rest`, which puts them in.

The tree's own notes (`userland/mkjobs.py:171`) give the recipe and its
measurement: with `nsurlsessiond` in the image the same build reaches 112
processes and 15 widget and poster processes, PosterBoard among them; without it
the loading reaches 76-78 processes and no widget or poster at all, and the home
screen never comes. `tools/mk-root.sh` allows `cloudd nsurlsessiond
syncdefaultsd appstored`, which is that recipe.

What PosterBoard writes, it writes into `/private/var`. On this phone
`/private/var` is a memory filesystem, empty at every boot: the tree's
persistent data volume is `var_hfs=DEV` (`userland/src/init.c:825`,
`mount_data_volume()`), an HFS Plus volume on the phone's flash named
`ufs_darwin_data`. This phone has no such partition, and nothing passes
`var_hfs` among the kernel's arguments.

So: the wallpaper needs a data volume the phone does not have yet. (The Apple logo
on the panel is a different thing: it is the boot UI overlay, and it is a separate
problem -- see above.)

The tree's own answer is `ufs_write=1 var_hfs=ufs_darwin_data` on the kernel's
command line: with `ufs_write=1` the UFS driver gives the stretch of the phone's
flash past the Darwin volume out as `/dev/ufs_darwin_data`
(`bsd/dev/hakios.ufs.c`, `UFS_WRITE_FIRST` = 2042368, 12.2 GB), and `init` mounts
it on `/private/var` (`userland/src/init.c:836`, `mount_data_volume()`). The
empty volume is made once with `tools/mkdatavol.py` and flashed to `darwin`; it
is 53 KB, because the image says DONT_CARE for everything the volume does not
use.

What is missing here is the loader's half: the kernel's writes are served by the
loader (`bsd/dev/hakios.ufs.c`: "one character device alone takes writes --
/dev/rufs_darwin, the port's own partition -- and in it only whole blocks from
UFS_WRITE_FIRST on"). This loader's shared block has the fields for it
(`struct loader_block.write_first`, `write_part_plus_one`) but the service that
answers them is not implemented. The tree's loader also proves the partition by
its GPT entry -- name, type, place, size and a unique GUID -- and the tree's
notes say that on a second phone the GUID did not match and the volume never
mounted ("`/dev/ufs_darwin_data: No such file or directory`, a memory
filesystem instead).

So the data volume is: a loader that serves the writes and names the partition,
`tools/mkdatavol.py` to make the volume, and two arguments on the command line.

The tree has all of this for its reference board, and the pieces are:

- `configs/persist.config`: the fragment that turns it on -- `CONFIG_UFS=2`,
  `CONFIG_UFS_WRITE=y`, `CONFIG_VAR_STORE=y` -- with the warning that it writes
  to the phone's storage, to the partition `darwin` only.
- `tools/userdata-carve.py`: cuts that partition off the end of Android's
  userdata, once, by hand. It shrinks Android's f2fs in place with Android's own
  kernel (F2FS_IOC_RESIZE_FS), then writes a new GPT in which userdata ends
  where the shrunk filesystem ends and `darwin` takes the rest from the next
  2 MB boundary. Both copies of the table are made and checked. It refuses
  unless the old table checks out, userdata is the last partition, and no f2fs
  segment past the new end holds a valid block.
- `tools/mkdatavol.py`: the empty HFS Plus volume (53 KB to flash), made
  case-sensitive (HFSX) and journaled, its root owned by root
  (`mkfs.hfsplus -U 0 -G 0 -M 755`); with another owner Darwin takes the volume
  for a data volume to migrate and stays at the progress bar.

The reference board (`redwood`, the POCO X5 Pro) keeps it this way: its
`device.conf` says "storage works UFS read through the loader; /private/var kept
with var_store=1 ufs_write=1".

Note what the carve costs: it takes the partition off Android's userdata, so
Android's own data area shrinks. That is a change to the phone's storage and
needs the user's own go-ahead.

## The small root is not the image the tree's phone runs

This is the difference between the two boards, and it is bigger than it looks.

The tree has two images (`userland/build.sh`):

- `root.apfs` (4 MB): "the small image: our init and test programs, etc/rc, and the
  launchd jobs ... It travels in the payload next to the kernel (the second RAM
  disk, /dev/md1) and is mounted on /redwood."
- `system.apfs` (2.4 GB): "the real one", with the Darwin files, the cryptexes and
  the applications Darwin installs on its first boot.

And the tree's phone runs the *flash* arrangement (`docs/u-boot.md`, step 4):

> **Darwin from `darwin`, still from RAM**: `CRYPTEXES=flash FLASHBOOT=1
> userland/build.sh`, `xl ramdisk userland/flashboot.apfs`, `root_apfs=ufs_darwin
> ufs=2`: the Darwin volume is the root, SpringBoard up.

That is: the Darwin volume on the phone's flash is the root, the cryptexes come
off the flash, and `flashboot.apfs` is the resident image. We run something else:
`root_apfs=ufs_xnusys` with a small root of 5 MB built by `tools/mk-root.sh`.

The four checks that fail in our run are the visible edge of this:

- "the OS cryptex: in neither md0 nor the flash" and the same for the App
  cryptex -- the cryptexes are not in our small root, and the tree's phone has
  them on the flash. The OS cryptex holds the dyld shared cache.
- "the first boot's applications in /Applications" -- the staged applications
  (`private/var/staged_system_apps`) are not in our small root.
- "a synthetic /redwood on it, for this image" -- our own image is already bound
  there.

But the phone's flash is better than that says. The tree's `system.apfs` was
written into the `xnusys` partition (`tools/system-flash.sh`), and the kernel
reads it:

    -- the iOS root volume on /dev/ufs_xnusys as the root --
    apfs: volume 0 "System" of container 0 (at 0 MB): 45044 files,
    16547 directories, 601698 blocks of 4096 used

So the whole system image -- the applications, the Darwin files, the manifest's
permissions -- is on the phone's flash and is the root.

## Exactly what the cryptex checks want

`mount_cryptex()` (`userland/src/init.c:149`) looks for the OS cryptex as
container 1 and the App cryptex as container 2 -- first of the *boot volume*
(`/dev/md0,c=1`), and if that has no such container (EINVAL) then of the flash
(`/dev/ufs_xnusys,c=1`):

    the OS cryptex from /dev/ufs_xnusys,c=1: Invalid argument
    [FAIL] the OS cryptex: in neither md0 nor the flash

Both fail here, and the reason is the container count. The tree's
`flashboot.apfs` is the boot volume with the cryptexes as its containers 1 and 2
(`userland/build.sh`: "the third container: Apple's App cryptex as it is"). The
tree's `root.apfs` -- the small image -- has one volume, `redwood`, and so does
the tree's `system.apfs`: one volume, `System`.

So the cryptexes are meant to be *additional containers of the boot volume*, and
our boot volume (our 5 MB small root, `tools/mk-root.sh`) has one container, like
the tree's small image. The tree's `CRYPTEXES=flash` recipe puts them on the
flash instead, as Apple's own images in `darwin` right after the Darwin volume
(containers 1 and 2 of the *Darwin volume*), which is a different arrangement
again -- one this phone has not got, because it has no `darwin` partition.

And the shared cache is not missing, though: the init check "run a program
through dyld and the shared cache" passes, and `/redwood/Cryptexes/OS` and
`/redwood/Cryptexes/App` are there. The tree's `select-system.py` says why:
"from the framework directories (the code itself is in the dyld shared cache)". So
the cache came with the `System` volume that is on the phone's flash, and the
cryptex mounts are a second copy of it that the tree's arrangement expects but
does not need.

In other words: the flash half of the tree's arrangement is done, the shared
cache is present, and the two failing cryptex checks are about a second copy of
it that this phone's arrangement does not have.

The tree's own steps (`docs/u-boot.md`, the phone's own list), which say exactly
what that takes:

1. **Backup** (`tools/backup-partitions.py --yes`, read only, as root): the boot
   partitions of both slots, `vbmeta*`, `misc`, `devinfo`, `persist`, `modemst1/2`,
   `fsg`, the first and last 8 blocks of every unit, each proven by three SHA-256s.
2. **`darwin`** (`tools/userdata-carve.py --wipe DIR 20`): userdata shortened,
   `darwin` (sda34, 20 GiB, APFS container type) at the end of LUN 0, two entries
   changed, both copies of the table checked; written from Android (root) with
   `dd`, read back; then `fastboot -w` -- **Android's data is erased**.
3. **The Darwin volume** into `darwin`: Apple's root volume of the IPSW
   (`tools/apfs/dmg.py extract`), the OS and App cryptexes after it, as the port's
   own layout, 8.4 GB as 11 sparse pieces (`tools/sparse-split.py`), read back
   with the image's own sum.
4. **Darwin from `darwin`, still from RAM**: `CRYPTEXES=flash FLASHBOOT=1
   userland/build.sh`, `xl ramdisk userland/flashboot.apfs`, `root_apfs=ufs_darwin
   ufs=2`: the Darwin volume is the root, SpringBoard up.
5. **The boot image of slot a**: U-Boot, the loader, a payload with the kernel,
   `root.apfs` and `flashboot.apfs` as its resident image, the flash-root command
   line; `fastboot flash boot_a`, `fastboot set_active a`.

Steps 2 and 5 change the phone: step 2 shortens Android's userdata and erases
Android's data, and step 5 replaces the boot image. Both need the user's
go-ahead.

## The phone's own partition table, and what a `darwin` would look like

This phone's GPT (`backup/sdc-gpt-primary.bin`, LUN 2, 238 GiB, 4096-byte
sectors): `super` 10.60 GiB, `tranfs` 0.29 GiB, `userdata` 226.03 GiB at lba
3223552..62476282, `flashinfo` at the end. The `xnusys` partition is not an
entry of the *original* table: it was carved out of userdata's tail by our own
`tools/gpt-carve.py`, which shrank userdata to lba 3223552..61427455 and added
`xnusys` at 61427456..62476282 (4 GiB) as an entry of its own. The carved
tables are `backup/sdc-gpt-primary.bin.new` and `sdc-gpt-secondary.bin.new`.

So the tool that would carve a `darwin` partition is already written and already
used on this phone: it takes space off the end of a partition and puts a new
entry in its place, leaving every other entry, the disk GUID and the entry order
exactly as they were. `darwin` would want 20 GiB, which is 9 % of userdata's
226 GiB.

What is missing for that is not the tool but the image: the tree's `darwin`
partition holds Apple's root volume of the IPSW with the OS and App cryptexes
after it, 8.4 GB in 11 sparse pieces. This tree has the IPSW's unpacked images
(`sysimage/rootfs`, `sysimage/cryptex`, `sysimage/cryptex-app`) and the tools to
make the volume (`tools/apfs/dmg.py extract`) and to split it for writing
(`tools/sparse-split.py`).

## The cryptexes build here, and quickly (but this phone does not need them)

The tree's own build of the two cryptexes works here -- and since the shared
cache is already on the phone's flash, they turn out not to be what this phone
is missing. They are here because they are the way the tree's arrangement puts
the cache into the boot volume, and this phone's arrangement gets it from the
`System` volume instead.

The tree's `flashboot.apfs` is the boot volume (our small root) with the OS
cryptex and the App cryptex as its containers 1 and 2 -- and the kernel takes the
shared cache from the first. The tree builds them with
`tools/apfs/mkapfs.py` from the unpacked cryptexes, and that works here, today:

    mkapfs.py -z -o os.apfs OS=<sysimage/cryptex>
      volume 0 "OS": 1412 files, 629 directories, 1 symbolic links,
      2281 blocks of data (272 compressed in their resource fork, 1125 whole in
      their attribute)
      os.apfs: 3053 blocks of 4096 bytes, 256 free        (12.5 MB)

    mkapfs.py -z -o app.apfs App=<sysimage/cryptex-app>
      volume 0 "App": 884 files, 163 directories, 466 blocks of data
      app.apfs: 1032 blocks of 4096 bytes, 256 free       (4.2 MB)

Both in about a second. So the two missing containers are not missing at all --
they are two commands away.

The tree's own build of them puts the dyld shared cache into the OS cryptex
(`userland/build.sh`: `D=flashboot.os/System/Library/Caches/com.apple.dyld`, the
cache linked in), and that build works here too, and the result is checked:

    mkapfs.py -z -o os.apfs OS=<cryptex + the shared cache>
      volume 0 "OS": 1455 files, 630 directories, 1 symbolic links,
      244908 blocks of data (315 compressed in their resource fork, 1125 whole in
      their attribute)
      os.apfs: 246406 blocks of 4096 bytes, 256 free       (1.0 GB)

    check-image.sh os.apfs OS=<the same directory>
      check-image.py: apfsck -cw: clean
      check-image.py: volume 0: 2086 entries read back by apfs-tool, every one as
      in <the directory>

98 seconds to build, 39 to check, and the tree's own checker says the image is
clean and reads back exactly as it was made.

And the system image with the applications in it, built the tree's way
(`sysimage/rootfs` with the tree's own changes, then `mkapfs.py`):

    174 applications in the volume (142 of Darwin's own plus the 32 staged ones:
    AppleTV, Books, Files, FindMy, Health ...)
    volume 0 "System": 42705 files, 15749 directories, 668 symbolic links,
    344041 blocks of data (7983 compressed in their resource fork, 29872 whole in
    their attribute)
    system.apfs: 357674 blocks of 4096 bytes, 256 free       (1.46 GB)

    check-image.sh system.apfs System=<the same directory>
      check-image.py: apfsck -cw: clean
      check-image.py: volume 0: 59122 entries read back by apfs-tool, every one
      as in <the directory>

48 seconds to build, 43 to check. `Files.app` is in it.

So the whole of what the tree's phone has that ours does not -- the OS cryptex
with the shared cache, the App cryptex, and the applications of the system image
-- can be built on this machine, from what this tree already has unpacked. The
applications are already on the phone (the `System` volume of 45044 files that
the kernel reads as the root), and the shared cache is in it.

## The Setup Assistant

The tree refuses it deliberately: `userland/src/inject_sb.c` replaces
`-[SBSetupManager _setSetupRequiredReason:]` and calls the original with 0 ("no
need for the setup assistant"), logging "hakios: SpringBoard wants the setup
assistant (reason %ld); not here".

The tree's notes (`userland/mkjobs.py:163`) say the setup assistant is not the
missing piece: the tree ships `com.apple.purplebuddy.plist` with `SetupDone = True`
and MobileGestalt answers `RegionInfo = LL/A` on purpose, so the port does not
have to go through the first-boot flow.

Showing it would mean: pass the reason through, ship
`com.apple.purplebuddy.budd`, and drop `SetupDone = true`. Its state could not
persist on this phone without the data volume above, and its Apple ID and
activation steps cannot complete at all -- `mobileactivationd` wants a Secure
Enclave this phone has not got ("Failed to bump UIK: 0xe00002c7").

## Files, internal storage and the SD card

The Files app is in the image and its "On My iPhone" provider can run, but what it
lists is the memory filesystem above, so it is empty at every boot. There is no
tree-written file manager and no daemon that lists volumes; `userland/src/vol_ls.c`
is the way to list a mounted volume by hand (`vol_ls fs|ls|cat|hash|sysctl|zero`).

The external SD card has no path at all on this phone: the tree's storage driver
is for the reference board's eMMC on MSDC0, and the MT6893's SD host controller
(MSDC1, the `msdcpll` node under `/soc/fhctl@1000ce00/map0/` in the phone's own
device tree) has no driver in the tree. Writing one is a new piece of work, not
a setting.

## The command channel

The phone leaves the host's USB entirely while iOS runs, so there is no way in
and no way to read anything out. The one channel that exists is the loader's: the
host writes its request into the phone's pmsg zone under Android, the loader puts
what it finds on the kernel's command line, and the kernel's bootargs are
readable from user space.

So the command goes the way the tree's own runtime parameters go
(`tools/ios-cmd.sh`, `userland/hakios-cmd.c`, `tools/batt-request.sh`'s
`HAKIOS_REQUEST_EXTRA`). What the command prints is the hard part: the running
system fills the console, and the loader keeps only the first 256 KB and the last
megabyte of it (see above), so the program waits until the end of the run
(`loader_return_after=`) and prints then.

## What the touch in the log is

The `wp2` run's log (100 s of kernel time, `touchscreen=1`) has backboardd
receiving real digitizer events:

    backboardd[52] [TouchEvents] contact 1 presence: touching
    backboardd[52] [TouchEvents] touchstats 151D... events starting 9.83801s
        ago: [ down: 2, up: 2, rangeIn: 2, rangeOut: 2, fromEdge: 1, pathsSeen: 1

and `rwtouch: notification com.hakios.unlock: posted` right after each one.

Those touches are not a finger. They are ours: `tools/mk-root.sh` puts three
lines in the image's `etc/rc.jobs` --

    /redwood/bin/rwtouch wait 35 tap 0.5 0.5 wait 1 swipe 0.5 0.99 0.5 0.35 250 wait 1 notify com.hakios.unlock &

-- and the log's three rounds of events land at about 35 s, 50 s and 70 s, which
is what those three lines say. `rwtouch` writes the samples into
`/dev/hakios-touch` itself (`userland/src/rwtouch.c`: "replay touches through
/dev/hakios-touch").

So the chain below the hardware is proven on the phone: the samples reach
`/dev/hakios-touch`, `libhakios.dylib` turns them into HID events in
backboardd, and backboardd counts down, up, rangeIn and rangeOut. What is not
proven is a finger on this phone's panel reaching the same place: the kernel the
phone runs has no FT3681 driver (the chip ID lines in the log are the *loader's*
own probe -- "probing MT6893 I2C0 for FocalTech FT3681...", "chip_id=0x5662",
"no point in 1000 report frames (the panel said 0xff every time)" -- and the
kernel's own driver prints nothing at all).

## What is left to prove

- The command's output has never come back. The transport is proven (the `cmd=`
  reaches the kernel's bootargs) but the output path is not.
- The panel has never been seen as a picture. The loader photographs it now, twice
  -- before XNU runs and when it comes back (`mtkloader/mtkloader.c`,
  `shot_panel`) -- in the form `tools/fbshot.py` makes a PNG of; the next run
  shows it.
- Whether a run long enough reaches the home screen. The tree's own numbers say
  minutes, not the 70 s the runs so far had; `tools/long-boot.sh` does a longer
  one with more cores.
- Nothing has been tried on the lock screen, the notification center, the control
  center or the setup assistant.
