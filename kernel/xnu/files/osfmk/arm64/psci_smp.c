/*
 * More than one processor on generic (non-Apple) ARMv8 boards.
 *
 * On Apple hardware the other cores are powered up by closed drivers. Here
 * the firmware does it, through the standard power interface of ARM systems
 * (PSCI): CPU_ON starts a core at an address of the caller's choice, CPU_OFF
 * stops the calling core for good. The address we give is the kernel's own
 * reset vector (LowResetVectorBase in start.s), so that a new core takes the
 * path Apple's cores take: it finds its cpu data by its MPIDR, turns the MMU
 * on and comes out in arm_init_cpu(). The calls are made with SMC or, under
 * a hypervisor that implements them itself, with HVC; the boot loader says
 * which (/chosen/psci-method).
 *
 * The platform expert (iokit/Kernel/GenericARMPE.cpp) registers the cores of
 * the device tree and starts them here, one after the other. Signals between
 * processors are interrupts of the GIC (gic_v3_irq.c).
 *
 * A new core begins with its MMU and caches off, and what it reads then it
 * reads from memory. On real hardware that is not the same as what the
 * running processors see: under Qualcomm's hypervisor (the phone this was
 * written for) a line that is dirty in another core's cache is simply not
 * there yet. QEMU has no cache and shows none of this. So before CPU_ON
 * everything that core will read before its MMU is on is written back to
 * memory (psci_smp_start_cpu()), and what it writes in that state is read
 * only after throwing away the reader's cached copy.
 *
 * The other half is the way out. The kernel ends by returning to the boot
 * loader (loader_return.c), and the loader gets back only what it started:
 * one core, the boot processor's. psci_smp_stop_others() makes every other
 * core switch itself off first, wherever it is.
 */
#include <mach/mach_types.h>
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <kern/debug.h>
#include <kern/kalloc.h>
#include <kern/misc_protos.h>
#include <libkern/kernel_mach_header.h>
#include <machine/atomic.h>
#include <arm/caches_internal.h>
#include <arm/cpu_data_internal.h>
#include <arm/cpu_internal.h>
#include <arm/machine_routines.h>
#include <arm64/proc_reg.h>
#include <arm64/psci_smp.h>
#include <arm64/psci_trampoline.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <pexpert/arm64/board_config.h>
#include <vm/pmap.h>

#if defined(GENERIC_PSCI_SMP)

#define PSCI_CPU_OFF            0x84000002u
#define PSCI_CPU_ON             0xC4000003u
#define PSCI_AFFINITY_INFO      0xC4000004u
#define PSCI_AFFINITY_OFF       1

#define PSCI_SMP_START_SECONDS  5       /* how long a started core is waited for */
#define PSCI_SMP_STOP_SECONDS   3       /* ... and the cores that were told to stop */

extern void loader_return(void);
extern void loader_log_putc(char c);
extern unsigned long gVirtBase, gPhysBase;

/* The firmware is called with HVC, not SMC. (start.s reads this too, at its physical address.) */
uint32_t psci_smp_hvc;

/*
 * Left by a core that took an exception between its first instruction and the kernel's vectors
 * (Lsecondary_early_exception in start.s). That core writes it with its cache off; it has a
 * cache line to itself so that looking at memory does not disturb anything else.
 */
struct psci_smp_start_fault {
	uint64_t valid, mpidr, esr, elr, far, lr, sp, unused;
} psci_smp_start_fault __attribute__((aligned(64)));
_Static_assert(sizeof(struct psci_smp_start_fault) == 64, "one cache line");

static uint32_t psci_smp_cpu_stage[MAX_CPUS];   /* PSCI_SMP_CPU_*, by cpu number */

/* psci_trampoline=PA: the loader reserves at least 192 bytes in low RAM.
 * Record actual physical entry before branching to the kernel's reset vector.
 * CPU_ON success alone, or absence of a C check-in, cannot prove execution.
 */
static uint64_t psci_smp_trampoline;
static vm_offset_t psci_smp_trampoline_va;
/* Mailbox learning adapted from the Exynos850 handshake in simpleXNU/ios-a135f
 * (Schoosh / Bruhboy666dev). Topology is read-only after lockdown: keep aliases here.
 */
