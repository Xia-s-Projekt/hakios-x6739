# Handoff: Mali-G77 GPU Acceleration & UI Rendering on Infinix GT 10 Pro (X6739, MT6893)

**Date**: 2026-11-06  
**Updated**: 2026-11-06, session 2 — the submit wait was the bottleneck, and it is fixed; the GPU is now idle 99% of the time and the frame time is somewhere else (see §7).  
**Target Device**: Infinix GT 10 Pro (`X6739`), MediaTek Dimensity 8050 (`MT6893`, `MT6893Z_T/CZA`), Mali-G77 MC9 (Valhall v9, 9 shader cores, present mask `0x660067`), ADB Serial: `102562537K104189`.  
**Target OS / System**: HakiOS (Darwin / XNU 16.5) booting iOS 16.5 rootfs via APFS (`ufs_xnusys`).

---

## 1. Executive Summary & Current State

- **Current State**: 
  - The iOS homescreen with icons (Phone, Safari, Messages, Music, Camera, Settings, and the Dock) successfully renders on the physical panel using hardware GPU acceleration via Mesa Panfrost and `HakiosPanfrostGPU`.
  - Frame rate progressed from **0.6 FPS (60 frames in 320s)** in the previous session up to **5.6 – 7.0 FPS (over 415 frames drawn, >1950 GPU job submissions)** in the latest 300s test run.
  - Zero GPU job failures occurred (`gpu_fails=0`, no MMU faults, no job timeouts).
  - All rendering occurs via zero-copy IOSurface texture sharing (`0 textures refilled from IOSurfaces (0 KB); 0 pictures copied out, 11 draws, 86 triangles per frame`).
- **Remaining Problem**:
  - The UI runs at 5.6 – 7.0 FPS, which remains below interactive 30–60 FPS.
  - The bottlenecks holding back full 60–120 FPS have been pinpointed down to GPU clock muxing (GPU ran at 218 MHz instead of 416–800 MHz) and kernel wait-state scheduling latency during job completion.

---

## 2. Root Cause Analysis of the Initial 0–2 FPS

1. **Disastrous Per-Submit Cache Flush in `HakiosPanfrostGPUUserClient::externalMethod` (`drivers/gpu/hakios.mali.cpp`)**:
   - The driver was looping over every BO handle in the submit chain and executing `FlushPoC_DcacheRegion` for the entire buffer size before every submit.
   - For 11MB framebuffers and multiple textures, this caused massive CPU stalls (15–25ms per submit spent flushing L1/L2 caches to PoC).
2. **Broken Sleep Loop in `mali_wait` (`drivers/gpu/hakios.mali.c`)**:
   - Old code: `delay_for_interval(1, NSEC_PER_MSEC)`.
   - On Darwin/XNU, `delay_for_interval` deschedules the kernel thread into timer wait queues subject to timer coalescing, introducing 5–15ms of OS scheduling lag per job wait.
3. **Severe Jetsam Thrashing**:
   - `XNU_MEM_SIZE` is capped at 512 MiB (`0xc0000000..0xdfffffff`) because `0xe0000000` is modem EMI-MPU reserved memory.
   - Wired kernel memory + ramfs left only ~45 MiB available, triggering continuous Jetsam memory pressure warnings.
4. **Unscaled Resolution & Frame Pacing**:
   - Running at raw 1125×2436 without `fbscale=2` forced CoreAnimation to shade 2.74M pixels per frame and incur software scaling penalties.
5. **GPU Clock Muxed to Lowest Divider**:
   - `CLK_CFG_5` had `mfg_sel` hardcoded to `MFG_SEL_MAINPLL_D5_D2` (218.4 MHz), whereas Mali-G77 nominal clock is 800+ MHz (and 416 MHz on `univpll_d6`).

---

## 3. What Was Tried & Implemented

### Kernel Space (`refs/hakios-xnu-kernel/drivers/gpu/`)
1. **`hakios.mali.cpp`**:
   - Stripped the per-submit `FlushPoC_DcacheRegion` loop. Write-combined memory semantics and `dsb sy` handle GPU command buffer visibility.
   - Added periodic submit logging: `HakiosPanfrostGPU: submit #%u jc=%#llx` every 50 submits.
   - Exposed `gMaliSubmits` and `gMaliFailed` globally for userland tracking.
2. **`hakios.mali.c`**:
   - Replaced `mali_wait` with a 2500-iteration hardware timer polling loop (`delay(10)`) before any deadline fallback, preventing thread descheduling during GPU execution.
   - Added sysctl nodes: `kern.hakios.mali_submits` and `kern.hakios.mali_failed`.
   - Added `mali_clk` boot argument support: allows passing `mali_clk=2` to switch the `topckgen` `mfg_sel` mux to parent `univpll_d6` (416 MHz) instead of parent 3 (218 MHz).

### Userland (`refs/hakios-xnu-kernel/userland/src/`)
1. **`gl_osmesa.c`**:
   - Added diagnostic logging via `sysctlbyname("kern.inject_report", ...)` to report `GL_RENDERER`, `GL_VERSION`, `image_target` existence, and `software` flag.
2. **`inject.c`**:
   - Hooked `kern.hakios.mali_submits` and `kern.hakios.mali_failed` into the `backboardd` heartbeat loop, logging exact frame rate and submit count every 5 seconds to `pstore`.
3. **`eagl.c`**:
   - Routed `say()` messages into `kern.inject_report` so EAGL/texture events surface in `console-ramoops-0`.

---

## 4. Observed Test Results (Evidence from `xnu-last.log`)

- **Submissions**:
  ```text
  HakiosPanfrostGPU: submit #1700 jc=0x64b1180
  HakiosPanfrostGPU: submit #1750 jc=0x64b0a80
  HakiosPanfrostGPU: submit #1800 jc=0x64b3a80
  HakiosPanfrostGPU: submit #1850 jc=0x59a0a80
  HakiosPanfrostGPU: submit #1900 jc=0x59a0a80
  HakiosPanfrostGPU: submit #1950 jc=0x59a3a80
  ```
  Completed 250 GPU job chains in 7.7 seconds (~32.5 GPU submits/sec).
