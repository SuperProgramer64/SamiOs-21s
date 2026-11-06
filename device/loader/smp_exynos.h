/* SPDX-License-Identifier: MIT
 * Opt-in, read-only CPU bring-up experiment. It runs before loading the payload
 * and never opens storage. All tested CPUs must be confirmed OFF before chain.
 */
#ifndef HAKIOS_SMP_EXYNOS_H
#define HAKIOS_SMP_EXYNOS_H
#include "cpu_fdt.h"
#include "smp_probe.h"
#define HAKIOS_SMP_RESCUE_MAGIC 0x31504d53544b4148ULL
volatile struct hakios_smp_record hakios_smp_mailbox __attribute__((aligned(64)));
extern char hakios_smp_low[], hakios_smp_high[], hakios_smp_high_end[];
static int64_t hakios_smp_call(uint64_t f, uint64_t a, uint64_t b, uint64_t c)
{
    register uint64_t x0 __asm__("x0")=f, x1 __asm__("x1")=a,
        x2 __asm__("x2")=b, x3 __asm__("x3")=c;
    __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
        "x4","x5","x6","x7","x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","memory");
    return (int64_t)x0;
}
static uint64_t hakios_smp_ticks(void) { return dbg_ticks(); }
static uint64_t hakios_smp_timeout(void) { return dbg_freq()*2; }
static void hakios_smp_barrier(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static void hakios_smp_watchdog(uint32_t count)
{
    wr32(WDT_CL1+WTCON,rd32(WDT_CL1+WTCON)&~(WTCON_ENABLE|WTCON_RESET));
    wr32(WDT_CL0+WTCON,rd32(WDT_CL0+WTCON)&~WTCON_ENABLE);
    wr32(WDT_CL0+WTDAT,count); wr32(WDT_CL0+WTCNT,count);
    wr32(PMU_BASE+PMU_CL0_NONCPU_INT_EN,rd32(PMU_BASE+PMU_CL0_NONCPU_INT_EN)|(1u<<2));
    wr32(PMU_BASE+PMU_CL0_NONCPU_OUT,rd32(PMU_BASE+PMU_CL0_NONCPU_OUT)|(1u<<7));
    wr32(WDT_CL0+WTCON,(0xffu<<8)|(3u<<3)|WTCON_ENABLE|WTCON_RESET);
}
static void __attribute__((noreturn)) hakios_smp_abort(void)
{
    dbg_puts("smp-probe: uncertain secondary state; warm reset, no Android chain\n");
    *(volatile uint64_t *)RESCUE_PA=HAKIOS_SMP_RESCUE_MAGIC;
    hakios_smp_barrier();
    hakios_smp_watchdog(4096); /* about five seconds; preserve the RAM log */
    for(;;) __asm__ volatile("wfe");
}
static void hakios_smp_add_id(uint64_t *ids, unsigned *n, uint64_t id)
{
    for(unsigned i=0;i<*n;i++) if(ids[i]==id) return;
    if(*n<24) ids[(*n)++]=id;
}
static int hakios_smp_report(uint64_t id, unsigned mode)
{
    uint64_t low=(uint64_t)hakios_smp_low;
    /* mode 0: low entry, mode 1: low -> high, mode 2: firmware -> high. */
    struct hakios_smp_result r=hakios_smp_attempt(id,mode==2?STAGING_PA:low,
        mode==1?STAGING_PA:0,mode==0?1:2);
    dbg_puts("smp-probe: "); kv("target",id,8); kv("mode",mode,1);
    kv("ret",(uint64_t)r.status,16); kv("aff",(uint64_t)r.affinity,16);
    kv("stage",hakios_smp_mailbox.stage,2); dbg_putc('\n');
    dbg_puts("smp-probe: "); kv("MPIDR",hakios_smp_mailbox.mpidr,16);
    kv("EL",hakios_smp_mailbox.el,1); kv("SCTLR",hakios_smp_mailbox.sctlr,8);
    kv("context",hakios_smp_mailbox.context,10); dbg_putc('\n');
    dbg_puts("smp-probe: "); kv("low-PC",hakios_smp_mailbox.low_pc,10);
    kv("high-PC",hakios_smp_mailbox.high_pc,10); dbg_putc('\n');
    if(r.outcome==HAKIOS_SMP_UNSAFE) hakios_smp_abort();
    wdt_pet();
    return r.outcome==HAKIOS_SMP_OK;
}
static void __attribute__((noreturn)) hakios_smp_probe_run(uint64_t dtb)
{
    uint64_t ids[24], dt_ids[8], passed[24]; unsigned n=0, good=0;
    int64_t affinity[24];
    dbg_puts("smp-probe: read-only low/bridge/direct CPU entry experiment\n");
    kv("boot-MPIDR",SYSREG(mpidr_el1),16); kv("EL",SYSREG(CurrentEL),1);
    kv("SCTLR",SYSREG(sctlr_el1),8); dbg_putc('\n');
    if(SYSREG(CurrentEL)!=4 || (SYSREG(sctlr_el1)&5) || dtb<0x80000000ULL ||
        dtb>0xfff00000ULL || (dtb&7)) {
        dbg_puts("smp-probe: unsupported boot state or FDT address; no CPU started\n");
        chain_android();
    }
    int count=hakios_fdt_cpu_ids((const void *)dtb,0x100000,dt_ids,8);
    if(count<=0) {
        dbg_puts("smp-probe: invalid/empty live CPU tree; no CPU started\n"); chain_android();
    }
    for(int i=0;i<count;i++) {
        dbg_puts("smp-probe: live-DT "); kv("cpu",i,1); kv("reg",dt_ids[i],16); dbg_putc('\n');
        hakios_smp_add_id(ids,&n,dt_ids[i]);
    }
    /* Explicit hypotheses from the Exynos topology and the previous loader.
     * These are only started if firmware reports this exact affinity OFF. */
    for(unsigned cl=0;cl<2;cl++) for(unsigned core=0;core<4;core++)
        hakios_smp_add_id(ids,&n,((uint64_t)cl<<16)|(core<<8));
    for(unsigned core=0;core<8;core++) hakios_smp_add_id(ids,&n,core<<8);
    uint64_t len=(uint64_t)(hakios_smp_high_end-hakios_smp_high);
    if(len>4096) { dbg_puts("smp-probe: high stub too large\n"); chain_android(); }
    for(uint64_t i=0;i<len;i++) ((volatile uint8_t *)STAGING_PA)[i]=(uint8_t)hakios_smp_high[i];
    /* MMU/D-cache are off; evict stale S-Boot lines before any later cached read. */
    uint64_t line=4ULL<<((SYSREG(ctr_el0)>>16)&15);
    for(uint64_t a=STAGING_PA;a<STAGING_PA+len;a+=line) __asm__ volatile("dc ivac, %0"::"r"(a):"memory");
    __asm__ volatile("dsb sy; ic iallu; dsb sy; isb":::"memory");
    *(volatile uint64_t *)RESCUE_PA=HAKIOS_SMP_RESCUE_MAGIC;
    hakios_smp_watchdog(0xffff);
    hakios_smp_active=1;
    kv("PSCI-version",(uint64_t)hakios_smp_call(0x84000000,0,0,0),8); dbg_putc('\n');
    /* Collect every affinity before a possibly broken CPU_ON can stall. */
    for(unsigned i=0;i<n;i++) {
        affinity[i]=hakios_smp_call(0xc4000004,ids[i],0,0);
        dbg_puts("smp-probe: "); kv("candidate",ids[i],8); kv("state",(uint64_t)affinity[i],16); dbg_putc('\n');
    }
    for(unsigned i=0;i<n;i++) {
        if(affinity[i]!=1) continue; /* ON includes the boot CPU; never touch it. */
        dbg_puts("smp-probe: starting "); kv("target",ids[i],8); dbg_putc('\n'); hakios_smp_barrier();
        if(hakios_smp_report(ids[i],0) && hakios_smp_report(ids[i],1)) passed[good++]=ids[i];
    }
    /* Direct high entry is last: its known firmware failure must not hide the
     * topology and low->high results for the other processors. */
    for(unsigned i=0;i<good;i++) hakios_smp_report(passed[i],2);
    dbg_puts("smp-probe: completed; all tested CPUs OFF; Android\n");
    *(volatile uint64_t *)RESCUE_PA=0;
    chain_android();
}
#endif
