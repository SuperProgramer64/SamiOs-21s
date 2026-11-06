"""Do not hand Android a running test CPU, or call CPU_ON success 'execution'."""
import pathlib, subprocess, tempfile, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class ProbePolicyTests(unittest.TestCase):
    def test_requires_entry_and_confirmed_power_off(self):
        self.assertTrue((ROOT/'loader/smp_probe.h').exists(),'missing bounded PSCI probe controller')
        code=r'''
#include <assert.h>
#include "smp_probe.h"
volatile struct hakios_smp_record hakios_smp_mailbox;
static unsigned scenario, ticks, starts;
static int64_t hakios_smp_call(uint64_t f,uint64_t a,uint64_t b,uint64_t c) {
    if(f==0xc4000003) {
        starts++; assert(a==0x10300 && b==0x80088000 && c==(uint64_t)&hakios_smp_mailbox);
        if(scenario==1) return -2;
        if(scenario!=2) hakios_smp_mailbox.stage=scenario==4?0xff:2;
        return 0;
    }
    assert(f==0xc4000004 && a==0x10300 && b==0 && c==0);
    return scenario==3?0:1;
}
static uint64_t hakios_smp_ticks(void) { return ticks++; }
static uint64_t hakios_smp_timeout(void) { return 10; }
static void hakios_smp_barrier(void) {}
int main(void) {
    struct hakios_smp_result r;
    scenario=0; ticks=starts=0;
    r=hakios_smp_attempt(0x10300,0x80088000,0x8c0000000,2);
    assert(r.outcome==HAKIOS_SMP_OK && starts==1 && hakios_smp_mailbox.high_entry==0x8c0000000);
    scenario=1; ticks=starts=0;
    r=hakios_smp_attempt(0x10300,0x80088000,0,1);
    assert(r.outcome==HAKIOS_SMP_REJECTED && r.status==-2 && starts==1);
    scenario=2; ticks=starts=0;
    r=hakios_smp_attempt(0x10300,0x80088000,0,1);
    assert(r.outcome==HAKIOS_SMP_UNSAFE && starts==1 && ticks<20); /* accepted but never ran */
    scenario=3; ticks=starts=0;
    r=hakios_smp_attempt(0x10300,0x80088000,0,1);
    assert(r.outcome==HAKIOS_SMP_UNSAFE && starts==1 && ticks<20); /* ran, still ON */
    scenario=4; ticks=starts=0;
    r=hakios_smp_attempt(0x10300,0x80088000,0,1);
    assert(r.outcome==HAKIOS_SMP_UNSAFE); /* CPU_OFF returned */
}
'''
        with tempfile.TemporaryDirectory() as t:
            p=pathlib.Path(t);(p/'test.c').write_text(code)
            subprocess.run(['clang','-std=c11','-fsanitize=address,undefined','-I',str(ROOT/'loader'),str(p/'test.c'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
if __name__=='__main__': unittest.main()
