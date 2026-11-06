#!/usr/bin/env python3
"""Execute the built XNU reset vector up to its validated CPU reset handler.
Does NOT emulate Samsung firmware, GIC, MMU setup or the scheduler. CPU data
is supplied as a fixture using offsets from this exact kernel build's assym.s.
"""
import pathlib,struct,subprocess,re,json
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_PC,UC_ARM64_REG_X15,UC_ARM64_REG_X21
root=pathlib.Path(__file__).resolve().parents[1]
b=(root/'hakios-kernel').read_bytes();base=0x880000000;slide=base-0xfffffff004000000
symbols={}
for line in subprocess.check_output(['nm','-n',str(root/'hakios-kernel')],text=True).splitlines():
 p=line.split()
 if len(p)==3:
  try: symbols[p[2]]=int(p[0],16)+slide
  except ValueError: pass
assym=(root/'xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/assym.s').read_text()
offsets={n:int(v) for n,v in re.findall(r'^#define (\w+) (\d+)$',assym,re.M)}
segments=[];o=32
for _ in range(struct.unpack_from('<I',b,16)[0]):
 cmd,size=struct.unpack_from('<II',b,o)
 if cmd==0x19:
  _,vm,vs,fo,fs=struct.unpack_from('<16sQQQQ',b,o+8)
  if vs: segments.append((vm+slide,vs,b[fo:fo+fs]))
 o+=size
cases=[('four-linear', [0,0x100,0x200,0x300],0x81000300,'handler'),
 ('six-linear',[0,0x100,0x200,0x300,0x400,0x500],0x81000500,'handler'),
 ('eight-linear',[i<<8 for i in range(8)],0x81000700,'handler'),
 ('live-DT-core1',[0,1,2,3,0x100,0x101,0x102,0x103],0x81000100,'handler'),
 ('aff2-cluster1',[0,0x100,0x200,0x300,0x10000,0x10100,0x10200,0x10300],0x81010300,'handler'),
 ('no-matching-id',[0,1,2,3],0x81000100,'stuck'),
 ('null-handler',[0,0x100],0x81000100,'stuck'),
 ('bad-handler',[0,0x100],0x81000100,'stuck')]
# Exercise every slot, not just the last CPU in each table.
for n in (1,2,4,6,8):
 for core in range(n):
  cases.append((f"{n}-linear-slot-{core}",[i<<8 for i in range(n)],0x81000000|(core<<8),'handler'))
rows=[]
for name,ids,mpidr,want in cases:
 uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
 lo=min(a for a,_,_ in segments)&~4095;hi=max(a+n for a,n,_ in segments);hi=(hi+4095)&~4095
 uc.mem_map(lo,hi-lo)
 for addr,size,data in segments:
  if data:uc.mem_write(addr,data)
 table=0x890000000;uc.mem_map(table,0x20000)
 uc.mem_write(symbols['_ResetHandlerData']+offsets['CPU_DATA_ENTRIES'],struct.pack('<Q',table))
 for i,id in enumerate(ids):
  cd=table+0x1000+i*0x1000
  uc.mem_write(table+i*16+offsets['CPU_DATA_PADDR'],struct.pack('<Q',cd))
  uc.mem_write(cd+offsets['CPU_PHYS_ID'],struct.pack('<I',id))
  h=0 if name=='null-handler' else table if name=='bad-handler' else symbols['_start_cpu']
  uc.mem_write(cd+offsets['CPU_RESET_HANDLER'],struct.pack('<Q',h))
 reached=[]
 def hook(em,addr,size,data):
  if addr in (symbols['_start_cpu'],symbols['_resume_idle_cpu']):
   reached.append(em.reg_read(UC_ARM64_REG_X21));em.emu_stop();return
  ins=struct.unpack('<I',em.mem_read(addr,4))[0]
  if ins==0xd53800af:
   em.reg_write(UC_ARM64_REG_X15,mpidr);em.reg_write(UC_ARM64_REG_PC,addr+4)
 uc.hook_add(UC_HOOK_CODE,hook)
 uc.emu_start(symbols['_LowResetVectorBase'],0,count=2000,timeout=2000000)
 if reached:
  actual=struct.unpack('<I',uc.mem_read(reached[0]+offsets['CPU_PHYS_ID'],4))[0]
  outcome='handler' if actual==(mpidr&0xffffff) else 'wrong-cpu'
 else:actual=None;outcome='stuck'
 row=dict(case=name,expected=want,outcome=outcome,mpidr=hex(mpidr),selected_id=None if actual is None else hex(actual),pc=hex(uc.reg_read(UC_ARM64_REG_PC)))
 print(json.dumps(row),flush=True);rows.append(row)
 assert outcome==want,row
print('Reset-vector characterization PASS. handler = validated reset handler reached, NOT scheduler or OS boot.')
