#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise the actual TCM reader with transient and persistent bus failures.

Changing the reader to abort on the first bad header breaks recovery; allowing
unbounded retries breaks the persistent-fault cases. The fake replaces only SPI.
"""
import pathlib, subprocess, tempfile

source = (pathlib.Path(__file__).resolve().parents[1] /
          'drivers/input/hakios.ovt_tcm.c').read_text()
reader = source[source.index('static bool\novt_tcm_inline_continuation(') if 'static bool\novt_tcm_inline_continuation(' in source else source.index('static int\novt_tcm_read_message('):
                source.index('static int\novt_tcm_poll(')]
prefix = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#define TCM_MSG_MARKER 0xa5
#define TCM_MSG_PADDING 0x5a
#define TCM_MSG_HEADER_SIZE 4
#define TCM_MIN_READ_LEN 9
#define TCM_MAX_READ_LEN 2048
#define SPI_CHUNK 63
#define TCM_STATUS_OK 1
#define TCM_STATUS_IDLE 0
#define TCM_STATUS_BUSY 2
#define TCM_STATUS_CONTINUED_READ 3
#define TCM_STATUS_ERROR 15
#define TCM_STATUS_INVALID 255
#define TCM_CMD_STATUS_BUSY 1
#define TCM_CMD_STATUS_ERROR -1
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
static uint8_t in_buf[2056], status_report_code;
static unsigned read_length=9, payload_length, touch_probe_mode;
static unsigned touch_read_length=9,touch_inline_cont;
static bool msg_dispatched,last_marker_ok;
static int command_status;
static struct {unsigned reads,bad_marker,bad_padding,retries,messages,bad_length,read_failures,idles,inline_continued;uint32_t spi_status;uint8_t last_rx[9],status_code;} diag;
static unsigned attempts, failures, waits, dispatches, large_payload, packet_payload, largest_read;
static bool bad_marker;
static unsigned bad_padding_reads;
static bool inline_packet,inline_bad_end,inline_bad_header;
static unsigned le2_to_uint(const uint8_t *p){return p[0]|(p[1]<<8);}
static void wait_ms(unsigned n){waits+=n;}
static int ovt_tcm_spi_read(uint8_t *p,unsigned n){
 if(n>largest_read)largest_read=n;
 attempts++;memset(p,0,n);
 if(inline_packet){
  assert(n==63);memset(p,0x5a,n);p[0]=0xa5;p[1]=0x11;p[2]=15;p[3]=0;
  p[9]=0xa5;p[10]=inline_bad_header?0x11:3;p[19]=p[20]=0x47;
  if(inline_bad_end)p[21]=0xee;
  return 0;
 }
 if(attempts<=failures){memset(p,0x7f,n);return bad_marker?0:EIO;}
 p[0]=0xa5;p[1]=1;p[2]=large_payload?255:packet_payload;p[3]=large_payload?255:0;
 if(n>=packet_payload+5)p[packet_payload+4]=0x5a;
 if(bad_padding_reads){bad_padding_reads--;p[4]=0xe6;}
 return 0;
}
static int ovt_tcm_continued_read(unsigned n){(void)n;assert(!"unexpected continuation");return -EIO;}
static void ovt_tcm_dispatch_message(void){dispatches++;command_status=0;}
'''
tests = r'''
static void fresh(void){memset(&diag,0,sizeof(diag));attempts=waits=dispatches=large_payload=packet_payload=largest_read=0;
 command_status=1;touch_probe_mode=1;read_length=touch_read_length=9;}
int main(void){
 fresh();inline_packet=true;touch_inline_cont=1;touch_read_length=read_length=63;
 assert(ovt_tcm_read_message()==0 && attempts==1 && dispatches==1 && diag.inline_continued==1);
 assert(in_buf[9]==0x5a && in_buf[17]==0x47 && in_buf[18]==0x47 && in_buf[19]==0x5a);
 fresh();touch_read_length=read_length=63;inline_bad_end=true;
 assert(ovt_tcm_read_message()<0 && dispatches==0 && diag.inline_continued==0);
 inline_bad_end=false;inline_bad_header=true;fresh();touch_read_length=read_length=63;
 assert(ovt_tcm_read_message()<0 && dispatches==0 && diag.inline_continued==0);
 inline_bad_header=false;touch_inline_cont=0;fresh();touch_read_length=read_length=63;
 assert(ovt_tcm_read_message()<0 && dispatches==0 && diag.inline_continued==0);
 inline_packet=false;
 fresh();failures=0;bad_padding_reads=1;
 assert(ovt_tcm_read_message()==0 && attempts==2 && waits==5 && dispatches==1);
 assert(diag.bad_padding==1 && diag.retries==1 && diag.messages==1);
 fresh();bad_padding_reads=3;
 assert(ovt_tcm_read_message()<0 && attempts==2 && dispatches==0 && waits==5);
 assert(diag.bad_padding==2 && diag.retries==1 && diag.messages==0);
 fresh();bad_padding_reads=1;touch_probe_mode=0;
 assert(ovt_tcm_read_message()<0 && attempts==1 && dispatches==0);
 assert(diag.bad_padding==1 && diag.retries==0);
 bad_padding_reads=0;
 for(unsigned e=0;e<2;e++){
  fresh();failures=1;bad_marker=e;
  assert(ovt_tcm_read_message()==0 && command_status==0);
  assert(attempts==2 && waits==5 && dispatches==1 && diag.reads==2);
  fresh();failures=10;
  assert(ovt_tcm_read_message()<0 && command_status==-1);
  assert(attempts==2 && waits==5 && dispatches==0 && diag.reads==2);
  assert(diag.retries==1 && diag.messages==0);
  fresh();touch_probe_mode=0;failures=1;
  assert(ovt_tcm_read_message()<0 && attempts==1 && waits==0 && dispatches==0);
 }
 fresh();failures=0;
 assert(ovt_tcm_read_message()==0 && attempts==1 && waits==0 && dispatches==1);
 fresh();large_payload=1;
 assert(ovt_tcm_read_message()==-EIO && attempts==1 && dispatches==0 && waits==0);
 assert(diag.bad_length==1);
 fresh();failures=0;touch_read_length=48;
 assert(ovt_tcm_read_message()==0 && read_length==48);
 assert(ovt_tcm_read_message()==0 && read_length==48 && dispatches==2);
 /* Five physical contacts occupy 64 wire bytes. The final byte can be
  * synthesized, but must never expand the next transaction beyond FIFO 63. */
 fresh();touch_read_length=read_length=63;packet_payload=59;
 assert(ovt_tcm_read_message()==0 && read_length==63);
 packet_payload=0;
 assert(ovt_tcm_read_message()==0 && largest_read==63 && dispatches==2);
 /* A previous long command response cannot leave a 256-byte touch read. */
 fresh();touch_read_length=63;read_length=256;
 assert(ovt_tcm_read_message()==0 && largest_read==63 && read_length==63);
 puts("actual TCM reader: transient header/SPI recovery, bounded faults, legacy gate and malformed length PASS");
}
'''
with tempfile.TemporaryDirectory() as directory:
    p=pathlib.Path(directory)
    (p/'test.c').write_text(prefix+reader+tests)
    subprocess.run(['clang','-std=c11','-fsanitize=address,undefined',
                    str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