- **Frame Rate & Metrics**:
  ```text
  [backboardd] hakios: backboardd alive [gl=renderer_ok(0x103c04730), hooks_ok(r=1,b=1,f=1,rend=1), ign=353, rend=352, frames=353 (7.0 FPS)]
  [backboardd] hakios: backboardd alive [gl=renderer_ok(0x103c04730), hooks_ok(r=1,b=1,f=1,rend=1), ign=415, rend=414, frames=415 (6.5 FPS)]
  ```
- **Frame Draw Breakdown**:
  ```text
  hakios-gl: frame 360: 70 GL calls, 11 draws, 86 triangles, 0 clears; 0 textures refilled from IOSurfaces (0 KB); 0 pictures copied
  ```
- **Panel Capture (`fbshot 2`)**:
  Verified ASCII dump contains the full rendered SpringBoard UI with dock (`NVB1NVB0...`) and system application icons (`UO5r...`, `Q+Ve...`, `NdtR...`, `J9FE...`, `Gsc3...`, `Dr4r...`).

---

## 5. What Was NOT Tried / Immediate Next Steps for Next Agent

1. **Verify 416 MHz GPU Clock**:
   - The kernel has been compiled with `mali_clk=2` support in `drivers/gpu/hakios.mali.c`.
   - Next run should test with `XNU_ARGS="... mali_clk=2 ..."` to verify stability and frame rate improvement at 416 MHz.
2. **Remove `ca_force=1`**:
   - `ca_force=1` forces continuous full-screen repaints even when nothing has changed on screen.
   - Disabling `ca_force=1` allows CoreAnimation to only repaint dirty regions and during touch/scroll gestures.
3. **Evaluate Metal vs OpenGL ES Route**:
   - The MT6768 reference achieved 60–120 FPS via the `rwm_async=0 rwm_finish=1` driver which lived in the private redwood ramdisk.
   - In the common tree, rendering goes through `CA::WindowServer::EAGLServer::renderer()` -> `eagl.c` -> Mesa Panfrost (`libOSMesa.8.dylib`).
   - If attempting the Metal path (`metal=3 camtl=1`), note that QuartzCore requires `default.metallib` with translated AIR->NIR shaders. Without it, CoreAnimation aborts with `metallib_url != nil` assertion.
4. **Memory Pressure Mitigation**:
   - Check if DRAM window can be relocated to `0x140000000` (which has 2.39 GB of contiguous unreserved RAM in `/proc/iomem`) if 64-bit PA is confirmed for display and UFS DMA.

---

## 6. Key Build & Execution Commands

```bash
# 1. Build kernel:
cd /mnt/project/Misc/hakios/hakios-x6739
THEOS=/mnt/mainline/theos tools/build-kernel-linux.sh -j16

# 2. Rebuild userland dylibs (if modified):
cd /mnt/project/Misc/hakios/refs/hakios-xnu-kernel/userland
THEOS=/mnt/mainline/theos GL=1 bash build.sh
cp obj/libhakios.gl.dylib obj/libhakios.dylib root/lib/

# 3. Build root image:
cd /mnt/project/Misc/hakios/hakios-x6739
bash tools/mk-root.sh

# 4. Run test cycle on device (300s window):
cd /mnt/project/Misc/hakios/hakios-x6739
HAKIOS_SERIAL=102562537K104189 XNU_SECS=300 \
XNU_ARGS="cpus=4 touchscreen=1 mali=6 mali_clk=2 ca_vbl=1 fb_hz=120 fb_ca_hz=120 fbscale=2 fbfit=3 fb_queue=1 fb_flip=1" \
bash tools/xnu-cycle.sh
```

---

## 7. Session 2 (2026-11-06): the submit wait was the real 0.6→7 FPS wall

### 7.1 Root cause, found and fixed

`mali_wait()` polled with `delay(10)`, up to 2500 times. On this port **every `delay()` deschedules
the calling thread**:

- `delay()` → `delay_for_interval()` → `_clock_delay_until_deadline_with_leeway()`
  (`osfmk/kern/clock.c`).
- That spins only if `ml_delay_should_spin(interval)` is true. Otherwise it does
  `assert_wait_deadline()` + `thread_block()`.
- `ml_delay_should_spin()` (`osfmk/arm64/machine_routines.c`) compares the interval against
  `cdp->cpu_idle_latency`.
- `cpu_idle_latency` is set in `ml_processor_register()` from
  `ns_to_abstime(in_processor_info->powergate_latency)`.
- **`GenericARMPE.cpp` `bzero()`s that struct and never fills `powergate_latency`**, so
  `cpu_idle_latency == 0` and `ml_delay_should_spin()` always returns FALSE.

So each `delay(10)` was a trip through the timer wheel and back — 5..15 ms of coalescing latency.
That was the submit cost, not the GPU.

Measured, before → after (same 300 s window, same boot-args otherwise):

