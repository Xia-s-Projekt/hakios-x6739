# Handoff: Mali-G77 GPU Acceleration & UI Rendering on Infinix GT 10 Pro (X6739, MT6893)

**Date**: 2026-11-06  
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
