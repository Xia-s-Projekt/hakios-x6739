# The kernel's own source, changed for this phone

The kernel the phone runs was made by patching a prebuilt binary (13 patches,
tools/patch-kernel-mt6893.py). Those patches do not survive a source build:
only two of their thirteen byte patterns match what a compiler produces now,
so the changes have to be made in the source instead.

Two of them are done. They are here as the record of what was changed and why,
and they apply to the tree in ../refs/xnu-src/xnu-8796.121.2 with
`patch -p1`.

## code-signing-16k.patch

`bsd/kern/ubc_subr.c`: the shared cache is 16 KB aligned, and the real page
hash check rejects it. `cs_validate_hash()` is stubbed out to accept the page
without comparing its hash, and `cs_validate_codedirectory()` is made to accept
a page shift of 14 (16 KB) beside the 12 (4 KB) it already took.

## ufs-lun.patch

`bsd/dev/hakios.ufs.c`: the phone's boot volume is not on LUN 0. The driver
took the unit the loader lists as LUN 0 and refused anything else, and the
mask of the LUNs its reads may go to was too narrow. Now:

  - the mask is 0x07 (the phone's eight logical units, read as 0 to 2), and
  - the LUN a unit is on is the one whose block 1 (the GPT header, with the
    unit's own GUID in it) reads the same through the loader as it does here,
    which need not be LUN 0.

The unit's own number is still what identifies it; the LUN is looked up. The
proof each unit is on the LUN it is read from is what the driver already
printed, and now it prints the LUN it found rather than the one it assumed.

## What is left

The other eleven are not done. Their sites are:

  - osfmk/kern/task_policy.c: the CPU monitor's warning
  - bsd/dev/hakios.ufshc.c: the UFS host controller's addresses and probe
  - bsd/miscfs/ramfs/ramfs.c: the ramfs mount and store
  - bsd/kern/ubc_subr.c: the code-signing hash validation (partly done above)
  - xnu/files/iokit/Kernel/MiniIOSurface.cpp
  - xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp

`origin/mtk` in the common tree carries the last two for the MT6768; they are
the tree's own, and the MT6893's numbers have to be put into them.