static uint32_t psci_smp_learn, psci_real_id[MAX_CPUS], psci_real_valid[MAX_CPUS];
static uint64_t psci_real_target[MAX_CPUS];
int hakios_psci_cpu_number(uint32_t id);
uint32_t hakios_psci_cpu_id_is_learned(unsigned cpu);
uint32_t hakios_psci_cpu_phys_id(int cpu);
int hakios_psci_cpu_number(uint32_t id) {
    id &= 0xffffff;
    for(unsigned i=0;i<MAX_CPUS;i++)
        if(os_atomic_load(&psci_real_valid[i],acquire) && psci_real_id[i]==id) return (int)i;
    return -1;
}
uint32_t hakios_psci_cpu_id_is_learned(unsigned cpu) {
    return cpu<MAX_CPUS && os_atomic_load(&psci_real_valid[cpu],acquire);
}
uint32_t hakios_psci_cpu_phys_id(int cpu) { return cpu_datap(cpu)->cpu_phys_id; }

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

/*
 * psci_ids=1 (Exynos 850): the id PSCI wants is not the core's MPIDR. Its eight A55s have MT set and
 * the core number in Aff1 (MPIDR 0x81000n00, n = 0..7), and cpu_phys_id is that (reset_vector in
 * start.s matches it), but Samsung's firmware takes the device tree's ids for CPU_ON and
 * AFFINITY_INFO: 0x0-0x3 for the first cluster, 0x100-0x103 for the second. Found by the ios-a135f
 * port on the Galaxy A13 (same SoC). Before this, CPU_ON(0x100) "for cpu 1" was answered 0 and
 * started core 4, which found no cpu data with its MPIDR (0x400) and went away: "no sign of it".
 */
static uint32_t psci_smp_ids = 2;           /* 2: not asked yet */

static uint64_t
psci_smp_target(uint32_t phys_id)
{
	int learned=hakios_psci_cpu_number(phys_id);
	if(learned>=0) return psci_real_target[learned];
	if (psci_smp_ids == 2) {
		uint32_t v = 0;
		psci_smp_ids = PE_parse_boot_argn("psci_ids", &v, sizeof(v)) && v == 1;
	}
	if (!psci_smp_ids || (phys_id & 0xff) != 0 || (phys_id & 0xff0000) != 0) {
		return phys_id;
	}
	uint32_t core = (phys_id >> 8) & 0xff;
	return core < 4 ? core : 0x100 + (core - 4);
}

static uint64_t
psci_smp_entry_via_trampoline(uint64_t entry, int cpu)
{
	if (psci_smp_trampoline == 0 &&
	    !PE_parse_boot_argn("psci_trampoline", &psci_smp_trampoline, sizeof(psci_smp_trampoline))) {
		psci_smp_trampoline = 1;
	}
	if (psci_smp_trampoline <= 1) {
		return entry;
	}
	if (psci_smp_trampoline_va == 0) {
		uint64_t offset = psci_smp_trampoline & 0x3fffULL;
		if ((psci_smp_trampoline & 63) || offset > 0x4000 - (psci_smp_learn ? 2048 : 192)) {
			printf("psci: invalid trampoline alignment/range; using direct entry\n");
			psci_smp_trampoline = 1;
			return entry;
		}
		vm_offset_t mapped = ml_io_map(psci_smp_trampoline & ~0x3fffULL, 0x4000);
		if (!mapped) {
			printf("psci: cannot map trampoline; using direct entry\n");
			psci_smp_trampoline = 1;
			return entry;
		}
		psci_smp_trampoline_va = mapped + offset;
		printf("psci: traced low entry at 0x%llx -> reset vector 0x%llx\n", psci_smp_trampoline, entry);
	}
	vm_offset_t slot = psci_smp_trampoline_va + (psci_smp_learn ? cpu * 256 : 0);
	if (psci_smp_learn) hakios_psci_trampoline_build_gated((volatile uint32_t *)slot, entry);
	else hakios_psci_trampoline_build((volatile uint32_t *)slot, entry);
	volatile uint64_t *record = (volatile uint64_t *)(psci_smp_trampoline_va + (psci_smp_learn ? cpu * 256 : 0) + 128);
	for (unsigned i = 0; i < 8; i++) {
		record[i] = 0;
	}
	__builtin_arm_dsb(DSB_SY);
	return psci_smp_trampoline + (psci_smp_learn ? cpu * 256 : 0);
}

