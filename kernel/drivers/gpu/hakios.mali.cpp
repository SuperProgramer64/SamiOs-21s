/*
 * RedwoodMali: the Mali-G52 (the A32's MC2, mali=1; the A21s's MP1, mali=2) for programs to draw with
 * (ported from the Infinix Hot 20).
 *
 * bsd/dev/hakios.mali.c brings the GPU up (boot-arg mali=1) and, once a job has run through its
 * page tables, starts this service. A program that opens it speaks the interface of Linux's
 * panfrost DRM driver, the one Mesa's panfrost knows: every request is a call of the user client
 * with the request's number as the selector and its structure passed in and back -- as HakiosGPU
 * does it for the other phone's Adreno and freedreno. What Linux does with mmap() on the device is
 * the client memory of the type that is the buffer's handle.
 *
 * What there is: parameters, buffers (made, entered into the GPU's address space, told their
 * address, mapped, given back), and job chains. A buffer is wired kernel memory, zero when made and
 * write-combined, so what the program writes goes to memory where the GPU reads it. A job chain is
 * run to its end before the call returns, so waiting for a buffer has nothing left to wait for, and
 * the synchronisation objects Mesa asks about can all be answered "signalled" in the program
 * (tools/mesa/redwood-drm). Not here yet: jobs that overlap, an address space for each program,
 * interrupts, buffers shared between programs.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <mach/vm_param.h>
#include <sys/systm.h>

#include "MiniIOSurface.h"

extern "C" void redwood_keep(const char * format, ...) __printflike(1, 2);

extern "C" {
/* bsd/dev/redwood_mali.c */
bool redwood_mali_ready(void);
bool redwood_mali_parameter(uint32_t which, uint64_t * value);
uint64_t redwood_mali_address(uint64_t size);
bool redwood_mali_map(uint64_t gpu, uint64_t phys);
void redwood_mali_mapped(void);
void redwood_mali_unmap(uint64_t gpu, uint64_t size);
void redwood_mali_to_memory(const void *start, uint64_t size);
void redwood_mali_tag(uint64_t address, uint32_t kind, uint32_t width, uint32_t height, uint32_t row_bytes,
    uint32_t format, uint32_t offset);
uint32_t redwood_mali_submit(uint64_t chain, bool fragment);
uint64_t redwood_mali_enqueue(uint64_t chain, bool fragment);
bool redwood_mali_wait(uint64_t seq, uint64_t timeout_ns);
bool redwood_mali_done(uint64_t seq);
uint64_t redwood_mali_last(void);

void RedwoodMaliStart(void);
void MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes);

/* what programs have done with the GPU so far, for bsd/dev/redwood_mali.c to repeat in the log */
uint32_t gMaliClients, gMaliBuffers, gMaliSubmits, gMaliFailed, gMaliShown;
/* the first buffers ever made, for matching a GPU fault's address to one of them */
struct mali_made { uint64_t address, size; uint32_t asked, flags; };
struct mali_made gMaliMade[32];
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
	kPanfrostSetLabelBO     = 0x49,         /* a label on a buffer: kept nowhere here */
	kRedwoodSyncobjSet      = 0x7b,         /* ours: a syncobj's fence set, and read (what redwood-drm makes sync files of) */
	kRedwoodSyncobjGet      = 0x7c,
	kRedwoodWait            = 0x7d,         /* ours: wait for a job by its number (struct redwood_wait) */
	kRedwoodSurface         = 0x7e,         /* ours, as RedwoodGPU's: an IOSurface's memory as a buffer */
	kRedwoodShow            = 0x7f,         /* ours, as RedwoodGPU's: a test's pixels onto the panel */
	kDrmSyncobjCreate       = 0xbf,         /* Linux's drm_syncobj.h: what Mesa's panfrost waits with */
	kDrmSyncobjDestroy      = 0xc0,
	kDrmSyncobjWait         = 0xc3,
	kDrmSyncobjReset        = 0xc4,
	kDrmSyncobjSignal       = 0xc5,
};

