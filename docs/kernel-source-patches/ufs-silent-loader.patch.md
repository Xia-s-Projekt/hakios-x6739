# `ufs_native_map`: a silent loader no longer stops the LUN search

`bsd/dev/hakios.ufs.c`, `ufs_native_map()`.

## What the phone did

Run of 2026-11-03 08:35, 120 s, `cpus=4 touchscreen=1 touch_lat=1 gl=1`:

```
ufs: native: unit2: no GPT header to know it by, from the loader: the LUNs are read here instead
ufs: native: transfer list switched from UEFI's 00000000:48447400 to this kernel's 00000000:80210000
ufs: native: proof: TEST UNIT READY in this list's slot 0 came back here (OCS 0, response 00, status 00)
ufs: native: LUN 2 holds a GPT header of its own: it is read here
ufs: the partitions (on 3 logical unit(s) of the phone's) are read by this kernel's own UFS driver, its list in the controller
apfs: volume 0 "System" of container 0 (at 0 MB): 45044 files, 16547 directories, 601698 blocks of 4096 used
[ ok ] mount the iOS volume read only
ufs: native: at the end: 37259 reads, 0 errors, 971898880 bytes, 5762 ms waiting, at most 7 in flight
```

Before the fallback patch:

```
ufs: native: usable: a list of 32 slots at 80210000 (the loader's), reads in 31 of them, 256 KB each
ufs: no answer from the loader to request 1 in 20 s: the storage is off from now on
ufs: native: unit2: no GPT header to know it by (block size 4096)
ufs: this kernel's own UFS driver is not used: through the loader
[FAIL] mount the iOS volume read only
```

## Why the loader's silence was fatal, and why it need not be

`ufs_native_map()` compares block 1 of every unit the loader lists, read **through the loader**
(`ufs_read_mailbox`), against block 1 of each LUN read **here**. A unit is on the LUN whose
block 1 matches.

This phone's loader never answers a read. `mtkloader/mtkloader.c` publishes the shared page
(`ufs_setup_shared()`, line 682) with `ask` and `answer` at fixed offsets 128 and 256, but
nothing ever looks at `ask.request` or writes `answer.answered` —
`grep -c 'ask.request' mtkloader/mtkloader.c` is 0. So the read times out after
`UFS_TIMEOUT_MS` (20 s), and `ufs_request()` then sets `ufs_dead`, which makes
`ufs_native_in()` fail immediately (`ufs_dead ? EIO : ufshc_switch_in()`, line 542).

The loader cannot be made to serve it: `jump_to_kernel()` (`mtkloader.c:3201`, `start.S:67`)
ends in `eret` into XNU at EL1 and never returns to C, so core 0 is gone. The reference
loader's `block_poll()` (`refs/hakios-xnu-kernel/xnuloader/loader.c:2128`) serves reads
through **UEFI's own Block I/O**, which is not present here.

But the loader's copy of block 1 is only a way of telling *which* LUN a unit is on. It is not
a precondition of this kernel's own driver working, and the native driver is already proved
(the TEST UNIT READY in this list's slot 0 comes back here). So the mapping can be reached the
other way.

## What changed

1. `ufs_dead = false` before the switch-in, so a silent loader cannot poison the native path.
2. The `e->block_size != 4096` check is split out of the loader-read condition: a unit with
   blocks of another size is still a hard failure, reported as such.
3. If the loader gives no copy of block 1 that starts "EFI PART", that is no longer a failure.
   It is reported once and the LUNs themselves become the search.
4. The search: block 1 of each LUN in `UFS_LUN_MASK` is read natively, and a LUN is kept if its
   header parses as a GPT header — the signature, sane `entries_lba`/table sizes, and the
   header's own CRC32 over its bytes (`crc32(0, h, hsize)` with `hsize` from the flash, as
   `ufs_native_check()` already does at line 1559).
5. A unit is then on the LUN of its own number, which is what the working prebuilt assumed,
   with the extra check here that that LUN was really read and really holds a header.
6. The loader-comparison loop is skipped when there was no loader copy to compare against.

## What the prebuilt actually did

Disassembled from `refs/hakios-xnu-kernel/prebuilt/hakios-kernel`
(`_ufs_native_map` at `0xfffffff0074aa4ac`–`0xfffffff0074aa8d8`, `__TEXT_EXEC` at
`0xfffffff007294000` / file `0x290000`):

* It **does** ask the loader for block 1 first — the call at `0xfffffff0074aa60c` is the read,
  with `0xfffffff0074aba10` behind it, and `0xfffffff0074aa618` the `memcmp` against "EFI PART".
* Its `mask` is built from the units' own numbers (`1u << e->unit`), not a fixed mask — which
  is why the reference port's `hakios.ufs.c` and ours agree on `mask` and disagree nowhere.
* Its final loop, at `0xfffffff0074aa828`, stores `e->unit` straight into `unit_lun[e->unit]`
  with **nothing read** — no comparison, no search.

The source tree's version had replaced that last step with a strict comparison against the
loader's copy. Diffing `refs/hakios-xnu-kernel/drivers/storage/hakios.ufs.c` (the reference
port's, identical to the prebuilt's source) against
`$XNU_SRC/bsd/dev/hakios.ufs.c` shows it: our tree added the LUN
search and the `unit_lun[]` bookkeeping, and dropped the prebuilt's "the unit's own number is
the LUN" assumption. On a loader that answers, both work. On a loader that does not, only the
prebuilt's shape does — which is the whole of this patch.

## The remaining cost

The 20 s wait still happens: the loader is asked once, before the fallback, and that ask is
what prints `no answer from the loader to request 1 in 20 s`. It costs nothing now (the native
path is already switched in by the time the mount runs), but it is dead weight worth removing:
either the loader's page should say up front that it cannot serve reads, or
`ufs_native_map` should try the native LUN search first and use the loader only as a
cross-check when it does answer.