static void
psci_smp_trampoline_report(int cpu)
{
	if (!psci_smp_trampoline_va) {
		return;
	}
	/* ml_io_map is uncached: the secondary's MMU-off stores are read from RAM. */
	volatile uint64_t *r = (volatile uint64_t *)(psci_smp_trampoline_va + (psci_smp_learn ? cpu * 256 : 0) + 128);
	__builtin_arm_dsb(DSB_SY);
	printf("psci: cpu %d low-entry=%llu MPIDR=0x%llx EL=%llu PC=0x%llx\n",
	    cpu, r[0], r[1], r[2] >> 2, r[3]);
}
static uint32_t psci_smp_started;               /* a bit per core that CPU_ON was granted for */

static int      psci_smp_stop_owner = -1;       /* the processor that began the return to the boot loader */
static bool     psci_smp_boot_wanted;           /* it was not the boot processor: that one is to finish */
static bool     psci_smp_boot_busy;             /* ... and has begun to */
static bool     psci_smp_stopping;              /* every other core switches itself off */

static int64_t
psci_call(uint64_t function, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = arg1;
	register uint64_t x2 __asm__("x2") = arg2;
	register uint64_t x3 __asm__("x3") = arg3;

	if (psci_smp_hvc) {
		__asm__ volatile ("hvc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
                    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	} else {
		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
                    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	}
	return (int64_t)x0;
}

static const char *
psci_error(int64_t status)
{
	static const char *const names[] = {
		"success", "not supported", "invalid parameters", "denied", "already on", "on pending",
		"internal failure", "not present", "disabled", "invalid address",
	};

	return (status <= 0 && status > -(int64_t)(sizeof(names) / sizeof(names[0]))) ? names[-status] : "?";
}

void
psci_smp_init(void)
{
	DTEntry chosen;
	void const *prop;
	unsigned int size;

	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) == kSuccess &&
	    SecureDTGetProperty(chosen, "psci-method", &prop, &size) == kSuccess &&
	    size >= 3 && strncmp((const char *)prop, "hvc", 3) == 0) {
		psci_smp_hvc = 1;
	}
}

/* A processor other than the boot processor says how far it has come. */
void
psci_smp_cpu_reached(uint32_t stage)
{
	os_atomic_store(&psci_smp_cpu_stage[cpu_number()], stage, release);
}

/* ... all the way: from GenericARMCPU::initCPU(), where the scheduler takes it. */
void
psci_smp_cpu_running(void)
{
	psci_smp_cpu_reached(PSCI_SMP_CPU_RUNNING);
}

static void
psci_smp_clean_segment(const char *name)
{
	unsigned long size = 0;
	vm_offset_t start = (vm_offset_t)getsegdatafromheader(&_mh_execute_header, name, &size);

	if (start != 0 && size != 0) {
		CleanPoC_DcacheRegion(start, size);
	}
}

/*
 * Start a registered processor: the platform's half of cpu_start(). Waits until the core has
 * joined the scheduler, and says what became of it if it has not: on a phone the log is all
 * there is to go by.
 */