struct redwood_surface {
	uint32_t surface;                       /* the IOSurface's ID */
	uint32_t handle;                        /* kRedwoodDescribe: no buffer is wanted; else, back: the buffer, */
	uint64_t size;                          /* how big it is, */
	uint32_t offset, rowBytes;              /* where the pixels start in it and how long a row of them is, */
	uint32_t width, height, pixelFormat, planes;    /* and what they are */
};
#define kRedwoodDescribe        0xffffffffu

/* kRedwoodWait: job `seq` (what kPanfrostSubmit gave back in `jc`) done within timeout_ns, or kIOReturnTimeout */
struct redwood_wait {
	uint64_t seq;
	int64_t timeout_ns;
};

struct drm_syncobj_create {
	uint32_t handle, flags;
};

struct drm_syncobj_destroy {
	uint32_t handle, pad;
};

struct drm_syncobj_wait {
	uint64_t handles;
	int64_t timeout_nsec;
	uint32_t count_handles, flags, first_signaled, pad;
	uint64_t deadline_nsec;
};

struct drm_syncobj_array {
	uint64_t handles;
	uint32_t count_handles, pad;
};

/* ours: one syncobj's fence -- the job it stands for, 0 when there is nothing to wait for */
struct redwood_syncobj_fence {
	uint32_t handle, pad;
	uint64_t fence;
};

/* ours: the GPU's counters, for a test to say whether it faulted under it */
struct redwood_mali_counts {
	uint64_t jobs, faults, resets, last;
};

struct redwood_show {
	uint64_t pixels;
	uint32_t width, height, rowBytes, pad;
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
	uint32_t jm_ctx_handle, pad;            /* (newer than Mesa 25.0.7, which passes 48 bytes) */
};
/* SUBMIT as the Metal driver's libdrm sends it (tools/mesa/redwood-drm): Linux's request and, after
 * it, where the job's sequence number is written back, so that the library can answer an idle query
 * itself. */
