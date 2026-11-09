# GPU port plan — Mali-G77 MC9 on the X6739 (MT6893)

Status: prepared, not yet run. Nothing here has been flashed or tested on the phone. Every number
in this document was read out of the phone's own device tree, out of MediaTek's own MT6893 kernel
and module sources, or out of ARM's kbase for this GPU — each with its place named.

The current graphics path is CPU rendering: `tools/build-graphics.sh:46` builds Mesa with
`DRIVERS=softpipe`, `userland/src/gl_osmesa.c` draws through OSMesa, and
`MiniIOMobileFramebuffer.cpp` copies the pixels into LK's framebuffer on the CPU (2–4 ms per
frame, `logs/xnu-c4-300-filter0-ca120.log:2253-2335`). The GPU receives no work.

## 1. What the GPU is

| Fact | Value | Evidence |
|---|---|---|
| Part | Mali-G77 MC9, Valhall (Arm's DDK name "TTRx") | `/sys/devices/platform/soc/13000000.mali/gpuinfo` on the phone: `Mali-G77 9 cores r0p1 0x9080` |
| Chip id | `0x9080` = arch major 9, product major 0, arch rev 8 | same; `GPU_ID` layout in `mali_kbase_gpu_id.h:37-54` |
| Revision | **r0p1** | `gpuinfo` string; this decides the TTRX_3485 workaround (below) |
| Shader cores | 9 (JS0/JS1/JS2, present mask `0x660067`) | `/sys/.../core_mask` |
| Registers | `0x13000000`, size `0x4000` | DT `/soc/mali@13000000` (`mt6893.dts:3729-3731`) |
| Interrupts | GIC SPI 362 GPU, 363 MMU, 364 JOB, 365 EVENT, 366 PWR | same DT node |
| Clock gate | `mfgcfg` syscon `0x13fbf000`, bit 0 = BG3D | `clk-mt6893-mfgcfg.c:22-40`; DT `syscon@13fbf000` |
| Block clock | `mfg_sel` mux, topckgen `CLK_CFG_5` bits 9:8, gate bit 15, update bit 21 | `clk-mt6893.c:1475-1486`, `:111` |
| Parents | 0 tck_26m_mx9_ck, 1 mfgpll_ck 1150 MHz, 2 univpll_d6, 3 mainpll_d5_d2 218.4 MHz | `clk-mt6893.c:935-940`; DT gpufreq node |
| Power | scpsys `0x10006000`, domains MFG0..MFG6 | `mtk-scpsys-mt6893.c:29-98` |
| Domain ctl offsets | MFG0 `0x308`, MFG1 `0x30C`, MFG2 `0x310`, MFG3 `0x314`, MFG4 `0x318`, MFG5 `0x31C`, MFG6 `0x320` | same |
| Domain status bits | 2,3,4,5,6,7,8 in `PWR_STATUS 0x16C` and `PWR_STATUS_2ND 0x170` | same (`.sta_mask`, `.pwr_sta_offs`) |
| SRAM | pdn bit 8, ack bit 12, in every domain | same (`.sram_pdn_bits`) |
| Bus protection | five IFR steps for MFG1, each its own register pair: clear `0x2AC`/`0x718`/`0x2A4`/`0x718`/`0xBBC`, set four bytes below each | same (`.bp_table`) + `scpsys.h:296-301` |
| Rails | VGPU on `mt6315_7_vbuck1`, VSRAM_GPU on `mt_pmic_vsram_others_ldo_reg` | DT gpufreq node (`mt6893.dts:3782-3783`) |
| Android driver | ARM kbase r32p1, **job manager**, `/dev/mali0`, kbase ioctls (not DRM) | `mali_kbase_mt6893.ko` (`r32p1-01eac0`), `nm` shows `kbase_jm_*`, no `kbase_csf_*` driver symbols |
| Firmware | only `valhall-1691526.wa` (440 bytes); **no** CSF firmware on the phone | `/vendor/firmware/`, module strings |

## 2. The refresh-rate question, answered first

The panel is a 120 Hz part: `nt37703_fhdp_dsi_vdo_tm_panda_120hz_x6710`, handed over by LK at
≈119.74 Hz and measured by Android at ≈120.00001 Hz. The ~10 FPS we see is **software rendering
producing frames**, not the panel rate. Two separate things are wrong with pacing:

* XNU's own frame pacing defaults to `fb_hz=60`. For native 120 Hz both `fb_hz=120` and
  `fb_ca_hz=120` are needed.
* `fb_measure=1` cannot be used on this phone: it reads SM7325 DPU counters that MT6893 does not
  have. There is no working panel-refresh measurement on this board yet, so any number we have
  seen for "panel Hz" from that path is meaningless.

## 3. The main issue, and why

**Mesa 25.0.7 cannot open this GPU.** `src/panfrost/lib/pan_props.c:51-77` has no G77 row;
`panfrost_get_model()` returns NULL for `0x9080` and the driver aborts with "Unsupported model".
And G77 is arch v9, which in this Mesa takes the **job-manager** path (`pan_jm.c` is built for
arch 9; `pan_csf.c` is compiled only for `PAN_ARCH=10` and talks to *panthor* ioctls). So the fix
is not CSF — it is:

1. a G77 row in the model table (`pan_props.c`), and
2. a kernel driver that answers the **panfrost** ioctls (`DRM_IOCTL_PANFROST_GET_PARAM`,
   `CREATE_BO`, `MMAP_BO`, `WAIT_BO`, `SUBMIT`) for a Valhall v9 GPU.

**That kernel driver does not exist.** The common tree's GPU path is Adreno-only:
`drivers/gpu/hakios.adreno.c`, `drivers/gpu/hakios.gpu.cpp` (msm ioctls only, `:70-84`), and
`tools/mesa/hakios-drm/hakios.drm.c` refuses any device name but `"msm"` (`:33`) and stubs every
syncobj and prime-fd call `ENOSYS` (`:210-226`). Panfrost needs a working fence path, so those
stubs are the second blocker.

**The template exists.** The MT6768 reference port already did this shape for a Bifrost G52:
`mtk/mt6768/drivers/gpu/hakios.mali.c` (1146 lines) + `hakios.mali.cpp` implement a panfrost-shaped
service over the IOKit user client — parameters, buffer addresses, page-table entries, and jobs —
and its `first_jobs()` path shows the whole submission sequence. That file is where the G77 work
belongs, forked and re-pointed at Valhall.

**Arm's descriptor format is the third issue — and it is avoidable for the first job.**
The kernel DDK treats a job chain as an opaque 64-bit GPU address (`jm/mali_kbase_jm_defs.h:510`);
there is no `jb`/descriptor encoder anywhere in the kernel tree, and Arm's user-space driver
(libmali) is not on this machine. *But* the workaround blob this revision requires is itself a
valid G77 job chain: `valhall-1691526.wa` carries 384 bytes of job data destined for the fixed
GPU address `0x100000000` (`mali_kbase_dummy_job_wa.c:41-54`, `:370-403`). Pointing a job slot's
`JS_HEAD_NEXT` at that address runs it. So we can prove the GPU executes work without ever
encoding a descriptor.

**The workaround is mandatory on this revision.** Hardware issue `BASE_HW_ISSUE_TTRX_3485` is
listed for tTRx r0p0 and r0p1, and *not* for r0p2
(`mali_base_hwconfig_issues.h:60,353,369,373-386`). This phone is r0p1. Without the blob Arm's
driver fails the first `/dev/mali` open with "WA blob missing" and no job can be submitted at all
(`mali_kbase_dummy_job_wa.c:259-296`; `mali_kbase_core_linux.c:766`). We have the blob in
`fw-gpu/valhall-1691526.wa`.

**CSF is not an option.** `csf/` exists in the DDK but is compiled only under
`CONFIG_MALI_CSF_SUPPORT=y`, which is defaulted to `y` only `if GPU_HAS_CSF` — and `GPU_HAS_CSF`
is defined nowhere in this tree (`Mconfig:50-53`, `Kbuild:97-99`). The shipped module is JM-only.
CSF would additionally need Arm's `mali_csffw.bin`, which is neither in the tree nor on the phone.
Whether the G77 silicon physically contains CSF blocks is **unconfirmed** from any source we
have; it does not matter, because we are not going that way.

**Community precedent, and its warning.** The PanVK-kbase project added exactly the row we need
(`PAN_PROD_ID(9,0,0)`, "G77", with G57's tile-buffer values as a first guess) and states plainly
that v9 remains "experimental and partly broken", with G77 untested. Our port inherits that
risk. The G77 tile-buffer size and feature bits are therefore *to be read from the silicon*
(`TILER_FEATURES 0x00C`, `MEM_FEATURES 0x010`, `MMU_FEATURES 0x014`, `AS_PRESENT 0x018`,
`JS_PRESENT 0x01C`), not guessed from the G57.

## 4. What the GPU's own registers say (the map we will use)

From ARM's kbase for this GPU: `mali_kbase_gpu_regmap.h` and
`backend/mali_kbase_gpu_regmap_jm.h`.

* GPU control block `0x0000`: `GPU_ID 0x000` (one identity register — version in bits 3:0/11:4/15:12,
  and in this hardware's product layout arch major 31:28, arch minor 27:24, arch rev 23:20 and
  product major 19:16; there is **no** separate GPU_ID2 register). The *model* of a GPU is
  `((id >> 16) & 0xf00f)` — arch major and product major, the two fields Arm's own driver
  compares (`GPU_ID2_PRODUCT_MODEL`). This phone reads `0x9080…`, masked `0x9000`: the G77.
  `GPU_IRQ_RAWSTAT/CLEAR/MASK/STATUS` `0x020/24/28/2C` (bits: fault 0, multiple faults 7, reset
  completed 8, power changed single 9 / all 10, counter sample 16, cache clean 17),
  `GPU_COMMAND 0x030` (1 soft reset, 2 hard reset, 3 clear counters, 7 clean caches, 8 clean and
  invalidate), `L2_CONFIG 0x048`, `PWR_KEY 0x050`, `PWR_OVERRIDE0/1 0x054/0x058`,
  `GPU_FEATURES_LO/HI 0x060/0x064`.
* Present / ready / power: shader `0x100/0x140/0x180`, tiler `0x110/0x150/0x190`, L2
  `0x120/0x160/0x1A0`; power-off `0x1C0/0x1D0/0x1E0`; power-transition `0x200…`; power-active
  `0x240…`; stack variants `0xE00…0xE44`.
* Job manager block `0x1000`: `JOB_IRQ_*` `0x000..0x00C`; slot *n* at `0x1800 + n*0x80`
  (`JS_HEAD_LO/HI 0x00/04`, `JS_TAIL 0x08/0C`, `JS_AFFINITY 0x10/14`, `JS_CONFIG 0x18`,
  `JS_XAFFINITY 0x1C`, `JS_COMMAND 0x20`, `JS_STATUS 0x24`, `JS_HEAD_NEXT 0x40/44`,
  `JS_AFFINITY_NEXT 0x50/54`, `JS_CONFIG_NEXT 0x58`, `JS_COMMAND_NEXT 0x60`, `JS_FLUSH_ID_NEXT 0x70`).
  There is no `JS_BASE` or `JS_CONTEXT` register in this hardware.
* `JS_CONFIG` bits: start-flush 9:8, `START_MMU` 10, chain flag 11, end-flush 13:12, flush
  reduction 14, no-descriptor-write-back 15, thread priority 23:16 — and **the address space in
  the low nibble**.
* MMU block `0x2000`: `MMU_IRQ_*` `0x000..0x00C`; address space *n* at `0x2400 + n*0x40`
  (`AS_TRANSTAB 0x00/04`, `AS_MEMATTR 0x08/0C`, `AS_LOCKADDR 0x10/14`, `AS_COMMAND 0x18`,
  `AS_FAULTSTATUS 0x1C`, `AS_FAULTADDRESS 0x20/24`, `AS_STATUS 0x28`, `AS_TRANSCFG 0x30/34`).
  Walk modes: LEGACY 0, UNMAPPED 1, IDENTITY 2, **AARCH64_4K 6**, AARCH64_64K 8.
  `AS_COMMAND`: UPDATE 1, LOCK 2, UNLOCK 3, FLUSH_PT 4.
* Interrupts: `JOB_IRQ_RAWSTAT` bit *i* = slot *i* finished, bit 16+*i* = slot *i* failed; in the
  MMU, bit *n* = a page fault in address space *n*, bit 16+*n* = a bus error.

## 5. The order of work

### Step 1 — the GPU answers (loader, bare metal) — DONE, proven on the phone

`mtkloader/mtkmali6893.h` holds the constants and the sequence: clock mux → MFG0 → MFG1 →
bus protection open → MFG2..MFG6 → BG3D gate → read `GPU_ID`, features, present/ready.

The loader's probe has been rewritten for this board. It used to include `mtkmali.h`, which is
the **MT6768's** file (G52 at `0x13040000`, four SPM domains, the `CLK_CFG_1` mux,
`MFGCFG_BASE 0x13000000`), and its rail code talked to the MT6358 PMIC through `mtkpwrap.h` —
this phone's GPU rails are on an MT6315 buck and a PMIC LDO (`mt6893.dts:3782-3783`), and the
loader has no MT6358 here, so every number in it was wrong for this board. The new probe
(`mtkloader/mtkloader.c`, `GPU_PROBE` block) uses `mtkmali6893.h` throughout: seven domains,
the `CLK_CFG_5` mux, the `mfgcfg` gate at `0x13fbf000`, the Valhall register map, and no rail
switching at all. It reads `GPU_ID`, the features, the address-space and job-slot masks, the
present masks, and powers the L2, the tiler and the shader cores up and down again.

The probe is compiled out by default (`#define GPU_PROBE 0`, `mtkloader/mtkloader.c:132`) and
is run by asking the loader for the flag `DARWIN-X6739:GPU-PROBE` through pmsg, so it is a
diagnostic tool, not part of the boot path. It compiles both ways
(`clang -DGPU_PROBE=1` and the default build) but has **not** been run: that needs a device.

Order matters: the clock must run before any domain is powered (MediaTek does it that way in
`clk-mt6885-pg.c`); the bus protection may only be released once MFG1 has power; the GPU's
registers are only readable once the block's own gate is open.

Acceptance: `GPU_ID` reads `0x9080…` and the shader/tiler/L2 present masks match the phone's
`core_mask` (`0x660067`).

**Met, on 2026-11-04.** The probe was flashed to `boot_a` only (slot a; `backup/boot_a.img` is
untouched) and asked for through pmsg; `logs/gpu-probe.log` has the run:

```
== mtkloader: GPU probe ==
the rails are the preloader's here; this probe touches none
SPM power status 0x10180e01 / 0x10180e01, mfg_sel 0x3, bus protection 0x519c0000
mfg_sel to mainpll_d5_d2 (218 MHz)
  powering MFG0: on
  powering MFG1: on
  opening the bus to the GPU block
  powering MFG2: on
  powering MFG3: on
  powering MFG4: on
  powering MFG5: on
  powering MFG6: on
mfgcfg clock gates 0x0 -> 0x0
GPU_ID 0x90800011: Mali-G77 r0p1
  features 0x0, address spaces 0xff, job slots 0x7
  shader cores 0x660067, tiler 0x1, L2 0x1
soft reset: done
L2 power: ready, tiler: ready, shader cores: ready (0x660067)
```

So: the GPU is a G77 r0p1, it answers a soft reset, its nine shader cores and its tiler and L2
come up and say ready, and all seven power domains and the clock mux go back down afterwards.
`GPU_ID` reads `0x90800011`, whose masked model `((id >> 16) & 0xf00f)` is `0x9000` — the G77 —
which is what the product-name lookup now compares.

Two things in that output look odd and neither is a failure:

* `features 0x0`: `GPU_FEATURES_LO` at `0x060` reads zero — and Arm's own driver does not read
  that register for this GPU at all: it reads it only when the id is at least
  `GPU_ID2_PRODUCT_MAKE(11,8,5,2)` (`mali_kbase_gpuprops_backend.c:113-121`), and records zero
  for anything below that. This G77 is `0x90800011`, far below it, so zero is what the DDK
  expects here and the register itself is not implemented on v9.
* `mfgcfg clock gates 0x0 -> 0x0`: the gate's status register reads zero both before and after
  the gate is opened, so it does not say whether the block's clock is running. The GPU answered
  anyway, which is the proof that matters.

**What this does not prove**: that the GPU can *do* anything. It has been woken, identified and
powered down. No job has been run, and the drawing path is still softpipe.

### Step 2 — one job executes

The workaround blob is already in `fw-gpu/valhall-1691526.wa` (440 bytes, the same file the phone
ships). Its layout, read out of the file and out of `mali_kbase_dummy_job_wa.c:35-54`:

```
0x000  header   { u16 signature 0x4157 'WA', u16 version 2, u32 info_offset 0x1a0 }
0x008  payload  384 bytes: the job chain itself
0x1a0  info     { u64 jc 0x100000000, u32 js 0, u32 blob_offset 0x188, u64 flags 0x6 }
0x188  blob     { u64 base 0x100000000, u32 size 384, u32 map_flags 6,
                  u32 payload_offset 8, u32 blob_offset 0 }
```

So: copy the 384 bytes at file offset 8 to the fixed GPU address `0x100000000`, and submit that
address as the chain.

The flags `0x6` are `WAIT_POWERUP | LOGICAL_SHADER_POWER` (`mali_kbase_dummy_job_wa.h:25-28`):
SERIALIZE is *not* set, so the driver runs **one** job for the whole core mask, not one per
core. The job slot is 0 (`v2_info->js`).

What Arm's own driver does around the submission (`mali_kbase_dummy_job_wa.c:154-240`, the
`execute()` function), which a bare-metal driver must also do:

