/*
 * a21sdiag: first bare-metal probe for the Galaxy A21s (SM-A217F/M, Exynos 850).
 *
 * Packed in front of Android's kernel in `boot` by tools/chainboot.py. S-Boot starts it as it
 * would start Linux (MMU/caches off, x0 = dtb). It finds out, and reports on screen and into
 * the ramoops pmsg zone:
 *
 *   - that S-Boot runs a chained kernel at all, where it put it, at which EL;
 *   - the CPU (MIDR, MPIDR, the 16K-granule and PA-size bits XNU needs), the timer's frequency;
 *   - the GIC (a GIC-400 at 0x12a01000 per the device tree: GICD_TYPER/IIDR, GICC_IIDR);
 *   - the GPU's power domain (PMU 0x11861d00) and, only if it is on, the Mali's GPU_ID,
 *     shader/tiler/L2 present masks (0x11500000) - read only, nothing is powered or clocked;
 *   - the display: DECON/DSIM/DPP registers into the log, coloured bars onto S-Boot's
 *     framebuffer (which bar shows which colour gives the byte order);
 *
 * then copies Android's Image down to where S-Boot put this program and jumps to it, so the
 * phone comes back to Android. No register is written except the framebuffer, the pmsg zone
 * and the watchdogs' counters (kicked if S-Boot left them running).
 */
#include <stdint.h>
#include "exydbg.h"

#define CHAIN_OFFSET  0x40000u

#define PMU_PD_G3D_CONF   0x11861d00ULL
#define PMU_PD_G3D_STATUS 0x11861d04ULL
#define PD_STATUS_ON      0x1u                  /* gpu_pmu_status_local_pwr_mask */

#define MALI_BASE         0x11500000ULL
#define GPU_ID            0x000
#define GPU_L2_FEATURES   0x004
#define GPU_SHADER_PRESENT_LO 0x100
#define GPU_TILER_PRESENT_LO  0x110
#define GPU_L2_PRESENT_LO     0x120

#define GICD_BASE         0x12a01000ULL
#define GICC_BASE         0x12a02000ULL

#define DECON_BASE        0x13060000ULL
#define DSIM_BASE         0x13070000ULL

extern char _start[];
extern char chain_stub[], chain_stub_end[];
static uint64_t chain_dtb;

#define SYSREG(name) ({ uint64_t _v; __asm__ volatile("mrs %0, " #name : "=r"(_v)); _v; })

static void __attribute__((noreturn))
chain_android(void)
{
	const volatile uint64_t *hdr = (const volatile uint64_t *)_start;
	const volatile uint32_t *img = (const volatile uint32_t *)((uint64_t)_start + CHAIN_OFFSET);
	uint64_t image_size = (hdr[5] + 7) & ~7ULL;          /* res2: the Image's length */
	uint64_t covered = hdr[2];                           /* image_size: everything we carry */

	if (img[14] != 0x644d5241u || image_size == 0) {    /* "ARM\x64" */
		dbg_puts("a21sdiag: no Linux Image to chain to; waiting forever\n");
		for (;;) {
			wdt_pet();
			__asm__ volatile("wfe");
		}
	}
	dbg_puts("a21sdiag: chaining to Android, ");
	dbg_dec(image_size);
	dbg_puts(" bytes\n");

	/*
	 * The stub goes inside the area Linux itself would have used (its image_size, which our
	 * covering image_size includes), past the end of the Image we copy from: nothing S-Boot
	 * placed (ramdisk, dtb) can be there.
	 */
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
	dbg_puts("\na21sdiag: FAULT vec=");
	dbg_hex(vec, 3);
	dbg_puts(" ESR=");
	dbg_hex(esr, 8);
	dbg_puts(" ELR=");
	dbg_hex(elr, 10);
	dbg_puts(" FAR=");
	dbg_hex(far, 10);
	dbg_puts("\n");
	dbg_delay_ms(3000);
	chain_android();
}

static void
bar(uint32_t y, uint32_t color, const char *label)
{
	fb_rect(0, y, PANEL_W, 40, color);
	uint32_t sy = fbc.y, sx = fbc.x, sbg = fbc.bg, sfg = fbc.fg;

	fbc.x = 8;
	fbc.y = y + 4;
	fbc.bg = color;
	fbc.fg = 0xff000000u;
	while (*label) {
		fbcon_putc(*label++);
	}
	fbc.x = sx;
	fbc.y = sy;
	fbc.bg = sbg;
	fbc.fg = sfg;
}

static void
kv(const char *k, uint64_t v, int digits)
{
	dbg_puts(k);
	dbg_putc('=');
	dbg_hex(v, digits);
	dbg_putc(' ');
}