struct redwood_mali_submit_ex {
	struct drm_panfrost_submit submit;
	uint64_t seqno;
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
#define MALI_SYNCOBJS           1024
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL 1u

struct mali_buffer {
	IOMemoryDescriptor * memory;            /* made here, or the pixels of an IOSurface (`surface`) */
	MiniIOSurface * surface;
	uint64_t address, size;
	uint64_t last_seq;                      /* the last job given it (kPanfrostSubmit's bo_handles) */
};

static IOLock * gMaliLock;

/* How long from now an absolute time of the caller's (mach_continuous_time in ns, what the Metal
 * driver's libdrm makes of Mesa's deadline) is, in ms: 0 when it has passed, 5 s for "no limit". */
static uint64_t
milliseconds_left(int64_t when)
{
	uint64_t now;
	int64_t left;

	if (when <= 0) {
		return 0;
	}
	if (when == INT64_MAX) {
		return 5000;
	}
	absolutetime_to_nanoseconds(mach_continuous_time(), &now);
	left = when - (int64_t) now;
	if (left <= 0) {
		return 0;
	}
	return (uint64_t) left / NSEC_PER_MSEC > 5000 ? 5000 : ((uint64_t) left + NSEC_PER_MSEC - 1) / NSEC_PER_MSEC;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class RedwoodMali : public IOService
{
	OSDeclareDefaultStructors(RedwoodMali);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(RedwoodMali, IOService);

bool
RedwoodMali::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "RedwoodMaliUserClient");
	setName("RedwoodMali");
	registerService();
	return true;
}

void
RedwoodMaliStart(void)
{
	IOService * provider = IOService::getPlatform();
	RedwoodMali * gpu;

	gMaliLock = IOLockAlloc();
	gpu = OSTypeAlloc(RedwoodMali);
	if (gpu == NULL || provider == NULL || !gpu->init() || !gpu->attach(provider)) {
		OSSafeReleaseNULL(gpu);
		IOLog("RedwoodMali: cannot create the service\n");
		return;
	}
	if (!gpu->start(provider)) {
		gpu->detach(provider);
		IOLog("RedwoodMali: the service did not start\n");
	}
	gpu->release();
	IOLog("RedwoodMali: the GPU is there for programs to draw with\n");
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class RedwoodMaliUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(RedwoodMaliUserClient);

	struct mali_buffer *    fBuffers;               /* handle n is fBuffers[n - 1] */
	uint64_t *              fSyncobjs;              /* handle n is fSyncobjs[n - 1]; ~0: none */
	uint32_t                fNextBuffer;
	uint32_t                fSubmits, fFailed, fRequests;

	struct mali_buffer * buffer(uint32_t handle);
	uint64_t * syncobj(uint32_t handle);
	void freeBuffer(struct mali_buffer * b);
	IOReturn createBO(struct drm_panfrost_create_bo * request);
	struct mali_buffer * newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t * handle);
	IOReturn surfaceBuffer(struct redwood_surface * request);
	IOReturn submit(struct drm_panfrost_submit * request, size_t size);
	IOReturn syncobjWait(struct drm_syncobj_wait * request);
	IOReturn syncobjArray(struct drm_syncobj_array * request, bool reset);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(RedwoodMaliUserClient, IOUserClient);

bool
RedwoodMaliUserClient::start(IOService * provider)
{
	if (!redwood_mali_ready() || !IOUserClient::start(provider)) {
		IOLog("RedwoodMali: a client could not be started\n");
		return false;
	}
	IOLog("RedwoodMali: a client opened the GPU\n");
	OSIncrementAtomic((SInt32 *) &gMaliClients);
	fBuffers = IONewZero(struct mali_buffer, MALI_BUFFERS);
	fSyncobjs = IONew(uint64_t, MALI_SYNCOBJS);
	if (fSyncobjs != NULL) {
		for (uint32_t i = 0; i < MALI_SYNCOBJS; i++) {
			fSyncobjs[i] = ~0ULL;
		}
	}
	return fBuffers != NULL && fSyncobjs != NULL;
}

uint64_t *
RedwoodMaliUserClient::syncobj(uint32_t handle)
{
	if (handle == 0 || handle > MALI_SYNCOBJS || fSyncobjs == NULL || fSyncobjs[handle - 1] == ~0ULL) {
		return NULL;
	}
	return &fSyncobjs[handle - 1];
}

struct mali_buffer *
RedwoodMaliUserClient::buffer(uint32_t handle)
{
	if (handle == 0 || handle > MALI_BUFFERS || fBuffers[handle - 1].memory == NULL) {
		return NULL;
	}
	return &fBuffers[handle - 1];
}

/* With the lock held. No job can still be using it: every job has finished before its call returned. */
void
RedwoodMaliUserClient::freeBuffer(struct mali_buffer * b)
{
	/* not while a job queued or running may still use it (jobs are run in a queue now) */
	if (b->last_seq != 0) {
		(void)redwood_mali_wait(b->last_seq, 2ull * NSEC_PER_SEC);
	}
	redwood_mali_unmap(b->address, b->size);
	b->memory->release();
	if (b->surface != NULL) {
		MiniIOSurfaceDrop(b->surface);
	}
	bzero(b, sizeof(*b));
}

/*
 * A handle and GPU addresses for `size` bytes of `memory` from byte `from` on, every page entered
 * into the GPU's tables. With the lock held; NULL: no handle, no addresses, or a page that would
 * not go in. The caller sets `memory` (and `surface`) of what comes back.
 */
struct mali_buffer *
RedwoodMaliUserClient::newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t * handle)
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
		IOLog("RedwoodMali: no handles left\n");
		return NULL;
	}
	b->address = redwood_mali_address(size);
	/* every 4 KB of it: the GPU's pages, a quarter of the kernel's 16 KB ones */
	for (uint64_t offset = 0; offset < size && mapped && b->address != 0; offset += 4096) {
		IOByteCount length = 0;
		addr64_t phys = memory->getPhysicalSegment(from + offset, &length, kIOMemoryMapperNone);

		mapped = phys != 0 && redwood_mali_map(b->address + offset, phys);
	}
	if (b->address == 0 || !mapped) {
		if (b->address != 0) {
			redwood_mali_unmap(b->address, size);
		}
		bzero(b, sizeof(*b));
		IOLog("RedwoodMali: a buffer of %llu bytes cannot be entered into the GPU's address space\n", size);
		return NULL;
	}
	redwood_mali_mapped();
	if (gMaliBuffers < 32) {
		gMaliMade[gMaliBuffers] = (struct mali_made) { b->address, size, (uint32_t) size, 0 };
	}
	OSIncrementAtomic((SInt32 *) &gMaliBuffers);
	b->size = size;
	return b;
}

