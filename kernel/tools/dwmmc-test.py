#!/usr/bin/env python3
"""Run the driver's actual PIO/read code against a deterministic DW register model.

Only the hardware boundary (MMIO, FIFO, clock) is replaced. Detects wrong command
arguments, byte counts, payload corruption, error recovery and escaped windows.
"""
import pathlib
import re
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
full_source = (root / 'drivers/storage/hakios.dwmmc.c').read_text()
source = full_source.split('/* ---- the block device ---- */')[0]
source = re.sub(r'^#include .*$', '', source, flags=re.M)
source = re.sub(r'^static LCK_.*?;\n', '', source, flags=re.M)
source = re.sub(r'static inline uint32_t dw_rd\([^\n]+', '', source)
source = re.sub(r'static inline void dw_wr\([^\n]+', '', source)
source = re.sub(r'static inline uint64_t\s+dw_(?:now|hz)\(void\)\s*\{.*?\n\}', '', source, flags=re.S)
source = re.sub(r'\*\(volatile uint(64|32|16)_t \*\)dw.fifo', lambda m: 'fake_fifo_read(' + str(int(m[1])//8) + ')', source)
# Writes, once supported, use the same MMIO boundary as reads.
source = re.sub(r'fake_fifo_read\((8|4|2)\)\s*=\s*(\w+);', r'fake_fifo_write(\1, \2);', source)
prelude = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_size_t;
#include <sys/disk.h>
#ifndef DKIOCSETBLOCKSIZE
#define DKIOCSETBLOCKSIZE _IOW('d', 24, uint32_t)
#endif
typedef void *proc_t;
#define B_READ 1
#define FWRITE 2
struct fake_buf { dev_t dev; int flags; int64_t blk; uint32_t count, resid; uint8_t *data; int error, done; };
typedef struct fake_buf *buf_t;
static int dw_lock;
static void lck_mtx_lock(int *p) { (void)p; }
static void lck_mtx_unlock(int *p) { (void)p; }

static dev_t buf_device(buf_t b) { return b->dev; }
static int buf_flags(buf_t b) { return b->flags; }
static int64_t buf_blkno(buf_t b) { return b->blk; }
static uint32_t buf_count(buf_t b) { return b->count; }
static void buf_seterror(buf_t b, int n) { b->error=n; }
static void buf_setresid(buf_t b, uint32_t n) { b->resid=n; }
static void buf_biodone(buf_t b) { b->done++; }
static int buf_map(buf_t b, caddr_t *p) { *p=(caddr_t)b->data; return 0; }
static void buf_unmap(buf_t b) { (void)b; }
#ifndef __unused
#define __unused __attribute__((unused))
#endif
/* the IDMAC path (emmc_dma=1, not exercised here: the simulated controller has no DMA engine) */
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#if !defined(__aarch64__)
#define __builtin_arm_dsb(x) __sync_synchronize()
#endif
static uint32_t dw_rd(uint32_t);
static void dw_wr(uint32_t, uint32_t);
static uint64_t dw_now(void);
static uint64_t dw_hz(void);
static uint64_t fake_fifo_read(unsigned);
static void fake_fifo_write(unsigned, uint64_t);
'''
model = r'''
static uint32_t regfile[256], ints, pos, length, media_at, expected_count;
static unsigned commands[64], ncmd, args[64], fail_multi, fail_status, short_read, fail_count, fail_flush, fail_write_after, starve_read, starved, starve_crc;
static unsigned reads17, reads18, stops, writes24, writes25;
static uint8_t disk[512 * 1024];
static uint64_t clock_ticks;
static uint64_t dw_now(void) { return clock_ticks += 260; }
static uint64_t dw_hz(void) { return 26000000; }
static uint32_t dw_rd(uint32_t off) {
    if (off == DW_CMD) return 0;
    if (off == DW_CTRL) return 0;
    if (off == DW_RINTSTS) {
        if (starve_read && commands[ncmd-1]==18 && pos>=512 && !starved) {
            starved=1; ints |= DW_INT_HTO | DW_INT_RXDR | (starve_crc?DW_INT_DCRC:0);
        }
        if (fail_multi && commands[ncmd-1] == 18) return DW_INT_DCRC;
        if (fail_write_after && commands[ncmd-1]==25 && pos>=fail_write_after) return DW_INT_DCRC;
        return ints | ((pos >= length && !short_read) ? DW_INT_DATA_OVER : 0);
    }
    if (off == DW_STATUS) {
        if (ncmd && (commands[ncmd-1] == 24 || commands[ncmd-1] == 25)) return 0;
        unsigned n = (length - pos) / dw.width;
        if (short_read && pos >= length / 2) n = 0;
        return (n > 32 ? 32 : n) << 17;
    }
    return regfile[off/4];
}
static void dw_wr(uint32_t off, uint32_t v) {
    regfile[off/4] = v;
    if (off == DW_RINTSTS) { ints &= ~v; return; }
    if (off != DW_CMD) return;
    unsigned cmd = v & 63, arg = regfile[DW_CMDARG/4];
    if (ncmd >= 64) { fprintf(stderr,"too many commands\n"); exit(2); }
    commands[ncmd] = cmd; args[ncmd++] = arg;
    ints = DW_INT_CMD_DONE;
    regfile[DW_RESP0/4] = (fail_status || (fail_count && cmd == 23) || (fail_flush && cmd == 6)) ? (1u << 22) : 0x900;
    if (cmd == 23) expected_count = arg;
    if (cmd == 12) { stops++; pos = length; }
    if (cmd == 17 || cmd == 18 || cmd == 24 || cmd == 25) {
        if (cmd == 17) reads17++;
        if (cmd == 18) reads18++;
        if (cmd == 24) writes24++;
        if (cmd == 25) writes25++;
        pos = 0; length = regfile[DW_BYTCNT/4]; media_at = arg * 512;
        if (length == 0 || media_at + length > sizeof(disk)) abort();
        if ((cmd == 18 || cmd == 25) && (expected_count * 512 != length || expected_count < 2)) abort();
    }
}
static uint64_t fake_fifo_read(unsigned width) {
    uint64_t v = 0;
    if (pos + width > length) abort();
    memcpy(&v, disk + media_at + pos, width); pos += width;
    return v;
}
static void fake_fifo_write(unsigned width, uint64_t value) {
    if (pos + width > length) abort();
    memcpy(disk + media_at + pos, &value, width); pos += width;
}
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while(0)
static void reset(void) {
    memset(&dw,0,sizeof(dw)); memset(regfile,0,sizeof(regfile));
    dw.width=8; dw.lba=16; dw.sectors=512;
    starve_read=starved=starve_crc=0;
    pos=length=ints=ncmd=reads17=reads18=stops=writes24=writes25=fail_write_after=fail_multi=fail_status=short_read=fail_count=fail_flush=0;
    for (unsigned i=0;i<sizeof(disk);i++) disk[i]=(uint8_t)(i * 7 + i/512);
    ENABLE_MULTI
    CLEAR_WRITEMULTI
}
int main(void) {
    uint8_t buf[4096];
    reset();
    CHECK(dw_read(2, 8, buf) == 0);
    CHECK(memcmp(buf, disk + 18*512, sizeof(buf)) == 0);
    CHECK(reads18 == 1 && reads17 == 0); /* baseline must fail here: eight CMD17s */
    CHECK(commands[0] == 23 && args[0] == 8 && commands[1] == 18 && args[1] == 18);
    reset();
    CHECK(dw_read(511, 2, buf) == EINVAL && ncmd == 0);
    CHECK(dw_read(UINT64_MAX, 1, buf) == EINVAL && ncmd == 0);
    for (unsigned width=2;width<=8;width*=2) {
        reset(); dw.width=width;
        CHECK(dw_read(0,8,buf)==0);
        CHECK(memcmp(buf,disk+16*512,sizeof(buf))==0);
    }
    static uint8_t bigbuf[512*512];
    reset();
    CHECK(dw_read(0,512,bigbuf)==0 && reads18==2);
    CHECK(args[0]==256 && args[2]==256);
    CHECK(memcmp(bigbuf,disk+16*512,sizeof(bigbuf))==0);
    reset(); dw.lba=UINT32_MAX-15;
    CHECK(dw_read(0,8,buf)==EINVAL && ncmd==0);
    reset(); fail_multi=1;
    CHECK(dw_read(0, 8, buf) == 0);
    CHECK(stops >= 1 && reads17 == 8);
    CHECK(memcmp(buf, disk+16*512, sizeof(buf)) == 0);
    reset(); fail_count=1;
    CHECK(dw_read(0, 8, buf) == 0);
    CHECK(stops == 0 && reads17 == 8);
    CHECK(memcmp(buf, disk+16*512, sizeof(buf)) == 0);
    reset(); short_read=1;
    CHECK(dw_read(0, 1, buf) == EIO);
    reset(); fail_status=1;
    CHECK(dw_read(0, 1, buf) == EIO); /* R1 errors are not a successful read */
    reset(); starve_read=1;
    CHECK(dw_read(0,8,buf)==0);
    CHECK(starved==1 && reads18==1 && reads17==0 && stops==0);
    CHECK(memcmp(buf,disk+16*512,sizeof(buf))==0);
    reset(); starve_read=1; short_read=1;
    CHECK(!dw_receive(18,16,8,buf)); /* HTO cannot reset the software deadline */
    CHECK(starved==1 && pos<4096);
    reset(); starve_read=1; starve_crc=1;
    CHECK(dw_read(0,8,buf)==0 && stops==1 && reads17==8);
    CHECK(memcmp(buf,disk+16*512,sizeof(buf))==0);
    WRITE_TESTS
    puts("DW MMC: read bytes, fallback, timeouts, R1, flush, device dispatch and write bounds PASS");
    return 0;
}
'''
write_tests = r"""
    reset(); dw.lba=12216320; dw.sectors=7944192;
    dw_var_lba=17068032; dw_var_sectors=3092480;
    CHECK(dw_var_valid(4848264));
    dw_var_lba=17063936;
    CHECK(!dw_var_valid(4848264)); /* overlaps APFS */
    dw_var_lba=17068032; dw_var_sectors=3094528;
    CHECK(!dw_var_valid(4848264)); /* reaches payload */
    dw_var_lba=UINT64_MAX; dw_var_sectors=131072;
    CHECK(!dw_var_valid(4848264));
    reset();
    memset(buf, 0xa5, sizeof(buf));
    CHECK(dw_write(0, 1, buf) == EROFS && ncmd == 0);
    dw_var_ready = true; dw_var_lba = 128; dw_var_sectors = 64;
    CHECK(dw_write(64, 1, buf) == EINVAL && ncmd == 0);
    CHECK(dw_write(UINT64_MAX, 1, buf) == EINVAL && ncmd == 0);
    CHECK(dw_write(0, 1, buf) == 0);
    CHECK(writes24 == 1 && memcmp(disk + 128*512, buf, 512) == 0);
    CHECK(disk[127*512] == (uint8_t)(127*512*7 + 127));
    CHECK(disk[129*512] == (uint8_t)(129*512*7 + 129));
    for (unsigned width=2;width<=8;width*=2) {
        reset(); dw.width=width; dw_var_ready=true; dw_var_lba=128; dw_var_sectors=64;
        CHECK(dw_write(0,1,buf)==0);
        CHECK(memcmp(disk+128*512,buf,512)==0);
    }
    reset(); dw_var_ready=true; dw_var_lba=128; dw_var_sectors=64; dw_cache_enabled=true;
    CHECK(dw_write(0, 1, buf) == 0);
    CHECK(ncmd == 4 && commands[2] == 6 && args[2] == 0x03200100);
    reset(); dw_var_ready=true; dw_var_lba=128; dw_var_sectors=64; dw_cache_enabled=true; fail_flush=1;
    CHECK(dw_write(0, 1, buf) == EIO && writes24 == 1);
    reset(); dw_var_ready = true; dw_var_lba=128; dw_var_sectors=64; fail_status=1;
    CHECK(dw_write(0, 1, buf) == EIO);
"""
dispatch_tests = r"""
    reset(); dw_ready=true;
    struct fake_buf b={.dev=0,.flags=0,.blk=0,.count=512,.data=buf};
    dw_dev_strategy(&b);
    CHECK(b.error==EROFS && b.done==1 && ncmd==0);
    CHECK(dw_dev_open(0,FWRITE,0,NULL)==EROFS);
    dw_var_ready=true; dw_var_lba=128; dw_var_sectors=64;
    CHECK(dw_dev_open(1,FWRITE,0,NULL)==0);
    b=(struct fake_buf){.dev=1,.flags=0,.blk=63,.count=1024,.data=buf};
    dw_dev_strategy(&b);
    CHECK(b.error==EINVAL && ncmd==0);
    b=(struct fake_buf){.dev=1,.flags=0,.blk=0,.count=511,.data=buf};
    dw_dev_strategy(&b);
    CHECK(b.error==EINVAL && ncmd==0);
    b=(struct fake_buf){.dev=1,.flags=0,.blk=-1,.count=512,.data=buf};
    dw_dev_strategy(&b);
    CHECK(b.error==EINVAL && ncmd==0);
    b=(struct fake_buf){.dev=1,.flags=0,.blk=63,.count=512,.data=buf};
    dw_dev_strategy(&b);
    CHECK(b.error==0 && b.resid==0 && b.done==1 && writes24==1);
    CHECK(memcmp(disk+191*512,buf,512)==0);
    uint32_t writable=99;
    CHECK(dw_dev_ioctl(0,DKIOCISWRITABLE,(caddr_t)&writable,0,NULL)==0 && writable==0);
    CHECK(dw_dev_ioctl(1,DKIOCISWRITABLE,(caddr_t)&writable,0,NULL)==0 && writable==1);
"""
write_tests += r"""
    reset(); dw_var_ready=true; dw_var_lba=128; dw_var_sectors=512;
    SET_WRITE_MULTI
    CHECK(dw_write(0,8,buf)==0 && writes25==1 && writes24==0);
    CHECK(commands[0]==23 && args[0]==8 && commands[1]==25 && args[1]==128);
    CHECK(memcmp(disk+128*512,buf,sizeof(buf))==0);
    for(unsigned width=2;width<=8;width*=2) {
        reset(); dw.width=width; dw_var_ready=true; dw_var_lba=128; dw_var_sectors=512;
        SET_WRITE_MULTI
        CHECK(dw_write(0,8,buf)==0 && writes25==1);
        CHECK(memcmp(disk+128*512,buf,sizeof(buf))==0);
    }
    reset(); dw_var_ready=true; dw_var_lba=128; dw_var_sectors=512;
    SET_WRITE_MULTI
    CHECK(dw_write(0,512,bigbuf)==0 && writes25==2 && writes24==0);
    CHECK(args[0]==256 && args[3]==256);
    CHECK(memcmp(disk+128*512,bigbuf,sizeof(bigbuf))==0);
    reset(); dw_var_ready=true; dw_var_lba=128; dw_var_sectors=512; fail_count=1;
    SET_WRITE_MULTI
    CHECK(dw_write(0,8,buf)==EIO && writes25==0 && writes24==0 && stops==0);
    reset(); dw_var_ready=true; dw_var_lba=128; dw_var_sectors=512; fail_write_after=512;
    SET_WRITE_MULTI
    CHECK(dw_write(0,8,buf)==EIO && writes25==1 && writes24==0 && stops==1);
    CHECK(memcmp(disk+128*512,buf,512)==0); /* partial uncertain write: never replay */
    CHECK(disk[129*512] == (uint8_t)(129*512*7 + 129));
    reset(); dw_var_ready=true; dw_var_lba=128; dw_var_sectors=512; dw_cache_enabled=true; fail_flush=1;
    SET_WRITE_MULTI
    CHECK(dw_write(0,8,buf)==EIO && writes25==1 && writes24==0);
    reset(); dw_var_ready=true; dw_var_lba=128; dw_var_sectors=512;
    SET_WRITE_MULTI
    CHECK(dw_write(508,8,buf)==EINVAL && ncmd==0);
    CHECK(dw_write(0,0,buf)==0 && ncmd==0);
"""
write_tests += dispatch_tests
# Include the production device dispatcher, not a test copy of its conditions.
for name in ('dw_dev_open', 'dw_dev_strategy', 'dw_dev_ioctl'):
    start = full_source.index('static ', full_source.rfind('\nstatic ', 0, full_source.index('\n'+name+'(')))
    brace = full_source.index('{', start); depth=1; end=brace+1
    while depth:
        depth += (full_source[end]=='{') - (full_source[end]=='}'); end+=1
    source += '\n' + full_source[start:end] + '\n'
model = model.replace('WRITE_TESTS', write_tests if 'dw_write(uint64_t' in source else 'CHECK(false && "gated var write window missing");')
model = model.replace('ENABLE_MULTI', ('dw_multiblock = 1;' if 'dw_multiblock' in source else '') + ('dw_var_ready=false; dw_cache_enabled=false;' if 'dw_var_ready' in source else ''))
model = model.replace('SET_WRITE_MULTI', 'dw_write_multiblock=1;' if 'dw_write_multiblock' in source else '')
model = model.replace('CLEAR_WRITEMULTI', 'dw_write_multiblock=0;' if 'dw_write_multiblock' in source else '')
with tempfile.TemporaryDirectory(prefix='hakios-dw-test-') as tmp:
    src = pathlib.Path(tmp)/'test.c'; binary = pathlib.Path(tmp)/'test'
    src.write_text(prelude + source + model)
    subprocess.run(['clang', '-std=c11', '-fsanitize=address,undefined', '-Wno-unused-function', '-Wno-format', str(src), '-o', str(binary)], check=True)
    raise SystemExit(subprocess.run([str(binary)]).returncode)
