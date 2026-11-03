#include <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <errno.h>
#include <sys/mount.h>
#include <execinfo.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern const void *_dyld_get_shared_cache_range(size_t *length);
extern kern_return_t mach_vm_protect(vm_map_t task, mach_vm_address_t address,
    mach_vm_size_t size, boolean_t maximum, vm_prot_t protection);

static uintptr_t slide;
static BOOL (*original_database_error)(id, SEL, long, long, id, id);

static BOOL diagnostic_database_error(id self, SEL selector, long error, long sqlite_error, id query, id on_error)
{
	NSString *filename = [self valueForKey:@"filename"];
	fprintf(stderr, "usertrap: SpringBoard-sqlite: error %ld sqlite %ld path %s query %s\n",
	    error, sqlite_error, [filename UTF8String], [[query description] UTF8String]);
	struct statfs fs;
	if (statfs([filename fileSystemRepresentation], &fs) == 0) {
		fprintf(stderr, "usertrap: SpringBoard-sqlite: blocks %llu free %llu available %llu block-size %u\n",
		    (unsigned long long)fs.f_blocks, (unsigned long long)fs.f_bfree,
		    (unsigned long long)fs.f_bavail, fs.f_bsize);
	} else {
		fprintf(stderr, "usertrap: SpringBoard-sqlite: statfs errno %d\n", errno);
	}
	fflush(stderr);
	return original_database_error(self, selector, error, sqlite_error, query, on_error);
}

__attribute__((naked, noreturn)) static void original_exit(int status __attribute__((unused)))
{
	__asm__ volatile("mov x16, #1\nsvc #0x80\nbrk #0\n");
}

__attribute__((noreturn)) static void diagnostic_exit(int status)
{
	void *frames[32];
	int count = backtrace(frames, 32);
	fprintf(stderr, "usertrap: SpringBoard-exit: status %d slide 0x%llx\n",
	    status, (unsigned long long)slide);
	for (int i = 0; i < count; i++) {
		fprintf(stderr, "usertrap: SpringBoard-exit: frame %d unslid 0x%llx\n",
		    i, (unsigned long long)((uintptr_t)frames[i] - slide));
	}
	fflush(stderr);
	original_exit(status);
}

__attribute__((constructor)) static void diagnostic_init(void)
{
	if (strcmp(getprogname(), "SpringBoard") != 0) {
		return;
	}
	struct statfs fs;
	if (statfs("/private/var", &fs) == 0) {
		fprintf(stderr, "usertrap: SpringBoard-sqlite: var-capacity %llu bytes free %llu bytes\n",
		    (unsigned long long)fs.f_blocks * fs.f_bsize,
		    (unsigned long long)fs.f_bavail * fs.f_bsize);
	}
	size_t length = 0;
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - 0x180000000ULL;
	Method method = class_getInstanceMethod(objc_getClass("_PASSqliteDatabase"),
	    sel_registerName("handleError:sqliteError:forQuery:onError:"));
	if (method && (uintptr_t)method_getImplementation(method) == slide + 0x1a03d9948ULL) {
		original_database_error = (void *)method_setImplementation(method, (IMP)diagnostic_database_error);
		fprintf(stderr, "usertrap: SpringBoard-sqlite: installed\n");
	} else {
		fprintf(stderr, "usertrap: SpringBoard-sqlite: unexpected database method\n");
	}
	uint32_t *target = (uint32_t *)(slide + 0x1c18584c0ULL);
	const uint32_t expected[] = { 0xd2800030, 0xd4001001, 0x540000e3, 0xa9bf7bfd };
	if (memcmp(target, expected, sizeof(expected)) != 0) {
		fprintf(stderr, "usertrap: SpringBoard-exit: unexpected exit prologue\n");
		return;
	}
	vm_address_t page = (vm_address_t)target & ~(vm_address_t)(vm_page_size - 1);
	kern_return_t kr = mach_vm_protect(mach_task_self(), page, vm_page_size, FALSE,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "usertrap: SpringBoard-exit: writable protection %d\n", kr);
		return;
	}
	uint32_t patch[] = { 0x58000050, 0xd61f0200, 0, 0 };
	uintptr_t destination = (uintptr_t)diagnostic_exit;
	memcpy(patch + 2, &destination, sizeof(destination));
	memcpy(target, patch, sizeof(patch));
	sys_icache_invalidate(target, sizeof(patch));
	kr = mach_vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	if (kr != KERN_SUCCESS) {
		memcpy(target, expected, sizeof(expected));
		sys_icache_invalidate(target, sizeof(expected));
		fprintf(stderr, "usertrap: SpringBoard-exit: executable protection %d\n", kr);
		return;
	}
	fprintf(stderr, "usertrap: SpringBoard-exit: installed\n");
}
