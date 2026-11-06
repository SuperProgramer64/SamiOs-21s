/*
 * What exyloader needs to start XNU: the payload ("XNUP", xnuloader/mkpayload.py), the kernel's
 * Mach-O segments and symbols, XNU's device tree and its boot_args. Ported from hakios-a32's
 * mtkloader (same project, same layouts), without what was MediaTek's.
 *
 * Position independent like the rest of exyloader: no table here holds a pointer (names are
 * char arrays), so the program has no absolute relocation (loader/build.sh checks).
 * Device memory with the MMU off: copies go 8 bytes at a time where both ends allow it.
 */
#ifndef XNU_H
#define XNU_H

#include <stdint.h>
#include <stddef.h>

/* ---- the C library the compiler may call ---- */

void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);

void *
memset(void *dst, int c, size_t n)
{
	uint8_t *d = dst;
	uint64_t v = (uint8_t)c * 0x0101010101010101ULL;

	while (n && ((uint64_t)d & 7)) {
		*d++ = (uint8_t)c, n--;
	}
	for (; n >= 8; n -= 8, d += 8) {
		*(volatile uint64_t *)d = v;
	}
	while (n--) {
		*d++ = (uint8_t)c;
	}
	return dst;
}

void *
memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	if ((((uint64_t)d ^ (uint64_t)s) & 7) == 0) {
		while (n && ((uint64_t)d & 7)) {
			*d++ = *s++, n--;
		}
		for (; n >= 8; n -= 8, d += 8, s += 8) {
			*(volatile uint64_t *)d = *(const volatile uint64_t *)s;
		}
	}
	while (n--) {
		*d++ = *s++;
	}
	return dst;
}

size_t
strlen(const char *s)
{
	size_t n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}

int
strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++, b++;
	}
	return (uint8_t)*a - (uint8_t)*b;
}

/* ---- the payload ---- */

#define PAYLOAD_MAGIC       0x50554e58u     /* "XNUP" */

struct payload_header {
	uint32_t magic;
	uint32_t size;                          /* bytes after the header */
};

struct payload_entry {
	char name[16];
	uint32_t size;
	uint32_t reserved;
};

struct payload_info {
	const uint8_t *kernel;
	uint32_t kernel_size;
	const uint8_t *ramdisk;
	uint32_t ramdisk_size;
	const char *cmdline;                    /* "cmdline": more boot arguments, NUL-terminated */
	const uint8_t *touchfw;                 /* "touchfw": the touch controller's firmware (ROMBOOT_APP_CODE), or NULL */
	uint32_t touchfw_size;
};

static int
payload_parse(const void *at, struct payload_info *pi)
{
	const struct payload_header *ph = at;

	memset(pi, 0, sizeof(*pi));
	if (ph->magic != PAYLOAD_MAGIC) {
		return 0;
	}
	const uint8_t *p = (const uint8_t *)(ph + 1), *end = p + ph->size;
	while (p + sizeof(struct payload_entry) <= end) {
		const struct payload_entry *e = (const struct payload_entry *)p;
		if (e->name[0] == 0) {
			break;
		}
		if (strcmp(e->name, "kernel") == 0) {
			pi->kernel = p + sizeof(*e);
			pi->kernel_size = e->size;
		} else if (strcmp(e->name, "ramdisk") == 0) {
			pi->ramdisk = p + sizeof(*e);
			pi->ramdisk_size = e->size;
		} else if (strcmp(e->name, "cmdline") == 0) {
			pi->cmdline = (const char *)(p + sizeof(*e));
		} else if (strcmp(e->name, "touchfw") == 0) {
			pi->touchfw = p + sizeof(*e);
			pi->touchfw_size = e->size;
		}
		p += sizeof(*e) + ((e->size + 15) & ~15u);
	}
	return pi->kernel != NULL;
}

/* ---- Mach-O ---- */

#define MH_MAGIC_64         0xfeedfacfu
#define LC_SEGMENT_64       0x19
#define LC_SYMTAB           0x2
#define LC_UNIXTHREAD       0x5

struct mach_header_64 {
	uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved;
};

struct load_command {
	uint32_t cmd, cmdsize;
};

struct segment_command_64 {
	uint32_t cmd, cmdsize;
	char segname[16];
	uint64_t vmaddr, vmsize, fileoff, filesize;
	uint32_t maxprot, initprot, nsects, flags;
};

struct symtab_command {
	uint32_t cmd, cmdsize, symoff, nsyms, stroff, strsize;
};

struct nlist_64 {
	uint32_t n_strx;
	uint8_t n_type, n_sect;
	uint16_t n_desc;
	uint64_t n_value;
};

/* The kernel's symbols the loader uses, by name; 0 when the kernel has not got one. */
enum {
	SYM_VC_COLORS, SYM_LOG, SYM_LOG_LEN, SYM_LOG_RING_AT, SYM_LOG_WRAPPED, SYM_LOG_PUTC,
	SYM_UART_TX_READY, SYM_UART_TX_DATA, SYM_UART_RX_READY, SYM_STUB_SLOT, SYM_COUNT
};
static const char sym_names[SYM_COUNT][32] = {
	"_vc_colors", "_loader_log", "_loader_log_len", "_loader_log_ring_at", "_loader_log_wrapped",
	"_loader_log_putc", "_vmapple_uart_transmit_ready", "_vmapple_uart_transmit_data",
	"_vmapple_uart_receive_ready", "_loader_stub_slot",
};

