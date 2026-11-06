"""Exercise the real bounded parser on CPU topology and malformed input."""
import pathlib
import struct
import subprocess
import tempfile
import unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
def fdt(ids, cells=1, duplicate=False):
    strings=b'#address-cells\0reg\0device_type\0'
    def word(*v): return struct.pack('>'+'I'*len(v),*v)
    def aligned(s): return s+b'\0'*((-len(s))%4)
    def node(s): return word(1)+aligned(s.encode()+b'\0')
    def prop(name,data): return word(3,len(data),strings.index(name.encode()+b'\0'))+aligned(data)
    tree=node('')+node('cpus')+prop('#address-cells',word(cells))
    # A cpu-map child has reg-like data but is not a CPU.
    tree+=node('cpu-map')+prop('reg',word(99))+word(2)
    for i,v in enumerate(ids):
        tree+=node('cpu@%x'%v)+prop('reg',word(v) if cells==1 else struct.pack('>Q',v))+prop('device_type',b'cpu\0')+word(2)
    tree+=word(2,2,9)
    off=56; ss=off+len(tree); total=ss+len(strings)
    return word(0xd00dfeed,total,off,ss,40,17,16,0,len(strings),len(tree))+bytes(16)+tree+strings

class CPUFDTTests(unittest.TestCase):
    def test_live_tree_and_bounds(self):
        self.assertTrue((ROOT/'loader/cpu_fdt.h').exists(),'missing bounded live CPU parser')
        code='''#include <stdio.h>
#include "cpu_fdt.h"
int main(int argc,char **argv) { unsigned char b[65536]; uint64_t ids[8]; FILE *f=fopen(argv[1],"rb"); if(!f) return 2; size_t n=fread(b,1,sizeof(b),f); fclose(f); int rc=hakios_fdt_cpu_ids(b,n,ids,8); printf("%d",rc); for(int i=0;i<rc;i++) printf(" %llx",(unsigned long long)ids[i]); puts(""); }'''
        with tempfile.TemporaryDirectory() as t:
            p=pathlib.Path(t);(p/'test.c').write_text(code)
            subprocess.run(['clang','-fsanitize=address,undefined','-I',str(ROOT/'loader'),str(p/'test.c'),'-o',str(p/'test')],check=True)
            def run(blob):
                (p/'in').write_bytes(blob)
                return subprocess.check_output([str(p/'test'),str(p/'in')],text=True).strip()
            self.assertEqual(run(fdt([0,1,2,3,0x100,0x101,0x102,0x103])),'8 0 1 2 3 100 101 102 103')
            self.assertEqual(run(fdt([0,0x100,0x10000,0x10300],2)),'4 0 100 10000 10300')
            self.assertEqual(run(fdt([0,0x100000000],2)),'2 0 100000000')
            self.assertEqual(run(fdt([0,0])),'-1')
            self.assertEqual(run(fdt(list(range(9)))),'-1')
            self.assertEqual(run(fdt([])),'0')
            for n in (0,8,39,55,80,len(fdt([0]))-1): self.assertEqual(run(fdt([0])[:n]),'-1')
            corrupt=bytearray(fdt([0]));struct.pack_into('>I',corrupt,12,0xfffffffc)
            self.assertEqual(run(corrupt),'-1')
            corrupt=bytearray(fdt([0]));struct.pack_into('>I',corrupt,36,0xfffffff0)
            self.assertEqual(run(corrupt),'-1')
if __name__=='__main__': unittest.main()
