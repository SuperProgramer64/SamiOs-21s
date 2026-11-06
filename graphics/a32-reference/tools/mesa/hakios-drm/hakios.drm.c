/*
 * The libdrm of xf86drm.h (see there): Linux's DRM requests to the kernel's GPU driver as calls
 * of its IOKit user client. A request's number (the low byte of the ioctl code) is the call's
 * selector, its structure goes in and comes back as it is; a buffer's memory is the client
 * memory of the type that is the buffer's handle.
 *
 * freedreno knows its device by a file descriptor. It gets one that means nothing (/dev/null):
 * there is one GPU and one connection to it in a process.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <IOKit/IOKitLib.h>

#include "xf86drm.h"

#define HAKIOS_GPU_SERVICE     "HakiosGPU"     /* the Adreno (iokit/Kernel/hakios.gpu.cpp) */
#define HAKIOS_MALI_SERVICE    "RedwoodMali"   /* the Mali (iokit/Kernel/hakios.mali.cpp) */
#define HAKIOS_SURFACE         0x7e            /* the request that is not Linux's */
#define HAKIOS_DESCRIBE        0xffffffffu     /* ... asked only what the buffer would be like */
#define HAKIOS_WAIT            0x7d            /* a job's number, waited for */

static int is_panfrost;

static io_connect_t connection;
static int connection_fd = -1;

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	io_service_t service;

	(void)busid;
	(void)type;
	if (strcmp(name, "msm") == 0) {
		is_panfrost = 0;
	} else if (strcmp(name, "panfrost") == 0) {
		is_panfrost = 1;
	} else {
		return -1;
	}
	if (connection_fd >= 0) {
		return dup(connection_fd);
	}
	service = IOServiceGetMatchingService(0,
	    IOServiceMatching(is_panfrost ? HAKIOS_MALI_SERVICE : HAKIOS_GPU_SERVICE));
	if (service == 0) {
		return -1;
	}
	if (IOServiceOpen(service, mach_task_self(), 0, &connection) != 0) {
		IOObjectRelease(service);
		return -1;
	}
	IOObjectRelease(service);
	connection_fd = open("/dev/null", O_RDWR);
	return dup(connection_fd);
}

static int
call(unsigned number, void *data, size_t size)
{
	size_t returned = size;
	kern_return_t result = IOConnectCallStructMethod(connection, number, data, size, data, &returned);

	switch (result) {
	case 0:
		return 0;
	case kIOReturnNoMemory:
		return ENOMEM;
	case kIOReturnBadArgument:
		return EINVAL;
	case kIOReturnTimeout:
		return ETIMEDOUT;
	case kIOReturnBusy:
		return EBUSY;
	case kIOReturnNotFound:
		return ENOENT;
	case kIOReturnUnsupported:
		return ENOSYS;
	default:
		return EIO;
	}
}

struct hakios_wait {
	uint64_t seq;
	int64_t timeout_ns;
};

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	unsigned number = (unsigned)(request & 0xff);
	int error = call(number, arg, IOCPARM_LEN(request));

	(void)fd;
	if (error != 0) {
		errno = error;
		return -1;
	}
	/*
	 * The Mali service queues a job chain and hands its number back in submit.jc. Every job
	 * must be over before the call returns here, so the syncobjs a driver waits on can all be
	 * said to be signalled (see below). The Adreno service runs its jobs before returning.
	 */
	if (is_panfrost && number == 0x40) {
		struct hakios_wait wait = { .seq = *(uint64_t *)arg, .timeout_ns = 2000000000 };

		error = call(HAKIOS_WAIT, &wait, sizeof(wait));
		if (error != 0) {
			errno = error;
			return -1;
		}
	}
	return 0;
}

int
drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size)
{
	(void)fd;
	return -call((unsigned)(DRM_COMMAND_BASE + index), (void *)(uintptr_t)data, size);
}

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	return -call((unsigned)(DRM_COMMAND_BASE + index), data, size);
}

/*
 * The driver calls itself what Linux's is called, and is of the version whose interface it has:
 * 1.7, buffers at addresses the kernel chooses and tells ("softpin"), nothing newer.
 */
drmVersionPtr
drmGetVersion(int fd)
{
	drmVersionPtr version = calloc(1, sizeof(*version));

	(void)fd;
	if (version != NULL) {
		version->version_major = 1;
		version->version_minor = is_panfrost ? 1 : 7;
		version->name = strdup(is_panfrost ? "panfrost" : "msm");
		version->name_len = (int)strlen(version->name);
		version->date = strdup("");
		version->desc = strdup(is_panfrost ? "Mali on Darwin" : "Adreno on Darwin");
		version->desc_len = (int)strlen(version->desc);
	}
	return version;
}

