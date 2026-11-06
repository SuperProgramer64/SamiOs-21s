/*
 * exyloader: the A21s's loader (Exynos 850), the successor of hakios-a32's mtkloader.
 *
 * Like mtkloader it sits in `boot` in front of Android's own kernel (tools/chainboot.py) and is
 * started by S-Boot as that kernel would be (0x80080000, EL1, MMU and D-cache off, x0 = dtb). It
 * is built in numbered steps, each read only first and each saying its result in the log
 * (the ramoops pmsg zone, /sys/fs/pstore/pmsg-ramoops-0 on the next Android boot). After the
 * steps it gives the phone back to Android, the way a21sdiag does.
 *
 *   step 1  the eMMC from bare metal: DW MMC 0 by PIO (dwmmc.h). Reads blocks whose contents are
 *           known (the MBR, the GPT header, `boot`'s first block), finds `boot` and `super` by name
 *           in the GPT, reads the first block of the xnusys region (tools/xnusys.py), and times
 *           1 MiB of reads. Nothing is written to the card.
 *
 * To come: the XNU payload and kernel off the eMMC, XNU's device tree and boot arguments
 * (mtkloader's dt_* and identity), the watchdogs handed over, the jump.
 *
 * STEP=N at build time chooses how far it goes (build.sh).
 */
#include <stdint.h>
#include "xnu.h"
#include "exydbg.h"
#include "dwmmc.h"
#include "logselect.h"
#include "../xnuloader/identity.h"

#ifndef STEP
#define STEP 1
#endif

#define CHAIN_OFFSET  0x40000u

/* The xnusys region in super's free tail (tools/xnusys.py plan, 2026-11-04). */
#define XNUSYS_LBA      12216320ULL
#define XNUSYS_SECTORS  8075264ULL
/* ... whose last 64 MiB hold the XNU payload (kernel + RAM disk, tools/xnusys.py payload) */
#define PAYLOAD_SECTORS 131072ULL
#define PAYLOAD_LBA     (XNUSYS_LBA + XNUSYS_SECTORS - PAYLOAD_SECTORS)

/*
 * XNU's memory: the 2 GiB at 0x880000000, four 512 MiB ranks in a row that Android uses as plain
 * RAM and in which no firmware reserves anything (every fixed reserved-memory node of the device
 * tree - the modem, the TEE, EL3, the cameras, audio, GNSS, the boot framebuffer - is below 4 GiB).
 */
#define XNU_PHYS_BASE       0x880000000ULL
#define XNU_MEM_SIZE        0x80000000ULL
#define KERNEL_VIRT_BASE    0xfffffff004000000ULL      /* boot_args.virtBase, as xnuloader has it */
#define XNU_SLIDE           (XNU_PHYS_BASE - KERNEL_VIRT_BASE)
#define PAGE_SIZE_MAX       0x4000ULL
/* where the payload is read to: 1 GiB into XNU's memory, far above where the kernel lands */
#define STAGING_PA          (XNU_PHYS_BASE + 0x40000000ULL)
/*
 * Outside XNU's memory, in RAM nobody else reserves (0xc0000000 + 512 MiB, Android's plain RAM):
 * the panic log ("pram"), a page the kernel's serial driver may write its PL011 registers into
 * (it is given this instead of the phone's real UART, and its text goes to loader_log), and the
 * note that tells the next boot where to find this run's log.
 */
#define PANIC_LOG_BASE      0xc1000000ULL
#define PANIC_LOG_SIZE      0x80000u
#define UART_SCRATCH        0xc1100000ULL
/*
 * The note lives in the last 64 bytes of the pmsg zone: the first XNU run showed that RAM at
 * 0xc1200000 does not survive a forced restart (S-Boot uses it), while the pmsg zone always has.
 */
#define RESCUE_PA           (RAMOOPS_PMSG + RAMOOPS_PMSG_SIZE - 64)
#define RESCUE_MAGIC        0x4150474f4c554e58ULL       /* "XNULOGPA" */
#define LOG_HEAD            (256 * 1024)               /* osfmk/arm64/loader_return.c */
#define LOG_RING            (1024 * 1024)
#define ARM_IO_BASE         0x10000000ULL
/* pmu_alive (docs/hardware-a21s.md); offsets from Linux's s3c2410_wdt.c for the Exynos 850 */
#define PMU_BASE            0x11860000ULL
#define PMU_RST_STAT        0x0404u     /* bit 24: the last reset was cluster 0's watchdog */
#define PMU_CL0_NONCPU_OUT  0x1220u
#define PMU_CL0_NONCPU_INT_EN 0x1244u
/* A pointer in the note is usable only if it is inside XNU's memory: a cold restart flips bits. */
#define PA_IN_XNU(p, n)     ((p) >= XNU_PHYS_BASE && (p) + (n) <= XNU_PHYS_BASE + XNU_MEM_SIZE)

/* Android asks for one XNU run by writing this to /dev/pmsg0 (root); it is taken away when seen. */
#define BOOT_XNU_FLAG       "HAKIOS:BOOT-XNU"

#ifndef HAKIOS_SMP_PROBE
#define HAKIOS_SMP_PROBE 0
#endif

#ifndef XNU_CORES
#define XNU_CORES           1
#endif
/*
 * XNU_FIRST=1: XNU at every boot that does not follow an XNU run, without Android asking. After a run
 * (left with Volume Down + Power) the note at RESCUE_PA is still there, and that boot is Android's,
 * with the run's log; the boot after that is XNU's again. So the phone alternates and never loops.
 */
#ifndef XNU_FIRST
#define XNU_FIRST           0
#endif
#ifndef XNU_VERBOSE
#define XNU_VERBOSE         1           /* the kernel's text console on the screen (-v) */
#endif
#ifndef XNU_EXTRA_ARGS
#define XNU_EXTRA_ARGS      "mali=2 buttons=3 fb_beam=0"  /* fb_beam: Qualcomm DPU registers, not here */
#endif
#define XNU_SERIAL          "HAKIOS0000000000"  /* a placeholder, not any real device's */
#define STR2(x) #x
#define STR(x) STR2(x)
/*
 * The root: the payload's RAM disk (md0), or with XNU_EMMC_ROOT=1 the iOS system image on the eMMC,
 * read by the kernel (bsd/dev/hakios.dwmmc.c) from the xnusys region less its last 64 MiB (the
 * payload's): xnusys.py plan gives lba 12216320, 8075264 sectors.
 */
#ifndef XNU_EMMC_ROOT
#define XNU_EMMC_ROOT       0
#endif
#define XNUSYS_LBA_DEC      12216320    /* XNUSYS_LBA, without its ULL: for STR() */
#define XNUSYS_IMAGE_SECTORS 7944192     /* 8075264 - 131072: a literal, for STR() */
#ifndef XNU_WDT_RUN
#define XNU_WDT_RUN 2700
#endif
#if XNU_EMMC_ROOT
/* wdt_run: the kernel feeds the watchdog that long, so the run outlasts its 80 s and still comes back */
#define XNU_ROOT_ARG        "rd=md0 emmc_root=1 emmc_lba=" STR(XNUSYS_LBA_DEC) " emmc_sectors=" STR(XNUSYS_IMAGE_SECTORS) " wdt_run=" STR(XNU_WDT_RUN)
#else
#define XNU_ROOT_ARG        "rd=md0"
#endif
/* XNU_VAR=1: the persistent /private/var of docs/storage-2026-11-05.md (a number: -D cannot pass a string) */
#if defined(XNU_VAR) && XNU_VAR && !defined(XNU_STORAGE_ARGS)
#define XNU_STORAGE_ARGS "emmc_multi=1 emmc_write=1 emmc_var_lba=17068032 emmc_var_sectors=3092480 var_hfs=emmc1"
#endif
#ifndef XNU_STORAGE_ARGS
#define XNU_STORAGE_ARGS "emmc_multi=1"
#endif
/* psci_ids=1: CPU_ON takes the device tree's ids (0x1, 0x100...), not the MPIDRs (osfmk/arm64/psci_smp.c) */
#if XNU_CORES > 1
#if defined(XNU_PSCI_LEARN) && XNU_PSCI_LEARN
#define XNU_SMP_ARGS        " psci_ids=1 psci_learn=1"
#else
#define XNU_SMP_ARGS        " psci_ids=1"
#endif
#else
#define XNU_SMP_ARGS        ""
#endif
/* XNU_IMPORTS=1: the ios-a135f drivers -- TD4150 touch, eMMC IDMAC reads, USB CDC-ECM + live log */
#ifndef XNU_IMPORT_ARGS
#if defined(XNU_IMPORTS) && XNU_IMPORTS
#define XNU_IMPORT_ARGS     " touchscreen=4 emmc_dma=1 a135f_usb=2"
#else
#define XNU_IMPORT_ARGS     ""
#endif
#endif /* XNU_IMPORT_ARGS override */
#define XNU_BOOT_ARGS "debug=0x8 serial=3 " XNU_ROOT_ARG " cpus=" STR(XNU_CORES) " " XNU_EXTRA_ARGS " " XNU_STORAGE_ARGS XNU_SMP_ARGS XNU_IMPORT_ARGS
_Static_assert(XNU_CORES >= 1 && XNU_CORES <= 8, "cores");