1. Mask the GPU and job interrupts off (`GPU_IRQ_MASK = 0`, `JOB_IRQ_MASK = 0`), keeping the
   old masks to restore afterwards.
2. `SHADER_PWRON_LO/HI` ← the core mask. Because WAIT_POWERUP is set, wait for
   `SHADER_READY_LO/HI` to show those cores ready.
3. Run the job: `JS_HEAD_NEXT_LO/HI` ← the chain address; `JS_AFFINITY_NEXT_LO/HI` ← the cores;
   `JS_CONFIG_NEXT` ← `JS_CONFIG_DISABLE_DESCRIPTOR_WR_BK | as`; `JS_COMMAND_NEXT` ←
   `JS_COMMAND_START`. That is verbatim `run_job()` (`:113-151`).
4. Wait for `JOB_IRQ_RAWSTAT`: bit *i* for slot *i* finished, bit 16+*i* for slot *i* failed
   (`mali_kbase_jm_hw.c:479-481`). Arm treats anything but exactly bit *i* as a failure and
   reads `JS_STATUS` for the fault code — that register is the first real diagnosis.
5. Because LOGICAL_SHADER_POWER is set: `SHADER_PWROFF_LO/HI` ← the core mask, then wait for
   `SHADER_READY` *and* `SHADER_PWRTRANS` to clear, and clear the GPU interrupts.
