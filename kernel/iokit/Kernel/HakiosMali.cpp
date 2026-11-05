/*
 * HakiosPanfrostGPU: the Mali-G77 of the Infinix GT 10 Pro (X6739, MT6893) for programs to draw
 * with.
 *
 * bsd/dev/hakios.mali.c brings the GPU up (boot-arg mali=N) and, once it is up, starts this
 * service. A program that opens it speaks the interface of Linux's panfrost DRM driver, the
 * one Mesa's panfrost knows: every request is a call of the user client with the request's
 * number as the selector and its structure passed in and back -- as HakiosGPU does it for the
 * other phone's Adreno and freedreno. What Linux does with mmap() on the device is the client
 * memory of the type that is the buffer's handle.
 *
 * What there is: parameters, buffers (made, entered into the GPU's address space, told their
 * address, mapped, given back), and job chains. A buffer is wired kernel memory, zero when
 * made and write-combined, so what the program writes goes to memory where the GPU reads it. A
 * job chain is run to its end before the call returns, so waiting for a buffer has nothing left
 * to wait for.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <mach/vm_param.h>
#include <sys/systm.h>

extern "C" {
bool hakios_mali_ready(void);
bool hakios_mali_parameter(uint32_t which, uint64_t * value);
uint64_t hakios_mali_address(uint64_t size);
bool hakios_mali_map(uint64_t gpu, uint64_t phys);
void hakios_mali_mapped(void);
void hakios_mali_unmap(uint64_t gpu, uint64_t size);
uint32_t hakios_mali_submit(uint64_t chain, bool fragment);

void HakiosMaliStart(void);

uint32_t gMaliClients, gMaliBuffers, gMaliSubmits, gMaliFailed;
struct hakios_mali_made { uint64_t address, size; uint32_t asked, flags; };
struct hakios_mali_made gMaliMade[32];
}

/* Linux's include/uapi/drm/panfrost_drm.h and drm.h: the requests' numbers and structures. */
enum {
	kDrmGemClose            = 0x09,
	kPanfrostSubmit         = 0x40,
	kPanfrostWaitBO         = 0x41,
	kPanfrostCreateBO       = 0x42,
	kPanfrostMmapBO         = 0x43,
	kPanfrostGetParam       = 0x44,
	kPanfrostGetBOOffset    = 0x45,
	kPanfrostMadvise        = 0x48,
};

struct drm_gem_close {
	uint32_t handle, pad;
};

struct drm_panfrost_submit {
	uint64_t jc;
	uint64_t in_syncs;
	uint32_t in_sync_count;
	uint32_t out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count;
	uint32_t requirements;
};
#define PANFROST_JD_REQ_FS      (1u << 0)

struct drm_panfrost_wait_bo {
	uint32_t handle, pad;
	int64_t timeout_ns;
};

struct drm_panfrost_create_bo {
	uint32_t size, flags, handle, pad;
	uint64_t offset;
};

struct drm_panfrost_mmap_bo {
	uint32_t handle, flags;
	uint64_t offset;
};

struct drm_panfrost_get_param {
	uint32_t param, pad;
	uint64_t value;
};

struct drm_panfrost_get_bo_offset {
	uint32_t handle, pad;
	uint64_t offset;
};

struct drm_panfrost_madvise {
	uint32_t handle, madv, retained;
};

#define MALI_BUFFERS            4096
#define MALI_BUFFER_MAX         (256ull << 20)

struct mali_buffer {
	IOMemoryDescriptor * memory;
	uint64_t address, size;
};

static IOLock * gMaliLock;

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class HakiosMali : public IOService
{
	OSDeclareDefaultStructors(HakiosMali);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosMali, IOService);

bool
HakiosMali::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "HakiosMaliUserClient");
	setName("HakiosPanfrostGPU");
	registerService();
	return true;
}

void
HakiosMaliStart(void)
{
	IOService * provider = IOService::getPlatform();
	HakiosMali * gpu;

	gMaliLock = IOLockAlloc();
	gpu = OSTypeAlloc(HakiosMali);
	if (gpu == NULL || provider == NULL || !gpu->init() || !gpu->attach(provider)) {
		OSSafeReleaseNULL(gpu);
		IOLog("HakiosMali: cannot create the service\n");
		return;
	}
	if (!gpu->start(provider)) {
		gpu->detach(provider);
		IOLog("HakiosMali: the service did not start\n");
	}
	gpu->release();
	IOLog("HakiosMali: the GPU is there for programs to draw with\n");
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class HakiosMaliUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(HakiosMaliUserClient);

	struct mali_buffer *    fBuffers;               /* handle n is fBuffers[n - 1] */
	uint32_t                fNextBuffer;
	uint32_t                fSubmits, fFailed, fRequests;

	struct mali_buffer * buffer(uint32_t handle);
	void freeBuffer(struct mali_buffer * b);
	IOReturn createBO(struct drm_panfrost_create_bo * request);
	struct mali_buffer * newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t * handle);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosMaliUserClient, IOUserClient);

