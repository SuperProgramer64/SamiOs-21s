#!/usr/bin/env python3
"""Observe firmware configuration requests without issuing new writes."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'drivers/input/hakios.ovt_tcm.c').read_text();a=s.index('static void\novt_tcm_dispatch_report(');b=s.index('\nstatic void\novt_tcm_dispatch_response(',a)
src=r'''
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>
#define TCM_MSG_HEADER_SIZE 4
#define TCM_REPORT_TOUCH 0x11
static uint8_t in_buf[16],status_report_code;
static uint8_t *report_buf;
static unsigned report_buf_len,payload_length,touch_probe_mode=1,touches;
static char logbuf[8192];static size_t loglen;
static unsigned le2_to_uint(const uint8_t *p){return p[0]|p[1]<<8;}
static int test_printf(const char *fmt,...){va_list ap;va_start(ap,fmt);int n=vsnprintf(logbuf+loglen,sizeof(logbuf)-loglen,fmt,ap);va_end(ap);loglen+=n;return n;}
#define printf test_printf
static void ovt_tcm_report_touch(void){touches++;}
BODY
int main(void){
 status_report_code=0x1b;payload_length=2;in_buf[4]=0x14;ovt_tcm_dispatch_report();
 assert(strstr(logbuf,"need-app=1") && strstr(logbuf,"hdl=2") && touches==0);
 size_t previous=loglen;payload_length=1;ovt_tcm_dispatch_report();assert(loglen==previous);
 payload_length=2;touch_probe_mode=0;ovt_tcm_dispatch_report();assert(loglen==previous);
 status_report_code=0x11;ovt_tcm_dispatch_report();assert(touches==1);
}
'''.replace('BODY',s[a:b])
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.c').write_text(src)
 subprocess.run(['clang','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Firmware status: app config/version decoded; short status and legacy mode ignored; touch unaffected: PASS')