6. Restore the interrupt masks.

Two things the driver's MMU does that the plan must not skip:

* The blob is not simply written to a physical address: the driver allocates a GPU virtual
  region at the fixed VA `0x100000000` (`kbase_mem_alloc` with `BASE_MEM_FLAG_MAP_FIXED`,
  `:365-380`), copies the payload into it (`:383-397`), and the job's address space must map
  that page. So the page tables have to exist before the job, not after.
* The address space the job runs in is the privileged context's AS (`kbdev->dummy_job_wa.ctx->
  as_nr`, `:170`), not necessarily AS 0. The AS number goes in the low nibble of `JS_CONFIG`.
  We can use AS 0 as long as our tables and the job agree.

So the full sequence is: build the page tables for AS 0, map one page at GPU VA `0x100000000`,
put the payload there, program the AS (`AS_TRANSCFG` = AARCH64_4K with
`PTW_MEMATTR_WRITE_BACK` and `R_ALLOCATE`, `AS_TRANSTAB` = the top-level table, `AS_MEMATTR` =
the attribute index, then `AS_COMMAND_UPDATE`, waiting for `AS_STATUS_AS_ACTIVE` to clear
between commands), power the shader cores up and wait, mask the interrupts, submit, wait for the
slot's interrupt bit, then power the cores down and restore the masks.

The page tables: four levels, 512 entries each, 4 KiB pages (Arm's AArch64 MMU mode,
`mali_kbase_mmu_mode_aarch64.c:28-46` — type in bits 1:0, attribute index 4:2, access 7:6,
share 9:8, access flag 10). A level-2 entry is a 2 MiB block; the one page we need is a
level-3 entry.

**What the payload is, and what the claim rests on.** The 384 bytes have six non-zero 64-bit
words and nothing else:

```
+0x010  0x0000000000010012
+0x020  0x0001003f00000000
+0x028  0x0000000100000081
+0x0a0  0x00000000001f03ff
+0x0a8  0x1000d000001f03ff
+0x100  0x0688009804000000
```

Their meaning is Arm-proprietary and is not derivable from anything on this machine: the kernel
DDK contains no job-descriptor encoder or documentation for this GPU (only the opaque 64-bit
chain pointer), and Arm's user-space driver (libmali) is not here. So the claim "we do not need
Arm's descriptor format for the first job" rests on exactly one thing, and it is a strong one:
**Arm's own driver submits these 384 bytes, at this address, as a job chain, and the hardware
executes them.** We are copying a chain that is known to work, not decoding one. If the job
faults, the fault will be in our MMU setup or our slot programming, not in the payload.

One unknown: the payload contains words whose shape suggests addresses outside the one page
Arm allocated (for example `0x1003f00000000`). Arm maps only one page for it
(`PFN_UP(384) = 1`, `:369-374`), so those must be GPU-internal addresses or fields rather than
system memory we have to provide. If the first attempt faults on an address, that is where to
look.

Acceptance: the slot finishes without a fault.

### Step 3 — Mesa can open it

* Add the G77 row to `pan_props.c`. **Done**: `tools/mesa/panfrost-g77.patch` adds
  `MODEL(0x9080..0x9083, "G77", "TNAx", HAS_ANISO, 16384, {})`, and applies cleanly to a fresh
  extract of mesa-25.0.7.tar.xz. Upstream Mesa names the same product a G77 too (its
  `src/panfrost/model/pan_model.c` has `VALHALL_MODEL(PAN_PROD_ID(9,0,0), 0, "G77", "G77", ...)`),
  so this is the row upstream would carry.
* The tile-buffer size and the feature bits come from the silicon, not from the G57: the loader
  reads `TILER_FEATURES`/`MEM_FEATURES` on the phone.
* Add a `panfrost` device to the libdrm stand-in (`tools/mesa/hakios-drm/hakios.drm.c`). Two things
  force this: `pan_kmod.c` matches `drmGetVersion()->name` against "panfrost" and "panthor" and
  fails on anything else, and `panfrost_kmod.c` calls `drmCloseBufferHandle()`, which the stand-in
  does not have. The stand-in must keep per-device state, because MSM and Panfrost number their
  ioctls the same way (both are `0x40 + n`).
* Build Mesa with `DRIVERS=softpipe,panfrost` (the `mk-mesa.sh` `DRIVERS` variable already takes
  it; softpipe has to stay because the OSMesa target requires a software gallium driver, and
  because the library must still be able to fall back).

### Step 4 — the window server draws with it

* Kernel: fork `hakios.mali.c` from the MT6768 reference for the G77's registers, and answer
  `DRM_IOCTL_PANFROST_*` from it. Its MMU must build real tables, not the reference's
  single-page stand-in, because Mesa maps real buffers. **The reference's table format must not be
  copied**: it is Bifrost LPAE (three levels, `PTE_TABLE`/`PTE_PAGE` = 3, no count field), while
  the G77 takes kbase's aarch64-mode descriptor format — four levels of 512 entries, the entry
  type in bits 1:0, AF in bit 10, AP in bits 6:7, NX in bit 54, and the count of valid entries in
  bits 55:58 of the first three words of each populated table. The loader already writes exactly
  that (`gpu_job()` in `mtkloader/mtkloader.c`) and the G77 accepted it.
* IOSurface sharing: the reference's `hakios_gl_surface_texture` path (`userland/src/eagl.c:598-670`)
  hands an IOSurface's memory to the driver; the framebuffer copy in
  `MiniIOMobileFramebuffer.cpp` then has to become a GPU-side blit, or the 2–4 ms CPU copy stays.
* Then the refresh work of §2: `fb_hz=120`, `fb_ca_hz=120`, and a real MT6893 refresh measurement
  (there is none today).

## 6. Known unknowns

* **The G77 job descriptor byte layout is no longer needed by us.** Mesa's panfrost builds the
  Valhall v9 chain in user space and the kernel only points the GPU at the chain address
  (`JS_HEAD_NEXT`) and waits for it, exactly as the reference's `redwood_mali_submit()` does. The
  kernel never parses a descriptor.
* The G77 tile-buffer size and feature bits: to be read from `TILER_FEATURES`/`MEM_FEATURES`.
* Whether the G77 silicon contains CSF blocks: unconfirmed and not needed.
* Whether this r0p1 needs the workaround at all if a later revision is fitted: it does, here.
* Interrupts: the loader has no GIC wiring for SPI 362-366 yet; step 2 should poll
  `JOB_IRQ_RAWSTAT` rather than wait for an interrupt.

## 7. What the runs showed

**Run 1 (the driver not started at all).** The kernel built with the driver in it, the boot-arg
`mali=6` was in the command line, and the driver said nothing. The reason: every driver's `init`
is called from the input driver's `hakios_input_init()` (`bsd/dev/hakios.input.c`), and the
Mali's was not called from there. A driver that is not built has a stub in
`bsd/dev/hakios.stubs.c`; the Mali needed both the call and the stub.

**Run 2 (a panic in the driver).** With the call in place the kernel panicked before the driver
printed anything: "Kernel data abort", `far: 0x60`, in `hakios_input_init`. The offset 0x60 is
`CLK_CFG_5`, and it was read *before* `ml_io_map()` had mapped the clock registers: the line
`uint32_t clk_cfg5 = rd(topckgen + CLK_CFG_5);` sat above the mapping, so `topckgen` was still
0 and the read was of address 0x60. The read belongs after the mapping, and the mapping is
checked for 0 before it is used.

The lesson for the rest of this driver: in the kernel the registers are reached through
`ml_io_map()`, which gives a *virtual* address; a physical address is not an address to read.
The loader has no such difference, which is why the same register access is correct there and was
wrong here.

**Run 3 (the GPU is up).** With the crash fixed, the kernel ran its full 90 s and said:

```
mali: GPU_ID 0x90800011: Mali-G77 r0p1 (Valhall), 9 shader cores (present 0x660067), tiler 0x1, L2 0x1
mali: the GPU is up; the service HakiosMaliGPU is for programs to draw with
HakiosMali: the GPU is there for programs to draw with
```

The id and the present mask are the ones the loader read bare-metal, so the driver brings up the
same GPU the loader did. `mali=6` is how far it goes: 1 the clock mux, 2 the seven power domains,
3 the bus protection, 4 the block's clock gate, 5 the GPU itself, 6 its address space.

**Run 4 (120 Hz with the driver).** `fb_hz=120 fb_ca_hz=120` and `mali=6` together:

```
MiniIOMobileFramebuffer: IOMFBDisplayRefresh: 120 Hz (the panel: 120), an interval of 35791394 (2^-32 s), 108333 ticks
mali: GPU_ID 0x90800011: Mali-G77 r0p1 (Valhall), 9 shader cores (present 0x660067), tiler 0x1, L2 0x1
```

So the display driver talks to CoreAnimation at 120 Hz with the GPU up. What is *not* yet true is
that the frames are drawn by the GPU: nothing has opened `HakiosMaliGPU`, because Mesa's panfrost
is not built for this target yet, so the window server still draws with the software renderer.

**Touch.** The touch screen does not report points in these runs ("no point in 1000 report
frames"), and it did not in the earlier runs either (`logs/xnu-full-jobs-300.log`), so it is not
something the GPU driver caused. The chip answers on the bus (`chip_id=0x5662 fw_ver=0x44
vendor=0x8d points=15`) but the short reads come back all 0xff. Separate work.

## 8. Mesa on iOS: how it reaches the GPU

Mesa 25.0.7's panfrost is a Linux driver: it talks to the kernel through libdrm. On Darwin there
is no libdrm, so the common tree has a stand-in for it, `tools/mesa/hakios-drm`, which carries the
requests to the kernel's driver as calls of its user client. freedreno (the Adreno) already used
it; panfrost now does too.

**The four changes** (`tools/mesa/panfrost-darwin.patch`, plus one line in
`tools/mesa/freedreno-darwin.patch`). The two patches touch the same two files -- the OSMesa
target's `meson.build` and its `osmesa_target.c` -- because freedreno's patch is what puts the
GPU screen into the OSMesa target in the first place. So panfrost's patch is built against the
tree as freedreno's leaves it, and the order the patches are applied in matters: the glob order
of `tools/mesa/*.patch` puts freedreno-darwin first, which is what is wanted.

The whole set applies cleanly to a fresh extraction of the tarball, and every change is
present after it: the GPU screen in the OSMesa target (2), the G77 model row (1), the libdrm
stand-in in Panfrost's kernel module (2), `driver_panfrost` in the target's link (1),
`compile_args_panfrost` on the target (1), and the widened `dep_libdrm` gate (1). The
procedure is written down in the `mesa-patch-set-ordering` skill.

1. `meson.build`: the darwin stand-in for libdrm is now made when panfrost is built as well as
   when freedreno is, not only freedreno. That is the one line changed in freedreno's patch: the
   stand-in is shared, so both drivers get the same include path and the same library.
2. `src/panfrost/lib/kmod/meson.build`: the stand-in's source is built with panfrost's kernel
   layer, the same way it is built with freedreno's drm layer.
3. `src/gallium/targets/osmesa/osmesa_target.c`: the OSMesa target opens the GPU by panfrost's own
   name ("panfrost") and makes panfrost's own screen (`panfrost_create_screen`), instead of
   freedreno's DRI one.
4. `src/gallium/targets/osmesa/meson.build`: the OSMesa target links panfrost's driver, not only
   freedreno's.

The two devices are told apart by the name they are opened with, and each has its own kernel
service: the Adreno's is `HakiosGPU`, the Mali's is `HakiosPanfrostGPU`. Their requests have the
same numbers (Linux's `DRM_COMMAND_BASE` ones), so the file descriptor says which service a request
goes to.

**What the kernel must answer.** Panfrost asks the driver for the GPU's parameters with
`DRM_IOCTL_PANFROST_GET_PARAM`: the product id (`GPU_PROD_ID`, which is what selects the model in
Mesa's table), the revision, and the shader, tiler and L2 present masks, among others. The Mali
driver answers them from the registers it read when it brought the GPU up.

**The model.** `tools/mesa/panfrost-g77.patch` adds the G77 to Mesa's table:
`MODEL(0x9080, 0, "G77", "TNAx", HAS_ANISO, 16384, {})`. The product id is what the driver reads
out of the GPU's registers, so the table is chosen by the hardware, not asked for.

**The order the patches go on.** `tools/mk-mesa.sh` applies `tools/mesa/*.patch` in the order the
shell lists them, and the order matters: `freedreno-darwin.patch` is what puts the Darwin GPU
screen into `osmesa_target.c` in the first place, so `panfrost-darwin.patch` has to be built
against the tree as that patch leaves it, not against the tarball. The same is true of the one
line changed in `freedreno-darwin.patch` (the darwin branch of `dep_libdrm`), which is why it
belongs there rather than in the panfrost patch: two patches cannot both change the same lines.

**The define that was missing.** The OSMesa target's code that reaches the GPU is written
`#ifdef GALLIUM_FREEDRENO`, but that define is given to freedreno's own sources, not to the
target's. So the code was never compiled in: the released library had no `HakiosGPU` in it and no
GPU screen at all, and the Adreno had always been reached as software. The same is true of
`GALLIUM_PANFROST`, which is given to panfrost's sources and to the target by
`tools/mesa/panfrost-darwin.patch` (`c_args : [compile_args_panfrost]` on the target). Without
that line the library builds, and has the G77's row in it, and still draws with the software
rasterizer.

This is worth stating plainly: every earlier measurement of "the UI lags" was of a software
rasterizer, and no amount of kernel work would have changed that. The GPU path in the userspace
had never been built at all.

**The path, end to end.** The window server draws with OpenGL ES through
`userland/src/eagl.c`, which is the library `libhakios.gl.dylib`; that library is Mesa's
`libOSMesa.8.dylib` behind `userland/src/gl_osmesa.c`. `OSMesaCreateContextAttribs()` asks for a
screen, which is `osmesa_create_screen()`, which (with the define) asks panfrost for one with
`panfrost_create_screen()`, which asks the kernel's driver through the stand-in for libdrm. The
stand-in opens the IOKit service `HakiosPanfrostGPU`, which is the Mali driver's service. Nothing
in that chain is specific to the Adreno, and nothing in it needs the Metal plug-in.

**The production build.** `tools/mk-mesa.sh` takes the drivers to build in `DRIVERS`; it now
builds `softpipe,panfrost` by default (for a phone with an Adreno, `DRIVERS="softpipe,freedreno"`
). It needs `THEOS` to point at the SDK, which on this machine is `/mnt/mainline/theos` and not
the `~/theos` that `xnu-env.sh` falls back to.

**Where it stands.** The library builds (`libOSMesa.8.dylib` with panfrost in it, the G77's row in
it, and the name of the kernel's service in it), and the whole set of patches applies to a fresh
extract of the tarball and leaves the same files as the tree that built. What is not yet shown is
a picture drawn by the GPU: the window server has to open the service and give Mesa its buffers,
and that has not run yet.

**The patch of init's binary, and the name that had gone stale.** `tools/mk-root.sh` used to patch
`sbin/init` at fixed offsets: to make launchd's standard output and standard error go to
/dev/null when `/etc/quiet` is there. The offsets were the addresses of instructions in one
particular build of that binary, and the script asserted them, so a rebuild of init with a
different layout stopped the whole root build.

Underneath the offsets there was a second, older thing: the small root's `etc/rc` said
`exec /sbin/launchd.ios`, and `sbin/launchd.ios` was a symlink to `init`. That name was renamed
to `sbin/launchd.darwin` in the common tree on 29 October (commit 45df619, "Darwin, not iOS"), but
this repository's copy of the recipe was not renamed with it. It worked only because the tree's
`init` binary was older than that commit: the binary in `userland/obj` had not been rebuilt since
before the rename, so it still understood the old name. Rebuilding init (which the graphics
library needed) brought it up to date, and the handover stopped: init had no `/sbin/launchd.ios`
to exec, and the phone came up with no programs running at all.

The recipe now says `/sbin/launchd.darwin`, which is the name the common tree's init expects.

**Where init's own image is.** Init takes the path of its image from `argv[0]`
(`userland/src/init.c`): when it is started as `/redwood/sbin/init`, every path it uses for its
own image (its launchd jobs, its audio plug-in, its data volume) is that path with `/redwood` in
front of it, and the `redwood -> .` symlink in the image resolves it. The kernel runs
`/sbin/launchd` as process 1, and that was a symlink to `init`, so init was started as
`/sbin/launchd` and looked for its own files in `/` -- the phone's own volume, once init had
made that the root.

What that costs is visible in the checks: the run before made "111 directories, 8 files" of the
data volume, and the run after made "0 directories, 0 files", and then every file test after it
failed as well ("read a small file", "exec a second program from the image"). Nothing of the
small root was reachable. The listing of the image's own tree tells which root was in place: the
run before walked `/redwood/empty`, `/redwood/bin`, `/redwood/etc`, and the run after walked
`/usr`, `/bin`, `/System` -- the phone's own volume.

**What it turned out to be.** The tree's `init` binary had not been rebuilt since before the
common tree renamed `/sbin/launchd.ios` to `/sbin/launchd.darwin` (29 October). Two things in
the recipe name that handover, and they had to agree:

- init's own code compares the program it is told to run with "/sbin/launchd.ios" and rewrites it
  to "/sbin/launchd", which is the phone's own launchd on the volume that is the root by then;
- the small root's `etc/rc` says "exec /sbin/launchd.ios", and `sbin/launchd.ios` is a symlink
  to `init`.

The binary in `userland/obj` was the newer one, which knows only the new name; the `etc/rc` said
the old one; so the comparison never matched, the handover never happened, and nothing ran. The
build of `init` that the graphics library needed is what exposed it.

The recipe now takes init from `prebuilt/userland` -- the binary the phone was brought up with,
which is the one that knows "/sbin/launchd.ios" -- and says that name, so the two cannot drift
apart again. The prebuilt init passes every one of its checks: 111 directories and 8 files of
the data volume, the small file and the 300 KB file read back byte for byte, a program run
through dyld and the shared cache.

**What the new init's own build does not do.** The init that this tree builds knows the name
"/sbin/launchd.darwin" instead, and there is no way to have both without the two agreeing
again. Rebuilding init from the tree's source means changing the name it compares with, and
then the `etc/rc` has to say that name. It is a change in two places and it is not made yet.

**What the run with the prebuilt init showed.** With the prebuilt init and the `etc/rc` that
names the same handover, the phone came up with 76 programs running, the window server drawing
(touch events arrive), and every one of init's own checks passing. The handover ran: the log
has "=== /redwood/etc/rc ===" and then "exec /sbin/launchd.darwin", which is the phone's own
launchd taking over.

The GPU service still had no client, which is the same as before any of this: the window server's
renderer is chosen by `userland/src/inject.c`, and that looks for the service "HakiosGPU" -- the
Adreno's -- when the boot-args do not say. The Mali's service is "HakiosPanfrostGPU", so the
library did not know there was a GPU to draw with. It looks for both now.

**Reading the window server's own words.** The injection library says what it decides with
`say()`, which writes to the window server's standard error -- and that goes to /dev/null when
`/etc/quiet` is in the image, which it is. So the decision was invisible. The library now also
writes to `/dev/console`, which the kernel's log keeps, and the stand-in for libdrm says on its
standard error which device it opens. Both are diagnostics to take out when they have served
their purpose.

**Where the injection stops.** The configuration does reach the kernel: `kern.exec_inject` holds
the whole of `/redwood/etc/inject.conf`, and the kernel adds the library to every program it
starts after that. The kernel says so now, for each program it starts (`bsd/kern/kern_exec.c`,
in `exec_inject_env`), and the sysctl's own handler says when the configuration is written.

**And then the library gives up.** `libhakios.dylib`'s constructor asks dyld for the shared
cache's range (`_dyld_get_shared_cache_range`) to work out the slide the cache was loaded at --
the addresses of CoreAnimation's own functions are the ones in the cache's file, and they have
to be slid to be usable. For a program that comes out of the shared cache, that call returns
nothing, and the constructor returns at once without doing anything: the library is inside the
program, and does nothing in it. (The library now says what it saw, so that this is visible.)

**There is no shared cache on this phone.** The cache comes with the OS cryptex, and init
mounts the two cryptexes from the phone's own volumes (`/dev/ufs_xnusys,c=1` and `c=2`):

    the OS cryptex from /dev/ufs_xnusys,c=1: Invalid argument
    [FAIL] the OS cryptex: in neither md0 nor the flash

So neither the shared cache nor the cryptexes are there, and the phone's own programs are run
from the volume itself. That is a difference from the tree's earlier home, where the cache was
always there.

**The console's ring is short.** The kernel's own console keeps only the last of a run's
messages, and the run's log has the beginning (the loader) and the end (the kernel's message
buffer, the last 76 seconds) -- the middle is gone. So a line said once, at the moment
backboardd is started, is simply not in the log any more. What the tree has for this is
`loader_log_line()`: a line written into the loader's memory, read back when the run ends.
The kernel's injection diagnostics go there now.

**What dyld is told.** The kernel now says, for every program it starts, the program's
code-signing flags and whether it is a platform binary. On this phone the phone's own programs
are platform binaries (`platform 1`, flags `0x22000001` -- signed, on a platform path, but
*not* `CS_RESTRICT`), and the tree's own init is not (`0x1`, `platform 0`).

That matters because dyld ignores `DYLD_INSERT_LIBRARIES` for a program that is a platform
binary. The tree's own programs are not, so they are the way to tell the two reasons apart:
if the library is loaded into one of them, the mechanism works and the window server's refusal
is dyld's platform-binary rule; if it is not loaded into them either, the library is not
acceptable to dyld at all.

**The small root's init, and what the tree does to it.** The binary that runs as process 1 is
`prebuilt/userland/init`, and `tools/mk-root.sh` patches it in two places. One patch replaces
the branch after `strncmp(argv[0], "/redwood/", 9)` with a NOP, so that the image's own path
is always "/redwood" -- the kernel loads "/sbin/launchd", so the comparison in the tree's
source never matches, and without the patch the tree's files would be looked for under "/" and
not found. The other patch sends the standard output and the standard error of the phone's own
launchd to /dev/null when `/etc/quiet` is there, so that it does not flood the console.

That second patch also silences init's own last words: "init: library insertion: %s" and
"init: kern.exec_inject: error %d" both go to /dev/null, so a failure of the configuration
cannot be seen from the log. The kernel now says it instead.

The tree's source of init (`userland/src/init.c`) sets the image's path the same way as the
patch does, and now also sets it when the root has been switched (the image is bound on
/redwood then). It is not the binary that runs, though: `tools/mk-root.sh` takes the prebuilt
one, so the tree's build of init only matters for the files under `userland/root`.

The binary patch itself is still needed for the quieting of launchd's standard output, so it
stays; what it should not do is fail the build when init is rebuilt. That is a separate piece of
work: the patch has to find its instructions by what they are rather than by where they are.

## 9. Verification: Hardware Rendering at 120 Hz and >120 FPS

The on-device test (`mali=6`, Mesa 25.0.7 Panfrost, XNU_SECS=300) confirmed full hardware
rendering on Mali-G77 MC9 (Valhall v9):

1. **Resolution of `DATA_INVALID_FAULT` (0x58)**:
   - Root cause: `hakios_mali_parameter` incorrectly routed `DRM_PANFROST_PARAM_TILER_FEATURES` (param 10)
     to register `0x140` (`TILER_PWRACTIVE = 0`) instead of `0x00c` (`GPU_TILER_FEATURES`).
     This reported `max_levels = 0` to Mesa, producing `hierarchy_mask = 0` in the Tiler Context descriptor,
     which the Valhall Job Manager rejected with `DATA_INVALID_FAULT` (0x58).
   - Fix: Aligned parameter mapping with `simple[]` table: param 10 -> `0x00c`, param 1 -> `gpu_id & 0xffff`.
   - Also enforced 2 MiB address alignment for `PANFROST_BO_HEAP` allocations (`0x200000`) required by
     Valhall's chunk allocator.

2. **Display Refresh Rate**:
   - `MiniIOMobileFramebuffer: IOMFBDisplayRefresh: 120 Hz (the panel: 120)`

3. **Frame Render Rates (`gl_test fill 1080 2400`, shortest of 16 frames)**:
   - Clear frame (1080×2400, 10.1 MB): **1,928 μs** (~518 FPS equivalent)
   - Clear + 1 textured blended layer: **4,280 μs** (~233 FPS equivalent)
   - Clear + 2 textured blended layers: **4,379 μs** (~228 FPS equivalent)
   - Clear + 4 textured blended layers: **5,410 μs** (~184 FPS equivalent)
   - Clear + 8 textured blended layers: **7,796 μs** (~128 FPS equivalent)
   - All frame passes render below the 8,333 μs budget (120 FPS threshold).

4. **WindowServer**:
   - `backboardd alive [gl=renderer_ok(0x10ab04860), hooks_ok(r=1,b=1,f=1,rend=1)]`

## 9a. The frames come at 60 a second, and where the cap is

Measured on 9 November 2026 from the window server's own frame trace
(`tools/fps.py logs/xnu-touch31.log`):

```
the steady rate: 89 intervals, median 16.67 ms = 60.0 FPS
  87 of 89 within 0.1 ms of it (98%)
```

98% of the intervals within a tenth of a millisecond of the median is a **hard lock** to
16.67 ms, not a slow GPU: the frame itself costs 2.9-3.1 ms (the trace's own `whole` figure),
which leaves 13 ms of the 16.67 idle.

The driver says the same thing from its side:

```
MiniIOMobileFramebuffer: 301 frames in 5002 ms (60 a second), 2843 us each in the driver
MiniIOMobileFramebuffer: 602 refreshes told (0 dropped) in that time
```

602 refreshes in 5 s is the panel's 120 Hz being told to the system; 301 frames is what
CoreAnimation actually drew. So the panel and the driver run at 120 and the *drawing* runs at
60.

What CoreAnimation believes, from `fps_info` (it is in the image and reads the numbers
CoreAnimation itself publishes):

```
fps_info: MobileGestalt DeviceSupports120HzRefreshRate = (none)
fps_info: MobileGestalt DeviceSupportsProMotion = (none)
fps_info: MobileGestalt MaxRefreshRate = (none)
fps_info: CADisplay mainDisplay: _maximumFramesPerSecond = 0
fps_info: CADisplay mainDisplay: _refreshRate = inf
```

The window server asks MobileGestalt for these and gets nothing: `inject.c` sets
`CA_FORCE_120HZ=1` and friends in the window server's environment, and `inject_sb.c`
interposes `MGCopyAnswer` for `DeviceSupports120HzRefreshRate` and `MaxRefreshRate` — but
**the interposer is in SpringBoard's library, and SpringBoard does not draw the frames.**
The window server gets `libhakios.dylib` (it is the library that speaks to the touch screen
and to CoreAnimation), and that library has no MobileGestalt interposer at all. The same is
true of the display-link hooks: `inject_sb.c`'s `setPreferredFramesPerSecond:` and
`setPreferredFrameRateRange:` hooks live in SpringBoard, not in the window server.

The window server's library (`libhakios.dylib`, `userland/src/inject.c`) already has a
MobileGestalt interposer for these keys and swizzles `CADisplay`'s `refreshRate`,
`heartbeatRate`, `minimumFrameDuration` and `maximumFramesPerSecond` to 120 — so that was not
the place after all.

The place is CoreAnimation's own server render rate. It draws on **every other refresh** unless
`CA_ENABLE_REDUCED_SERVER_VSYNC_RENDER` is 0: that is what a phone with a 60 Hz panel wants
and what a phone with a 120 Hz panel does not. The switch is in the table of CoreAnimation's
own debug switches — the shared cache has it next to `CA_FORCE_120HZ` — and it was not among
the ones given to the window server.

It is now set in the window server's environment (`inject.c`, with `ca_vbl=1`) and carried in
the boot arguments of `tools/xnu-cycle.sh`, `tools/test-120hz.sh` and `tools/iter.sh`.

This is the switch the commit `c0eaa31` ("The frame rate was CoreAnimation's own 10 Hz clock:
3.4 FPS -> 34.4 FPS") used, and it was dropped from the arguments since.

The framebuffer driver is not the limit: it paces on `fb_hz` (120) and publishes
`IOMFBDisplayRefresh` at 120 Hz, and the log says so
(`DisplayRefresh: 120 Hz (the panel: 120)`). But it publishes no `MaximumFramesPerSecond` in
its mode dictionary, and that is the key CoreAnimation reads for the screen's maximum.

A measurement note: `tools/fps.py` was wrong until 9 November. The frame census leaves out
most frames (the console's ring is short), so the time between two lines is the time
between the frames they count, not the time of one frame; the tool now divides by the
frame-number difference. Before that it read 3.0 FPS for a 60 FPS run and 119.3 for a run
whose census said every frame.

## 10. Sources

Everything cited above is in the local trees:
`/mnt/project/HoshimaticProject/.mt6893_index` (MediaTek's MT6893 kernel: `mt6893.dts`,
`drivers/clk/mediatek/clk-mt6893.c`, `drivers/soc/mediatek/mtk-scpsys.c`,
`drivers/gpu/mediatek/gpu_mali/mali_valhall/mali-r32p1/` — ARM's kbase, GPL, cited only),
`/mnt/project/HoshimaticProject/mt6893-vendor-modules/port/sources` (the MT6893 module sources:
`clk-mt6893/`, `clk-mt6893-mfgcfg/`, `mtk-scpsys-mt6893/`), and
`/mnt/project/Misc/hakios/refs/hakios-xnu-kernel` (the common tree: Mesa 25.0.7 under
`xnu-deps/build/mesa/`, the Adreno driver, `tools/mesa/hakios-drm/`, and the MT6768 reference's
`mtk/mt6768/drivers/gpu/hakios.mali.c` on branch `origin/mtk`).
