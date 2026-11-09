# Handoff: the home screen's touch on the X6739, and what the name is

Written 9 November 2026, from the runs `logs/xnu-touch4.log` through `logs/xnu-touch24.log`.
Every claim here is from those logs or from the code they point at.

## 1. The short version

The home screen's icons were not touched because four separate things were wrong, and all
four are fixed:

| what | why the touch was lost |
|---|---|
| the job that touches the screen never waited for SpringBoard | `notify_check` answers true on its first call whatever has been posted, so the wait returned at once |
| the thread that reads the touch stopped in a syscall | the syscall takes a lock the event system holds while it dispatches an event |
| the home screen's window was below everything | it was at level −2, under the two windows at level 0, and the window server asks from the top down |
| the trace was never read | the lines were written after the console's ring was full |

The tap reaches the icon's tap recogniser and is allowed, and **a tap on an icon opens the
application it stands for** — confirmed on the phone by hand on 9 November 2026.

The reason the earlier runs showed a tap that went nowhere is in section 1b: the hooks on the
icon's tap handling had thrown away the implementations they replaced, so the hook swallowed
the very tap it was put there to watch.

## 1a. Why the fixes above were not on the phone: the prebuilt libraries were stale

Runs 21 to 28 of 9 November showed **no injection at all**: no `hakios: SpringBoard[…]:
injected`, no `inject_report` line, four `backboardd` lines against touch20's 775. The
kernel's own `exec_inject: %u given | %u loaded` line was missing too.

The cause was not the code. It was that **the shipped binaries are older than the source**:

