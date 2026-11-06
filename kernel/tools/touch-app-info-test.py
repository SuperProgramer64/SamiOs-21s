#!/usr/bin/env python3
"""Run the production application-info reader through TD4150 startup states."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'drivers/input/hakios.ovt_tcm.c').read_text();a=s.index('static int\novt_tcm_get_app_info(');b=s.index('\nstatic int\novt_tcm_get_report_config(',a)
src=r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#define APP_LEN 38
#define APP_MAX_X 32
#define APP_MAX_Y 34
#define APP_MAX_OBJECTS 36
#define TCM_MAX_OBJECTS 10
#define TCM_CMD_GET_APPLICATION_INFO 0x20
#define MIN(a,b) ((a)<(b)?(a):(b))
static uint8_t app_info[APP_LEN],payload[APP_LEN];
static unsigned max_x,max_y,max_objects,calls,slept;
static unsigned status_sequence[12],sequence_count,length=38;
static unsigned touch_probe_mode=1;
static unsigned le2_to_uint(const uint8_t *p){return p[0]|p[1]<<8;}
static void wait_ms(unsigned ms){slept+=ms;}
static int ovt_tcm_write_message(unsigned cmd,void *p,unsigned n,const uint8_t **r,unsigned *rn){
 (void)p;(void)n;assert(cmd==0x20);unsigned i=calls<sequence_count?calls:sequence_count-1;
 payload[2]=status_sequence[i];payload[3]=0;calls++;*r=payload;*rn=length;return 0;
}
BODY
static void reset(unsigned status){memset(payload,0,sizeof(payload));payload[32]=0xcf;payload[33]=2;payload[34]=0x3f;payload[35]=6;payload[36]=5;calls=slept=0;sequence_count=1;status_sequence[0]=status;length=38;}
int main(void){
 reset(0);assert(!ovt_tcm_get_app_info() && calls==1 && max_x==719 && max_y==1599 && max_objects==5);
 reset(1);sequence_count=3;status_sequence[1]=2;status_sequence[2]=0;assert(!ovt_tcm_get_app_info() && calls==3 && slept==200);
 reset(1);assert(ovt_tcm_get_app_info()==-ETIMEDOUT && calls==11 && slept==1000);
 reset(0xff);assert(ovt_tcm_get_app_info()==-EINVAL && calls==1);
 reset(0);length=37;assert(ovt_tcm_get_app_info()==-EINVAL);
 reset(0);payload[32]=payload[33]=0;assert(ovt_tcm_get_app_info()==-EINVAL);
 reset(1);touch_probe_mode=0;assert(!ovt_tcm_get_app_info() && calls==1);
}
'''.replace('BODY',s[a:b])
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.c').write_text(src)
 subprocess.run(['clang','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('TCM app info: ready, booting/updating, timeout, bad config, truncated, invalid dimensions, legacy gate: PASS')
