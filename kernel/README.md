# kernel/

This phone's drivers in Apple's XNU (xnu-8796.121.2), as files of the kernel tree (the paths below
are the tree's). Licensed under the Apple Public Source License 2.0 (`../LICENSE.APSL`), as code
built into XNU.

**Nothing here yet.** The reference MediaTek port (`hakios-mt6768`) carries these files for the
MT6768 board:

| file | what | boot-arg |
| --- | --- | --- |
| `bsd/dev/redwood_emmc.c`, `bsd/sys/redwood_emmc.h` | the eMMC's `xnusys` partition as a read-only block device and the root | `emmc_root=1` |
| `bsd/dev/redwood_tcm.c` | the touch screen (TD4160, TCM protocol over SPI2) | `touchscreen=2` |
| `bsd/dev/redwood_mtkkeys.c` | the buttons; the big cores' clock | `buttons=2`, `mtk_cpufreq=` |
| `bsd/dev/redwood_mali.c` | the Mali-G52 brought up: power, clocks, MMU, jobs | `mali=1` |
| `iokit/Kernel/RedwoodMali.cpp` | the GPU for programs: panfrost's DRM interface as an IOKit user client | `mali=1` |
| `bsd/dev/redwood_scp.c` | the SCP sensor hub: accelerometer, `kern.redwood.accel` | `scp_step=7` |
| `bsd/dev/redwood_connsys.c` | CONNSYS (Wi-Fi/Bluetooth): power, firmware, BTIF, STP/WMT | `connsys_step=1..8` |

**None of them is this phone's.** Every address in them is the MT6768 board's, and the hardware is
different: eMMC against UFS, a TCM touch controller against whatever this phone has, a Mali-G52
against a Mali-G77. They are here as a record of the *shape* of each driver, not as code to copy
wholesale.

What this phone needs, in the order of [../docs/port-plan.md](../docs/port-plan.md):

1. a **UFS** driver (the reference's is eMMC), which is the largest piece of new code in the port;
2. the **touch** controller (the reference's is an OmniVision TD4160 over SPI2);
3. the **GPU** bring-up for Mali-G77 MC9;
4. the **buttons** and the big cores' clock for this board;
5. **CONNSYS**/Wi-Fi, the **SCP** sensor hub, audio and the battery.

They are not a kernel on their own: they hook into the common kernel (its `bsd/conf/files`, the
input device `/dev/hakios-touch`, the display driver, `IOFindBSDRoot` for `emmc_root=1`), which
lives in the common repository (`hakios-xnu-kernel`).