extern void jump_to_kernel(uint64_t entry_pa, uint64_t boot_args_pa) __attribute__((noreturn));
#define SYSREG(name) ({ uint64_t _v; __asm__ volatile("mrs %0, " #name : "=r"(_v)); _v; })

struct rescue {
	uint64_t magic;
	uint64_t log_pa, log_len_pa, ring_at_pa, wrapped_pa;
	uint64_t run;
};

extern char _start[];
extern char chain_stub[], chain_stub_end[];
static uint64_t chain_dtb;
#if HAKIOS_SMP_PROBE
static int hakios_smp_active;
static void hakios_smp_abort(void) __attribute__((noreturn));
#endif

static uint8_t blk[512] __attribute__((aligned(16)));
#ifndef XNU_PSCI_TRAMPOLINE
#define XNU_PSCI_TRAMPOLINE 0
#endif
#if XNU_CORES > 1 && XNU_PSCI_TRAMPOLINE
/* the cores' trampoline (psci_trampoline=): nothing of ours uses it, the kernel writes it */
static uint8_t psci_trampoline_page[4096] __attribute__((aligned(4096)));
#endif
static uint8_t gpt_entries[512 * 16] __attribute__((aligned(16)));

static void __attribute__((noreturn))
chain_android(void)
{
	const volatile uint64_t *hdr = (const volatile uint64_t *)_start;
	const volatile uint32_t *img = (const volatile uint32_t *)((uint64_t)_start + CHAIN_OFFSET);
	uint64_t image_size = (hdr[5] + 7) & ~7ULL;
	uint64_t covered = hdr[2];

	if (img[14] != 0x644d5241u || image_size == 0) {
		dbg_puts("exyloader: no Linux Image to chain to; waiting forever\n");
		for (;;) {
			wdt_pet();
			__asm__ volatile("wfe");
		}
	}
	dbg_puts("exyloader: back to Android\n");
	uint64_t stub_pa = ((uint64_t)_start + CHAIN_OFFSET + image_size + 0xfff) & ~0xfffULL;
	if (stub_pa + 0x1000 > (uint64_t)_start + covered) {
		stub_pa = (uint64_t)_start + covered - 0x1000;
	}
	volatile uint32_t *dst = (volatile uint32_t *)stub_pa;
	const volatile uint32_t *src = (const volatile uint32_t *)chain_stub;
	for (uint64_t i = 0; i < (uint64_t)(chain_stub_end - chain_stub) / 4; i++) {
		dst[i] = src[i];
	}
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	wdt_pet();
	void (*stub)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) = (void *)stub_pa;
	stub(chain_dtb, (uint64_t)_start, (uint64_t)img, image_size, (uint64_t)_start);
	__builtin_unreachable();
}

void
diag_fault(uint64_t vec, uint64_t esr, uint64_t elr, uint64_t far)
{
	fbc.fg = 0xffff4040u;
	dbg_puts("\nexyloader: FAULT vec=");
	dbg_hex(vec, 3);
	dbg_puts(" ESR=");
	dbg_hex(esr, 8);
	dbg_puts(" ELR=");
	dbg_hex(elr, 10);
	dbg_puts(" FAR=");
	dbg_hex(far, 10);
	dbg_puts("\n");

#if HAKIOS_SMP_PROBE
	if (hakios_smp_active) {
		hakios_smp_abort();
	}
#endif
	dbg_delay_ms(3000);
	chain_android();
}

static void kv(const char *k, uint64_t v, int digits)
{
	dbg_puts(k);
	dbg_putc('=');
	dbg_hex(v, digits);
	dbg_putc(' ');
}

#if HAKIOS_SMP_PROBE
#include "smp_exynos.h"
#endif

static int same(const uint8_t *a, const char *b, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		if (a[i] != (uint8_t)b[i]) {
			return 0;
		}
	}
	return 1;
}

static uint64_t le64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--) {
		v = v << 8 | p[i];
	}
	return v;
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void show16(const uint8_t *p)
{
	for (int i = 0; i < 16; i++) {
		dbg_hex(p[i], 2);
		dbg_putc(i == 7 ? '-' : ' ');
	}
	dbg_putc('\n');
}

/* Does a GPT entry's UTF-16LE name equal this ASCII one? */
static int gpt_name_is(const uint8_t *e, const char *name)
{
	const uint8_t *n = e + 56;
	uint32_t i = 0;
	for (; name[i]; i++) {
		if (n[2 * i] != (uint8_t)name[i] || n[2 * i + 1] != 0) {
			return 0;
		}
	}
	return n[2 * i] == 0 && n[2 * i + 1] == 0;
}