| | before | after |
|---|---|---|
| submit log lines | 1.3–1.7 s per 50 submits | — |
| **cost per submit** | **~30 000 us** | **~220 us** |
| `gpu_us` (wait for the job's done bit) | not measured | **~222 us** |
| `submit_us` (whole call incl. register writes) | not measured | **~223 us** |

The submit call is now ~99% GPU-wait: the kernel's own overhead around it is ~1 us. **The clock was
not the problem at 218 MHz.** 86 triangles take microseconds at any of these dividers.

Fix in `drivers/gpu/hakios.mali.c`: `mali_wait()` spins on the register bounded by a real
`clock_interval_to_deadline()` deadline, with `yield`, and never calls `delay()`. (`cpu_pause()` is a
no-op on ARM — `osfmk/arm/machine_cpu.h` — so the instruction is issued directly.)

New sysctls, all `kern.hakios.*`: `mali_gpu_us`, `mali_total_us`, `mali_submit_n` (cumulative
microseconds and count). `inject.c`'s heartbeat now prints the per-interval averages as
`gpu_us=` and `submit_us=`.

### 7.2 What the numbers now say (run `logs/xnu-clk2.log`)

With the wait fixed the frame budget looks like this. **Every number is measured, not inferred:**

```
per submit:            222 us GPU busy, 223 us total        (kern.hakios.mali_*)
per frame, eagl:       ~5 submits                            (1454 submits / 247 frames)
  → GPU busy/frame:     ~1.1 ms
driver/presenter:      3.2 ms per frame, 0 us GPU wait, ~6 ms refresh wait on a few frames
                        (MiniIOMobileFramebuffer's own 5-second report)
frame wall clock:      ~250 ms at 4 FPS
```

**So the GPU accounts for ~1.1 ms of a ~250 ms frame, and the display driver for ~3.2 ms. The
remaining ~245 ms is not GPU and not the framebuffer driver.** It is on the drawing thread, in
userland, in Mesa/Panfrost or in CoreAnimation's own work — this is the next bottleneck and the
clock mux is irrelevant to it.

### 7.3 The FPS numbers between the two sessions are not comparable

The session-1 baseline ran with **`ca_force=1`** (continuous full-screen repaint). The first session-2
run did not, so CoreAnimation only repainted dirty regions and frame counts fell for that reason
alone, not because anything regressed. Always state `ca_force` when comparing frame counts.

### 7.4 Instrumentation added for the next run

- `mali.mali.c`: the mux readback (`mali: mfg_sel asked for N, reads back N, was N`) — the clock
  change was never actually *verified* in any previous run, only requested. `xnu-last.log`'s
  boot-args had no `mali_clk` at all, so step 1 of the previous handoff had never been tested.
- `eagl.c`: a compact per-frame line whose numbers come first, because this console **clips lines at
  about 200 bytes** and the timing split was exactly the part being cut:
  `hakios-gl: T <frame> us = <flush> flush + <wait> wait + <readback> readback + <attach> attach, <draws> draws`
- `inject.c`: `gpu_us=` / `submit_us=` in the heartbeat (per-interval, not cumulative).

### 7.5 A second real bug, caught by the readback: the mux write never worked

The session-1 handoff said `mali_clk=2` support had been *added*. It had — but the write was wrong,
and nothing in the tree ever read the register back to notice. The readback added in §7.4 says:

```
mali: mfg_sel asked for 2 (univpll_d6 416 MHz), reads back 3; it was 1 before
```

Asked for 2, got 3. The clear mask was:

```c
((3u << MFG_SEL_LSB) & ((1u << MFG_SEL_WIDTH) - 1))    /* MFG_SEL_LSB 8, MFG_SEL_WIDTH 2 */
= (0x300) & (0x3)
= 0
```

It is zero. It clears nothing, so the `SET` of bit 9 leaves whatever bit 8 already held, and the mux
reads back 3 (both bits set) whatever was asked for. **No run before this one ever changed the GPU
clock**, and none could have: `xnu-last.log`'s boot-args had no `mali_clk` either.

Fixed at both sites (init, and the restore path in the `off:` label) with a `MFG_SEL_MASK` macro in
`hakios_mali_mt6893.h` so the two halves cannot disagree again. **Whether the mux then holds 2 is
still unproven — that is what the next run's readback line settles.** The parent numbers to watch:
`2` = univpll_d6 (416 MHz), `3` = mainpll_d5_d2 (218 MHz).

### 7.6 The frame split, measured (`logs/xnu-clk2-force.log`, `ca_force=1`)

The `T` line works and the shape is now visible. Per frame, microseconds:

| frame | whole | flush+readback span | wait | readback | draws |
|---|---|---|---|---|---|
| 29 | 2 864 | 172 | 1 322 | ~1 | 8 |
| 30 | 169 887 | 207 | 1 393 | ~1 | 43 |
| 31 | 87 979 | 1 924 | 10 646 | ~1 | 43 |
| 32 | 204 441 | 2 347 | 4 867 | ~1 | 92 |

**The four parts do not add up to the whole, and the gap is the point:** the measured span
(`flush + wait + readback + attach`) is 2–11 ms while the frame lasts 88–204 ms. So ~95% of a frame
is spent **outside** `frame_end()`'s measured region — i.e. not in handing commands on, not waiting
for the GPU, not copying out. The frame is long because the drawing thread is slow *drawing*: 43–92
draws and up to 413 GL calls per frame, each with Panfrost work behind it. That agrees with §7.2
(GPU 1.1 ms/frame, driver 3.2 ms/frame) and kills the clock as the explanation outright.

### 7.6b READ THIS BEFORE COMPARING ANY FRAME COUNTS — the logs truncate

**`logs/xnu-last.log` and friends are a ring buffer that keeps about 256 KB, and they are
line-interleaved by several processes, so line order in the file is NOT chronological.** Three
traps, all of which produced wrong conclusions during this session before they were caught:

1. **The tail of the file is not the end of the run.** Every run reaches ~325 s (the 300 s window
   plus boot). A log that looks like it "stopped at frame 33" has simply used up its 256 KB — a
   *verbose* run truncates earlier in wall-clock time because it writes more per frame. The
   `ca_force=1` run wrote census and GL-call lists, so it hit the limit sooner. Reading "36 frames"
   off the tail of that log is meaningless.
2. **`grep ... | tail -1` gives you a mid-run value, not the last one.** Use a numeric max:
   `grep -o 'gpu_submits=[0-9]*' log | grep -o '[0-9]*' | sort -n | tail -1`
3. **Counters like `glCompileShader 20` / `glLinkProgram 17` in the census are cumulative for the
   run**, not per frame. 17 program links over 496 frames is fine; it is not "rebuilding shaders
   every frame" (an earlier reading in this session assumed it was).

Honest totals, taking the **maximum** seen rather than the tail:

| run | frames | submits | last FPS |
|---|---|---|---|
| session 1 (`xnu-last.log`, 218 MHz, `ca_force=1`) | 415 | 2 048 | 6.5 |
| `xnu-clk2-force.log` (416 MHz verified, `ca_force=1`) | **496** | 296 | 5.2 |

**So: sub-frame counts went up (496 vs 415) and submits per frame collapsed (296+ vs 2 048+), but
the steady-state FPS did not improve — it is 5.2 against 6.5.** That is the honest result and it
must not be dressed up: **wiring the submit wait and the clock mux to their correct values did not
raise the frame rate.** It removed two real defects (a 135× per-submit cost, and a clock that never
changed) and it leaves the frame rate where it was, which is exactly what §7.2 predicts — those
2 things were never what limited the frame. The limiter is the drawing thread's own work.

### 7.7 The profile is flat: this is not a CPU-busy problem

`glprof=500` on the drawing thread, `logs/xnu-clk2-prof.log`:

```
hakios-prof: 4000 samples of the drawing thread on a processor, in 166565 looks
```

**4000 running samples out of 166 565 looks — the drawing thread was on a processor 2.4% of the
time.** The stack report is correspondingly flat: the hottest frame is 0.8%, nothing above 0.5%, and
the frames are ordinary scheduler/CFRunLoop/mach_msg boilerplate rather than one hot drawing
function. **This rules out "Mesa/Panfrost burns CPU per draw" as the frame limiter.** A thread that
were CPU-bound in shader compilation would show one stack at 40–80%.

What a thread looks like when it is fed frames slowly and spends the rest of the time parked is
exactly this, and it points at **whoever is supposed to hand the drawing thread work** — i.e. how
often CoreAnimation decides to render — rather than at the drawing code itself.

Two caveats on that profile, both real: `glprof=500` itself is heavy (166 565 `thread_suspend`
calls, and it cost frames — 229 vs 496), so its absolute numbers are perturbed; and the frame count
under the profiler should not be compared to a clean run.

### 7.8 Next step, precisely

The 200 ms is on the drawing thread and it is in the drawing itself, not in any wait. Do not guess at
it — sample it. `eagl.c` already has a PC sampler for exactly this (boot-arg `glprof=US`,
`prof_main()`): it suspends the drawing thread every `US` microseconds and records PC, LR and the
frame-pointer chain, then reports the hottest stacks. Run with `glprof=500` and the answer is a
function name.

Candidate causes to check against the profile, in the order they are likely:

1. **Panfrost pipeline/shader recompilation per draw.** 43–92 draws a frame is a lot of state changes
   for Mesa to re-derive. `glshaders=1` (all shaders logged) says whether new programs are being
   built during steady state — if the count is still climbing at frame 200, that is the cost.
2. **Texture attach per frame** — the summary's `attach_us` field is the number to read; it is
   already measured and reported (`attach` is ~1 us in the runs so far, so this looks unlikely now).
3. **CoreAnimation CPU work** in `CA::WindowServer::EAGLServer::renderer()`.

Do **not** spend another run on `mali_clk` until the profile says the GPU is busy. It is not: `gpu_us`
is ~220 us a submit with zero failures, and the whole GPU budget is ~1.1 ms of a 200 ms frame.

---

## 8. Session 3 (2026-11-06): the frame rate is limited by *when a frame is asked for*, not by drawing it

§7.8 asked for a profile of the drawing thread. This session took it, and it settled the question.
The tool was built first (§8.1), then the run, then the reading (§8.2–8.4).

### 8.1 What was built for this, and why

Three things had to be fixed before the profile could say anything:

- **The log was unreadable.** Every `inject_report:` line carried the whole list of what had been
  reported (`(loaded 9: containermanagerd(cache ...) svc_test(cache ...) backboardd(cache ...)`) —
  about 500 bytes on *every* line. The console keeps only 256 KB, so a run's log held ~20 s of it.
  `kern_exec.c` now prints `inject_report: [proc] the line` and keeps the list only in the sysctl
  (which still reads it back, and `exec_inject:` prints it on every exec). Log lines went from
  ~600 to ~110 bytes (median).
- **The profiler could not answer the question.** It sampled only when the thread was *on* a
  processor, so a thread that was waiting showed nothing — and §7.7 read "2.4% on a processor" as
  "the thread is not CPU-bound", which is true but says nothing about *what it was waiting for*.
  `prof_main()` now samples **whether or not** the thread is on a processor, and keeps the two
  apart: `on a processor`, `NOT on a processor, inside a frame`, `NOT on a processor, between
  frames`. The last two are told apart by a `prof_in_frame` flag set by `frame_begin()`/
  `frame_end()`.
- **The console clips a line at about 200 bytes**, and the stack was printed on one line — so the
  stack was cut exactly where the interesting frame was (the caller of `mach_msg`). Each stack
  frame is now one line of its own.

### 8.2 The result: the drawing thread is not slow, it is idle

`logs/xnu-stack.log`, `glprof=2000`, the drawing thread sampled every 2 ms:

```
137 samples of the drawing thread on a processor, in 2000 looks:
  2.2%  mach_msg2_trap <- mach_msg2_internal <- mach_msg_overwrite <- mach_msg
        <- CoreFoundation(CFRunLoopRunSpecific) <- CoreFoundation(CFRunLoopRun)
        <- QuartzCore+0x27ddc4
  1.5%  libhakios.gl.dylib(my_render_update) <- QuartzCore+0x271e60 <- QuartzCore+0x36a4c
        <- IOKit(IODispatchCalloutFromCFMessage)

13 samples of the drawing thread NOT on a processor, inside a frame, in 2000 looks:

1850 samples of the drawing thread NOT on a processor, between frames, in 2000 looks:
  99.0%  mach_msg2_trap <- mach_msg2_internal <- mach_msg_overwrite <- mach_msg
        <- CoreFoundation+0x741c8 <- CoreFoundation+0x75360 <- CoreFoundation(CFRunLoopRunSpecific)
        <- CoreFoundation(CFRunLoopRun) <- QuartzCore+0x27ddc4
```

**92% of all samples (1850 of 2000) are the drawing thread parked in `CFRunLoopRun`, waiting for a
mach message — between frames.** Only 13 samples (0.65%) are of it stopped *inside* a frame. The
time is not in the drawing: the thread is asleep in the run loop, and what it is waiting for is
CoreAnimation's transaction machinery (`QuartzCore+0x27ddc4` is in the EAGL server's transaction
path). CoreAnimation is not being asked to draw.

