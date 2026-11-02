# What the iOS side shows on this phone, and what is missing

Findings from the tree (`/mnt/project/Misc/hakios/refs/hakios-xnu-kernel`) and from
the runs on the phone. Every claim here is file:line or log evidence.

## The picture on the panel is the Apple boot logo

The window server draws and presents: with `gl=1` the Mesa software GLES renderer
is up (190 `hakios-gl` lines, 1000+ frames) and the swap puts a 1125x2436 BGRA
surface on the panel (`MiniIOMobileFramebuffer: swap 1 layer 0: surface 4`).

But the loader's own reading of the panel (8x16 text cells) finds only a shape
centred on it, about 184 x 224 pixels, and no cell of it matches XNU's console
font. The root filesystem carries the boot logo the progress UI shows:
`sysimage/rootfs/usr/share/progressui/applelogo@3x.tga`, and
`System/Library/PrivateFrameworks/ProgressUI.framework/apple-logo@3x~iphone.png`
is 194 x 237 pixels.

It is not the kernel's or the loader's: it is Darwin's own boot UI overlay, drawn
by `backboardd` once user space is up. `usr/libexec/backboardd` has the strings
`Current bootUI is an Apple Logo`, `Dismissing boot logo (pid:%d)`,
`Sustaining the boot logo: %{public}@`, `Cannot dismiss nonexistent boot logo
(pid:%d)` and the class `BKDisplayRenderOverlayAppleLogo`, and it links
`ProgressUI.framework/ProgressUI`.

So the phone stands on the boot logo because nothing dismisses it. The overlay is
sustained while a system app has it up (`Not dismissing because overlay has been
sustained by the system app`), and the system app here is SpringBoard.

The lock screen is a different thing and the tree already handles it:
`userland/src/inject_sb.c` dismisses it forcibly ten seconds after SpringBoard's
run loop starts (`unlock()`, at `inject_sb.c:1581`), unless `autounlock=0` is
among the kernel's arguments.

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

So: the wallpaper needs a data volume the phone does not have yet.

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
`HAKIOS_REQUEST_EXTRA`). What the command prints is the hard part: the phone's
console is a ring of 256 KB and the running system fills it, so the program waits
until the end of the run (`loader_return_after=`) and prints then.

## What is left to prove

- The command's output has never come back. The transport is proven (the `cmd=`
  reaches the kernel's bootargs) but the output path is not.
- The panel has never been seen as a picture. The loader photographs it now
  (`mtkloader/mtkloader.c`, `shot_panel`), in the form `tools/fbshot.py` makes a
  PNG of; the next run shows it.
- Nothing has been tried on the lock screen, the notification center, the control
  center or the setup assistant.
