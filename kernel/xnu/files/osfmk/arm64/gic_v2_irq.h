/*
 * Copyright (c) 2026 The HakiOS authors. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * GICv2 (ARM GIC-400) for the boards that have one: the Exynos 850 of the Galaxy A21s.
 *
 * Included by gic_v3_irq.c, which hands every public call over to these when /arm-io/gic says it
 * is a GICv2 (compatible "arm,gic-400" or "arm,cortex-a15-gic"); the GICv3 path stays exactly as
 * it was. Same model as there: everything is a Group 1 IRQ of the non-secure world, the EL1
 * virtual timer PPI drives rtclock, SGI 0 and 1 carry cpu_signal() and the stop order, an SPI is
 * a driver's once attached, anything else is masked and counted.
 *
 * What differs from v3:
 *   - the CPU interface is memory mapped (GICC), not the ICC_*_EL1 system registers, which do
 *     not exist here (ID_AA64PFR0_EL1.GIC is 0 on this phone);
 *   - there are no redistributors: SGIs and PPIs are banked per processor in the distributor;
 *   - SGIs and SPIs are addressed by CPU-interface number (GICD_SGIR's target list, the
 *     GICD_ITARGETSR bytes), not by affinity. Which interface a processor is, each one learns for
 *     itself (GICD_ITARGETSR0..7 read back the reading processor's own bit) and records against
 *     its MPIDR, which is what the kernel names processors by (psci_smp.c, GenericARMPE.cpp);
 *   - EL3 owns the groups: interrupt groups are not writable from the non-secure side, and the
 *     secure firmware has the non-secure interrupts in Group 1 already (as for Linux).
 *
 * Registers come from /arm-io/gic: reg = <distributor offset, size, CPU interface offset, size>.
 * Register layout from ARM's GIC architecture specification (IHI 0048B).
 */

#define GIC2_D_CTLR             0x000
#define GIC2_D_CTLR_ENABLE      (1u << 0)           /* the non-secure view: Group 1 forwarding */
#define GIC2_D_TYPER            0x004
#define GIC2_D_ISENABLER(n)     (0x100 + 4 * (n))
#define GIC2_D_ICENABLER(n)     (0x180 + 4 * (n))
#define GIC2_D_ICPENDR(n)       (0x280 + 4 * (n))
#define GIC2_D_ICACTIVER(n)     (0x380 + 4 * (n))
#define GIC2_D_IPRIORITYR(n)    (0x400 + (n))
#define GIC2_D_ITARGETSR(n)     (0x800 + (n))
#define GIC2_D_ICFGR(n)         (0xc00 + 4 * (n))
#define GIC2_D_SGIR             0xf00
#define GIC2_D_CPENDSGIR(n)     (0xf10 + (n))

#define GIC2_C_CTLR             0x000
#define GIC2_C_CTLR_ENABLE      (1u << 0)
#define GIC2_C_PMR              0x004
#define GIC2_C_BPR              0x008
#define GIC2_C_IAR              0x00c
#define GIC2_C_EOIR             0x010
#define GIC2_C_APR0             0x0d0
#define GIC2_C_IIDR             0x0fc
#define GIC2_IAR_INTID(iar)     ((iar) & 0x3ff)

/* gic_is_v2 is gic_v3_irq.c's: it decides, in gic_v3_irq_cpu_init() */
static vm_offset_t gicc_base;
/* each processor's CPU-interface bit and the MPIDR (low affinity fields) it was seen with */
static uint8_t     gic2_cpu_mask[MAX_CPUS];
static uint32_t    gic2_cpu_phys[MAX_CPUS];

/* Is /arm-io/gic a GICv2? Looked at once, before anything is mapped. */
static bool
gic2_compatible(DTEntry entry)
{
	void const *prop;
	unsigned int size;

	if (SecureDTGetProperty(entry, "compatible", &prop, &size) != kSuccess) {
		return false;
	}
	/* a list of NUL-separated strings */
	for (const char *s = prop, *end = (const char *)prop + size; s < end; s += strlen(s) + 1) {
		if (strcmp(s, "arm,gic-400") == 0 || strcmp(s, "arm,cortex-a15-gic") == 0 || strcmp(s, "arm,gic-v2") == 0) {
			return true;
		}
	}
	return false;
}

