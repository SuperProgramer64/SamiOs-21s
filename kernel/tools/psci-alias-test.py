#!/usr/bin/env python3
"""Run the actual learned-ID lookup and XNU topology lookup against shuffled affinities."""
import pathlib,tempfile,subprocess
root=pathlib.Path(__file__).resolve().parents[1]
s=(root/'xnu/files/osfmk/arm64/psci_smp.c').read_text()
m=(root/'xnu-src/xnu-8796.121.2/osfmk/arm64/machine_routines.c').read_text()
def func(s,key):
 start=s.index(key);end=s.index('{',start);depth=1;i=end+1
 while depth:
  depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[start:i]
code='''#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#define MAX_CPUS 8
#define GENERIC_PSCI_SMP 1
#define MPIDR_AFF0_MASK 0xff
#define MPIDR_AFF1_MASK 0xff00
#define MPIDR_AFF2_MASK 0xff0000
#define os_atomic_load(p,o) (*(p))
static uint32_t psci_real_id[8],psci_real_valid[8];
static struct {unsigned num_cpus;struct {uint32_t phys_id;} cpus[8];} topology_info;
'''
code+=func(s,'int hakios_psci_cpu_number(uint32_t id) {')+'\n'+func(s,'uint32_t hakios_psci_cpu_id_is_learned(unsigned cpu) {')+'\n'+func(m,'int\nml_get_cpu_number(uint32_t phys_id)')
code+='''
int main(void){topology_info.num_cpus=8;for(unsigned i=0;i<8;i++)topology_info.cpus[i].phys_id=i<<8;
assert(ml_get_cpu_number(0x81000000)==0);assert(ml_get_cpu_number(0x81000300)==3);
psci_real_id[1]=0x10300;psci_real_valid[1]=1;
assert(ml_get_cpu_number(0x81010300)==1);assert(ml_get_cpu_number(0x81000300)==3);
assert(ml_get_cpu_number(0x81000100)==-1);assert(ml_get_cpu_number(0x81000000)==0);
psci_real_id[4]=0x300;psci_real_valid[4]=1;assert(ml_get_cpu_number(0x81000300)==4);
assert(ml_get_cpu_number(0x81000400)==-1);assert(ml_get_cpu_number(0x81000800)==-1);
puts("actual XNU ID lookup: learned affinity, Aff2, stale alias rejection and boot CPU PASS");}
'''
with tempfile.TemporaryDirectory() as t:
 p=pathlib.Path(t);(p/'t.c').write_text(code);subprocess.run(['clang','-std=c11','-fsanitize=address,undefined',str(p/'t.c'),'-o',str(p/'t')],check=True);subprocess.run([str(p/'t')],check=True)