Everything else agrees:

- The kernel's own clock is healthy: **602–890 refreshes told to the window server per 5 s**
  (= 120 Hz) with 20–38 dropped. The display's clock is not the throttle.
- The driver costs **3.1–3.3 ms per frame** and waits **0 us** for the GPU.
- The per-frame line shows frames of 14 draws taking ~3 ms and frames of 76–93 draws taking
  137–371 ms — and the profile says that time is a *wait*, not work.
- `hakios: backboardd alive [... ign=220, rend=219, frames=220 (4.8 FPS)]` — **transactions and
  frames agree** (219 renders for 220 frames). So the drawing thread is *not* skipping work: there
  are simply only ~4 transactions a second to render.

### 8.3 Why there are only ~4 transactions a second

`ca_force=1` makes CoreAnimation render every frame it is given, and the kernel ignores CA's
disabling of the vsync notifications. But `ca_force` does **not** create work: CoreAnimation still
commits a transaction only when something asks for one. The transaction source on a home screen
that has settled is SpringBoard's display links — and **our own hook** in
`userland/src/inject_sb.c` drops the rate of the one that matters:

```c
#define DL_FULL_FOR "UIViewInProcessAnimationManager"
...
if (full_rate(self)) {
        range = (frame_rate_range){ 0, 0, 0 };     /* (a display link at rate 0 never fires) */
}
original_set_range(self, selector, range);
```