static int step_emmc(void)
{
	struct dwmmc h;
	int pass = 0, tests = 0;

	dbg_puts("step 1: eMMC (DW MMC 0 by PIO)\n");
	dw_poll = wdt_pet;
	struct dwmmc raw = { .base = DWMMC0_BASE };
	kv("CTRL", dw_rd(&raw, DW_CTRL), 8);
	kv("BMOD", dw_rd(&raw, DW_BMOD), 8);
	kv("CTYPE", dw_rd(&raw, DW_CTYPE), 8);
	dbg_putc('\n');
	kv("CLKDIV", dw_rd(&raw, DW_CLKDIV), 8);
	kv("CLKENA", dw_rd(&raw, DW_CLKENA), 8);
	kv("STATUS", dw_rd(&raw, DW_STATUS), 8);
	dbg_putc('\n');
	kv("FIFOTH", dw_rd(&raw, DW_FIFOTH), 8);
	kv("INTMASK", dw_rd(&raw, DW_INTMASK), 8);
	kv("TMOUT", dw_rd(&raw, DW_TMOUT), 8);
	dbg_putc('\n');

	if (!dwmmc_open(&h, DWMMC0_BASE)) {
		kv("open failed, err", h.last_err, 8);
		dbg_putc('\n');
		return 0;
	}
	kv("VERID", h.verid, 8);
	kv("HCON", h.hcon, 8);
	dbg_putc('\n');
	dbg_puts("FIFO ");
	dbg_dec(h.width * 8);
	dbg_puts(" bits at +");
	dbg_hex(h.fifo - h.base, 3);
	dbg_puts(h.hold ? ", hold reg\n" : ", no hold reg\n");

	uint32_t r1 = 0;
	for (uint32_t rca = 1; rca <= 2; rca++) {
		if (dwmmc_status(&h, rca, &r1)) {
			dbg_puts("CMD13 rca=");
			dbg_dec(rca);
			kv(" R1", r1, 8);
			dbg_puts("state=");
			dbg_dec((r1 >> 9) & 0xf);
			dbg_puts((((r1 >> 9) & 0xf) == 4) ? " (transfer)\n" : "\n");
			break;
		}
		dbg_puts("CMD13 rca=");
		dbg_dec(rca);
		kv(" failed, err", h.last_err, 8);
		dbg_putc('\n');
	}

	/* LBA 0: the protective MBR */
	tests++;
	if (dwmmc_read_block(&h, 0, blk) && blk[510] == 0x55 && blk[511] == 0xaa) {
		pass++;
		dbg_puts("LBA 0: MBR 55 aa  OK\n");
	} else {
		kv("LBA 0: FAIL err", h.last_err, 8);
		dbg_putc('\n');
		show16(blk + 496);
	}

	/* LBA 1: the GPT header, and the entries it points to */
	uint64_t boot_lba = 0, super_lba = 0, super_size = 0;
	tests++;
	if (dwmmc_read_block(&h, 1, blk) && same(blk, "EFI PART", 8)) {
		pass++;
		uint64_t ent = le64(blk + 72);
		uint32_t n = le32(blk + 80), es = le32(blk + 84);
		dbg_puts("LBA 1: GPT  OK  entries at ");
		dbg_dec(ent);
		dbg_puts(", ");
		dbg_dec(n);
		dbg_puts(" x ");
		dbg_dec(es);
		dbg_putc('\n');
		uint32_t blocks = (n * es + 511) / 512;
		if (blocks > sizeof(gpt_entries) / 512) {
			blocks = sizeof(gpt_entries) / 512;
		}
		tests++;
		if (es == 128 && dwmmc_read(&h, ent, blocks, gpt_entries)) {
			for (uint32_t i = 0; i < blocks * 512 / es; i++) {
				const uint8_t *e = gpt_entries + i * es;
				if (gpt_name_is(e, "boot")) {
					boot_lba = le64(e + 32);
				} else if (gpt_name_is(e, "super")) {
					super_lba = le64(e + 32);
					super_size = le64(e + 40) + 1 - super_lba;
				}
			}
			if (boot_lba && super_lba) {
				pass++;
			}
			dbg_puts("GPT: boot at ");
			dbg_dec(boot_lba);
			dbg_puts(", super at ");
			dbg_dec(super_lba);
			dbg_puts(" +");
			dbg_dec(super_size);
			dbg_puts(boot_lba && super_lba ? "  OK\n" : "  NOT FOUND\n");
		} else {
			kv("GPT entries: FAIL err", h.last_err, 8);
			dbg_putc('\n');
		}
	} else {
		kv("LBA 1: FAIL err", h.last_err, 8);
		dbg_putc('\n');
		show16(blk);
	}

	/* boot's first block: the Android boot image we are running out of */
	if (boot_lba) {
		tests++;
		if (dwmmc_read_block(&h, boot_lba, blk) && same(blk, "ANDROID!", 8)) {
			pass++;
			dbg_puts("boot: ANDROID!  OK\n");
		} else {
			kv("boot: FAIL err", h.last_err, 8);
			dbg_putc('\n');
			show16(blk);
		}
	}

	/* the xnusys region: inside super, its first block (zeros until tools/xnusys.py writes it) */
	if (super_lba) {
		tests++;
		int inside = XNUSYS_LBA >= super_lba && XNUSYS_LBA + XNUSYS_SECTORS <= super_lba + super_size;
		dbg_puts(inside ? "xnusys region inside super  OK\n" : "xnusys region NOT inside super\n");
		pass += inside;
		if (dwmmc_read_block(&h, XNUSYS_LBA, blk)) {
			dbg_puts("xnusys first block: ");
			show16(blk);
			dbg_puts(same(blk + 32, "NXSB", 4) ? "  an APFS container (system.apfs written)\n" :
			    "  no APFS container yet\n");
		}
	}

	/* speed: 1 MiB of single-block reads out of boot */
	if (boot_lba) {
		uint64_t t0 = dw_now();
		int ok = 1;
		for (uint32_t i = 0; i < 2048 && ok; i++) {
			ok = dwmmc_read_block(&h, boot_lba + i, blk);
		}
		uint64_t us = (dw_now() - t0) * 1000000 / dw_hz();
		dbg_puts("1 MiB in ");
		dbg_dec(us / 1000);
		dbg_puts(" ms");
		if (ok && us) {
			dbg_puts(" = ");
			dbg_dec(1048576ULL * 1000000 / us / 1024);
			dbg_puts(" KiB/s");
		} else {
			kv(" FAIL err", h.last_err, 8);
		}
		dbg_putc('\n');
	}

	dwmmc_close(&h);
	dbg_puts("step 1: ");
	dbg_dec(pass);
	dbg_putc('/');
	dbg_dec(tests);
	dbg_puts(pass == tests ? " passed\n" : " passed: SEE ABOVE\n");
	return pass == tests;
}

/* ---- Android or XNU ---- */

/*
 * Did Android ask for an XNU run? `echo HAKIOS:BOOT-XNU > /dev/pmsg0` (as root) puts the words into
 * the pmsg zone, which survives the reboot. Looked for before our own log overwrites the zone, and
 * wiped where found: one request is one run, so the boot after it is Android's again.
 */
static int
xnu_requested(void)
{
	static const char flag[] = BOOT_XNU_FLAG;
	const uint32_t n = sizeof(flag) - 1;
	volatile char *z = (volatile char *)(RAMOOPS_PMSG + 12);
	int found = 0;

	for (uint32_t i = 0; i + n <= RAMOOPS_PMSG_SIZE - 12; i++) {
		uint32_t k = 0;
		while (k < n && z[i + k] == flag[k]) {
			k++;
		}
		if (k == n) {
			for (k = 0; k < n; k++) {
				z[i + k] = '-';
			}
			found = 1;
		}
	}
	return found;
}

/* The previous boot's own loader text (the zone's contents before plog_reset), kept after an XNU run. */
static char prev_log[RAMOOPS_PMSG_SIZE];
static uint32_t prev_len;

static void
keep_previous_loader_log(void)
{
	uint32_t start = PLOG_HDR->start, size = PLOG_HDR->size;

	prev_len = 0;
	if (PLOG_HDR->sig != RAMOOPS_SIG || start >= PLOG_CAP || size > PLOG_CAP) {
		return;
	}
	/* oldest first: [start, CAP) then [0, start) once wrapped, else [0, size) */
	uint32_t from = size == PLOG_CAP ? start : 0;
	for (uint32_t i = 0; i < size && i < sizeof(prev_log); i++) {
		prev_log[i] = PLOG_DATA[(from + i) % PLOG_CAP];
	}
	prev_len = size < sizeof(prev_log) ? size : sizeof(prev_log);
}

static void plog_raw(const volatile char *p, uint64_t n)
{
	uint64_t fb = fbc.base;

	fbc.base = 0;                       /* into the log only */
	for (uint64_t i = 0; i < n; i++) {
		char c = p[i];
		dbg_putc(c == 0 ? '.' : c);
	}
	fbc.base = fb;
}

/*
 * The previous run's kernel log, if there was one: exyloader leaves a note at RESCUE_PA saying where
 * the kernel's loader_log is (the kernel always lands at the same address), and a forced restart
 * (Volume Down + Power) keeps the RAM. Its start and its end go into the pmsg zone for Android.
 */
static int log_rescued;                 /* the previous run's log is in pmsg already: once */