/* With the lock held. */
IOReturn
RedwoodMaliUserClient::createBO(struct drm_panfrost_create_bo * request)
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
		IOLog("RedwoodMali: no memory for a buffer of %llu bytes\n", size);
		return kIOReturnNoMemory;
	}
	b = newBuffer(memory, 0, size, &request->handle);
	if (b == NULL) {
		memory->release();
		return kIOReturnNoMemory;
	}
	b->memory = memory;
	request->offset = b->address;
	redwood_mali_tag(b->address, 1, 0, 0, 0, 0, 0);
	return kIOReturnSuccess;
}

/*
 * The pixels of an IOSurface as a buffer, from the page after the surface's shared state on
 * (MiniIOSurface.h): what the window server draws into, and what is shown. The surface is held
 * for as long as the buffer is there; a surface that is a buffer of this client's already is that
 * buffer again. Asked only to describe (kRedwoodDescribe), no buffer is made. As RedwoodGPU's.
 */
IOReturn
RedwoodMaliUserClient::surfaceBuffer(struct redwood_surface * request)
{
	MiniIOSurface * s = MiniIOSurfaceCopy(request->surface);
	struct mali_buffer * b = NULL;
	IOMemoryDescriptor * memory;
	uint64_t size;

	if (s == NULL) {
		return kIOReturnNotFound;
	}
	if (s->client != NULL) {                /* a program's own memory with a surface around it: not whole pages */
		MiniIOSurfaceDrop(s);
		return kIOReturnUnsupported;
	}
	size = round_page_64(s->info.allocSize);
	memory = IOSubMemoryDescriptor::withSubRange(s->memory, PAGE_SIZE, size, kIODirectionInOut);
	if (memory == NULL || size == 0 || size > MALI_BUFFER_MAX) {
		OSSafeReleaseNULL(memory);
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	request->size = size;
	request->offset = s->info.offset;
	request->rowBytes = s->info.bytesPerRow;
	request->width = s->info.width;
	request->height = s->info.height;
	request->pixelFormat = s->info.pixelFormat;
	request->planes = s->info.planeCount;
	if (request->handle == kRedwoodDescribe) {
		request->handle = 0;
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnSuccess;
	}
	for (uint32_t i = 0; i < MALI_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL && fBuffers[i].surface == s) {
			b = &fBuffers[i];
			request->handle = i + 1;
			break;
		}
	}
	if (b != NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnSuccess;
	}
	b = newBuffer(s->memory, PAGE_SIZE, size, &request->handle);
	if (b == NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	b->memory = memory;
	b->surface = s;
	redwood_mali_tag(b->address, 2, s->info.width, s->info.height, s->info.bytesPerRow, s->info.pixelFormat,
	    (uint32_t) s->info.offset);
	return kIOReturnSuccess;
}

/*
 * SUBMIT: the chain is queued, not run here; its number goes back in `jc` (what the GL shim
 * waits on) and, when the request is long enough, in `seqno` (what the Metal driver's libdrm
 * reads so that it can answer an idle query without a call); every buffer it names remembers it.
 * With the lock held.
 */
IOReturn
RedwoodMaliUserClient::submit(struct drm_panfrost_submit * request, size_t size)
{
	uint32_t * handles = NULL;
	uint32_t count = request->bo_handle_count;
	uint64_t seq = 0, * out;
	struct mali_buffer * b;

	if (request->jc == 0 || count > MALI_BUFFERS) {
		return kIOReturnBadArgument;
	}
	if (count != 0) {
		handles = IONew(uint32_t, count);
		if (handles == NULL) {
			return kIOReturnNoMemory;
		}
		if (copyin((user_addr_t) request->bo_handles, handles, count * sizeof(uint32_t)) != 0) {
			IODelete(handles, uint32_t, count);
			return kIOReturnVMError;
		}
	}
	fSubmits++;
	gMaliSubmits++;
	/*
	 * For telling a job that runs from one that faults: the first four submissions' descriptors,
	 * 32 words each, with the buffer they sit in.
	 */
	if (fSubmits <= 4) {
		for (uint32_t i = 0; i < count; i++) {
			struct mali_buffer * jb = buffer(handles[i]);
			IOBufferMemoryDescriptor * mb;
			uint8_t * base;
			uint64_t at;

			if (jb == NULL || request->jc < jb->address || request->jc >= jb->address + jb->size) {
				continue;
			}
			at = request->jc - jb->address;
			mb = OSDynamicCast(IOBufferMemoryDescriptor, jb->memory);
			base = mb != NULL ? (uint8_t *) mb->getBytesNoCopy() : NULL;
			if (base == NULL || at + 128 > jb->size) {
				break;
			}
			{
				volatile uint32_t * w = (volatile uint32_t *) (base + at);

				IOLog("RedwoodMali: job %u at %#llx (+%#llx, buffer %u), fragment %u, %u handles\n",
				    fSubmits, request->jc, at, handles[i],
				    (request->requirements & PANFROST_JD_REQ_FS) ? 1 : 0, count);
				for (unsigned line = 0; line < 4; line++) {
					IOLog("RedwoodMali:   %#04x: %08x %08x %08x %08x %08x %08x %08x %08x\n",
					    line * 32, w[line * 8], w[line * 8 + 1], w[line * 8 + 2], w[line * 8 + 3],
					    w[line * 8 + 4], w[line * 8 + 5], w[line * 8 + 6], w[line * 8 + 7]);
				}
			}
			break;
		}
	}
	/*
	 * What the program wrote through its own mapping, to memory where the GPU reads it: a
	 * Metal buffer's contents are the program's writes and nothing else, and this is the
	 * only point that knows which buffers a job will read. The IOSurfaces of the display are
	 * separate memory and are not cleaned here.
	 */
	uint32_t cleaned = 0;
	uint64_t cleanedBytes = 0;

	for (uint32_t i = 0; i < count; i++) {
		struct mali_buffer *wb = buffer(handles[i]);
		IOBufferMemoryDescriptor *mb;

		if (wb == NULL) {
			continue;
		}
		mb = OSDynamicCast(IOBufferMemoryDescriptor, wb->memory);
		if (mb != NULL && mb->getBytesNoCopy() != NULL && wb->size <= (1ull << 20)) {
			/* Small buffers are the program's (uniforms, vertices, indices, a command
			 * stream). The big ones a job names are the driver's pools and the tiler
			 * heap: memory written and read on the GPU side, where a clean of 33 MB a
			 * submit costs more than it can ever fix. */
			redwood_mali_to_memory(mb->getBytesNoCopy(), wb->size);
			cleaned++;
			cleanedBytes += wb->size;
		}
	}
	redwood_mali_to_memory(NULL, 0);
	if (fSubmits <= 4) {
		IOLog("RedwoodMali: submit %u: %u handles, %u buffers cleaned (%llu bytes)\n", fSubmits + 1,
		    count, cleaned, cleanedBytes);
	}
	seq = redwood_mali_enqueue(request->jc, request->requirements & PANFROST_JD_REQ_FS);
	if (seq != 0) {
		for (uint32_t i = 0; i < count; i++) {
			b = buffer(handles[i]);
			if (b != NULL) {
				b->last_seq = seq;
				if (b->surface != NULL) {
					b->surface->maliSeq = seq;      /* (MiniIOSurfaceDrawn) */
				}
			}
		}
		if (request->out_sync != 0 && (out = syncobj(request->out_sync)) != NULL) {
			*out = seq;
		}
		if (size >= sizeof(struct redwood_mali_submit_ex)) {
			((struct redwood_mali_submit_ex *) request)->seqno = seq;
		}
	}
	if (handles != NULL) {
		IODelete(handles, uint32_t, count);
	}
	request->jc = seq;
	return seq != 0 ? kIOReturnSuccess : kIOReturnNotReady;
}

/*
 * One syncobj, or several: signalled when the job it stands for is done. The fences are taken
 * under the lock and then waited for without it, the last one being enough -- the queue runs in
 * order. A timeout of 0 is a look, INT64_MAX "as long as it takes" (capped), and anything else an
 * absolute time on mach_continuous_time (the Metal driver's libdrm converts Mesa's deadline).
 */
IOReturn
RedwoodMaliUserClient::syncobjWait(struct drm_syncobj_wait * request)
{
	uint32_t count = request->count_handles, handles[64];
	uint64_t fences[64], patience = milliseconds_left(request->timeout_nsec), began = mach_absolute_time(), now;
	bool all = (request->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0;

	if (count == 0 || count > 64 || copyin((user_addr_t) request->handles, handles, count * sizeof(uint32_t)) != 0) {
		return kIOReturnBadArgument;
	}
	IOLockLock(gMaliLock);
	for (uint32_t i = 0; i < count; i++) {
		uint64_t * s = syncobj(handles[i]);

		if (s == NULL) {
			IOLockUnlock(gMaliLock);
			return kIOReturnBadArgument;
		}
		fences[i] = *s;
	}
	IOLockUnlock(gMaliLock);
	for (;;) {
		uint32_t done = 0;
		uint64_t latest = 0;

		for (uint32_t i = 0; i < count; i++) {
			if (fences[i] == 0 || redwood_mali_done(fences[i])) {
				if (done++ == 0) {
					request->first_signaled = i;
				}
			}
		}
		if (all ? done == count : done != 0) {
			return kIOReturnSuccess;
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &now);
		if (now / NSEC_PER_MSEC >= patience) {
			static uint32_t told;

			if (told++ < 16) {
				IOLog("RedwoodMali: a syncobj wait gave up after %llu ms: %u fence(s), first %llu, last "
				    "submitted %llu, done %d\n", now / NSEC_PER_MSEC, count, fences[0],
				    redwood_mali_last(), (int) redwood_mali_done(fences[0]));
			}
			return kIOReturnTimeout;
		}
		for (uint32_t i = 0; i < count; i++) {
			latest = fences[i] > latest ? fences[i] : latest;
		}
		(void)redwood_mali_wait(all ? latest : fences[0], (patience - now / NSEC_PER_MSEC) * NSEC_PER_MSEC);
	}
}

/* RESET and SIGNAL: both leave a syncobj with nothing to wait for. */
IOReturn
RedwoodMaliUserClient::syncobjArray(struct drm_syncobj_array * request, bool reset)
{
	uint32_t handles[64], count = request->count_handles;

	(void) reset;
	if (count == 0 || count > 64 || copyin((user_addr_t) request->handles, handles, count * sizeof(uint32_t)) != 0) {
		return kIOReturnBadArgument;
	}
	IOLockLock(gMaliLock);
	for (uint32_t i = 0; i < count; i++) {
		uint64_t * s = syncobj(handles[i]);

		if (s != NULL) {
			*s = 0;
		}
	}
	IOLockUnlock(gMaliLock);
	return kIOReturnSuccess;
}

IOReturn
RedwoodMaliUserClient::clientClose(void)
{
	IOLockLock(gMaliLock);
	for (uint32_t i = 0; fBuffers != NULL && i < MALI_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL) {
			freeBuffer(&fBuffers[i]);
		}
	}
	IOLockUnlock(gMaliLock);
	if (fSubmits != 0) {
		IOLog("RedwoodMali: a client goes after %u job chains, %u of them unfinished\n", fSubmits, fFailed);
	}
	terminate();
	return kIOReturnSuccess;
}

void
RedwoodMaliUserClient::free(void)
{
	if (fBuffers != NULL) {
		IODelete(fBuffers, struct mali_buffer, MALI_BUFFERS);
		fBuffers = NULL;
	}
	if (fSyncobjs != NULL) {
		IODelete(fSyncobjs, uint64_t, MALI_SYNCOBJS);
		fSyncobjs = NULL;
	}
	IOUserClient::free();
}

IOReturn
RedwoodMaliUserClient::clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory)
{
	struct mali_buffer * b;
	IOReturn ret = kIOReturnBadArgument;

	IOLockLock(gMaliLock);
	b = buffer(type);
	if (b != NULL) {
		b->memory->retain();
		*memory = b->memory;
		/*
		 * A buffer the program writes (a Metal buffer, a command buffer) is mapped write
		 * combined: its writes reach memory where the GPU and the driver's own reads of it
		 * find them. The pixels of an IOSurface are memory outside XNU's and keep the
		 * mapping they had (the display's).
		 */
		*options = b->surface != NULL ? 0 : kIOMapInhibitCache;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gMaliLock);
	return ret;
}