kern_return_t
psci_smp_start_cpu(int cpu)
{
	cpu_data_t *cdp = cpu_datap(cpu);
	uint32_t phys_id = cdp->cpu_phys_id, stage;
	(void)PE_parse_boot_argn("psci_learn", &psci_smp_learn, sizeof(psci_smp_learn));
	psci_smp_learn = psci_smp_learn == 1;
	uint64_t entry = ml_static_vtop((vm_offset_t)&LowResetVectorBase);
	uint64_t now, deadline;
	int64_t status;

	/*
	 * What that core reads with its MMU off: the code and the page tables of start.s, and what
	 * the reset vector finds its way by (ResetHandlerData, CpuDataEntries, the boot arguments,
	 * cpu_ttep, psci_smp_hvc ...), all of it in the kernel's own segments; and the cpu data of
	 * every processor, which the reset vector goes through looking for its own.
	 * Clean, not flush: the processors that are running keep using all of it.
	 */
	psci_smp_start_fault.valid = 0;
	psci_smp_clean_segment("__TEXT");
	psci_smp_clean_segment("__DATA_CONST");
	psci_smp_clean_segment("__TEXT_EXEC");
	psci_smp_clean_segment("__DATA");
	psci_smp_clean_segment("__BOOTDATA");
	for (int other = 0; other <= ml_get_max_cpu_number(); other++) {
		if (cpu_datap(other) != NULL) {
			CleanPoC_DcacheRegion((vm_offset_t)cpu_datap(other), sizeof(cpu_data_t));
		}
	}
	__builtin_arm_dsb(DSB_SY);

	os_atomic_store(&psci_smp_cpu_stage[cpu], PSCI_SMP_CPU_ASKED, release);
	if (psci_smp_target(phys_id) != phys_id) {
		printf("psci: cpu %d: MPIDR 0x%x is PSCI id 0x%llx (psci_ids=1)\n", cpu, phys_id, psci_smp_target(phys_id));
	}
	uint64_t target=psci_smp_target(phys_id);
	uint64_t low = psci_smp_entry_via_trampoline(entry, cpu);
	if(psci_smp_learn && low == entry) {
		printf("psci: cpu %d learning requires reserved low trampoline; refused\n",cpu);
		return KERN_FAILURE;
	}
	status = psci_call(PSCI_CPU_ON, target, low, 0);
	if (status != 0) {
		os_atomic_store(&psci_smp_cpu_stage[cpu], 0, release);
		printf("psci: cpu %d (MPIDR 0x%x) was not started: the firmware answers %lld (%s)\n",
		    cpu, phys_id, status, psci_error(status));
		return KERN_FAILURE;
	}
	os_atomic_or(&psci_smp_started, 1u << cpu, relaxed);
    if(psci_smp_learn) {
        volatile uint64_t *r=(volatile uint64_t *)(psci_smp_trampoline_va+cpu*256+128);
        clock_interval_to_deadline(2,NSEC_PER_SEC,&deadline);
        while(r[0]!=1 && mach_absolute_time()<deadline) delay_for_interval(1,NSEC_PER_MSEC);
        __builtin_arm_dsb(DSB_SY);
        if(r[0]!=1) {
            /* A late arrival sees abort; never reuse this CPU's private slot. */
            r[4]=2; __builtin_arm_dsb(DSB_SY);
            psci_smp_trampoline_report(cpu);
            printf("psci: cpu %d did not reach learning entry; abort requested\n",cpu);
            return KERN_FAILURE;
        }
        uint32_t actual=(uint32_t)r[1]&0xffffff;
        int duplicate=hakios_psci_cpu_number(actual);
        if((r[2]>>2)!=1 || actual==cpu_datap(master_cpu)->cpu_phys_id ||
            (duplicate>=0 && duplicate!=cpu)) {
            r[4]=2; __builtin_arm_dsb(DSB_SY);
            printf("psci: cpu %d rejected low-entry MPIDR 0x%x EL %llu\n",cpu,actual,r[2]>>2);
            return KERN_FAILURE;
        }
        cdp->cpu_phys_id=actual;
        psci_real_id[cpu]=actual;
        psci_real_target[cpu]=target;
        os_atomic_store(&psci_real_valid[cpu],1,release);
        CleanPoC_DcacheRegion((vm_offset_t)cdp,sizeof(*cdp));
        CleanPoC_DcacheRegion((vm_offset_t)psci_real_id,sizeof(psci_real_id));
        CleanPoC_DcacheRegion((vm_offset_t)psci_real_valid,sizeof(psci_real_valid));
        __builtin_arm_dsb(DSB_SY);
        printf("psci: cpu %d learned MPIDR 0x%x for PSCI id 0x%llx; released\n",cpu,actual,target);
        r[4]=1; __builtin_arm_dsb(DSB_SY);
    }


	clock_interval_to_deadline(PSCI_SMP_START_SECONDS, NSEC_PER_SEC, &deadline);
	do {
		stage = os_atomic_load(&psci_smp_cpu_stage[cpu], acquire);
		if (stage >= PSCI_SMP_CPU_RUNNING) {
			psci_smp_trampoline_report(cpu);
			printf("psci: cpu %d joined the XNU scheduler\n", cpu);
			return KERN_SUCCESS;
		}
		/* memory, not this processor's copy of it */
		FlushPoC_DcacheRegion((vm_offset_t)&psci_smp_start_fault, sizeof(psci_smp_start_fault));
		if (psci_smp_start_fault.valid != 0) {
			break;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
		now = mach_absolute_time();
	} while (now < deadline);

	psci_smp_trampoline_report(cpu);
	if (psci_smp_start_fault.valid != 0) {
		struct psci_smp_start_fault *f = &psci_smp_start_fault;
		uint64_t to_virtual = gVirtBase - gPhysBase;

		printf("psci: cpu %d (MPIDR 0x%llx) took an exception on its way in and has switched itself off: "
		    "ESR 0x%llx ELR 0x%llx FAR 0x%llx LR 0x%llx SP 0x%llx (ELR and LR as kernel addresses: 0x%llx 0x%llx)\n",
		    cpu, f->mpidr, f->esr, f->elr, f->far, f->lr, f->sp, f->elr + to_virtual, f->lr + to_virtual);
		os_atomic_andnot(&psci_smp_started, 1u << cpu, relaxed);        /* nothing of it is left to stop */
	} else {
		printf("psci: cpu %d (MPIDR 0x%x) was started %d seconds ago and has not joined: %s\n",
		    cpu, phys_id, PSCI_SMP_START_SECONDS,
		    stage >= PSCI_SMP_CPU_IN_KERNEL ? "it got as far as arm_init_cpu()" : "no C check-in (see low-entry witness)");
	}
	return KERN_FAILURE;
}

/*
 * The calling processor switches itself off. Its interrupted work, if any, is abandoned where
 * it was: only for the end of the system.
 */
void
psci_smp_cpu_off(void)
{
	__builtin_arm_wsr("DAIFSet", DAIFSC_ALL);
	/* leave nothing behind that would interrupt a core that is no longer there */
	__builtin_arm_wsr64("CNTV_CTL_EL0", CNTV_CTL_EL0_IMASKED);
	gic_v3_irq_cpu_shutdown();
	os_atomic_store(&psci_smp_cpu_stage[cpu_number()], PSCI_SMP_CPU_OFF, release);
	__builtin_arm_dsb(DSB_SY);

	(void) psci_call(PSCI_CPU_OFF, 0, 0, 0);
	for (;;) {
		__builtin_arm_wfi();    /* the firmware refused: stay out of the way all the same */
	}
}

static void
psci_smp_note(const char *what, int cpu)
{
	static const char prefix[] = "\npsci: cpu ";

	for (const char *c = prefix; *c; c++) {
		loader_log_putc(*c);
	}
	loader_log_putc((char)('0' + cpu));
	for (const char *c = what; *c; c++) {
		loader_log_putc(*c);
	}
}

/* Tell every core but this one to switch itself off (psci_smp_stop_here() is where they hear it). */
static void
psci_smp_stop_rest(void)
{
	int me = cpu_number(), max_cpu = ml_get_max_cpu_number();

	os_atomic_store(&psci_smp_stopping, true, release);
	__builtin_arm_dsb(DSB_SY);
	for (int cpu = 0; cpu <= max_cpu; cpu++) {
		cpu_data_t *cdp = cpu_datap(cpu);

		if (cpu != me && cdp != NULL && (cpu == master_cpu || (psci_smp_started & (1u << cpu)))) {
			gic_v3_irq_signal_stop(cdp->cpu_phys_id);
		}
	}
	/* for those that wait with their interrupts off (wait_while_mp_kdp_trap(), after a panic) */
	__builtin_arm_sev();
}

/*
 * The system is going back to the boot loader (loader_return()). Returns on the boot processor
 * only, once the other cores are off -- or have been given up on. On any other processor it
 * does not return: that one hands the job to the boot processor, which the loader's side of
 * the return belongs to, and switches itself off like the rest.
 */
void
psci_smp_stop_others(void)
{
	int me, max_cpu;
	uint64_t now, deadline;

	if (psci_smp_started == 0) {
		return;                 /* there is only this one */
	}
	__builtin_arm_wsr("DAIFSet", DAIFSC_ALL);
	me = cpu_number();
	max_cpu = ml_get_max_cpu_number();
	if (!os_atomic_cmpxchg(&psci_smp_stop_owner, -1, me, acq_rel) && psci_smp_stop_owner != me && me != master_cpu) {
		psci_smp_cpu_off();     /* another processor is at it already */
	}

	if (me != master_cpu) {
		/*
		 * Nobody is stopped before the boot processor has answered: a core that is switched
		 * off while it holds a lock the boot processor is waiting for would take the return
		 * with it.
		 */
		psci_smp_note(" ends the system: the return to the boot loader is the boot processor's to make\n", me);
		os_atomic_store(&psci_smp_boot_wanted, true, release);
		__builtin_arm_dsb(DSB_SY);
		gic_v3_irq_signal_stop(cpu_datap(master_cpu)->cpu_phys_id);
		__builtin_arm_sev();
		now = mach_absolute_time();
		nanoseconds_to_absolutetime(PSCI_SMP_STOP_SECONDS * NSEC_PER_SEC, &deadline);
		deadline += now;
		while (!os_atomic_load(&psci_smp_stopping, acquire) && now < deadline) {
			now = mach_absolute_time();
		}
		if (!os_atomic_load(&psci_smp_stopping, acquire)) {
			/*
			 * It does not hear (stuck with its interrupts off). There is no return without
			 * it; a loader that watches from its own core will find the kernel silent and
			 * take the log as it is, and so this goes into the log.
			 */
			psci_smp_note(" cannot return to the boot loader and the boot processor does not answer: all cores stop here\n", me);
			psci_smp_stop_rest();
		}
		psci_smp_cpu_off();
	}

	psci_smp_stop_rest();
	now = mach_absolute_time();
	nanoseconds_to_absolutetime(PSCI_SMP_STOP_SECONDS * NSEC_PER_SEC, &deadline);
	deadline += now;
	for (int cpu = 0; cpu <= max_cpu; cpu++) {
		cpu_data_t *cdp = cpu_datap(cpu);

		if (cpu == me || cdp == NULL || !(psci_smp_started & (1u << cpu))) {
			continue;
		}
		while (now < deadline) {
			if (os_atomic_load(&psci_smp_cpu_stage[cpu], acquire) == PSCI_SMP_CPU_OFF) {
				/* it has said it is going; the firmware knows when it has gone (if it can tell at all) */
				int64_t state = psci_call(PSCI_AFFINITY_INFO, psci_smp_target(cdp->cpu_phys_id), 0, 0);

				if (state == PSCI_AFFINITY_OFF || state < 0) {
					break;
				}
			}
			now = mach_absolute_time();
		}
		if (os_atomic_load(&psci_smp_cpu_stage[cpu], acquire) != PSCI_SMP_CPU_OFF &&
		    psci_call(PSCI_AFFINITY_INFO, psci_smp_target(cdp->cpu_phys_id), 0, 0) != PSCI_AFFINITY_OFF) {
			psci_smp_note(" did not stop: it is left running\n", cpu);
		}
	}
}

/*
 * Where a processor learns that the system is ending: the interrupt psci_smp_stop_rest() sends,
 * and the loop in which the processors wait while another one panics (they have their
 * interrupts off there). Comes back if there is nothing of the kind going on.
 */
void
psci_smp_stop_here(void)
{
	if (psci_smp_started == 0) {
		return;
	}
	if (cpu_number() != master_cpu) {
		if (os_atomic_load(&psci_smp_stopping, acquire)) {
			psci_smp_cpu_off();
		}
		return;
	}
	if (os_atomic_load(&psci_smp_boot_wanted, acquire) && !psci_smp_boot_busy) {
		psci_smp_boot_busy = true;
		loader_return();        /* stops the rest (psci_smp_stop_others()); back only if no loader listens */
		for (;;) {
			__builtin_arm_wfi();
		}
	}
}

#endif /* defined(GENERIC_PSCI_SMP) */