static void
rescue_previous_log(void)
{
	volatile struct rescue *r = (volatile struct rescue *)RESCUE_PA;

	if (r->magic != RESCUE_MAGIC) {
		return;
	}
	r->magic = 0;
	/*
	 * 2026-11-04: after a forced restart the magic was intact but a pointer was not (FAR
	 * 0xc8356c9c0, outside DRAM): the read was an external abort and the log was never shown.
	 */
	if (!PA_IN_XNU(r->log_pa, LOG_HEAD + LOG_RING) || !PA_IN_XNU(r->log_len_pa, 4) ||
	    !PA_IN_XNU(r->ring_at_pa, 4) || !PA_IN_XNU(r->wrapped_pa, 1) ||
	    (r->log_len_pa & 3) || (r->ring_at_pa & 3)) {
		dbg_puts("the note about the previous XNU run is damaged (RAM not kept by a cold restart)\n");
		kv("log", r->log_pa, 10);
		kv(" len", r->log_len_pa, 10);
		kv(" ring_at", r->ring_at_pa, 10);
		dbg_putc('\n');
		return;
	}
	uint32_t len = *(volatile uint32_t *)r->log_len_pa;
	uint32_t ring_at = *(volatile uint32_t *)r->ring_at_pa;
	uint8_t wrapped = *(volatile uint8_t *)r->wrapped_pa;
	const volatile char *log = (const volatile char *)r->log_pa;

	dbg_puts("=== the previous XNU run's log (loader_log ");
	dbg_dec(len);
	dbg_puts(wrapped ? " bytes, wrapped) ===\n" : " bytes) ===\n");
	if (len > LOG_HEAD + LOG_RING || ring_at >= LOG_RING) {
		dbg_puts("(the kernel's log counters make no sense: RAM not kept, or a different kernel)\n");
		return;
	}
	log_rescued = 1;
	/*
	 * Android shows only 16 KiB of the pmsg zone (pmsg-ramoops-0 is 16372 bytes), and a panic ends
	 * the log with ~6 KiB of binary stackshot. So: the banner, then the 10 KiB of text before the
	 * panic buffer (or before the end), then the panic's first lines -- what led up to the end.
	 */
	uint32_t head = len < 1024 ? len : 1024;
	plog_raw(log, head);
	dbg_puts("\n[...]\n");
	struct hakios_storage_summary storage = {0};
	if (wrapped) {
		hakios_storage_summary_add(&storage, log, LOG_HEAD);
		hakios_storage_summary_add(&storage, log + LOG_HEAD + ring_at, LOG_RING - ring_at);
		hakios_storage_summary_add(&storage, log + LOG_HEAD, ring_at);
	} else {
		hakios_storage_summary_add(&storage, log, len);
	}
	dbg_puts("--- storage: initial checks and latest statistics ---\n");
	plog_raw(storage.first, storage.first_len);
	if (storage.last_len) {
		dbg_puts("[...]\n");
		plog_raw(storage.last, storage.last_len);
	}
	dbg_puts("--- CPU result and first storage failures ---\n");
	plog_raw(storage.cores, storage.cores_len);
	plog_raw(storage.faults, storage.faults_len);
	/*
	 * User space's lines, wherever they are in the log: the kernel's own all begin with its
	 * "[  seconds]: " stamp, what programs write to /dev/console (km.c's kmoutput) does not.
	 * Diagnostics have 2 KiB above; leave 5 KiB for user-space messages.
	 */
	{
		/* The initial LOG_HEAD bytes are retained even after the ring wraps. */
		uint32_t linear_len = wrapped ? LOG_HEAD : len;
		uint32_t shown = 0, lines = 0;
		dbg_puts("--- the lines that are not the kernel's (user space) ---\n");
		/* from the first stamped line on: the early boot's lines have no stamp, and are kernel's */
		uint32_t first = head;
		while (first + 1 < linear_len && !(log[first] == '\n' && log[first + 1] == '[')) {
			first++;
		}
		for (uint32_t i = first + 1; i < linear_len && shown < 5120; ) {
			uint32_t e = i;
			while (e < linear_len && log[e] != '\n') {
				e++;
			}
			if (e > i && log[i] != '[' && log[i] != ' ' && log[i] != '\t') {
				uint32_t n = e - i > 200 ? 200 : e - i;
				plog_raw(log + i, n);
				dbg_putc('\n');
				shown += n + 1;
				lines++;
			}
			i = e + 1;
		}
		dbg_puts("--- ");
		dbg_dec(lines);
		dbg_puts(" lines ---\n");
	}
	uint32_t tail = 4096;
	if (len <= LOG_HEAD || !wrapped) {
		static const char mark[] = "--- panic buffer ---";
		uint32_t end = len, panic_at = 0;
		for (uint32_t i = head; i + sizeof(mark) - 1 <= len; i++) {
			uint32_t k = 0;
			while (k < sizeof(mark) - 1 && log[i + k] == mark[k]) {
				k++;
			}
			if (k == sizeof(mark) - 1) {
				panic_at = i;
			}
		}
		if (panic_at) {
			end = panic_at;
		}
		uint32_t from = end > head + tail ? end - tail : head;
		plog_raw(log + from, end - from);
		if (panic_at) {
			uint32_t n = len - panic_at < 1200 ? len - panic_at : 1200;
			plog_raw(log + panic_at, n);     /* the panic string, the boot args, the first lines */
		}
	} else {
		/* the ring's newest bytes end at ring_at */
		const volatile char *ring = log + LOG_HEAD;
		if (ring_at >= tail) {
			plog_raw(ring + ring_at - tail, tail);
		} else {
			plog_raw(ring + LOG_RING - (tail - ring_at), tail - ring_at);
			plog_raw(ring, ring_at);
		}
	}
	dbg_puts("\n=== end of the previous XNU run's log ===\n");
}

static int step_payload(struct payload_info *pi);
static void timer_probe(void);
static uint64_t kva_to_pa(uint64_t va);

/*
 * Is the previous XNU run's kernel still in RAM? Its first segment lands at 0x883004000 every time,
 * so a Mach-O header there means a run happened and the RAM was kept through the restart. Then the
 * payload on the eMMC says where loader_log is (the same kernel), and its start and end go to pmsg.
 */
static void
rescue_from_ram(void)
{
	volatile uint32_t *k = (volatile uint32_t *)kva_to_pa(0xfffffff007004000ULL);
	if (log_rescued) {
		k[0] = 0;                               /* rescue_previous_log() has shown it: once */
		return;
	}
	struct payload_info pi;
	uint64_t syms[SYM_COUNT];
	struct rescue r;

	if (k[0] != MH_MAGIC_64 || k[1] != 0x0100000c) {    /* arm64 */
		kv("no XNU kernel in RAM (no run since, or RAM not kept); word", k[0], 8);
		dbg_putc('\n');
		return;
	}
	dbg_puts("the previous XNU run's kernel is still in RAM: its log\n");
	if (!step_payload(&pi)) {
		return;
	}
	find_syms(pi.kernel, syms);
	if (!syms[SYM_LOG] || !syms[SYM_LOG_LEN] || !syms[SYM_LOG_RING_AT] || !syms[SYM_LOG_WRAPPED]) {
		dbg_puts("the payload's kernel has no loader_log\n");
		return;
	}
	r.log_pa = kva_to_pa(syms[SYM_LOG]);
	r.log_len_pa = kva_to_pa(syms[SYM_LOG_LEN]);
	r.ring_at_pa = kva_to_pa(syms[SYM_LOG_RING_AT]);
	r.wrapped_pa = kva_to_pa(syms[SYM_LOG_WRAPPED]);
	r.magic = RESCUE_MAGIC;
	*(volatile struct rescue *)RESCUE_PA = r;
	rescue_previous_log();
	k[0] = 0;                                           /* once */
}

/* ---- step 2: the payload off the eMMC ---- */