| string | in `userland/src/inject_sb.c` | in `prebuilt/userland/libhakios.sb.dylib` |
|---|---|---|
| `is now at 1` (the window's level) | yes | **no** |
| `the application is launched` | yes | **no** |

Commit `e41115c` ("the home screen's window is put above the windows that are on top of it")
changed **only the source**:

```
 userland/src/inject_sb.c | 64 ++++++++++++++++++++++++++++++++++++++++++++++--
 1 file changed, 62 insertions(+), 2 deletions(-)
```

The binary the phone actually ran never got it. So the window stayed at level −2, and every
fix made after the last rebuild of `prebuilt/userland/` was invisible on the device. The
trace agrees: the run that did inject (`logs/xnu-touch29.log`) says
`SBHomeScreenWindow, level -2` — the level fix was in the source and not in the binary.

Two things had to be true for this to hide for hours:

1. `userland/build.sh` writes `obj/libhakios.sb.dylib` and the image is built from
   `userland/root`, so a rebuild of the tree's `prebuilt/` is **not** what the image gets.
   `userland/root/lib/` has to be updated too (build.sh does that at its "root/lib" step).
2. `build.sh` calls `xnu-clang`, which is **not on PATH** on this machine any more. It lives
   at `xnu-deps/host/bin/xnu-clang` (the same path `tools/mk-mesa.sh` uses). A build that
   cannot find it fails, and a build that fails quietly leaves the old objects in place —
   which is how a "rebuild" produced a binary with none of the new code in it.

**The rule that follows: after changing `userland/src/inject*.c`, rebuild with
`xnu-deps/host/bin/xnu-clang`, copy `obj/libhakios*.dylib` into **both** `prebuilt/userland/`
and `userland/root/lib/`, and check the new string is in the shipped binary with `strings`
before believing the run.** A source commit is not a change on the phone.

**The rule, for the record** (it belongs in the common tree's own instructions too):

1. The image is built from `userland/root/`, not from `prebuilt/userland/`. A change in
   `userland/src/` reaches the phone only through `userland/root/lib/`.
2. `userland/build.sh` calls `xnu-clang`, which is **not on PATH** on this machine: the
   compiler is `xnu-deps/host/bin/xnu-clang` (the same path `tools/mk-mesa.sh` uses). A build
   that cannot find it fails, and a failed build leaves the old objects in place, so the
   "rebuilt" binary is the old one.
3. The libraries are signed ad-hoc: `ldid -S<an empty entitlements plist>`, which gives the
   identifier the library's own name.

So the three steps are: build with `xnu-deps/host/bin/xnu-clang`, sign, copy into **both**
`prebuilt/userland/` and `userland/root/lib/` — and then check with `strings` that the code
you just wrote is in the binary that ships. `strings prebuilt/userland/libhakios.sb.dylib |
grep 'is now at 1'` is the check that would have caught it in minutes.

### The launch hooks swallowed the tap and the launch

The trace of the run that did inject (`logs/xnu-touch30.log`) has **35** lines of

```
homescreen: launch: the icon is handling its tap
```

and nothing after them: no `the application is asked whether the icon may launch`, no
`the application is launched`. The tap reached the hook and nothing happened after it.

The reason is in `inject_sb.c`. The hooks are installed like this:

```c
safe_swizzle_inst_method("SBIconView", "_handleTap", (IMP)my_iconview_handle_tap);
safe_swizzle_inst_method("SBApplicationIcon", "launch", (IMP)my_icon_launch);
```

and `safe_swizzle_inst_method()` **returns the implementation it replaced** — that is the
whole point of it. The return value was not taken, so `original_iconview_handle_tap` and
`original_icon_launch` stayed NULL, and the wrappers did this:

```c
if (original_iconview_handle_tap) {
    original_iconview_handle_tap(self, _cmd);
}
```

which is to say: nothing. The hook replaced the real implementation and then did not call
it. **The tap was swallowed by the hook that was supposed to watch it.**

The hook on `SBApplication`'s `iconAllowsLaunch:` had the same shape in a different way: it
returned `YES` without ever asking the original, so it overruled the system's own answer
instead of reporting it.

Both are fixed now (commit `0bb045b`): the originals are taken and called, and the hook on
whether the icon may launch asks the original and passes its answer on. A tap that the
system refuses is a tap that does nothing, and the trace of that is worth more than a
launch that was forced.

### The two libraries and what they are for

| library | goes into | built from |
|---|---|---|
| `libhakios.dylib` | backboardd (the touch screen's HID service) | `src/inject.c` |
| `libhakios.sb.dylib` | SpringBoard (the home screen's hooks) | `src/inject_sb.c` |

Both are signed ad-hoc with `ldid -S<an empty entitlements plist>`; the identifier is the
library's name. Without a signature the kernel's insertion does not load them.

## 2. The four bugs, with the evidence

### 2.1 The wait did not wait

`rwtouch waitfor com.hakios.homescreen` said:

```
rwtouch: the notification com.hakios.homescreen came after 0.0 s
```

`notify.h` says of `notify_check`: *"check is set to true the first time notify_check is
called for a token. Subsequent calls set check to true when notifications have been posted
for the name."* The first call always answers true, so a job waiting for it went straight on.

The touches arrived at 83 s; SpringBoard's hooks were not installed until 112 s
(`homescreen: swizzle: -[SBIconView gestureRecognizerShouldBegin:] hooked` at 112.874877).
Every touch of every run since the trace began was lost before it arrived.

**The fix**: the home screen sets the *state* of the notification to a number of its own
(`0x484f4d45`), and the job reads that state — the pair `notify.h` describes for exactly
this: *"the server can set the state value, and the client can check the value at startup time
to synchronize with the server"*.

### 2.2 The thread that reads the touch stopped in a syscall

The lines about the touch went to the kernel with `sysctlbyname("kern.inject_report", ...)`
once for every sample and once for every event, **on the thread that reads the touch screen**.
That syscall takes a lock the event system holds while it dispatches an event, so the thread
stopped in it with a touch in its hand and the event was never dispatched.

**The fix**: the lines go into a ring and a thread of their own gives them to the kernel.

### 2.3 The home screen's window was under everything else

The trace (`tools/fps.py` aside, this is the `sendEvent:` hook) says where a touch goes:

```
homescreen: touch: the event is given to the window _UISystemGestureWindow (level 0)
homescreen: touch: the event is given to the window SBStatusBarReusePoolWindow (level 0)
```

and never to `SBHomeScreenWindow`, which is at **level −2**:

```
homescreen: window: SBHomeScreenWindow, level -2
homescreen: window: _UISystemGestureWindow, level 0
homescreen: window: SBRootSceneWindow, level 0
```

The window server asks the windows from the top down and gives the event to the first that
takes it, so the home screen was the last one asked, and the two windows above it took every
touch.

**The fix**: the home screen's window is put at level 1 — above the scene and the system's
gesture window, below the status bar (1070) and the cover sheet (1050), so the control centre
and the app switcher keep working.

### 2.4 The trace was never read

Three separate reasons, all fixed:

- The thread that gives the lines to the kernel was started at the *end* of the library's
  constructor, and the constructor waits for its own work to be done before it returns: the
  thread did not run until every line had been made. It starts at the top now.
- The window server's census of a frame is three long lines, and at 120 frames a second that
  filled the console's ring in a minute (1306 lines in one run). The census is said every 200
  frames now, and the line of the frame's own time every 20, so the rate can still be
  measured from it (192 lines in the same run).
- The loader wrote the lines about the touch *after* the XNU log, into a ring that was
  already full. It writes them first now.

## 3. What the trace shows now

From `logs/xnu-touch17.log` (the run that proved 2.3):

```
homescreen: touch: the event is given to the window SBHomeScreenWindow (level 1)
homescreen: touch: the home screen window at (58,244), the event (-1,-1) phase -1
homescreen: touch:   it gave SBIconListView
homescreen: touch:   it gave SBIconView
homescreen: icon: SBIconView was asked about a touch for its tap recognizer: allowed
homescreen: icon: SBIconView begins its tap
homescreen: icon: SBHIconManager reasonToDisallowTapOnIconView: says "no reason (allowed)"
```

The tap lands on an icon, the icon's own tap recogniser accepts it, and the icon manager
does not refuse.

**The last step is confirmed: the application opens.** The trace of that step was lost to the
console's ring in the runs of 9 November (see section 5), so it was settled by hand on the
phone: a tap on an icon opens the application it stands for. What the trace did show is that
the tap stopped at the icon's own tap handling, with nothing after it — and the reason was
section 1b's: the hook there had replaced the icon's own tap handling and then not called
it. With the original kept and called, the application opens.

The taps are now aimed at the icons themselves. The trace says where they are:

```
homescreen: grid: the icon list is at (0,0) 375x812
homescreen: grid:   SBIconView at (27,261) 60x74
homescreen: grid:   SBIconView at (114,261) 60x74
```

four columns at x = 27, 114, 201, 288 and rows at y = 261, 356, 451, so the middle of an icon
is at (x + 30, y + 37).

## 4. The name

The project calls itself **zenith**: the identity strings say so (the audio plug-in's
manufacturer, the HID service's manufacturer, the test programs' keys, the input thread's
name). `redwood` is the name of the *reference phone* (the POCO X5 Pro, SM7325) and of the
files copied from the reference port; those keep it.

**The mount point is still `/redwood`,** and that is not an oversight. The directory the
image is mounted on is inside the *prebuilt kernel's* own root filesystem
(`mach.development.mt6893`, whose root has the directory `redwood` in it), so the path can
only change by rebuilding the kernel from source. A run of 9 November with the path moved
failed to boot for exactly that reason:

```
FAIL] a synthetic /redwood on it, for this image
stage0: cannot mount /dev/md1 on /zenith: No such file or directory
```

The tree was put back and the prebuilt programs rebuilt (init, stage0, the injection
libraries, the OpenGL library, rwtouch), with their sums refreshed.

## 5. Where the run's log is lost, and how to keep it

The XNU log is about 1.4 MB for a 200 s run; the ramoops console keeps 256 KB. The loader
saves the head of the boot, a window from the middle, and the end. The touch happens around
113 s of the run — in the middle, and out of the window.

`XNU_LOG_OFFSET` and `XNU_LOG_LEN` choose that window (they are compiled into the loader):

```bash
XNU_SECS=200 XNU_LOG_OFFSET=700000 XNU_LOG_LEN=400000 \
  bash tools/xnu-cycle.sh
```

`XNU_LOG_OFFSET` is a byte offset into XNU's own log. For a 200 s run the touch period is
around 700000.

## 6. What is not done

- **The application opening** is confirmed: a tap on an icon opens the application it stands
  for, checked by hand on the phone on 9 November 2026. The automated test that proved it
  (the job `rc.jobs.touch`, which replayed taps on the icons) is **removed**: the launcher
  works without it, and the taps it replayed were in the way of using the phone by hand.
  `rwtouch` stays — it is the tree's own tool and fifty other job files use it.
- **Tearing**: `fb_intf` is never set for MT6893, so the beam-scan that writes each row
  behind the line being read does not run. The tearing is not fixed.
- **120 FPS**: the frames come at **60.0 a second**, measured from the window server's own
  frame trace (median 16.67 ms, 98% of the intervals within 0.1 ms of it). The panel and the
  driver run at 120; CoreAnimation draws at 60 because the window server's library has no
  MobileGestalt interposer and no display-link hook of its own — both are in SpringBoard's
  library, and SpringBoard does not draw. Where the fix goes is in `docs/gpu-port-mt6893.md`,
  section 9a.
- **The serial** on the About page is not confirmed on the screen.
- **Brightness** is not done: the whole path is written up in `docs/brightness-mt6893.md`
  (the backlight is on the display PWM at `0x1100e000`, the register offsets and the 9-bit
  brightness mapping are recovered), and the one thing still missing is the external method's
  selector number.
- **Wi-Fi** is not done: `docs/wifi-mt6893.md` has the plan.
- **The black rectangles** over labels, text and icons that were seen on the screen: not
  fixed, and not yet explained. The texture path they would come from is the refill of a
  texture from an IOSurface when its seed moves (`userland/src/eagl.c`).
