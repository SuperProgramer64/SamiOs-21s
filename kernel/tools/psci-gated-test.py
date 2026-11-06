#!/usr/bin/env python3
"""Execute real gated-entry bytes: wait, context preservation, high jump and CPU_OFF."""
import pathlib,struct,subprocess,tempfile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X17,UC_ARM64_REG_PC
root=pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as t:
 p=pathlib.Path(t);(p/'t.c').write_text('#include <stdint.h>\n#include <stdio.h>\n#include "psci_trampoline.h"\nint main(void){uint32_t p[64]={0};hakios_psci_trampoline_build_gated(p,0x88343e000ULL);return fwrite(p,1,sizeof(p),stdout)!=sizeof(p);}')
 subprocess.run(['clang','-I',str(root/'xnu/files/osfmk/arm64'),str(p/'t.c'),'-o',str(p/'t')],check=True);blob=subprocess.check_output([str(p/'t')])
for cpu in (1,3,7):
 for flag in (1,2):
  base=0x81000000+cpu*256;high=0x88343e000;uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM);uc.mem_map(0x81000000,4096);uc.mem_write(base,blob);uc.mem_map(high,4096);uc.reg_write(UC_ARM64_REG_X0,0x13572468);events=[]
  def hook(em,pc,size,data):
   ins=struct.unpack('<I',em.mem_read(pc,4))[0]
   if pc==high:events.append('high');em.emu_stop()
   elif ins==0xd53800b1:em.reg_write(UC_ARM64_REG_X17,0x81000000|(cpu<<8));em.reg_write(UC_ARM64_REG_PC,pc+4)
   elif ins==0xd4000003:
    assert em.reg_read(UC_ARM64_REG_X0)==0x84000002;events.append('off');em.emu_stop()
  uc.hook_add(UC_HOOK_CODE,hook);uc.emu_start(base,0,count=80);assert not events
  r=struct.unpack('<8Q',uc.mem_read(base+128,64));assert r[:4]==(1,0x81000000|(cpu<<8),4,base),r
  assert uc.reg_read(UC_ARM64_REG_X0)==0x13572468
  uc.mem_write(base+160,struct.pack('<Q',flag));uc.emu_start(uc.reg_read(UC_ARM64_REG_PC),0,count=100)
  assert events==(['high'] if flag==1 else ['off']),events
  if flag==1:assert uc.reg_read(UC_ARM64_REG_X0)==0x13572468
print('Gated PSCI entry: six instruction-level wait/release/abort cases PASS')
