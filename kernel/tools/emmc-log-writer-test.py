from pathlib import Path
import subprocess,tempfile,re
p=Path(__file__).resolve().parents[1]/'drivers/storage/hakios.dwmmc.c';s=p.read_text();a=s.index('static int\ndw_log_write(');b=s.index('\nstatic void\ndw_log_thread',a);fn=s[a:b]
src=r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#define SECTOR 512u
static uint64_t dw_log_lba=20283392;
static unsigned writes,stop,recover,fail;
static bool dw_write_blocks(uint64_t lba,uint32_t n,const uint8_t *data,bool *started) {
 assert(lba>=20283392&&lba<20291584);assert(n==1);assert(data);
 *started=true;writes++;return writes!=fail;
}
static bool dw_stop(void){stop++;return true;}
static void dw_recover(void){recover++;}
'''+fn+r'''
int main(void) {
 uint8_t data[4096]={0};assert(dw_log_write(8184,data)==0);assert(writes==8);
 writes=0;assert(dw_log_write(8185,data)==EINVAL);assert(writes==0);
 assert(dw_log_write(UINT64_MAX,data)==EINVAL);assert(writes==0);
 fail=3;assert(dw_log_write(0,data)==EIO);assert(writes==3&&stop==1&&recover==1);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as t:
 t=Path(t);(t/'a.c').write_text(src);subprocess.run(['clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(t/'a.c'),'-o',str(t/'a')],check=True);subprocess.run([str(t/'a')],check=True)
print('actual eMMC log writer: last legal record, out-of-window rejection, interrupted write stops without retry PASS')