static int
step_payload(struct payload_info *pi)
{
	struct dwmmc h;
	const struct payload_header *ph = (const struct payload_header *)STAGING_PA;

	dbg_puts("step 2: the XNU payload off the eMMC (LBA ");
	dbg_dec(PAYLOAD_LBA);
	dbg_puts(")\n");
	dw_poll = wdt_pet;
	if (!dwmmc_open(&h, DWMMC0_BASE) || !dwmmc_read_block(&h, PAYLOAD_LBA, (void *)STAGING_PA)) {
		kv("eMMC failed, err", h.last_err, 8);
		dbg_putc('\n');
		dwmmc_close(&h);
		return 0;
	}
	if (ph->magic != PAYLOAD_MAGIC || ph->size == 0 || ph->size + 8 > PAYLOAD_SECTORS * 512) {
		dbg_puts("no payload there (tools/xnusys.py payload writes one)\n");
		dwmmc_close(&h);
		return 0;
	}
	uint64_t blocks = (ph->size + 8 + 511) / 512, t0 = dbg_ticks(), shown = 0;
	dbg_puts("payload ");
	dbg_dec(ph->size + 8);
	dbg_puts(" bytes: ");
	for (uint64_t b = 1; b < blocks; b++) {
		if (!dwmmc_read_block(&h, PAYLOAD_LBA + b, (void *)(STAGING_PA + b * 512))) {
			kv("\nread failed at block", b, 8);
			kv("err", h.last_err, 8);
			dbg_putc('\n');
			dwmmc_close(&h);
			return 0;
		}
		if (b * 10 / blocks > shown) {
			shown = b * 10 / blocks;
			dbg_dec(shown * 10);
			dbg_puts("% ");
		}
	}
	dwmmc_close(&h);
	dbg_puts("in ");
	dbg_dec((dbg_ticks() - t0) * 1000 / dbg_freq());
	dbg_puts(" ms\n");
	if (!payload_parse(ph, pi) || ((const struct mach_header_64 *)pi->kernel)->magic != MH_MAGIC_64) {
		dbg_puts("the payload has no Mach-O kernel\n");
		return 0;
	}
	dbg_puts("kernel ");
	dbg_dec(pi->kernel_size);
	dbg_puts(" bytes, ramdisk ");
	dbg_dec(pi->ramdisk_size);
	dbg_puts(" bytes\n");
	return 1;
}

/* ---- step 3: the kernel in place, its device tree, its boot_args ---- */

static uint64_t kva_to_pa(uint64_t va) { return va + XNU_SLIDE; }

/* What the iPhone's own device tree says (model, compatible, product ...), from your IPSW. */
static void
dt_identity(struct dt_writer *w, const struct identity_prop *props, uint32_t count, const unsigned char *data)
{
	for (uint32_t i = 0; i < count; i++) {
		if (strcmp(props[i].name, "artwork-display-gamut") == 0) {
			dt_prop(w, props[i].name, "sRGB", 5);   /* the panel is sRGB, not the iPhone X's P3 */
			continue;
		}
		if (strcmp(props[i].name, "artwork-device-subtype") == 0) {
			static const uint32_t subtype = 2436;   /* the iPhone X's screen class (see hakios-a32) */
			dt_prop(w, props[i].name, &subtype, sizeof(subtype));
			continue;
		}
		dt_prop(w, props[i].name, data + props[i].offset, props[i].length);
	}
}

static uint8_t nvram_image[NVRAM_BANK_SIZE];

/* set by step 3 before build_device_tree(): the payload's touch firmware, if any */
static const uint8_t *dt_touchfw;
static uint32_t dt_touchfw_size;

static uint32_t
build_device_tree(uint64_t dt_pa, uint64_t rd_pa, uint64_t rd_size)
{
	struct dt_writer w = { .base = (uint8_t *)dt_pa, .cur = (uint8_t *)dt_pa, .depth = 0 };
	uint64_t cntfrq = dbg_freq();
	uint8_t seed[64];

	for (unsigned i = 0; i < sizeof(seed); i++) {
		seed[i] = (uint8_t)(dbg_ticks() >> (i & 7)) ^ (uint8_t)(i * 37);
	}

	dt_begin(&w, "device-tree");
	dt_identity(&w, identity_root, sizeof(identity_root) / sizeof(identity_root[0]), identity_root_data);
	dt_prop_str(&w, "platform-name", "t8015");
	dt_prop_padded(&w, "serial-number", XNU_SERIAL, 32);
	dt_prop_padded(&w, "mlb-serial-number", XNU_SERIAL "MLB", 32);
	dt_prop_padded(&w, "model-number", "MQAC2", 32);
	dt_prop_padded(&w, "region-info", "LL/A", 32);
	dt_prop_padded(&w, "regulatory-model-number", "A1865", 32);
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);

	dt_begin(&w, "chosen");
	dt_prop_str(&w, "darwin-board", "a21s");
	dt_prop(&w, "random-seed", seed, sizeof(seed));
	dt_prop_u64(&w, "dram-base", XNU_PHYS_BASE);
	dt_prop_u64(&w, "dram-size", XNU_MEM_SIZE);
	dt_prop_u32(&w, "debug-enabled", 1);
	dt_prop_u32(&w, "display-scale", 2);
	dt_prop_u32(&w, "display-rotation", 0);
	dt_prop_str(&w, "psci-method", "smc");
	static const uint8_t wifi_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x01 };
	static const uint8_t bluetooth_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x02 };
	static const uint8_t manifest_hash[48] = { 0x72, 0x65, 0x64, 0x77, 0x6f, 0x6f, 0x64 };
	static const uint8_t no_behavior[16];
	dt_prop_u64(&w, "unique-chip-id", 0x000052445730001ULL);
	dt_prop_u32(&w, "chip-id", 0x8015);
	dt_prop_u32(&w, "board-id", 0x06);
	dt_prop(&w, "mac-address-wifi0", wifi_address, sizeof(wifi_address));
	dt_prop(&w, "mac-address-bluetooth0", bluetooth_address, sizeof(bluetooth_address));
	dt_prop(&w, "boot-manifest-hash", manifest_hash, sizeof(manifest_hash));
	dt_prop(&w, "software-behavior", no_behavior, sizeof(no_behavior));
	dt_prop_str(&w, "firmware-version", "iBoot-8422.100.650");
	dt_prop_u32(&w, "certificate-production-status", 1);
	dt_prop_u32(&w, "certificate-security-mode", 1);
	dt_prop_u32(&w, "effective-production-status-ap", 1);
	dt_prop_u32(&w, "effective-security-mode-ap", 1);
	dt_prop_u32(&w, "secure-boot", 1);
	dt_prop_u32(&w, "production-cert", 1);
	dt_prop_u32(&w, "development-cert", 0);
	dt_prop_u32(&w, "embedded-panic-log-size", PANIC_LOG_SIZE);
	build_nvram_image(nvram_image);
	dt_prop_u32(&w, "nvram-bank-size", NVRAM_BANK_SIZE);
	dt_prop_u32(&w, "nvram-bank-count", 1);
	dt_prop_u32(&w, "nvram-current-bank", 0);
	dt_prop(&w, "nvram-proxy-data", nvram_image, sizeof(nvram_image));
	/* "RAMDisk" is /dev/md0, the root (rd=md0): the payload's RAM disk */
	dt_begin(&w, "memory-map");
#if XNU_EMMC_ROOT
	/*
	 * The root is the system image on the eMMC (rd=emmc0); its /sbin/launchd (stage0) mounts the
	 * small image -- the payload's RAM disk, userland/root.apfs: the real init, the jobs -- from
	 * /dev/md1 and hands over to it. So md0 is an empty page (it is made first, from "RAMDisk")
	 * and the payload's RAM disk is "RAMDisk1".
	 */
	if (rd_size != 0) {
		uint64_t md0_reg[2] = { rd_pa + rd_size, PAGE_SIZE_MAX };
		uint64_t rd_reg[2] = { rd_pa, rd_size };
		dt_prop(&w, "RAMDisk", md0_reg, sizeof(md0_reg));
		dt_prop(&w, "RAMDisk1", rd_reg, sizeof(rd_reg));
	}
#else
	if (rd_size != 0) {
		uint64_t rd_reg[2] = { rd_pa, rd_size };
		dt_prop(&w, "RAMDisk", rd_reg, sizeof(rd_reg));
	}
