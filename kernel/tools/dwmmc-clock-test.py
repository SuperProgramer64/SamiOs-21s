#!/usr/bin/env python3
"""The actual driver clock must use XNU's monotonic time when CNTPCT is broken."""
import pathlib,re,subprocess,tempfile
root=pathlib.Path(__file__).resolve().parents[1]
s=(root/'drivers/storage/hakios.dwmmc.c').read_text()
parts=[]
for name in ('dw_now','dw_hz'):
 m=re.search(r'static inline uint64_t\s+'+name+r'\(void\)\s*\{.*?\n\}',s,re.S)
 assert m,name
 body=m[0]
 # Only the architectural register boundary is substituted on the host.
 body=re.sub(r'__asm__ volatile\s*\("isb; mrs %0, cntpct_el0".*?;', 'v = 0;',body)
 body=re.sub(r'__asm__ volatile\s*\("mrs %0, cntfrq_el0".*?;', 'f = 26000000;',body)
 parts.append(body)
code='''#include <stdint.h>
#include <assert.h>
#define NSEC_PER_SEC 1000000000ULL
static uint64_t ticks=0;
static uint64_t mach_absolute_time(void) { return ticks; }
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *n) { *n=t*1000/26; }
'''+ '\n'.join(parts)+'''
int main(void) { ticks=26000000; uint64_t a=dw_now(); ticks+=13000000; uint64_t b=dw_now(); assert(b>a); assert((b-a)*1000/dw_hz()==500); }
'''
with tempfile.TemporaryDirectory() as t:
 p=pathlib.Path(t);(p/'test.c').write_text(code)
 subprocess.run(['clang','-fsanitize=undefined','-o',str(p/'test'),str(p/'test.c')],check=True)
 r=subprocess.run([str(p/'test')]);assert r.returncode==0,'broken CNTPCT must not drive timeouts/throughput'
print('DW MMC clock: broken raw counter ignored; XNU monotonic 500 ms measured correctly PASS')
