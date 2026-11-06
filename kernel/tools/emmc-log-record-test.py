from pathlib import Path
import subprocess,tempfile,struct,zlib
root=Path(__file__).resolve().parents[1]
h=root/'drivers/storage/hakios.emmc_log.h'
assert h.exists(),'missing bounded, checksummed eMMC log record implementation'
src=r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include "hakios.emmc_log.h"
int main(int argc,char **argv) {
 (void)argc;uint8_t out[4096];const uint8_t data[]="first log\n";
 assert(hakios_emmc_log_window(12216320,7944192,20160512,3092480,20283392,8192,25000000,21266944));
 assert(!hakios_emmc_log_window(12216320,7944192,20160512,3092480,20160512,8192,25000000,21266944));
 assert(!hakios_emmc_log_window(12216320,7944192,20160512,3092480,20283392,8192,20290000,21266944));
 assert(!hakios_emmc_log_window(12216320,7944192,20160512,3092480,20283392,8192,25000000,64*1024*1024));
 assert(!hakios_emmc_log_window(UINT64_MAX-10,50,0,0,0,8192,UINT64_MAX,100));
 assert(hakios_emmc_log_pack(out,3,9,123,data,sizeof(data)-1));
 assert(hakios_emmc_log_valid(out));
 assert(!hakios_emmc_log_pack(out,3,9,123,data,4033));
 assert(hakios_emmc_log_valid(out));
 FILE *f=fopen(argv[1],"wb");assert(f);assert(fwrite(out,1,4096,f)==4096);fclose(f);
 out[77]^=1;assert(!hakios_emmc_log_valid(out));out[77]^=1;
 out[35]=255;assert(!hakios_emmc_log_valid(out));
 assert(!hakios_emmc_log_pack(out,0,9,123,data,9));
 return 0;
}
'''
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'test.c').write_text(src)
 subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(h.parent),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test'),str(p/'block.bin')],check=True)
 b=bytearray((p/'block.bin').read_bytes());assert b[:8]==b'HAKLOG01'
 seq,run,pos,n,crc=struct.unpack_from('<QQQII',b,8)
 assert (seq,run,pos,n)==(3,9,123,10)
 b[36:40]=bytes(4);assert zlib.crc32(b)==crc
 assert b[64:74]==b'first log\n'
print('eMMC log: fixed tail window, bounds, payload overlap, card capacity, CRC and interrupted record detection PASS')
