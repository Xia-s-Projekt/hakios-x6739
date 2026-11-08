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

The tap now reaches the icon's tap recogniser and is allowed. What is **not** yet proven on
the device is that the application opens: the last run that could have shown it was lost to
the console's ring, and the run after it could not be made because the phone went off USB.

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
does not refuse. The last step — that the application opens — is traced but not yet seen.

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

- **The application opening** is not yet proven on the device. The trace is in place
  (`homescreen: launch: the application is launched`, `homescreen: launch: the icon is handling
  its tap`); the run that would have shown it was lost to the ring, and the run after it was
  not made because the phone went off USB for good.
- **Tearing**: `fb_intf` is never set for MT6893, so the beam-scan that writes each row
  behind the line being read does not run. The frame rate is measured (119.3 FPS, median
  8.38 ms, `tools/fps.py`); the tearing is not fixed.
- **The serial** on the About page is not confirmed on the screen.
- **Brightness** and **Wi-Fi** are not done.
