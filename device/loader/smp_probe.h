/* SPDX-License-Identifier: MIT */
#ifndef HAKIOS_SMP_PROBE_H
#define HAKIOS_SMP_PROBE_H
#include <stdint.h>
#include <stddef.h>
struct hakios_smp_record {
    uint64_t stage, mpidr, el, sctlr, low_pc, context, high_entry, high_pc;
    uint64_t reserved[8];
};
_Static_assert(offsetof(struct hakios_smp_record, context)==40, "assembly context");
_Static_assert(offsetof(struct hakios_smp_record, high_entry)==48, "assembly jump");
_Static_assert(offsetof(struct hakios_smp_record, high_pc)==56, "assembly PC");
_Static_assert(sizeof(struct hakios_smp_record)==128, "private cache lines");
extern volatile struct hakios_smp_record hakios_smp_mailbox;
static int64_t hakios_smp_call(uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t hakios_smp_ticks(void);
static uint64_t hakios_smp_timeout(void);
static void hakios_smp_barrier(void);
enum { HAKIOS_SMP_OK, HAKIOS_SMP_REJECTED, HAKIOS_SMP_UNSAFE };
struct hakios_smp_result { int outcome; int64_t status, affinity; };
/* The caller has checked OFF and has exclusive use of the mailbox. No next
 * attempt, Android chain or XNU boot is allowed after an UNSAFE result. */
static struct hakios_smp_result
hakios_smp_attempt(uint64_t id, uint64_t entry, uint64_t high, uint64_t expected)
{
    volatile uint64_t *p=(volatile uint64_t *)&hakios_smp_mailbox;
    for(unsigned i=0;i<sizeof(hakios_smp_mailbox)/8;i++) p[i]=0;
    hakios_smp_mailbox.high_entry=high;
    hakios_smp_barrier();
    struct hakios_smp_result r={HAKIOS_SMP_UNSAFE,0,-1};
    r.status=hakios_smp_call(0xc4000003, id, entry, (uint64_t)&hakios_smp_mailbox);
    if(r.status) {
        /* ALREADY_ON/ON_PENDING are not harmless rejections. */
        if(r.status!=-4 && r.status!=-5) r.outcome=HAKIOS_SMP_REJECTED;
        return r;
    }
    uint64_t begin=hakios_smp_ticks(), limit=hakios_smp_timeout();
    do {
        hakios_smp_barrier();
        r.affinity=hakios_smp_call(0xc4000004,id,0,0);
        if(hakios_smp_mailbox.stage==expected && r.affinity==1) {
            r.outcome=HAKIOS_SMP_OK; return r;
        }
        if(hakios_smp_mailbox.stage==0xff) return r;
    } while(hakios_smp_ticks()-begin<limit);
    return r;
}
#endif