static void
find_syms(const uint8_t *kernel, uint64_t *syms)
{
	const struct mach_header_64 *mh = (const struct mach_header_64 *)kernel;
	const struct load_command *lc = (const struct load_command *)(mh + 1);
	const struct symtab_command *st = NULL;

	memset(syms, 0, SYM_COUNT * sizeof(uint64_t));
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SYMTAB) {
			st = (const void *)lc;
		}
	}
	if (!st) {
		return;
	}
	const struct nlist_64 *sym = (const struct nlist_64 *)(kernel + st->symoff);
	const char *strs = (const char *)(kernel + st->stroff);
	for (uint32_t i = 0; i < st->nsyms; i++) {
		const char *n = strs + sym[i].n_strx;
		if (n[0] != '_' || (n[1] != 'l' && n[1] != 'v')) {
			continue;
		}
		for (unsigned k = 0; k < SYM_COUNT; k++) {
			if (!syms[k] && strcmp(n, sym_names[k]) == 0) {
				syms[k] = sym[i].n_value;
			}
		}
	}
}

/* ---- boot_args (pexpert/arm64/boot.h) ---- */

struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

#define BOOT_LINE_LENGTH 608

struct boot_args {
	uint16_t revision;
	uint16_t version;
	uint64_t virtBase;
	uint64_t physBase;
	uint64_t memSize;
	uint64_t topOfKernelData;
	struct boot_video video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char commandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags;
	uint64_t memSizeActual;
};

_Static_assert(__builtin_offsetof(struct boot_args, virtBase) == 8, "boot_args");
_Static_assert(__builtin_offsetof(struct boot_args, physBase) == 16, "boot_args");
_Static_assert(__builtin_offsetof(struct boot_args, video) == 40, "boot_args");
_Static_assert(__builtin_offsetof(struct boot_args, deviceTreeP) == 96, "boot_args");
_Static_assert(__builtin_offsetof(struct boot_args, commandLine) == 108, "boot_args");
_Static_assert(__builtin_offsetof(struct boot_args, bootFlags) == 720, "boot_args");

/* ---- XNU's device tree (Apple's flattened format, as mtkloader writes it) ---- */

struct dt_writer {
	uint8_t *base, *cur;
	uint32_t *counts[16];
	int depth;
};

static void
dt_prop(struct dt_writer *w, const char *name, const void *data, uint32_t len)
{
	memset(w->cur, 0, 32);
	memcpy(w->cur, name, strlen(name));
	*(volatile uint32_t *)(w->cur + 32) = len;
	memset(w->cur + 36, 0, (len + 3) & ~3u);
	memcpy(w->cur + 36, data, len);
	w->cur += 36 + ((len + 3) & ~3u);
	w->counts[w->depth - 1][0]++;
}

static void dt_prop_str(struct dt_writer *w, const char *name, const char *value) { dt_prop(w, name, value, (uint32_t)strlen(value) + 1); }
static void dt_prop_u32(struct dt_writer *w, const char *name, uint32_t value) { dt_prop(w, name, &value, sizeof(value)); }
static void dt_prop_u64(struct dt_writer *w, const char *name, uint64_t value) { dt_prop(w, name, &value, sizeof(value)); }

static void
dt_prop_padded(struct dt_writer *w, const char *name, const char *value, uint32_t size)
{
	uint8_t field[64];
	memset(field, 0, sizeof(field));
	memcpy(field, value, strlen(value));
	dt_prop(w, name, field, size);
}

static void
dt_begin(struct dt_writer *w, const char *name)
{
	if (w->depth > 0) {
		w->counts[w->depth - 1][1]++;
	}
	w->counts[w->depth++] = (uint32_t *)w->cur;
	memset(w->cur, 0, 8);
	w->cur += 8;
	dt_prop_str(w, "name", name);
}

static void dt_end(struct dt_writer *w) { w->depth--; }

/* An empty NVRAM image in CHRP format (iokit/Kernel/IONVRAMCHRPHandler.cpp): IOKit needs one. */
#define NVRAM_BANK_SIZE 0x2000u

static void
nvram_chrp_header(uint8_t *h, uint8_t sig, uint32_t bytes, const char *name)
{
	uint16_t blocks = (uint16_t)(bytes / 16), sum;

	memset(h, 0, 16);
	h[0] = sig;
	h[2] = (uint8_t)blocks;
	h[3] = (uint8_t)(blocks >> 8);
	memcpy(h + 4, name, strlen(name));
	sum = h[0];
	for (int i = 2; i < 16; i++) {
		sum += h[i];
	}
	while (sum > 0xff) {
		sum = (sum & 0xff) + (sum >> 8);
	}
	h[1] = (uint8_t)sum;
}

static void
build_nvram_image(uint8_t *image)
{
	uint32_t lo = 1, hi = 0, generation = 1;

	memset(image, 0, NVRAM_BANK_SIZE);
	nvram_chrp_header(image, 0x5a, 0x20, "nvram");
	nvram_chrp_header(image + 0x20, 0x70, NVRAM_BANK_SIZE - 0x20, "common");
	memcpy(image + 20, &generation, 4);
	for (uint32_t i = 20; i < NVRAM_BANK_SIZE; i++) {
		lo = (lo + image[i]) % 65521;
		hi = (hi + lo) % 65521;
	}
	uint32_t adler = (hi << 16) | lo;
	memcpy(image + 16, &adler, 4);
}

#endif /* XNU_H */
