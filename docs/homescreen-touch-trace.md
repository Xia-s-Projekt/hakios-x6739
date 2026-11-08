# The home screen's touch on the X6739: what the trace says

Written 9 November 2026, from the runs `logs/xnu-touch6.log` through `logs/xnu-touch11.log`.
Every number and line quoted here is from those logs; the commands that produced them are in
`tools/fps.py` and the loader's own trace.

## 1. The question

The home screen's app icons do not launch when they are tapped, the pages do not move when they
are swiped, and the touch of the "home launcher app" does nothing. Widgets open, the control
centre comes down, and the app switcher comes up. The suspicion was a layer over the icons that
takes the touch before they see it.

**That suspicion is wrong.** The trace shows the touch reaching the window server and being
given to the HID service, and SpringBoard's own windows are not being asked about it at all.

## 2. How the trace is made, and why it took a while to have one

The trace lives in the loader's log, not in the console: the console's ring is short and the
window server's own lines fill it up in a minute, so a line said there is gone before the run's
log is read. The lines about the touch go through `kern.inject_report`, and the loader keeps
those in a part of its own buffer (`mtkloader/mtkloader.c`, `save_log`).

Four things had to be fixed before the trace said anything:

| what | why it said nothing |
|---|---|
| `sizeof(homescreen)` on a `char *` (`mtkloader.c:1563`) | 8, not 32768: the buffer was cut to nothing after the first line |
| the ring of lines was flushed by a timer on the main queue | the main queue's timers are run by the main thread's run loop, and that run loop does not turn while the library's constructor waits for its own work: the timer never fired and the lines stayed in the ring. It is a thread of its own now |
| the test job touched the screen 32 s into the run | SpringBoard's library is loaded at 112 s of a run: every touch arrived before there was a home screen to touch. The job waits for the notification `com.hakios.homescreen`, which the home screen posts when its hooks are in place |
| the loader's `.bss` grew by 32 KB | the loader runs with the MMU off and its image has to fit in the 256 KB the chainboot gives it. The buffer is the one that was there already: `.bss` is `0x26900`, as before |

## 3. What the trace says

### 3.1 The touch reaches the window server

The window server's library reads the touch screen's queue and makes HID events out of it.
Every sample of the test is there, at the place it was aimed at:

```
homescreen: sample 1 finger 0 at 10158 19661 (0.1550, 0.3000)
homescreen: sample 1 finger 0 at 25231 19661 (0.3850, 0.3000)
homescreen: sample 1 finger 0 at 40304 19661 (0.6150, 0.3000)
homescreen: sample 1 finger 0 at 55705 32768 (0.8500, 0.5000)
```

283 samples in the run of 9 November, and the events are given to the HID service:

```
homescreen: the event is sent: move at (0.3367, 0.5000)
```

So the touch screen, the kernel's driver, `/dev/hakios-touch`, the window server's library and
the HID service all work.

### 3.2 SpringBoard's windows are not asked

None of SpringBoard's hooks fire: not `-[UIStatusBarWindow hitTest:withEvent:]`, not
`-[SBHomeScreenWindow hitTest:withEvent:]`, not `-[SBIconView gestureRecognizerShouldBegin:]`,
not the icon's own `reasonToDisallowTapOnIconView:`. Not one line.

The windows the home screen has are these, with the level each is at:

```
homescreen: window: UIStatusBarWindow, level 1070
homescreen: window: SBCoverSheetWindow, level 1050
homescreen: window: UITextEffectsWindow, level 10
homescreen: window: SBRootSceneWindow, level 0
homescreen: window: _UISystemGestureWindow, level 0
homescreen: window: SBHomeScreenWindow, level -2
```

**The home screen's window is at level −2: below everything else, including the two windows at
level 0.** The window server asks the windows from the top down and gives the event to the
first that takes it. The home screen's window is the last one asked, so everything above it has
to let the touch through before it is reached.

### 3.3 The grid itself is in order

```
homescreen: grid: the folder view is SBRootFolderView (0x1008a0000), scroll view SBIconScrollView,
scrolling on, the scroll view takes touches, the folder takes touches, 0 reason(s) for the
scrolling being off
```

So `SBFolderView`'s scrolling is not disabled, its scroll view takes touches, and no reason
for refusing them is recorded. The paging is not blocked by the grid's own state.

## 4. Where this points

The touch is delivered to the window server and to the HID service, and SpringBoard's windows
are not asked about it. The home screen's window is at the bottom of the window order, and the
two windows at level 0 (`SBRootSceneWindow`, `_UISystemGestureWindow`) are above it.

The next run traces `-[UIWindow sendEvent:]` to say which window the event is given to. That is
the question this document leaves open: whether the event goes to one of the windows above the
home screen, or to a window that is not on this list at all.

## 5. What is not the cause

- **Not a layer over the icons.** No window between the icons and the screen takes the touch:
  the trace shows the touch reaching the window server untouched by any of SpringBoard's own
  windows.
- **Not the grid's state.** The scroll view takes touches and nothing disables the scrolling.
- **Not the timing.** The earlier runs said nothing because the touches arrived before the home
  screen existed, not because the touch was refused.
- **Not the loader's memory.** The 32 KB buffer that broke the loader's image budget (the black
  screen) is gone; `.bss` is back to `0x26900`.
