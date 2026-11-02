#!/usr/bin/env python3
"""
The XNU kernel this phone runs: the common tree's prebuilt with the MT6893's own patches.

The common tree ships prebuilt/hakios-kernel, built for the board it was written for. The
phone needs a different UFS controller, a different set of LUNs, and a code-signing check that
accepts a 16 KB page shift, so the kernel that runs here is that prebuilt with a handful of
instructions changed. The changes are listed below as the bytes that are looked for and the
bytes that replace them, so they can be applied to a freshly built kernel: a build moves every
offset, but the instructions it produces are the same.

The result was verified against the kernel this repository runs: applying this to
prebuilt/hakios-kernel reproduces mach.development.mt6893 byte for byte.

    usage: patch-kernel-mt6893.py <kernel-in> <kernel-out>

Each patch is one entry: what it is for, the bytes to find, the bytes to put there. The bytes
to find have to be in the file exactly once, or the script stops and says so.
"""
import sys

PATCHES = [
	# _proc_set_task_ruse_cpu: the printf of the task's rusage, which this kernel does not
	# have the data for, is a nop.
	("proc_set_task_ruse_cpu: no printf",
	 "a0eafff000482a917859ff9779521039781602f9",
	 "a0eafff000482a911f2003d579521039781602f9"),

	# _ufs_native_map: the controller is mapped at the MT6893's own address, 0x11270000.
	("ufshc_probe: the UFS controller's address",
	 "e8038052a88314b815008852153ba072e0dfff9000643091",
	 "e8038052a88314b8f524a2521f2003d5e0dfff9000643091"),
	("ufshc_probe: the UFS controller's address (the map)",
	 "004c3391daffff1700008852003ba072010086525163fe97",
	 "004c3391daffff17e024a2521f2003d5010086525163fe97"),

	# _ufs_native_map: the branch that asks for the LUN list is unconditional here.
	("ufs_native_map: the LUN list is always read",
	 "e08301ad48c34ab9480600341a0080521b0080d2",
	 "e08301ad48c34ab9320000141a0080521b0080d2"),

	# _ufs_native_map: every LUN the phone has (0 to 2), not the one the prebuilt knew.
	("ufs_native_map: the LUNs",
	 "c3faff54030000141b0080d21a008052e0031aaaff",
	 "c3faff5403000014fb0080d2fa008052e0031aaaff"),
	("ufs_native_map: the LUN mask",
	 "82ecf897d60a0035bb0a00b4130080d2170080d2",
	 "82ecf897d60a003556000014130080d2170080d2"),

	# _hc_uefi_slot0: the prebuilt asks the loader for the slot it read the UEFI from; this
	# phone has none, so the lookup returns nothing instead of dereferencing it.
	("hc_uefi_slot0: no slot to look at",
	 "34ffff17a3b41994485100d0082942f9091540b90a1140b9",
	 "34ffff17a3b41994000080d2c0035fd6091540b90a1140b9"),

	# _ramfs_mount and _ramfs_store: the ramfs is this kernel's root, and it is filled by
	# the loader, not by the kernel's own writer.
	("ramfs_mount: the root is not written here",
	 "e922c91a297d40930809c99a681e00f9882e00f0",
	 "e922c91a297d40938800a0d2681e00f9882e00f0"),
	("ramfs_store: the root is not written here",
	 "892b43a93f010aeb82040054087b3bf8ff0200f9",
	 "892b43a93f010aeb1f2003d5087b3bf8ff0200f9"),

	# _cs_validate_hash: the code-signing check of the kernel's own binaries. The phone's
	# dyld shared cache is 16 KB aligned and the check as it stands rejects it, so the
	# check is replaced by one that accepts the page size it is given.
	("cs_validate_hash: a 16 KB page size is accepted",
	 "ff830191c0035fd6fc6fbaa9fa6701a9f85f02a9f65703a9f44f04a9fd7b05a9fd430191ffc30ad1f30305aa",
	 "ff830191c0035fd608008252440000b4880000f9450000b4bf0000b920008052c0035fd6ffc30ad1f30305aa"),
	("cs_validate_codedirectory: the page shift of the shared cache",
	 "290140b91f3100712411487a81060054089440",
	 "290140b91f31007104194e7a81060054089440"),

	# The window server's IOSurface and framebuffer paths: two instructions that the
	# prebuilt's board does not need and this phone does.
	("IOSurfaceRootUserClient: one flag",
	 "fff0087d2891f30300aae00308aaba9efe",
	 "fff0087d2891f3031faae00308aaba9efe"),
	("IOMobileFramebuffer::show: the check that is not done here",
	 "01f9ff54c4ffff1797f6ffb5e86340f948f60736",
	 "01f9ff54c4ffff17b4ffff17e86340f948f60736"),
]


def patch(kernel):
	"""The kernel with this phone's patches in it. Every pattern must be found once."""
	out = bytearray(kernel)
	for what, old_hex, new_hex in PATCHES:
		old, new = bytes.fromhex(old_hex), bytes.fromhex(new_hex)
		found = []
		at = 0
		while True:
			at = out.find(old, at)
			if at < 0:
				break
			found.append(at)
			at += 1
		if len(found) != 1:
			raise SystemExit("patch-kernel: %s: the pattern is at %d places (%s), expected one"
			                 % (what, len(found), ", ".join(hex(f) for f in found)))
		out[found[0]:found[0] + len(old)] = new
	return bytes(out)


def main():
	if len(sys.argv) != 3:
		raise SystemExit("usage: %s <kernel-in> <kernel-out>" % sys.argv[0])
	with open(sys.argv[1], "rb") as f:
		kernel = f.read()
	patched = patch(kernel)
	with open(sys.argv[2], "wb") as f:
		f.write(patched)
	changed = sum(1 for a, b in zip(kernel, patched) if a != b)
	print("patch-kernel: %d patches, %d bytes differ" % (len(PATCHES), changed))


if __name__ == "__main__":
	main()