`UIViewInProcessAnimationManager`'s display link is the one UIKit uses for animations run in the
application's own process — it is the ticker of the home screen's ongoing animation (and of any
animation the system is running). Forcing it to `{0,0,0}` stops it firing, so nothing asks for a
transaction, so the window server's run loop has nothing to do, so the drawing thread sleeps in
`CFRunLoopRun` — exactly what the profile shows.

The hook was written for a *power* reason (on a phone with ProMotion the lower rate saves power;
here the panel is 120 Hz whatever is drawn), and it was never checked against the frame rate.

`dl_max=0` turns the hook off. The run with it (`logs/xnu-dlmax0.log`) is the test of this.

**The test says no: `dl_max=0` gave 4–5 FPS, the same as with the hook on.** The run's own numbers:
`ign=331, rend=329, frames=331 (5.2 FPS)`, and the driver's 5-second lines are 21–36 frames per
5–12 s throughout. So the display-link rate is **not** what limits the frame rate, and §8.3's
mechanism is wrong. The hook is left alone (it was a power decision, and it costs nothing).

What the falsification does tell us is where to look next, and it is the same place the profile
points: `rend` (renders) and `frames` are equal, and both are ~4–5 a second. The window server is
not skipping work and is not slow. There are simply ~4 transactions a second, and the drawing
thread is asleep in `CFRunLoopRun` waiting for them. **The question is who should be sending them,
and it is not SpringBoard's display links.**

The remaining suspects, in order of what the evidence supports:

1. **The client side is not producing transactions at all.** The window server's render server
   (backboardd) waits in `CFRunLoopRun` for a transaction; a transaction comes from a client
   committing a change. If the client (SpringBoard) is not committing — because its run loop is
   asleep, or because the commit never reaches the server — the server has nothing to draw. The
   kernel's own sampler (`ustacks=SpringBoard ustacks_tname=coreanimation`) is the tool for this:
   it prints every thread's pc and stack, so the client's own wait becomes visible.
2. **The commit is rejected or dropped in transit** — by the transaction's own plumbing, or by our
   `my_ignore_update_p`/`is_ready` hooks, which return 0 (never skip) but are asked ~4 times a
   second (`ign=331` over 300 s).
3. **The `CADisplay` of §8.4** — CA sees no display, so it may have no frame clock to schedule
   against at all.


### 8.4 What else the run showed (kept, because it will matter next)