bool
HakiosMaliUserClient::start(IOService * provider)
{
	if (!hakios_mali_ready() || !IOUserClient::start(provider)) {
		IOLog("HakiosMali: a client could not be started\n");
		return false;
	}
	IOLog("HakiosMali: a client opened the GPU\n");
	OSIncrementAtomic((SInt32 *) &gMaliClients);
	fBuffers = IONewZero(struct mali_buffer, MALI_BUFFERS);
	return fBuffers != NULL;
}

struct mali_buffer *
HakiosMaliUserClient::buffer(uint32_t handle)
{
	if (handle == 0 || handle > MALI_BUFFERS || fBuffers[handle - 1].memory == NULL) {
		return NULL;
	}
	return &fBuffers[handle - 1];
}

/* With the lock held. No job can still be using it: every job has finished before its call returned. */
void
HakiosMaliUserClient::freeBuffer(struct mali_buffer * b)
{
	hakios_mali_unmap(b->address, b->size);
	b->memory->release();
	bzero(b, sizeof(*b));
}

/*
 * A handle and a GPU address for `size` bytes of `memory` from byte `from` on, every page entered
 * into the GPU's tables. With the lock held; NULL: no handle, no address, or a page that would
 * not go in. The caller sets `memory` of what comes back.
 */
struct mali_buffer *
HakiosMaliUserClient::newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t * handle)
{
	struct mali_buffer * b = NULL;
	bool mapped = true;

	for (uint32_t n = 0; n < MALI_BUFFERS; n++) {
		uint32_t i = (fNextBuffer + n) % MALI_BUFFERS;

		if (fBuffers[i].memory == NULL) {
			b = &fBuffers[i];
			*handle = i + 1;
			fNextBuffer = i + 1;
			break;
		}
	}
	if (b == NULL) {
		IOLog("HakiosMali: no handles left\n");
		return NULL;
	}
	b->address = hakios_mali_address(size);
	/* every 4 KB of it: the GPU's pages, a quarter of the kernel's 16 KB ones */
	for (uint64_t offset = 0; offset < size && mapped && b->address != 0; offset += 4096) {
		IOByteCount length = 0;
		addr64_t phys = memory->getPhysicalSegment(from + offset, &length, kIOMemoryMapperNone);

		mapped = phys != 0 && hakios_mali_map(b->address + offset, phys);
	}
	if (b->address == 0 || !mapped) {
		if (b->address != 0) {
			hakios_mali_unmap(b->address, size);
		}
		bzero(b, sizeof(*b));
		IOLog("HakiosMali: a buffer of %llu bytes cannot be entered into the GPU's address space\n", size);
		return NULL;
	}
	hakios_mali_mapped();
	if (gMaliBuffers < 32) {
		gMaliMade[gMaliBuffers] = (struct hakios_mali_made) { b->address, size, (uint32_t) size, 0 };
	}
	OSIncrementAtomic((SInt32 *) &gMaliBuffers);
	b->size = size;
	return b;
}

/* With the lock held. */
IOReturn
HakiosMaliUserClient::createBO(struct drm_panfrost_create_bo * request)
{
	uint64_t size = round_page_64(request->size);
	IOBufferMemoryDescriptor * memory;
	struct mali_buffer * b;

	if (size == 0 || size > MALI_BUFFER_MAX) {
		return kIOReturnBadArgument;
	}
	memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared |
	    kIOMapWriteCombineCache, (vm_size_t) size, PAGE_SIZE);
	if (memory == NULL) {
		IOLog("HakiosMali: no memory for a buffer of %llu bytes\n", size);
		return kIOReturnNoMemory;
	}
	b = newBuffer(memory, 0, size, &request->handle);
	if (b == NULL) {
		memory->release();
		return kIOReturnNoMemory;
	}
	b->memory = memory;
	request->offset = b->address;
	return kIOReturnSuccess;
}

IOReturn
HakiosMaliUserClient::clientClose(void)
{
	IOLockLock(gMaliLock);
	for (uint32_t i = 0; fBuffers != NULL && i < MALI_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL) {
			freeBuffer(&fBuffers[i]);
		}
	}
	IOLockUnlock(gMaliLock);
	if (fSubmits != 0) {
		IOLog("HakiosMali: a client goes after %u job chains, %u of them unfinished\n", fSubmits, fFailed);
	}
	terminate();
	return kIOReturnSuccess;
}

