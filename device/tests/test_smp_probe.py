"""Execute the actual reset entry: catch lost 36-bit addresses, wrong mailbox,
missing progress, context dependence, and CPU_OFF with a stale argument.
Requires unicorn and pyelftools (only for this architecture test).
"""
import pathlib
import struct
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

class SecondaryEntryTests(unittest.TestCase):
    def test_secondary_records_low_and_high_entry_then_powers_off(self):
        self.assertTrue((ROOT/'loader/smp_probe.S').exists(), 'missing executable secondary probe')
        from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
        from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_X1
        from elftools.elf.elffile import ELFFile
        with tempfile.TemporaryDirectory() as t:
            p = pathlib.Path(t)
            (p/'link.ld').write_text('ENTRY(hakios_smp_low) SECTIONS { . = 0x80080000; .text : { *(.text*) } . = ALIGN(4096); .bss : { *(.bss*) } }')
            (p/'mailbox.S').write_text('.bss\n.balign 64\n.global hakios_smp_mailbox\nhakios_smp_mailbox: .skip 128\n')
            for name, src in [('probe', ROOT/'loader/smp_probe.S'), ('mailbox',p/'mailbox.S')]:
                subprocess.run(['clang','--target=aarch64-linux-gnu','-c',str(src),'-o',str(p/(name+'.o'))],check=True)
            linker = pathlib.Path.home()/'.local/zig/zig'
            subprocess.run([str(linker),'ld.lld','-T',str(p/'link.ld'),str(p/'probe.o'),str(p/'mailbox.o'),'-o',str(p/'test.elf')],check=True)
            with (p/'test.elf').open('rb') as f:
                elf=ELFFile(f)
                syms={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
                sec=elf.get_section_by_name('.text'); base=sec['sh_addr']; code=sec.data()
            high=0x8c0000000; box=syms['hakios_smp_mailbox']
            h0=syms['hakios_smp_high']; h1=syms['hakios_smp_high_end']
            for mode in ('low', 'bridge', 'direct'):
                with self.subTest(mode=mode):
                    uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
                    uc.mem_map(0x80080000,0x10000); uc.mem_write(base,code)
                    uc.mem_map(high,0x1000); uc.mem_write(high,code[h0-base:h1-base])
                    uc.mem_write(box,struct.pack('<16Q',0,0,0,0,0,0,high if mode=='bridge' else 0,*([0]*9)))
                    # An ordinary Cortex-A55 secondary in cluster 1, core 3; preserve Aff2.
                    uc.reg_write(UC_ARM64_REG_X0,box if mode=='direct' else 0xdeadbeef)
                    calls=[]
                    def hook(emu,addr,size,data):
                        insn=struct.unpack('<I',emu.mem_read(addr,4))[0]
                        if insn==0xd53800a1: # MPIDR is read-only in Unicorn; supply the hardware value
                            emu.reg_write(UC_ARM64_REG_X1,0x81010300)
                            emu.reg_write(UC_ARM64_REG_PC,addr+4)
                        if insn==0xd4000003: # SMC, observe real CPU_OFF emitted by assembly
                            calls.append(emu.reg_read(UC_ARM64_REG_X0)); emu.emu_stop()
                    uc.hook_add(UC_HOOK_CODE,hook)
                    uc.emu_start(high if mode=='direct' else syms['hakios_smp_low'],0,count=500)
                    values=struct.unpack('<16Q',uc.mem_read(box,128))
                    self.assertEqual(calls,[0x84000002])
                    self.assertEqual(values[0],2 if mode!='low' else 1)
                    self.assertEqual(values[1],0x81010300)
                    self.assertEqual(values[2],4) # CurrentEL=EL1 under the emulator
                    self.assertEqual(values[5],box if mode=='direct' else 0xdeadbeef)
                    self.assertEqual(values[7] >= high,mode!='low')
                    if mode!='direct': self.assertTrue(base<=values[4]<base+len(code))

if __name__=='__main__': unittest.main()