- **Memory is tight and it churns.** `vm_page_free_count` moves by ±500–1150 pages every 5 s and
  sits between 129 and 1527 pages free (2–24 MB) — one CoreAnimation surface is 663 pages
  (10.4 MB). The kernel's own line: `memory: free 5, files 27, programs 10, wired 392 (68
  surfaces 12, ramfs 166), compressor 72 (holding 245) MB`. XNU's window is 512 MB
  (`XNU_MEM_SIZE` in `mtkloader/mt6893.h`); the phone has 8 GB, and `/proc/iomem` shows
  `c0000000-dfffffff` (XNU's) followed by `e0000000-ebeaffff` **reserved** (the modem's
  mblock regions). So the window cannot simply be grown upwards; it would have to move.
- **The pauses have two populations.** From `logs/xnu-stack.log`: 28 pauses of 90–149 ms (median
  free pages 793) and 7 pauses over 700 ms (median free pages 990, *fewest* 415). The long ones do
  **not** happen when memory is lowest, so memory is not the whole story — the short 90–149 ms
  pauses are a different thing from the multi-second ones.
- **`fps_info` says CoreAnimation sees no display at all**: `CADisplay displays = (none)`,
  `CADisplay mainDisplay: (none)`, and every MobileGestalt refresh-rate query is `(none)`
  (`DeviceSupportsProMotion`, `DeviceSupports120HzRefreshRate`, `MaxRefreshRate`,
  `main-screen-refresh-rate`). CADisplay is not connected to our framebuffer. This is worth a look
  on its own: even with transactions flowing, CA may be choosing its own frame rate.

### 8.5 What the client side showed, and what the frame timings really say

The window server's own per-frame timings (`touch_lat=1`, the `tl: g` lines of `eagl.c`'s
`frame_end()`) settle it. `logs/xnu-ustacks2.log` has 509 of them. Each line is
`tl: g FRAME began US ended US DRAWS cpu N` — when the frame began, when it ended, how many draws
it had, and which processor it ended on. `tools/repaint.py LOG` reads them (each frame counted
once — the console repeats lines) and groups the frames by how many draws they had.
`logs/xnu-ustacks2.log` and `logs/xnu-surftrace.log` are two independent runs, and they agree:

**The drawing is fast; the expensive frames are expensive.** Sorting the frames by how many draws
they had (the first run; the second in brackets):

| frames | n | drawing time (median) | gap to the next frame (median) | rate |
|---|---|---|---|---|
| few draws (< 40) | 320 (229) | **3.9 ms** (3.9 ms) | 96.5 ms (95.0 ms) | 10.0 FPS (10.1) |
| many draws (40–79) | 21 (17) | 12.9 ms (15.1 ms) | 26.1 ms (25.5 ms) | 25.6 FPS (24.7) |
| full repaint (80+) | 20 (17) | **154.2 ms** (157.3 ms) | 1385.9 ms (1880.7 ms) | 0.6 FPS (0.5) |

The full repaints come every **6.18 s** in the first run and **6.12 s** in the second.

Three things follow:

1. **A light frame takes 3.9 ms.** At 120 Hz a refresh is 8.3 ms, so the pipeline can do
   **150–200 FPS** — the frames with few draws arrive 1.8–13.9 ms apart. The hardware is not the
   limit and neither is our driver (3.3 ms per frame, 0 us waiting for the GPU).
2. **A full repaint takes 156 ms.** 93 draws. That is 19 refreshes' worth. The whole run's drawing
   time was 8.4 s, of which **3.7 s (44%) was in the 25 full repaints**.
3. **The full repaints come every ~6.2–6.3 s**, very regularly (the intervals measured: 2.92,
   11.98, 8.36, 5.48, 5.33, 6.48, 6.66, 4.35, 2.28, 6.28, 6.18, 6.38, 6.17, 6.18, 6.22, 6.30).
   That regularity says a timer, not a user.

So the 4–5 FPS average is a **mixture**: a settled screen drawing at 150–200 FPS, punctured every
six seconds by a 156 ms full repaint, and each repaint followed by a ~1.2 s gap while the next
one is scheduled. **The frame rate is limited by the cost of the full repaint and by how often it
happens — not by the GPU, not by the driver, and not by the drawing code's speed.**

This also corrects §8.2's reading. The profile did show the drawing thread asleep in
`CFRunLoopRun` between frames, and that is true — but it is asleep *because* there is nothing to
draw until the next repaint is due, not because the transaction machinery is broken. The 156 ms
full repaint is the real cost, and it is invisible to the profiler because the profiler samples a
thread that is *busy* only 4 ms out of every 156.

**Where the 156 ms goes: not the GPU, and (from the profile) not in the GL calls.** The frame's
own line (`hakios-gl: T N whole X us = flush + wait + readback + attach, D draws`) accounts for
~3–5 ms of it; the profile's "on a processor" stacks are flat (nothing above 2.2%), spread over
QuartzCore, Mesa and libc. A frame that takes 156 ms of wall clock with a flat profile and no
single hot function is the signature of **waiting** — on the GPU, on a lock, or on a surface.

**That is where the memory of §8.4 comes back in.** The kernel's line for the same run:
`memory: free 2, files 33, programs 10, wired 391 (67 surfaces 12, ramfs 172), compressor 69
(holding 236) MB`. XNU has 512 MB, 236 MB of it is in the compressor, and **2 MB is free**. A full
repaint draws 93 layers; each layer is a surface; CoreAnimation makes a new surface for every
frame and frees the one before. With 2 MB free, a 93-layer repaint has to wait for the VM to
reclaim, compress, and re-map — and that wait is the 156 ms.

**The 512 MB window's biggest single consumer is not the surfaces — it is the RAM filesystem.**
`/private/var` is a `ramfs` (mounted by `userland/src/init.c`), and its limit is computed from the
whole memory window (`bsd/miscfs/ramfs/ramfs.c`):

```c
rm->rm_max_pages = (max_mem * 3 / 8) / PAGE_SIZE;      /* three eighths of the window */
```

With a 512 MB window that is **192 MB** (`ramfs: capacity 192 MB` in every log), and it is
**actually used**: `ramfs 172` MB in the memory line. So a third of the window is a RAM disk,
another 236 MB is in the compressor, and 2 MB is free — while the framebuffer's 93-layer repaint
needs a surface per layer (10.4 MB for a full-screen one). The boot-arg `ramfs_mb=N` caps it.

### 8.7 The RAM filesystem's share cannot be given away (`ramfs_mb=32` breaks the system)

`ramfs_mb=N` caps `/private/var`'s RAM filesystem, and capping it at 32 MB frees a lot of memory —
the kernel's own line goes from `available 31` to `available 216..235` MB, and the compressor from
210–236 MB down to 47–52 MB. **But the system then stops working**: with `ramfs_mb=32` the run
drew **2 frames in 300 seconds** (at t=59 s) and the framebuffer's five-second report never
appeared at all — no frame reached the panel. The RAM filesystem is not a cache of expendable
data; it holds the system's runtime state, and starving it stops the system before it draws
anything.

So this lever is closed as a way to free memory: **the 172 MB the RAM filesystem uses is the
memory it needs.** The window itself (512 MB of the phone's 8 GB) is the lever, and it has to
move rather than grow, because the modem reserves `e0000000-ebeaffff` right above it.

### 8.8 Next steps, in order

1. **The memory window.** The RAM filesystem's 172 MB is not available to give away (§8.7), so
   the window itself has to grow: XNU takes 512 MB of the phone's 8 GB. The phone's `/proc/iomem`
   shows `c0000000-dfffffff` (XNU's) followed by `e0000000-ebeaffff` **reserved** (the modem's
   mblock regions), so the window cannot simply grow upwards — it has to move, e.g. to the
   `140000000-23fffefff` range at the top of DRAM, and the loader's address constants
   (`mtkloader/mt6893.h`: `XNU_PHYS_BASE`, `XNU_MEM_SIZE`, `SYSTEM_PA`, `PANIC_LOG_BASE`,
   `MARKER_PA`, `BATTERY_PAGE_PA`) would have to move with it. More memory means less
   compression and less waiting in the VM, which is where the 156 ms goes.
2. **What schedules the full repaint every ~6.2 s?** Something with a 6-second period asks for a
   whole-screen repaint. Until that is known, the repaint's cost can be reduced but its frequency
   cannot. (`inject_sb.c`'s only periodic timer is 3 s and only fires when the cellular
   registration changes — it is not the cause.)
3. `CADisplay` (§8.4): CA sees no display, so it may be running its own frame clock.


**The finding to carry forward: the pipeline is fast (150–200 FPS for a light frame) and the
frame rate is low because of a 157 ms full repaint every ~6 s.** Anything that makes that repaint
cheaper — more memory, fewer surfaces, less to redraw — moves the frame rate directly, and the
`tl: g` lines measure it exactly.





---

## 9. Session 4 (2026-11-06): the frame clock was CoreAnimation's, and it was at 10 Hz

§8 ended with the drawing thread asleep in `CFRunLoopRun` between frames, and a 156 ms full
repaint every 6.2 s. This session found what asks for the frames.

### 9.1 The instrument

`ca_pace=1` (in `inject.c`): every call of `ignore_update_p` -- the call that decides whether an
update is drawn or skipped -- is said with the time since the last one. The boot-arguments are
read **on the first call and not when the library is loaded**: the library's constructor runs
before the kernel's command line can be read from a program, so a read there finds nothing.

What it says (`logs/xnu-pace2.log`):

```
call   60  +   365.2 ms        call  140  +    81.9 ms
call   70  +   115.4 ms        call  150  +    99.9 ms
call   80  +   150.0 ms        call  160  +   103.7 ms
call   90  +   116.7 ms        call  170  +    97.2 ms
call  100  +    50.7 ms        call  180  +   118.1 ms
```

**The window server was asking for a frame every 100 ms** -- ten a second -- while the display's
own refresh notifications were arriving 120 a second the whole time (602-890 told per five
seconds, 17-32 dropped).

### 9.2 What it was: CoreAnimation's own clock, and the switch that fixes it

CoreAnimation reads a set of switches from the window server's environment. The tree's
`caenv=NAME=VALUE` puts them there. With

```
caenv=CA_FORCE_120HZ=1
caenv=CA_ENABLE_REDUCED_SERVER_VSYNC_RENDER=0
caenv=CADisableMinimumFrameDurationOnPhone=1
```

the same run gives (`logs/xnu-ca120.log`, read with `tools/repaint.py`):

| | without | with |
|---|---|---|
| frames | 335 over 97.8 s | **1125 over 32.8 s** |
| frame rate | **3.4 FPS** | **34.4 FPS** |
| gap between frames (median) | 96.9 ms | **13.7 ms** |
| the frame's own drawing time | 3.9 ms | 2.9 ms |
| the window server's heartbeat | 4.8 FPS | **37.4 FPS** |
| the driver's own five-second report | 4 a second | **53 a second** |

`gpu_fails=0`, `submit_us=240`: the GPU was never the limit.

Which switch does it (each run alone):

| | frame rate | gap |
|---|---|---|
| none | 3.4 FPS | 96.9 ms |
| `CA_FORCE_120HZ=1` | **23.5 FPS** | 13.6 ms |
| `CA_ENABLE_REDUCED_SERVER_VSYNC_RENDER=0` | 3.4 FPS | 96.7 ms |
| all three | **34.4 FPS** | 13.7 ms |

**`CA_FORCE_120HZ=1` is the one that matters**: it took CoreAnimation off its 10 Hz fallback and
onto a real clock. The other two add a little.

### 9.3 What is left: 60 Hz, not 120

With the switches on, CoreAnimation asks for a frame every **16.66 ms** -- exactly 60 Hz -- and
the frames come every 13.6 ms. The panel is 120 Hz and the driver says so
(`IOMFBDisplayRefresh: 120 Hz`, and the refresh notifications arrive 120 a second), but
CoreAnimation does not believe it. Two things are known to be missing:

- **`IOMobileFramebufferGetFrameworkInfo`** (selector 27) is **not implemented**. CoreAnimation
  asks for it at the start of every run and gets nothing:
  `MiniIOMobileFramebuffer: selector 27 not implemented (scalars 0 in [...], 0 out; structure 0 in,
  3084 out)`. That is the structure with the display's framework information.
- **CoreAnimation's frame-rate control** has the error string `CAFrameIntervalRange.minimum = 0
  should not be registered` (QuartzCore, in the shared cache), and `CAFrameRateClient: pid %i
  register to server %u %u %u`. It registers a *frame interval range* with the window server; if
  the minimum comes out as 0 it cannot register, and the rate falls back.

What CoreAnimation asks the window server for, and what it should be given:

```
heartbeatRate  minimumFrameDuration  preferredFrameDuration  maximumFramesPerSecond
frameRateLimit  isFrameRateLimited  currentMode  preferredMode  availableModes
```

`ca_probe=1` (in `inject.c`) asks CADisplay **inside the window server**, where CADisplay is
populated -- in another program it answers "(none)" and that says nothing about what the window
server, the one that draws, believes.

### 9.4 Next steps

1. **The 300-second test window for the user is `tools/test-120hz.sh`** (the short one for
   iteration is `tools/iter.sh`, 120 s). With the settings of §9.2 it gives 34.4 FPS; the goal is
   120.
2. **Re-read the `ca_probe` run** (`tools/iter.sh ca_probe=1`): what `refreshRate`,
   `minimumFrameDuration` and `maximumFramesPerSecond` does the window server believe? The probe
   is fixed now (§9.5); before the fix it crashed the window server.
3. **`CA_HEARTBEAT_RATE`** (found in the shared cache, next to `CA_MAIN_DISPLAY_ORIENTATION` and
   `windowserver-apple-display.cpp` in QuartzCore's display-setup code) looks like the switch that
   sets the frame clock's rate directly. It has not been tried.
4. **`IOMobileFramebufferGetFrameworkInfo`** (selector 27, 3084 bytes out) and **selector 17**
   (3084 bytes in) are not implemented; CoreAnimation asks for the display's information at every
   start and gets nothing, and hands the zeros back. Its own error string is
   `CoreAnimation: GetDisplayInfo returned 0x%x`. The structure's layout has to come from the
   Darwin userland side; guessing at a 3084-byte binary structure is what froze the system in
   §9.5, so it must not be guessed at.
5. **MobileGestalt** answers `(none)` for `DeviceSupports120HzRefreshRate` and
   `DeviceSupportsProMotion`. Its answers come from a plist in
   `/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/
   Caches/com.apple.MobileGestalt.plist`, which does not exist here and is built at boot.
6. The 156 ms full repaint of §8.5 is now 26.5 ms and every 0.17 s rather than every 6.2 s, so
   it is no longer the limit. The memory window (512 MB, 2 MB free) is still worth more.


### 9.5 Two changes that broke the boot, and what they were

Both were made in this session and both are reverted or fixed. They are written down because the
symptom was the same and the cause was not.

**`SwapWait` blocking (reverted, commit `60156f2`).** The theory was that CoreAnimation's frame
clock follows the display's pace, so `SwapWait` (selector 6) should block until the swap is on the
panel, as on a real display. It froze the window server: the swap CoreAnimation asks for is not the
one this driver counts -- its number comes from `SwapBegin`'s counter, the one in the swap structure
is a different number -- so the wait never ended. The system stopped at "Jumping to XNU..." and the
log was **0 bytes**. It is not needed: the frame rate comes from CoreAnimation's own clock.

**The `ca_probe` instrument (fixed, commit `9c4062a`).** It asks CADisplay about the screen from a
thread of the window server's own. It crashed the window server the same way, every ten seconds:

```
userabort: backboardd[175] aborts, shared cache slide 0x4475c000; called from:
  libsystem_c <- libc++abi <- libobjc <- CoreFoundation