IOReturn
RedwoodMaliUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	union {
		struct drm_gem_close close;
		struct drm_panfrost_submit submit;
		struct redwood_mali_submit_ex submitEx;
		struct drm_panfrost_wait_bo wait;
		struct drm_panfrost_create_bo create;
		struct drm_panfrost_mmap_bo mmap;
		struct drm_panfrost_get_param param;
		struct drm_panfrost_get_bo_offset offset;
		struct drm_panfrost_madvise madvise;
		struct drm_syncobj_create syncCreate;
		struct drm_syncobj_destroy syncDestroy;
		struct drm_syncobj_wait syncWait;
		struct drm_syncobj_array syncArray;
		struct redwood_syncobj_fence syncFence;
		struct redwood_mali_counts counts;
		struct redwood_show show;
		struct redwood_surface surface;
		struct redwood_wait waitseq;
	} r;
	size_t size = args->structureInputSize;
	struct mali_buffer * b;
	uint64_t * s;
	IOReturn ret = kIOReturnSuccess;

	if (args->structureInput == NULL || size > sizeof(r) || fBuffers == NULL) {
		return kIOReturnBadArgument;
	}
	if (!redwood_mali_ready()) {
		return kIOReturnNotReady;
	}
	bzero(&r, sizeof(r));
	memcpy(&r, args->structureInput, size);

	/* the waits, without holding everybody else up */
	if (selector == kRedwoodWait && size == sizeof(struct redwood_wait)) {
		ret = r.waitseq.seq == 0 || redwood_mali_wait(r.waitseq.seq,
		    r.waitseq.timeout_ns > 0 ? (uint64_t) r.waitseq.timeout_ns : 0) ? kIOReturnSuccess : kIOReturnTimeout;
		goto answer;
	}
	if (selector == kPanfrostWaitBO) {
		int64_t timeout;
		uint64_t seq;

		IOLockLock(gMaliLock);
		b = buffer(r.wait.handle);
		seq = b != NULL ? b->last_seq : 0;
		IOLockUnlock(gMaliLock);
		if (b == NULL) {
			ret = kIOReturnNotFound;
			goto answer;
		}
		timeout = r.wait.timeout_ns;
		ret = seq == 0 || redwood_mali_wait(seq,
		    timeout > 0 ? (uint64_t) timeout : 0) ? kIOReturnSuccess : kIOReturnTimeout;
		goto answer;
	}
	if (selector == kDrmSyncobjWait) {
		ret = syncobjWait(&r.syncWait);
		goto answer;
	}
	IOLockLock(gMaliLock);
	if (fRequests++ < 32) {                 /* the first ones a client makes, for following it from the log */
		IOLog("RedwoodMali: request %#x, %zu bytes\n", selector, size);
	}
	switch (selector) {
	case kPanfrostGetParam:
		if (!redwood_mali_parameter(r.param.param, &r.param.value)) {
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
	case kPanfrostSubmit:
		ret = submit(&r.submit, size);
		break;
	case kPanfrostSetLabelBO:               /* a label is for the driver's debug output: kept nowhere */
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
	case kDrmSyncobjCreate:
		ret = kIOReturnNoResources;
		for (uint32_t i = 0; i < MALI_SYNCOBJS; i++) {
			if (fSyncobjs[i] == ~0ULL) {
				fSyncobjs[i] = 0;               /* signalled or not, there is nothing to wait for yet */
				r.syncCreate.handle = i + 1;
				ret = kIOReturnSuccess;
				break;
			}
		}
		break;
	case kDrmSyncobjDestroy:
		if ((s = syncobj(r.syncDestroy.handle)) != NULL) {
			*s = ~0ULL;
		} else {
			ret = kIOReturnBadArgument;
		}
		break;
	case kDrmSyncobjReset:
	case kDrmSyncobjSignal:
		ret = syncobjArray(&r.syncArray, selector == kDrmSyncobjReset);
		break;
	case kRedwoodSyncobjSet:
	case kRedwoodSyncobjGet:
		if ((s = syncobj(r.syncFence.handle)) == NULL) {
			ret = kIOReturnBadArgument;
		} else if (selector == kRedwoodSyncobjSet) {
			*s = r.syncFence.fence;
		} else {
			r.syncFence.fence = *s;
		}
		break;
	case kRedwoodWait:                      /* 0x7d, 32 bytes: the Metal driver's counters (struct redwood_mali_counts) */
		r.counts.jobs = gMaliSubmits;
		r.counts.faults = gMaliFailed;
		r.counts.resets = 0;
		r.counts.last = redwood_mali_last();
		break;
	case kRedwoodSurface:
		ret = surfaceBuffer(&r.surface);
		break;
	case kRedwoodShow: {
		struct redwood_show * show = (struct redwood_show *) &r;
		uint64_t bytes = (uint64_t) show->rowBytes * show->height;
		uint8_t * pixels;

		if (show->width == 0 || show->width > 4096 || show->height == 0 || show->height > 4096 ||
		    show->rowBytes < 4 * show->width || (pixels = (uint8_t *) IOMallocData(bytes)) == NULL) {
			ret = kIOReturnBadArgument;
			break;
		}
		if (copyin((user_addr_t) show->pixels, pixels, bytes) != 0) {
			ret = kIOReturnVMError;
		} else {
			MiniIOMobileFramebufferShow(pixels, show->width, show->height, show->rowBytes);
			gMaliShown++;
		}
		IOFreeData(pixels, bytes);
		break;
	}
	default:
		IOLog("RedwoodMali: request %#x (%zu bytes) is not known here\n", selector, size);
		ret = kIOReturnUnsupported;
		break;
	}
	IOLockUnlock(gMaliLock);

answer:
	if (args->structureOutput != NULL && args->structureOutputSize >= size) {
		memcpy(args->structureOutput, &r, size);
		args->structureOutputSize = (uint32_t) size;
	}
	return ret;
}
