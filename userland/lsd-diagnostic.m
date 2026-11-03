#include <Foundation/Foundation.h>
#include <mach/mach.h>
#include <objc/message.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern const void *_dyld_get_shared_cache_range(size_t *length);

static uintptr_t slide;
static void (*original_pool_pop)(void *);

__attribute__((used, noinline)) static void diagnostic_pool_pop_impl(void *pool, uintptr_t error_slot, uintptr_t caller)
{
	if (caller == slide + 0x181c85c08ULL) {
		fprintf(stderr, "usertrap: lsd-save-return: error-slot %p\n", (void *)error_slot);
		if (error_slot) {
			id error = *(id *)error_slot;
			if (error) {
				fprintf(stderr, "usertrap: lsd-save-error: %s\n", [[error description] UTF8String]);
				id defaults = ((id (*)(void))(slide + 0x181bef618ULL))();
				id url = ((id (*)(id, SEL))objc_msgSend)(defaults,
				    sel_registerName("systemContentDatabaseStoreFileURL"));
				fprintf(stderr, "usertrap: lsd-save-url: %s\n", [[url description] UTF8String]);
				fflush(stderr);
			}
		}
	}
	original_pool_pop(pool);
}

__attribute__((naked)) static void diagnostic_pool_pop(void *pool __attribute__((unused)))
{
	__asm__ volatile(
	    "stp x29, x30, [sp, #-16]!\n"
	    "mov x29, sp\n"
	    "mov x1, x21\n"
	    "mov x2, x30\n"
	    "bl _diagnostic_pool_pop_impl\n"
	    "ldp x29, x30, [sp], #16\n"
	    "ret\n");
}

__attribute__((constructor)) static void diagnostic_init(void)
{
	if (strcmp(getprogname(), "lsd") != 0) {
		return;
	}
	size_t length = 0;
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - 0x180000000ULL;
	uintptr_t *slot = (uintptr_t *)(slide + 0x1dae50510ULL);
	uintptr_t expected = slide + 0x1800bc4b4ULL;
	if (*slot != expected) {
		fprintf(stderr, "usertrap: lsd-diagnostic: unexpected pool-pop import %p\n", (void *)*slot);
		return;
	}
	vm_address_t page = (vm_address_t)slot & ~(vm_address_t)(vm_page_size - 1);
	kern_return_t kr = vm_protect(mach_task_self(), page, vm_page_size, FALSE,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "usertrap: lsd-diagnostic: vm_protect %d\n", kr);
		return;
	}
	original_pool_pop = (void (*)(void *))*slot;
	*slot = (uintptr_t)diagnostic_pool_pop;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	fprintf(stderr, "usertrap: lsd-diagnostic: installed\n");
}