#endif
	dt_end(&w);
	dt_end(&w);

	dt_begin(&w, "defaults");
	dt_prop_u32(&w, "kern.vm_compressor", 2);
	dt_end(&w);

	/*
	 * The touch controller's firmware (payload item "touchfw", extracted from YOUR phone's vendor
	 * image: it is Samsung's and never in the repository), where the kernel's TD4150 driver
	 * (bsd/dev/hakios.ovt_tcm.c, from ios-a135f) looks for it: /a135f-touch "firmware". The
	 * controller has no flash for it and needs it at every boot.
	 */
	if (dt_touchfw && dt_touchfw_size) {
		dt_begin(&w, "a135f-touch");
		dt_prop(&w, "firmware", dt_touchfw, dt_touchfw_size);
		dt_end(&w);
	}

	dt_begin(&w, "product");
	dt_identity(&w, identity_product, sizeof(identity_product) / sizeof(identity_product[0]), identity_product_data);
	dt_begin(&w, "haptics");
	dt_identity(&w, identity_haptics, sizeof(identity_haptics) / sizeof(identity_haptics[0]), identity_haptics_data);
	dt_end(&w);
	dt_end(&w);

	dt_begin(&w, "options");
	dt_end(&w);

	uint64_t pram_reg[2] = { PANIC_LOG_BASE, PANIC_LOG_SIZE };
	memset((void *)PANIC_LOG_BASE, 0, PANIC_LOG_SIZE);
	dt_begin(&w, "pram");
	dt_prop(&w, "reg", pram_reg, sizeof(pram_reg));
	dt_end(&w);

	/* the eight Cortex-A55s, numbered by Aff1 (MPIDR.MT); the first XNU_CORES of them, this one first */
	uint32_t self = (uint32_t)((SYSREG(mpidr_el1) >> 8) & 0xff);
	dt_begin(&w, "cpus");
	for (int i = 0; i < XNU_CORES; i++) {
		char name[5] = { 'c', 'p', 'u', (char)('0' + i), 0 };
		uint32_t core = i == 0 ? self : (uint32_t)(i <= (int)self ? i - 1 : i);

		dt_begin(&w, name);
		dt_prop_str(&w, "device_type", "cpu");
		dt_prop_str(&w, "state", i == 0 ? "running" : "waiting");
		dt_prop_u32(&w, "reg", core << 8);
		dt_prop_u32(&w, "cpu-id", (uint32_t)i);
		dt_prop_u64(&w, "timebase-frequency", cntfrq);
		dt_prop_u64(&w, "fixed-frequency", cntfrq);
		dt_prop_u64(&w, "clock-frequency", 2000000000ULL);
		dt_prop_u64(&w, "bus-frequency", 100000000ULL);
		dt_prop_u64(&w, "memory-frequency", 100000000ULL);
		dt_prop_u64(&w, "peripheral-frequency", 26000000ULL);
		dt_end(&w);
	}
	dt_end(&w);

	dt_begin(&w, "arm-io");
	dt_prop_str(&w, "device_type", "a21s");
	uint64_t ranges[3] = { 0, ARM_IO_BASE, 0xc0000000ULL };
	dt_prop(&w, "ranges", ranges, sizeof(ranges));
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);

	/* the GIC-400: gic_v3_irq.c hands everything to gic_v2_irq.h for this compatible */
	dt_begin(&w, "gic");
	dt_prop_str(&w, "device_type", "interrupt-controller");
	dt_prop_str(&w, "compatible", "arm,gic-400");
	uint64_t gic_reg[4] = { 0x12a01000 - ARM_IO_BASE, 0x1000, 0x12a02000 - ARM_IO_BASE, 0x2000 };
	dt_prop(&w, "reg", gic_reg, sizeof(gic_reg));
	dt_end(&w);

	/* the console the kernel is told about: a page of RAM, not a UART (its text goes to loader_log) */
	dt_begin(&w, "uart0");
	dt_prop_str(&w, "device_type", "uart");
	dt_prop_str(&w, "compatible", "hakios,loader-log");
	uint64_t uart_reg[2] = { UART_SCRATCH - ARM_IO_BASE, 0x1000 };
	dt_prop(&w, "reg", uart_reg, sizeof(uart_reg));
	dt_prop_u32(&w, "boot-console", 1);
	dt_end(&w);

	dt_end(&w);
	dt_end(&w);
	return (uint32_t)(w.cur - w.base);
}

/*
 * Do the timers' interrupts reach the GIC-400 at EL1? The first XNU run stopped where the kernel
 * first waits for a deadline, which suggests its timer (the EL1 virtual one, PPI 27) never fires.
 * Each timer is armed for 1 ms with interrupts masked in the processor, and then: did it expire
 * (ISTATUS), does the distributor see its PPI pending, and does the CPU interface hand it out
 * (GICC_IAR) - which is what XNU would get. Everything is left as XNU must find it.
 */
#define GICD                0x12a01000ULL
#define GICC                0x12a02000ULL

static int
timer_try(uint32_t ppi, int virt)
{
	uint64_t f = dbg_freq(), now, ctl = 0;
	uint32_t pend = 0, hppir = 0, iar = 0x3ff;

	wr32(GICD + 0x280, 1u << ppi);                      /* ICPENDR0 */
	*(volatile uint8_t *)(GICD + 0x400 + ppi) = 0xa0;   /* priority */
	wr32(GICD + 0x100, 1u << ppi);                      /* ISENABLER0 */
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(now));
	if (virt) {
		uint64_t vnow;
		__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(vnow));
		__asm__ volatile("msr cntv_cval_el0, %0; msr cntv_ctl_el0, %1; isb" : : "r"(vnow + f / 1000), "r"(1UL));
	} else {
		__asm__ volatile("msr cntp_cval_el0, %0; msr cntp_ctl_el0, %1; isb" : : "r"(now + f / 1000), "r"(1UL));
	}
	for (uint64_t end = now + f / 50; dbg_ticks() < end;) {   /* 20 ms */
		if (virt) {
			__asm__ volatile("isb; mrs %0, cntv_ctl_el0" : "=r"(ctl));
		} else {
			__asm__ volatile("isb; mrs %0, cntp_ctl_el0" : "=r"(ctl));
		}
		pend = rd32(GICD + 0x200);
		hppir = rd32(GICC + 0x18);
		if ((ctl & 4) && (pend & (1u << ppi))) {
			break;
		}
	}
	if (pend & (1u << ppi)) {
		iar = rd32(GICC + 0x0c);                    /* take it, as XNU's handler would */
	}
	/* the timer off and masked, then the interrupt ended */
	if (virt) {
		__asm__ volatile("msr cntv_ctl_el0, %0; isb" : : "r"(2UL));
	} else {
		__asm__ volatile("msr cntp_ctl_el0, %0; isb" : : "r"(2UL));
	}
	if ((iar & 0x3ff) < 1020) {
		wr32(GICC + 0x10, iar);
	}
	wr32(GICD + 0x180, 1u << ppi);                      /* ICENABLER0 */
	wr32(GICD + 0x280, 1u << ppi);                      /* ICPENDR0 */
	wr32(GICD + 0x380, 1u << ppi);                      /* ICACTIVER0 */
	__asm__ volatile("dsb sy; isb");

	int ok = (iar & 0x3ff) == ppi;
	dbg_puts(virt ? "  virtual timer  PPI " : "  physical timer PPI ");
	dbg_dec(ppi);
	dbg_puts(ctl & 4 ? ": expired" : ": DID NOT EXPIRE");
	dbg_puts(pend & (1u << ppi) ? ", pending in the GIC" : ", NOT pending in the GIC");
	kv(", HPPIR", hppir, 3);
	kv("IAR", iar, 3);
	dbg_puts(ok ? " -> DELIVERED\n" : " -> not delivered\n");
	return ok;
}