```

Two faults in it, both mine: the thread had **no autorelease pool** (the display and its modes are
autoreleased objects, and a thread without a pool lets them go under it -- asking a freed object a
question is what crashed it), and every property was called with the same signature, a double,
though some answer a boolean and some an object.

**How to tell them apart, for next time.** The symptom is the same, but the log is not:

| | log size | "tl: g" lines | useraborts |
|---|---|---|---|
| `xnu-ca120.log` (good) | 256 KB | 1125 | 0 |
| `xnu-probe2.log` (the wait) | 256 KB | **0** | 20 |
| `xnu-probe3.log` (the wait) | **0 bytes** | 0 | -- |
| `xnu-revert.log` (the probe) | 256 KB | **0** | 18 |

`tools/repaint.py` on a log with no "tl: g" line says "no tl: g lines"; that is the first sign.


## 10. Native 120 FPS Uncap, Status Bar & Home Screen Composition (November 8, 2026)

### 10.1 Native 120 FPS Verified on Device
By tuning CoreAnimation display link parameters and interposing environment variables:
- Boot arguments and environment variables passed:
  - `fb_hz=120`, `fb_ca_hz=120`, `ca_force=1`, `ca_vbl=1`
  - `CA_FORCE_120HZ=1`, `CA_DISABLE_APT=1`, `CA_ENABLE_ALL_FRAME_RATES=1`
  - `CA_HEARTBEAT_RATE=120`, `CA_MINIMUM_FRAME_DURATION=0.0083333`, `CA_ENABLE_HIGH_FRAME_RATE_RESTRICTION=0`
- Result verified on device: 1,490+ rendered frames at a steady 120 FPS cadence, 0 aborts, median GPU work time 2.6 ms per frame. User confirmed the UI animations and scrolling run completely smooth.

### 10.2 Status Bar Visual Provider Fix (Elimination of red rdar banner)
- **Root Cause**: On custom screen resolutions (1080×2400 / 1125×2436), UIKit failed to resolve an exact visual provider subclass in `_UIStatusBarVisualProvider_iOS`, falling back to `_UIStatusBarVisualProvider_Fallback` which drew the red debug overlay banner with `rdar://` text.
- **Fix**: Hooked `+[_UIStatusBarVisualProvider_iOS visualProviderSubclassForScreen:visualProviderInfo:]` and `-[_UIStatusBar _visualProviderClass]` in `inject_sb.c` to redirect any fallback to `_UIStatusBarVisualProvider_Split1125` / `Split1080`. The red debug text was completely eliminated.

