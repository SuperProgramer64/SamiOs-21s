"""Storage diagnostics must survive a long, noisy boot without overrunning pmsg's budget."""
import pathlib
import subprocess
import tempfile
import unittest

class LogSelectionTests(unittest.TestCase):
    def test_keeps_initial_benchmark_and_latest_stats_in_fixed_space(self):
        header = pathlib.Path(__file__).parents[1] / 'loader/logselect.h'
        self.assertTrue(header.exists(), 'missing bounded storage log selection')
        code = r'''
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "logselect.h"
int main(void) {
    struct hakios_storage_summary s = {0};
    const char *initial="[ 1]: emmc0: CMD18 verified against CMD17\n";
    hakios_storage_summary_add(&s,initial,strlen(initial));
    const char *failure="[ 2]: psci: cpu 1 (MPIDR 0x100) was started 5 seconds ago and has not joined: no C check-in\n";
    hakios_storage_summary_add(&s,failure,strlen(failure));
    const char *entry="[ 3]: psci: cpu 7 low-entry=1 MPIDR=0x81000700 EL=1 PC=0x8008e700\n";
    for(int j=0;j<50;j++) hakios_storage_summary_add(&s,entry,strlen(entry));
    const char *cores="[ 2]: GenericARMPE: 4 of 4 processors are running\n";
    hakios_storage_summary_add(&s,cores,strlen(cores));
    char line[180];
    for (int i=0;i<600;i++) {
        int n=snprintf(line,sizeof(line),"[ %d]: MiniIOMobileFramebuffer: a noisy frame\n",i);
        hakios_storage_summary_add(&s,line,n);
        n=snprintf(line,sizeof(line),"[ %d]: emmc0: %d seconds; retries 0, errors 0\n",i,i);
        hakios_storage_summary_add(&s,line,n);
        if(i==150) { const char *e="[ 150]: emmc0: IDMAC read failed (err 0xdead0009)\n"; hakios_storage_summary_add(&s,e,strlen(e)); }
    }
    assert(s.first_len <= sizeof(s.first)); assert(s.last_len <= sizeof(s.last));
    char joined[1537]={0};
    memcpy(joined,s.first,s.first_len); memcpy(joined+s.first_len,s.last,s.last_len);
    assert(strstr(joined,"CMD18 verified")); assert(strstr(joined,"599 seconds"));
    assert(!strstr(joined,"MiniIOMobileFramebuffer"));
    assert(strstr(s.cores,"4 of 4 processors are running"));
    assert(strstr(s.cores,"no C check-in"));
    assert(strstr(s.faults,"0xdead0009"));
    assert(sizeof(s.first)+sizeof(s.last)+sizeof(s.cores)+sizeof(s.faults)==2048);
    puts("bounded storage diagnostics PASS");
}
'''
        with tempfile.TemporaryDirectory() as t:
            p=pathlib.Path(t)/'test.c'; p.write_text(code)
            b=pathlib.Path(t)/'test'
            subprocess.run(['clang','-std=c11','-fsanitize=address,undefined','-I',str(header.parent),str(p),'-o',str(b)],check=True)
            subprocess.run([str(b)],check=True)

if __name__ == '__main__':
    unittest.main()