static void
timer_probe(void)
{
	uint32_t cctl = rd32(GICC + 0x00), pmr = rd32(GICC + 0x04);

	dbg_puts("timer probe (EL1, GIC-400):\n");
	wr32(GICC + 0x04, 0xff);                            /* every priority */
	wr32(GICC + 0x00, cctl | 1);                        /* Group 1 on */
	int v = timer_try(27, 1);
	int ph = timer_try(30, 0);
	wr32(GICC + 0x04, pmr);
	wr32(GICC + 0x00, cctl);
	fbc.fg = v ? 0xff40ff40u : 0xffff4040u;
	dbg_puts(v ? "  XNU's virtual timer works\n" : ph ? "  ONLY THE PHYSICAL TIMER WORKS: XNU needs it\n" :
	    "  neither timer reaches the GIC\n");
	fbc.fg = 0xffffffffu;
}

static void
cache_invalidate(uint64_t from, uint64_t to)
{
	uint64_t ctr = SYSREG(ctr_el0), line = 4u << ((ctr >> 16) & 0xf);

	for (uint64_t at = from & ~(line - 1); at < to; at += line) {
		__asm__ volatile("dc ivac, %0" : : "r"(at) : "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

static void __attribute__((noreturn))
run_xnu(const struct payload_info *pi)
{
	uint64_t syms[SYM_COUNT];
	const struct mach_header_64 *mh = (const struct mach_header_64 *)pi->kernel;
	const struct load_command *lc = (const struct load_command *)(mh + 1);
	uint64_t vm_hi = 0, entry_va = 0;

	dbg_puts("step 3: the kernel at ");
	dbg_hex(XNU_PHYS_BASE, 10);
	dbg_putc('\n');
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *sg = (const void *)lc;
			if (sg->vmsize == 0) {
				continue;
			}
			uint64_t dst = kva_to_pa(sg->vmaddr);
			if (dst < XNU_PHYS_BASE || dst + sg->vmsize > STAGING_PA) {
				kv("segment outside XNU's memory at", dst, 10);
				dbg_putc('\n');
				dbg_delay_ms(5000);
				chain_android();
			}
			if (sg->vmaddr + sg->vmsize > vm_hi) {
				vm_hi = sg->vmaddr + sg->vmsize;
			}
			memset((void *)dst, 0, sg->vmsize);
			memcpy((void *)dst, pi->kernel + sg->fileoff, sg->filesize);
		} else if (lc->cmd == LC_UNIXTHREAD) {
			entry_va = ((const uint64_t *)((const uint8_t *)lc + 16))[32];  /* the thread state's pc */
		}
		wdt_pet();
	}
	kv("entry", kva_to_pa(entry_va), 10);
	dbg_putc('\n');

	find_syms(pi->kernel, syms);
	/* the console's colours opaque (the display layer composes with per-pixel alpha) */
	if (syms[SYM_VC_COLORS]) {
		volatile uint32_t *c = (volatile uint32_t *)kva_to_pa(syms[SYM_VC_COLORS]);
		for (int i = 0; i < 8; i++) {
			c[i * 4 + 2] |= 0xff000000u;
		}
	}
	/*
	 * The kernel's serial driver is a PL011 one (vmapple_uart): ready always, its bytes to
	 * loader_log_putc (the kernel's own log buffer, which the next boot hands to Android), never any
	 * input. Its registers are a page of RAM (uart0 above), so its initialisation harms nothing.
	 */
	if (syms[SYM_UART_TX_READY] && syms[SYM_UART_TX_DATA] && syms[SYM_LOG_PUTC]) {
		volatile uint32_t *rdy = (volatile uint32_t *)kva_to_pa(syms[SYM_UART_TX_READY]);
		volatile uint32_t *tx = (volatile uint32_t *)kva_to_pa(syms[SYM_UART_TX_DATA]);
		int64_t rel = (int64_t)(syms[SYM_LOG_PUTC] - syms[SYM_UART_TX_DATA]) >> 2;

		rdy[0] = 0x52800020u;                   /* mov w0, #1 */
		rdy[1] = 0xd65f03c0u;                   /* ret */
		tx[0] = (rel >= -(1 << 25) && rel < (1 << 25)) ? 0x14000000u | ((uint32_t)rel & 0x03ffffffu) : 0xd65f03c0u;
		if (syms[SYM_UART_RX_READY]) {
			volatile uint32_t *rx = (volatile uint32_t *)kva_to_pa(syms[SYM_UART_RX_READY]);
			rx[0] = 0x52800000u;            /* mov w0, #0 */
			rx[1] = 0xd65f03c0u;
		}
		dbg_puts("console -> loader_log\n");
	}

	uint64_t kern_hi = (kva_to_pa(vm_hi) + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	uint64_t rd_pa = kern_hi, rd_size = (pi->ramdisk_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	if (rd_size) {
		memset((void *)rd_pa, 0, rd_size);
		memcpy((void *)rd_pa, pi->ramdisk, pi->ramdisk_size);
	}
	uint64_t dt_pa = rd_pa + rd_size;
#if XNU_EMMC_ROOT
	/* one empty page after the RAM disk: md0 (build_device_tree), so the RAM disk is md1 */
	memset((void *)dt_pa, 0, PAGE_SIZE_MAX);
	dt_pa += PAGE_SIZE_MAX;
#endif
	dt_touchfw = pi->touchfw;
	dt_touchfw_size = pi->touchfw_size;
	if (dt_touchfw) {
		kv("touchfw", dt_touchfw_size, 6);
		dbg_putc('\n');
	}
	uint32_t dt_len = build_device_tree(dt_pa, rd_pa, rd_size);
	uint64_t ba_pa = (dt_pa + dt_len + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	uint64_t top = ba_pa + PAGE_SIZE_MAX;

	struct boot_args *ba = (struct boot_args *)ba_pa;
	memset(ba, 0, sizeof(*ba));
	ba->revision = 2;
	ba->version = 2;
	ba->physBase = XNU_PHYS_BASE;
	ba->virtBase = KERNEL_VIRT_BASE;
	ba->memSize = XNU_MEM_SIZE;
	ba->memSizeActual = XNU_MEM_SIZE;
	ba->topOfKernelData = top;
	ba->deviceTreeP = dt_pa - XNU_SLIDE;
	ba->deviceTreeLength = dt_len;
	ba->video.v_baseAddr = BOOT_FB_BASE;
	ba->video.v_display = XNU_VERBOSE ? 0 : 1;      /* 0: the kernel's text console; 1: iOS draws */
	ba->video.v_rowBytes = BOOT_FB_PITCH;
	ba->video.v_width = PANEL_W;
	ba->video.v_height = PANEL_H;
	ba->video.v_depth = 32;
	{
		char *c = ba->commandLine;
		uint32_t n = 0;
		const char *parts[3];
		parts[0] = XNU_BOOT_ARGS;
		parts[1] = XNU_VERBOSE ? " -v" : "";
		parts[2] = pi->cmdline;
		for (int p = 0; p < 3; p++) {
			if (p == 2 && parts[p] && parts[p][0] && n < BOOT_LINE_LENGTH - 1) {
				c[n++] = ' ';
			}
			for (const char *s = parts[p]; s && *s && n < BOOT_LINE_LENGTH - 1; s++) {
				c[n++] = *s;
			}
		}
#if XNU_CORES > 1 && XNU_PSCI_TRAMPOLINE
		/*
		 * psci_trampoline=PA: a reserved page of this loader's own .bss, inside the range S-Boot loaded the
		 * OS's kernel at, where the kernel writes a jump to its reset vector and starts the other
		 * cores (osfmk/arm64/psci_smp.c): the firmware took CPU_ON for 0x880000000 and never
		 * started a core there.
		 */
		{
			static const char key[] = " psci_trampoline=0x";
			uint64_t pa = (uint64_t)psci_trampoline_page;
			for (const char *s = key; *s && n < BOOT_LINE_LENGTH - 1; s++) {
				c[n++] = *s;
			}
			for (int sh = 36; sh >= 0 && n < BOOT_LINE_LENGTH - 1; sh -= 4) {
				c[n++] = "0123456789abcdef"[(pa >> sh) & 0xf];
			}
		}
#endif
		c[n] = 0;
	}
	kv("ramdisk", rd_pa, 10);
	kv("dt", dt_pa, 10);
	dbg_putc('\n');
	kv("boot_args", ba_pa, 10);
	kv("top", top, 10);
	dbg_putc('\n');
	dbg_puts("boot-args: ");
	dbg_puts(ba->commandLine);
	dbg_putc('\n');

	/* step 4: what the next boot needs to find this run's log, the watchdogs, the jump */
	if (syms[SYM_LOG] && syms[SYM_LOG_LEN] && syms[SYM_LOG_RING_AT] && syms[SYM_LOG_WRAPPED]) {
		volatile struct rescue *r = (volatile struct rescue *)RESCUE_PA;
		r->log_pa = kva_to_pa(syms[SYM_LOG]);
		r->log_len_pa = kva_to_pa(syms[SYM_LOG_LEN]);
		r->ring_at_pa = kva_to_pa(syms[SYM_LOG_RING_AT]);
		r->wrapped_pa = kva_to_pa(syms[SYM_LOG_WRAPPED]);
		r->magic = RESCUE_MAGIC;
		dbg_puts("step 4: the log will be in pstore after the next boot\n");
	}
	{
		uint64_t pct, vct;
		__asm__ volatile("isb; mrs %0, cntpct_el0; mrs %1, cntvct_el0" : "=r"(pct), "=r"(vct));
		kv("CNTPCT-CNTVCT", pct - vct, 10);
		dbg_putc('\n');
	}
	timer_probe();
	/*
	 * The watchdog of cluster 0 at its longest instead of off: if XNU hangs, it resets the SoC
	 * warm, the RAM (and with it the kernel's log) stays, and the next boot hands that log to
	 * Android. A forced restart with the buttons is a cold one: the first runs lost their log so.
	 * Prescaler 255, divider 128, count 0xffff: about 82 s if the watchdog runs at 26 MHz.
	 */
	kv("PMU CL0 NONCPU_OUT", rd32(PMU_BASE + PMU_CL0_NONCPU_OUT), 8);
	kv(" INT_EN", rd32(PMU_BASE + PMU_CL0_NONCPU_INT_EN), 8);
	dbg_putc('\n');
	dbg_puts("watchdog: counter and reset enabled in the PMU; a warm reset in about 80 s\n");
	dbg_puts("Jumping to XNU in 5 s...\n");
	dbg_delay_ms(5000);
	wr32(WDT_CL1 + WTCON, rd32(WDT_CL1 + WTCON) & ~(WTCON_ENABLE | WTCON_RESET));
	wr32(WDT_CL0 + WTCON, rd32(WDT_CL0 + WTCON) & ~WTCON_ENABLE);
	wr32(WDT_CL0 + WTDAT, 0xffff);
	wr32(WDT_CL0 + WTCNT, 0xffff);
	/*
	 * On the Exynos 850 WTCON alone does nothing: the PMU gates the counter and masks the reset
	 * (Linux s3c2410_wdt.c, drv_data_exynos850_cl0): CLUSTER0_NONCPU_OUT bit 7 runs the counter,
	 * CLUSTER0_NONCPU_INT_EN bit 2 (inverted: 1 = unmasked) lets it reset the SoC. Without these
	 * the run that hung on 2026-11-04 stayed on screen for good instead of coming back.
	 */
	wr32(PMU_BASE + PMU_CL0_NONCPU_INT_EN, rd32(PMU_BASE + PMU_CL0_NONCPU_INT_EN) | (1u << 2));
	wr32(PMU_BASE + PMU_CL0_NONCPU_OUT, rd32(PMU_BASE + PMU_CL0_NONCPU_OUT) | (1u << 7));
	wr32(WDT_CL0 + WTCON, (0xffu << 8) | (3u << 3) | WTCON_ENABLE | WTCON_RESET);
	fb_rect(0, 0, PANEL_W, PANEL_H, 0xff000000u);
	cache_invalidate(XNU_PHYS_BASE, top);
	jump_to_kernel(kva_to_pa(entry_va), ba_pa);
}

void
diag_main(uint64_t dtb, uint64_t x1, uint64_t x2, uint64_t x3)
{
	(void)x1; (void)x2; (void)x3;
	chain_dtb = dtb;
	wdt_pet();
	int asked = STEP >= 2 && xnu_requested();       /* before our log overwrites the zone */
	int after_run = ((volatile struct rescue *)RESCUE_PA)->magic == RESCUE_MAGIC;
	int after_probe = 0;
#if HAKIOS_SMP_PROBE
	after_probe = *(volatile uint64_t *)RESCUE_PA == HAKIOS_SMP_RESCUE_MAGIC;
#endif
	if (after_run || after_probe) {
		keep_previous_loader_log();                 /* before plog_reset: it holds the timer probe */
	}
	int want_xnu = !after_probe && (asked || (STEP >= 2 && XNU_FIRST && !after_run));
	plog_reset();

	fbcon_init(BOOT_FB_BASE, BOOT_FB_PITCH, 0, 2);
	fb_rect(0, 0, PANEL_W, 900, 0xff000000u);
	fbc.fg = 0xff40c0ffu;
	dbg_puts("EXYLOADER: Galaxy A21s (Exynos 850), step ");
	dbg_dec(STEP);
	dbg_puts(asked ? ", XNU asked for\n" : want_xnu ? ", XNU first\n" : after_run ? ", after an XNU run\n" : "\n");
	kv("start", (uint64_t)_start, 10);
	kv("dtb", dtb, 10);
	dbg_putc('\n');
	{
		uint32_t rst = rd32(PMU_BASE + PMU_RST_STAT);
		kv("PMU RST_STAT", rst, 8);
		dbg_puts(rst & (1u << 24) ? " (the watchdog: a warm reset, RAM kept)\n" : "\n");
	}
	fbc.fg = 0xffffffffu;

	if (after_probe) {
		dbg_puts("=== previous SMP probe, recovered after warm reset ===\n");
		plog_raw(prev_log, prev_len > 12000 ? 12000 : prev_len);
		*(volatile uint64_t *)RESCUE_PA = 0;
		chain_android();
	}

	if (!want_xnu) {
		if (after_run && prev_len) {
			/* the part from "step 3" on: what was loaded, the timer probe, the jump */
			uint32_t at = 0;
			for (uint32_t i = 0; i + 7 <= prev_len; i++) {
				if (prev_log[i] == 's' && prev_log[i + 1] == 't' && prev_log[i + 2] == 'e' && prev_log[i + 3] == 'p' &&
				    prev_log[i + 4] == ' ' && prev_log[i + 5] == '3' && prev_log[i + 6] == ':') {
					at = i;
					break;
				}
			}
			uint32_t n = prev_len - at > 1200 ? 1200 : prev_len - at;
			dbg_puts("=== the loader before the XNU run ===\n");
			plog_raw(prev_log + at, n);
			dbg_puts("\n");
		}
		rescue_previous_log();
		if (STEP >= 2) {
			/* nothing has to survive a reboot for these: they are this boot's own findings */
			timer_probe();
			rescue_from_ram();
		}
		if (STEP == 1) {
			int ok = step_emmc();
			fbc.fg = ok ? 0xff40ff40u : 0xffff4040u;
			dbg_puts(ok ? "eMMC: OK\n" : "eMMC: FAILED\n");
			fbc.fg = 0xffffffffu;
			dbg_puts("Android in 5 s; log in pstore (pmsg-ramoops-0)\n");
			dbg_delay_ms(5000);
		} else {
			dbg_puts("Android (for XNU: echo " BOOT_XNU_FLAG " > /dev/pmsg0, then reboot)\n");
		}
		chain_android();
	}

#if HAKIOS_SMP_PROBE
	hakios_smp_probe_run(dtb);
#endif
	struct payload_info pi;
	if (!step_payload(&pi)) {
		fbc.fg = 0xffff4040u;
		dbg_puts("no XNU: Android in 5 s\n");
		dbg_delay_ms(5000);
		chain_android();
	}
	run_xnu(&pi);
}