void
drmFreeVersion(drmVersionPtr version)
{
	if (version != NULL) {
		free(version->name);
		free(version->date);
		free(version->desc);
		free(version);
	}
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	struct drm_gem_close close = { .handle = handle };

	return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close);
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	(void)capability;
	*value = 0;
	return 0;
}

void *
hakios_drm_map(int fd, uint32_t handle, uint64_t size)
{
	mach_vm_address_t address = 0;
	mach_vm_size_t mapped = 0;

	(void)fd;
	(void)size;
	/*
	 * Linux gives freedreno its buffers write-combined, with no cache of this processor's between
	 * the program and the memory the GPU sees. So does the kernel's driver here: the memory is
	 * made that way, and this mapping is as the memory is.
	 */
	if (IOConnectMapMemory64(connection, handle, mach_task_self(), &address, &mapped, kIOMapAnywhere) != 0) {
		return (void *)-1;              /* mmap()'s MAP_FAILED */
	}
	return (void *)(uintptr_t)address;
}

void
hakios_drm_unmap(int fd, uint32_t handle, void *address)
{
	(void)fd;
	IOConnectUnmapMemory64(connection, handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)address);
}

int
hakios_drm_surface(int fd, struct hakios_drm_surface *surface)
{
	int error;

	(void)fd;
	surface->handle = HAKIOS_DESCRIBE;
	error = call(HAKIOS_SURFACE, surface, sizeof(*surface));
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

int
drmPrimeFDToHandle(int fd, int surface, uint32_t *handle)
{
	struct hakios_drm_surface request = { .surface = (uint32_t)surface };
	int error = call(HAKIOS_SURFACE, &request, sizeof(request));

	(void)fd;
	if (error != 0) {
		errno = error;
		return -1;
	}
	*handle = request.handle;
	return 0;
}

static int
none(void)
{
	errno = ENOSYS;
	return -1;
}

/* Buffers and fences as file descriptors, for handing them to other programs and drivers: none of that here. */
#define NONE(name, ...) int name(__VA_ARGS__) { return none(); }
NONE(drmPrimeHandleToFD, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) uint32_t flags, __attribute__((unused)) int *prime_fd)
/*
 * Fences are the one thing this driver's jobs do not need: a job is over before SUBMIT returns
 * (see drmIoctl). So every syncobj is created already signalled, waits answer at once, and a
 * sync file is a descriptor to /dev/null: what a consumer waits on is already done.
 */
static uint32_t next_syncobj = 1;

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	(void)fd; (void)flags;
	*handle = next_syncobj++;
	return 0;
}
int
drmSyncobjDestroy(int fd, uint32_t handle) { (void)fd; (void)handle; return 0; }
int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count) { (void)fd; (void)handles; (void)count; return 0; }
int
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
	(void)fd; (void)obj_fd;
	*handle = next_syncobj++;
	return 0;
}
int
drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
	(void)fd; (void)handle;
	*obj_fd = open("/dev/null", O_RDONLY);
	return *obj_fd < 0 ? -1 : 0;
}
int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	(void)fd; (void)handle; (void)sync_file_fd;
	return 0;
}
int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	(void)fd; (void)handle;
	*sync_file_fd = open("/dev/null", O_RDONLY);
	return *sync_file_fd < 0 ? -1 : 0;
}
int
drmSyncobjWait(int fd, const uint32_t *handles, uint32_t count, int64_t timeout_nsec,
    uint32_t flags, uint32_t *first_signaled)
{
	(void)fd; (void)timeout_nsec; (void)flags;
	if (first_signaled != NULL && count > 0)
		*first_signaled = handles[0];
	return 0;
}
int
drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle,
    uint64_t src_point, uint32_t flags)
{
	(void)fd; (void)dst_handle; (void)dst_point; (void)src_handle; (void)src_point; (void)flags;
	return 0;
}
int
drmSyncobjTimelineWait(int fd, const uint32_t *handles, const uint64_t *points,
    uint32_t count, int64_t timeout_nsec, uint32_t flags, uint32_t *first_signaled)
{
	(void)fd; (void)points; (void)timeout_nsec; (void)flags;
	if (first_signaled != NULL && count > 0)
		*first_signaled = handles[0];
	return 0;
}