void
HakiosMaliUserClient::free(void)
{
	if (fBuffers != NULL) {
		IODelete(fBuffers, struct mali_buffer, MALI_BUFFERS);
		fBuffers = NULL;
	}
	IOUserClient::free();
}

IOReturn
HakiosMaliUserClient::clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory)
{
	struct mali_buffer * b;
	IOReturn ret = kIOReturnBadArgument;

	IOLockLock(gMaliLock);
	b = buffer(type);
	if (b != NULL) {
		b->memory->retain();
		*memory = b->memory;
		*options = 0;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gMaliLock);
	return ret;
}

IOReturn
HakiosMaliUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	union {
		struct drm_gem_close close;
		struct drm_panfrost_submit submit;
		struct drm_panfrost_wait_bo wait;
		struct drm_panfrost_create_bo create;
		struct drm_panfrost_mmap_bo mmap;
		struct drm_panfrost_get_param param;
		struct drm_panfrost_get_bo_offset offset;
		struct drm_panfrost_madvise madvise;
	} r;
	size_t size = args->structureInputSize;
	struct mali_buffer * b;
	IOReturn ret = kIOReturnSuccess;

	if (args->structureInput == NULL || size > sizeof(r) || fBuffers == NULL) {
		return kIOReturnBadArgument;
	}
	if (!hakios_mali_ready()) {
		return kIOReturnNotReady;
	}
	bzero(&r, sizeof(r));
	memcpy(&r, args->structureInput, size);

	IOLockLock(gMaliLock);
	if (fRequests++ < 32) {                 /* the first ones a client makes, for following it from the log */
		IOLog("HakiosMali: request %#x, %zu bytes\n", selector, size);
	}
	switch (selector) {
	case kPanfrostGetParam:
		if (!hakios_mali_parameter(r.param.param, &r.param.value)) {
			ret = kIOReturnBadArgument;
		}
		break;
	case kPanfrostCreateBO:
		ret = createBO(&r.create);
		break;
	case kPanfrostMmapBO:                   /* the offset is the handle: the program maps client memory of that type */
		ret = buffer(r.mmap.handle) != NULL ? kIOReturnSuccess : kIOReturnNotFound;
		r.mmap.offset = (uint64_t) r.mmap.handle << 12;
		break;
	case kPanfrostGetBOOffset:
		b = buffer(r.offset.handle);
		ret = b != NULL ? kIOReturnSuccess : kIOReturnNotFound;
		r.offset.offset = b != NULL ? b->address : 0;
		break;
	case kPanfrostSubmit: {
		uint32_t status = 0;

		fSubmits++;
		gMaliSubmits++;
		if (r.submit.jc != 0) {
			status = hakios_mali_submit(r.submit.jc, r.submit.requirements & PANFROST_JD_REQ_FS);
		}
		if (status != 0) {
			fFailed++;
			ret = kIOReturnError;
		}
		/*
		 * Every buffer the chain names remembers the job, so that a buffer is not freed
		 * under a job that uses it.
		 */
		for (uint32_t i = 0; r.submit.jc != 0 && i < r.submit.bo_handle_count && i < 4096; i += 64) {
			uint32_t handles[64];
			uint32_t n = r.submit.bo_handle_count - i < 64 ? r.submit.bo_handle_count - i : 64;

			if (copyin((user_addr_t) (r.submit.bo_handles + i * sizeof(uint32_t)), handles, n * sizeof(uint32_t)) != 0) {
				break;
			}
			for (uint32_t k = 0; k < n; k++) {
				(void)buffer(handles[k]);
			}
		}
		break;
	}
	case kPanfrostWaitBO:                   /* every job has finished before its submission returned */
		ret = buffer(r.wait.handle) != NULL ? kIOReturnSuccess : kIOReturnNotFound;
		break;
	case kPanfrostMadvise:
		r.madvise.retained = 1;
		break;
	case kDrmGemClose:
		b = buffer(r.close.handle);
		if (b != NULL) {
			freeBuffer(b);
		} else {
			ret = kIOReturnNotFound;
		}
		break;
	default:
		IOLog("HakiosMali: request %#x (%zu bytes) is not known here\n", selector, size);
		ret = kIOReturnUnsupported;
		break;
	}
	IOLockUnlock(gMaliLock);

	if (args->structureOutput != NULL && args->structureOutputSize >= size) {
		memcpy(args->structureOutput, &r, size);
		args->structureOutputSize = (uint32_t) size;
	}
	return ret;
}
