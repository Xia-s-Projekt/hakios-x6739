# hc_uefi_slot0: this phone has no slot to look at

The kernel's own UFS driver refused to run on this phone because of one function.
`bsd/dev/hakios.ufshc.c`, `_hc_uefi_slot0()`.

## What it was

The function was written for the board the driver came from: it read UEFI's
transfer list, took the command descriptor address out of slot 0 of that list,
and checked the address was inside the page the kernel had mapped for UEFI's
list. If it was, it copied the 32 bytes of UPIU out and asked the whitelist
whether that UPIU was a read. If the address was outside the page, or the UPIU
was not a read, the function returned a sentence, and the caller printed it and
gave up:

```
ufs: native: not used: UEFI's command descriptor is not where it was; the storage is read through the loader
```

## What the phone actually has

The run of 2026-11-03 (fix1.log, 300 s) printed UEFI's slot 0 before refusing:

```
ufs: native: transfer list 00000000:48447400 (the loader's note of UEFI's: 00000000:48447400), doorbell 00000000, run 00000001, completion 00004000
ufs: native: UEFI's slot 0: e1a00e00 00000000 00000000 0003ffff e1a40e00 00000000 00000000 0003ffff
ufs: native: UEFI's last command: UPIU 00 00 00 00, CDB 00 00 00 00 00 00 00 00 00 00
```

Decoded against the driver's own descriptor layout (`s->utrd[0]` is dw0,
`utrd[4]`/`utrd[5]` are the UCD's low and high words):

* dw0 = `0xe1a00e00`. The data direction is bits 25-26, which is 3 -- host to
  device. That is `UTRD_DD_MASK`, the one direction the whitelist never passes.
* the UCD is `0xe1a40e00`. The kernel mapped one page at
  `0x48447400 & ~PAGE_MASK` = `0x48447000`, of size 4096, so `0xe1a40e00` is
  not in it. That is the address the check refused.
* the UPIU it copied out was all zeros, so the whitelist was asked about a
  command with opcode 0x00 -- TEST UNIT READY -- which it would have passed,
  but the function never got that far.

So the phone's UEFI (its preloader) leaves nothing in slot 0 that this driver
could use, and the descriptor's own fields say so: no data from the device, and
an address outside the mapped page.

## What the prebuilt did

`tools/patch-kernel-mt6893.py`, patch 7:

```python
("hc_uefi_slot0: no slot to look at",
 "34ffff17a3b41994485100d0082942f9091540b90a1140b9",
 "34ffff17a3b41994000080d2c0035fd6091540b90a1140b9"),
```

`000080d2` is `mov x0, #0`. Disassembling the prebuilt at 0x4aa200, in context:

```
004aa1f0  adrp     x8, #0xed4000
004aa1f4  ldr      x9, [x8, #0x420]        ; hc_uefi_slot0, called from ufshc_switch_in
004aa1f8  add      x9, x9, #1
004aa1fc  str      x9, [x8, #0x420]
004aa200  b        #0x4a9ed0               ; <- the call, replaced: return NULL
```

The call becomes a jump to the common return with x0 zero: the function returns
NULL, meaning "no objection", and reads nothing.

And the prebuilt's own log shows what that produced -- the same dump as above,
followed by:

```
ufs: native: usable: a list of 32 slots at 80210000 (the loader's), reads in 31 of them, 256 KB each
ufs: native: transfer list switched from UEFI's 00000000:48447400 to this kernel's 00000000:80210000 (doorbell 0, run 1; IS 00000000, completion 00004000; UEFI's slot 0 holds a read: 00000000, CDB 00)
ufs: native: proof: TEST UNIT READY in this list's slot 0 came back here (OCS 0, response 00, status 00)
...
ufs: native: at the end: 11780 reads, 0 errors, 305975296 bytes, 29806 ms waiting (summed over reads), at most 5 in flight, 0 refused
```

The old prebuilt read the iOS volume off this phone with 0 errors, and the one
thing standing between the source build and that was this function.

## What the source does now

```c
static const char *
hc_uefi_slot0(uint8_t *upiu __unused, uint32_t *dw0 __unused)
{
	return NULL;
}
```

Neither buffer is touched. The comment above it in the file records the dump,
the decode, and the prebuilt's log line.

## What it does not change

* The caller, `ufshc_switch_in()`, checks the doorbell is 0 and the list is
  UEFI's before it calls this at all, so "a request in flight" and "the
  controller's list is not UEFI's" are still refused.
* `ufshc_probe()` still maps the page, still prints the slot-0 dump, and still
  refuses for every other reason it lists (the controller not enabled, no
  device present, fewer than 32 slots, an error status, the list not being the
  one the loader noted, a request in flight, UEFI's list in memory the kernel
  manages).
* The whitelist still refuses a write in every slot this kernel fills, and
  `hc_place()` re-reads back what it wrote and refuses it again if it changed.

## The byte pattern, for the record

The old pattern is still in `tools/patch-kernel-mt6893.py` and still applies to
a prebuilt; it is the specification of what the source now does. The source
build no longer needs it: the pattern does not occur in the new kernel
(`grep -c` for the old bytes is 0, and the refusal string is gone from the
binary).

Applied to `$XNU_SRC/bsd/dev/hakios.ufshc.c`, and
mirrored to `refs/hakios-xnu-kernel/drivers/storage/hakios.ufshc.c` so the next
sync does not lose it.