void
diag_main(uint64_t dtb, uint64_t x1, uint64_t x2, uint64_t x3)
{
	uint64_t el = (SYSREG(CurrentEL) >> 2) & 3;

	chain_dtb = dtb;
	wdt_pet();
	plog_reset();

	fbcon_init(BOOT_FB_BASE, BOOT_FB_PITCH, 0, 2);
	fb_rect(0, 0, PANEL_W, 760, 0xff000000u);
	/* written as 0xAARRGGBB: if the "red" bar shows blue, the layer is ABGR */
	bar(0, 0xffff0000u, "A21SDIAG red");
	bar(44, 0xff00ff00u, "A21SDIAG green");
	bar(88, 0xff0000ffu, "A21SDIAG blue");
	bar(132, 0xffffffffu, "A21SDIAG white");

	fbc.x = 0;
	fbc.y = 184;
	fbc.fg = 0xff40ff40u;
	dbg_puts("A21SDIAG: Galaxy A21s (Exynos 850)\n");

	kv("start", (uint64_t)_start, 10);
	kv("dtb", dtb, 10);
	dbg_putc('\n');
	kv("x1", x1, 8);
	kv("x2", x2, 8);
	kv("x3", x3, 8);
	dbg_putc('\n');
	dbg_puts("EL=");
	dbg_dec(el);
	dbg_puts(" CNTFRQ=");
	dbg_dec(dbg_freq());
	dbg_putc('\n');
	kv("MIDR", SYSREG(midr_el1), 8);
	kv("MPIDR", SYSREG(mpidr_el1), 10);
	dbg_putc('\n');
	/* XNU wants the 16K granule (TGran16, bits 23:20) and reports PARange (bits 3:0) */
	kv("MMFR0", SYSREG(id_aa64mmfr0_el1), 8);
	kv("PFR0", SYSREG(id_aa64pfr0_el1), 8);
	dbg_putc('\n');
	kv("ISAR1", SYSREG(id_aa64isar1_el1), 8);
	if (el == 1) {
		kv("SCTLR1", SYSREG(sctlr_el1), 8);
	}
	dbg_putc('\n');

	kv("WDT0", rd32(WDT_CL0 + WTCON), 8);
	kv("WDT1", rd32(WDT_CL1 + WTCON), 8);
	dbg_putc('\n');

	kv("GICD_CTLR", rd32(GICD_BASE + 0x000), 8);
	kv("TYPER", rd32(GICD_BASE + 0x004), 8);
	dbg_putc('\n');
	kv("GICD_IIDR", rd32(GICD_BASE + 0x008), 8);
	kv("GICC_IIDR", rd32(GICC_BASE + 0x0fc), 8);
	dbg_putc('\n');

	uint32_t pd_conf = rd32(PMU_PD_G3D_CONF), pd_stat = rd32(PMU_PD_G3D_STATUS);
	kv("PD_G3D conf", pd_conf, 8);
	kv("status", pd_stat, 8);
	dbg_putc('\n');
	if ((pd_stat & PD_STATUS_ON) == PD_STATUS_ON) {
		kv("GPU_ID", rd32(MALI_BASE + GPU_ID), 8);
		kv("L2F", rd32(MALI_BASE + GPU_L2_FEATURES), 8);
		dbg_putc('\n');
		kv("SHADER", rd32(MALI_BASE + GPU_SHADER_PRESENT_LO), 8);
		kv("TILER", rd32(MALI_BASE + GPU_TILER_PRESENT_LO), 8);
		kv("L2", rd32(MALI_BASE + GPU_L2_PRESENT_LO), 8);
		dbg_putc('\n');
	} else {
		dbg_puts("G3D powered off: GPU registers not read\n");
	}

	/* the display path, into the log only */
	dbg_dump("decon", DECON_BASE, 0x000, 0x200, 1);
	dbg_dump("dsim", DSIM_BASE, 0x000, 0x100, 1);
	dbg_dump("dpp1", 0x130b1000ULL, 0x000, 0x100, 1);
	dbg_dump("dpp2", 0x130b2000ULL, 0x000, 0x100, 1);
	dbg_dump("dpp3", 0x130b3000ULL, 0x000, 0x100, 1);
	dbg_dump("dpp4", 0x130b4000ULL, 0x000, 0x100, 1);
	dbg_puts("fb words:");
	for (int i = 0; i < 4; i++) {
		dbg_putc(' ');
		dbg_hex(rd32(BOOT_FB_BASE + (uint64_t)(PANEL_H / 2) * BOOT_FB_PITCH + 1200 + 4 * i), 8);
	}
	dbg_putc('\n');

	dbg_puts("Android in 10 s; log in pstore (pmsg-ramoops-0)\n");
	for (int s = 10; s > 0; s--) {
		dbg_puts("a21sdiag: ");
		dbg_dec(s);
		dbg_putc('\n');
		dbg_delay_ms(1000);
	}
	chain_android();
}