static uint8_t
gic2_mask_of(uint32_t phys_id)
{
	for (int i = 0; i < MAX_CPUS; i++) {
		if (gic2_cpu_mask[i] != 0 && gic2_cpu_phys[i] == phys_id) {
			return gic2_cpu_mask[i];
		}
	}
	/*
	 * A processor that has not come up yet has said nothing. With one cluster of up to eight
	 * the interfaces are numbered as the cores are; the core number is Aff1 when MPIDR.MT is
	 * set (the A55s of the Exynos 850: 0x000, 0x100, ... 0x700), else Aff0.
	 */
	uint32_t core = (phys_id & 0xff00) ? (phys_id >> 8) & 0xff : phys_id & 0xff;
	return (uint8_t)(1u << (core & 7));
}

static void
gic2_mask(uint32_t intid)
{
	gic_write32(gicd_base + GIC2_D_ICENABLER(intid / 32), 1u << (intid % 32));
}

static void
gic2_send_sgi(uint32_t phys_id, uint32_t sgi)
{
	/* TargetListFilter 0 (the list), CPUTargetList in [23:16], the SGI in [3:0] */
	__builtin_arm_dsb(DSB_ISHST);
	gic_write32(gicd_base + GIC2_D_SGIR, (uint32_t)gic2_mask_of(phys_id) << 16 | (sgi & 0xf));
}

static void
gic2_spi_mask(uint32_t intid)
{
	if (gicd_base != 0 && intid >= GIC_INTID_SPI_MIN && intid < GIC_INTID_SPURIOUS_MIN) {
		gic2_mask(intid);
	}
}

static void
gic2_spi_unmask(uint32_t intid)
{
	if (gicd_base != 0 && intid >= GIC_INTID_SPI_MIN && intid < GIC_INTID_SPURIOUS_MIN) {
		gic_write32(gicd_base + GIC2_D_ISENABLER(intid / 32), 1u << (intid % 32));
	}
}

/* As gic_v3_spi_attach(), routed to this processor's interface through its ITARGETSR byte. */
static void
gic2_spi_program(uint32_t intid, bool edge)
{
	uint32_t config;

	gic2_mask(intid);
	gic_write32(gicd_base + GIC2_D_ICPENDR(intid / 32), 1u << (intid % 32));
	gic_write32(gicd_base + GIC2_D_ICACTIVER(intid / 32), 1u << (intid % 32));
	*(volatile uint8_t *)(gicd_base + GIC2_D_IPRIORITYR(intid)) = GIC_PRIORITY_DEFAULT;
	config = gic_read32(gicd_base + GIC2_D_ICFGR(intid / 16)) & ~(3u << (intid % 16 * 2));
	gic_write32(gicd_base + GIC2_D_ICFGR(intid / 16), config | (edge ? 2u : 0u) << (intid % 16 * 2));
	*(volatile uint8_t *)(gicd_base + GIC2_D_ITARGETSR(intid)) = gic2_cpu_mask[cpu_number()];
	gic2_spi_unmask(intid);
}

static void
gic2_spi_clear_pending(uint32_t intid)
{
	gic_write32(gicd_base + GIC2_D_ICPENDR(intid / 32), 1u << (intid % 32));
}

static void
gic2_irq_handler(__unused void *target, __unused void *refCon, __unused void *nub, __unused int source)
{
	int cpu = cpu_number();

	for (;;) {
		uint32_t iar = gic_read32(gicc_base + GIC2_C_IAR);
		uint32_t intid = GIC2_IAR_INTID(iar);

		if (intid >= GIC_INTID_SPURIOUS_MIN) {
			break;
		}
		gic_active_iar[cpu] = (uint64_t)iar | GIC_IAR_TAKEN;

		if (intid == GIC_INTID_VTIMER) {
			getCpuDatap()->cpu_decrementer = -1; /* Large */
			rtclock_intr(TRUE);
		} else if (intid == GIC_SGI_SIGNAL) {
			cpu_signal_handler();
		} else if (intid == GIC_SGI_STOP) {
			psci_smp_stop_here();
		} else if (intid >= GIC_INTID_SPI_MIN && gic_spi_handle(intid)) {
			/* a driver's */
		} else {
			gic2_mask(intid);
			gic_unhandled_count++;
			gic_last_unhandled = intid;
		}

		/* the whole IAR back, the source processor of an SGI with it */
		gic_write32(gicc_base + GIC2_C_EOIR, iar);
		__builtin_arm_dsb(DSB_SY);
		gic_active_iar[cpu] = 0;
	}
}

