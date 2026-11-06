#!/usr/bin/env python3
"""Exercise actual SPI transport with a fake FIFO boundary, checking pacing and CS."""
import pathlib,re,tempfile,subprocess
root=pathlib.Path(__file__).resolve().parents[1]
s=(root/'drivers/input/hakios.ovt_tcm.c').read_text()
f=s[s.index('static int\nspi_xfer('):s.index('static int\novt_tcm_spi_read(')]
prefix=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define SPI_CHUNK 63
static unsigned touch_byte_delay_us;
static unsigned calls, bytes, waits, asserted, edges, fail_at;
static void cs_set(bool active){assert(active?!asserted:asserted);asserted=active;edges++;}
static void delay(unsigned us){waits+=us;}
static int spi_chunk(const uint8_t *tx,uint8_t *rx,unsigned n){assert(asserted);calls++;if(fail_at==calls)return 5;for(unsigned i=0;i<n;i++){if(tx)assert(tx[i]==(uint8_t)(bytes+i));if(rx)rx[i]=(uint8_t)(bytes+i);}bytes+=n;return 0;}
'''
tests=r'''
int main(void){uint8_t b[130],r[130];for(unsigned i=0;i<130;i++)b[i]=(uint8_t)i;
for(unsigned d=0;d<=20;d+=20){touch_byte_delay_us=d;calls=bytes=waits=edges=0;assert(spi_xfer(b,r,130)==0);assert(!asserted&&edges==2&&bytes==130);assert(memcmp(b,r,130)==0);assert(calls==(d?130:3));assert(waits==35+(d?129*20:0));}
calls=bytes=waits=edges=0;fail_at=2;assert(spi_xfer(b,r,130)==5);assert(!asserted&&edges==2);puts("actual touch transport: byte pacing, content, CS continuity and failure cleanup PASS");}
'''
with tempfile.TemporaryDirectory() as t:
 p=pathlib.Path(t);(p/'t.c').write_text(prefix+f+tests);subprocess.run(['clang','-std=c11','-fsanitize=address,undefined',str(p/'t.c'),'-o',str(p/'t')],check=True);subprocess.run([str(p/'t')],check=True)
