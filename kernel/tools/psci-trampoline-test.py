#!/usr/bin/env python3
"""Execute the trampoline bytes installed by the kernel, including a 36-bit jump.
Requires Unicorn. Only the CPU's read-only MPIDR is supplied by a hardware hook.
"""
import pathlib, struct, subprocess, tempfile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X17, UC_ARM64_REG_PC
root=pathlib.Path(__file__).resolve().parents[1]
header=root/'xnu/files/osfmk/arm64/psci_trampoline.h'
assert header.exists(), 'kernel trampoline does not record physical entry'
code='''#include <stdint.h>
#include <stdio.h>
#include "psci_trampoline.h"
int main(void) { uint32_t b[48]={0}; hakios_psci_trampoline_build(b,0x88343e000ULL); return fwrite(b,1,sizeof(b),stdout)==sizeof(b)?0:1; }'''
with tempfile.TemporaryDirectory() as t:
    p=pathlib.Path(t);(p/'test.c').write_text(code)
    subprocess.run(['clang','-I',str(header.parent),str(p/'test.c'),'-o',str(p/'test')],check=True)
    blob=subprocess.check_output([str(p/'test')])
for base in (0x80088000,0x8100c040):
    uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM);uc.mem_map(base&~4095,4096);uc.mem_write(base,blob)
    high=0x88343e000;uc.mem_map(high,4096)
    uc.reg_write(UC_ARM64_REG_X0,0x13572468)
    reached=[]
    def hook(emu,addr,size,data):
        if addr==high: reached.append(addr);emu.emu_stop();return
        if struct.unpack('<I',emu.mem_read(addr,4))[0]==0xd53800b1:
            emu.reg_write(UC_ARM64_REG_X17,0x81010300);emu.reg_write(UC_ARM64_REG_PC,addr+4)
    uc.hook_add(UC_HOOK_CODE,hook);uc.emu_start(base,0,count=100)
    assert reached==[high],reached
    record=struct.unpack('<8Q',uc.mem_read(base+128,64))
    assert record[:4]==(1,0x81010300,4,base),record
    assert uc.reg_read(UC_ARM64_REG_X0)==0x13572468
print('PSCI trampoline: actual generated instructions record MPIDR/EL/PC and reach 36-bit target PASS')