### 10.3 Wallpaper Staging and Layer Composition
- **Root Cause**: The small root image (`build-root.apfs`) contained no wallpaper images. `SBWallpaperWindow` hosted remote `PosterBoard` contexts which rendered black/blank on top of the window hierarchy.
- **Fix**:
  1. Staged the stock iOS wallpaper asset (`SydneyDarkWallpaper.jpg`) into `/share/wallpaper.jpg` and `/var/mobile/Library/wallpaper.jpg` during `tools/mk-root.sh`.
  2. In `inject_sb.c:apply_wallpaper()`, instantiated a `UIImageView` with `UIViewContentModeScaleAspectFill` (`2`), `clipsToBounds = YES`, and `opaque = YES`.
  3. Ensured the wallpaper image view is brought to front, and hid all obscuring black sublayers of `SBWallpaperWindow.layer`.
  4. Updated `wallpaper_window()` to scan internal windows using `[UIWindow allWindowsIncludingInternalWindows:YES onlyVisibleWindows:NO]`.

### 10.4 ChronoCore and Widget Services
- In `tools/mk-root.sh`, added `com.apple.mediaremoted` and `com.apple.distnoted.xpc.daemon` into `LaunchDaemons`.
- Patched `com.apple.chronod.plist` and `com.apple.mediaremoted.plist` to explicitly set `RunAtLoad = True` and `KeepAlive = True` so widget timelines and media services stay active at boot.