static void
gic2_irq_quiesce(void)
{
	int cpu = cpu_number();

	for (unsigned i = 0; i < GIC_SPI_MOST; i++) {
		uint32_t intid = gic_spi[i].intid;

		if (intid != 0) {
			gic2_mask(intid);
		}
	}
	if (gic_active_iar[cpu] != 0) {
		gic_write32(gicc_base + GIC2_C_EOIR, (uint32_t)(gic_active_iar[cpu] & ~GIC_IAR_TAKEN));
		__builtin_arm_dsb(DSB_SY);
		gic_active_iar[cpu] = 0;
	}
}

static void
gic2_irq_cpu_shutdown(void)
{
	gic2_irq_quiesce();
	/* this processor's banked SGIs and PPIs: off, nothing pending, nothing active */
	gic_write32(gicd_base + GIC2_D_ICENABLER(0), 0xffffffffu);
	gic_write32(gicd_base + GIC2_D_ICPENDR(0), 0xffffffffu);
	gic_write32(gicd_base + GIC2_D_ICACTIVER(0), 0xffffffffu);
	for (int n = 0; n < 16; n += 4) {
		gic_write32(gicd_base + GIC2_D_CPENDSGIR(n), 0xffffffffu);
	}
	gic_write32(gicc_base + GIC2_C_CTLR, 0);
	__builtin_arm_dsb(DSB_SY);
}

static void
gic2_irq_cpu_init(int cpu)
{
	uint32_t mine = 1u << GIC_INTID_VTIMER;
	uint64_t mpidr = __builtin_arm_rsr64("MPIDR_EL1");

	if (gic2_cpu_mask[cpu] != 0) {
		return;
	}
	/* which CPU interface this processor is: SGI 0..3's target byte reads back its own bit */
	uint32_t self = gic_read32(gicd_base + GIC2_D_ITARGETSR(0)) & 0xff;
	if (self == 0) {
		self = gic2_mask_of((uint32_t)(mpidr & 0xffffff));
	}
	gic2_cpu_phys[cpu] = (uint32_t)(mpidr & 0xffffff);
	gic2_cpu_mask[cpu] = (uint8_t)self;

	if (ml_get_cpu_count() > 1) {
		uint32_t sgis = (1u << GIC_SGI_SIGNAL) | (1u << GIC_SGI_STOP);

		gic_write32(gicd_base + GIC2_D_ICACTIVER(0), sgis);
		gic_write32(gicd_base + GIC2_D_CPENDSGIR(0), 0xffffffffu);   /* SGI 0..3 from every source */
		*(volatile uint8_t *)(gicd_base + GIC2_D_IPRIORITYR(GIC_SGI_SIGNAL)) = GIC_PRIORITY_DEFAULT;
		*(volatile uint8_t *)(gicd_base + GIC2_D_IPRIORITYR(GIC_SGI_STOP)) = GIC_PRIORITY_DEFAULT;
		mine |= sgis;
	}
	*(volatile uint8_t *)(gicd_base + GIC2_D_IPRIORITYR(GIC_INTID_VTIMER)) = GIC_PRIORITY_DEFAULT;
	gic_write32(gicd_base + GIC2_D_ISENABLER(0), mine);

	if ((gic_read32(gicd_base + GIC2_D_CTLR) & GIC2_D_CTLR_ENABLE) == 0) {
		gic_write32(gicd_base + GIC2_D_CTLR, GIC2_D_CTLR_ENABLE);
	}
	/* no priority active (see gic_v3_irq_cpu_init), every priority accepted, no grouping, on */
	gic_write32(gicc_base + GIC2_C_APR0, 0);
	gic_write32(gicc_base + GIC2_C_PMR, 0xff);
	gic_write32(gicc_base + GIC2_C_BPR, 0);
	gic_write32(gicc_base + GIC2_C_CTLR, GIC2_C_CTLR_ENABLE);
	__builtin_arm_dsb(DSB_SY);
	__builtin_arm_isb(ISB_SY);

	ml_install_interrupt_handler(NULL, 0, NULL, gic2_irq_handler, NULL);
	kprintf("gic: v2 (GICD_TYPER 0x%x, GICC_IIDR 0x%x), distributor %p, cpu interface %p, cpu %d is interface 0x%x "
	    "(MPIDR 0x%llx), virtual timer on PPI %d\n",
	    gic_read32(gicd_base + GIC2_D_TYPER), gic_read32(gicc_base + GIC2_C_IIDR), (void *)gicd_base,
	    (void *)gicc_base, cpu, self, mpidr, GIC_INTID_VTIMER);
}
