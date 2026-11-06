#!/usr/bin/env python3
"""Actual report decoder: malformed input must not create contacts or hang."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'drivers/input/hakios.ovt_tcm.c').read_text()
a=s.index('static int\ntcm_get_report_bits(');b=s.index('\n/* ------------------------------------------------------------------\n * Controller bring-up',a)
defs='\n'.join(re.findall(r'^#define TCM_(?:TOUCH|OBJECT|MAX_OBJECTS)[^\n]*',s,re.M))
program=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
DEFS
struct tcm_object {unsigned status,x,y,z;};
static uint8_t report_config[512],report_buf[512];
static unsigned report_config_size,report_buf_len,max_objects=5,max_x=719,max_y=1599,reports_seen;
static bool objects_down[10],touch_dbg;
static struct tcm_object objects[10];
static unsigned hk_panel_w,hk_panel_h,posts;
static struct {unsigned bad_reports;} diag;
static void a135f_hid_touch(int i,int down,int x,int y){(void)i;(void)down;(void)x;(void)y;posts++;}
BODY
int main(void){
 unsigned out=9;uint8_t v=0xab;
 assert(tcm_get_report_bits(&v,1,0,8,&out)==0 && out==0xab);
 assert(tcm_get_report_bits(&v,1,4,8,&out)==-EINVAL);
 const uint8_t cfg[]={TCM_TOUCH_FOREACH_ACTIVE_OBJECT,TCM_TOUCH_OBJECT_N_INDEX,4,
 TCM_TOUCH_OBJECT_N_CLASSIFICATION,4,TCM_TOUCH_OBJECT_N_X_POSITION,16,
 TCM_TOUCH_OBJECT_N_Y_POSITION,16,TCM_TOUCH_FOREACH_END,TCM_TOUCH_END};
 memcpy(report_config,cfg,sizeof(cfg));report_config_size=sizeof(cfg);
 report_buf[0]=0x10;report_buf[1]=10;report_buf[3]=20;report_buf_len=5;
 ovt_tcm_report_touch();assert(posts==1 && objects_down[0]);
 report_buf_len=1;ovt_tcm_report_touch();assert(posts==1 && objects_down[0]);
 const uint8_t loop[]={TCM_TOUCH_FOREACH_ACTIVE_OBJECT,TCM_TOUCH_FOREACH_END,TCM_TOUCH_END};
 memcpy(report_config,loop,sizeof(loop));report_config_size=sizeof(loop);report_buf_len=1;
 ovt_tcm_report_touch();assert(posts==1 && objects_down[0]);
 const uint8_t zero[]={TCM_TOUCH_NUM_OF_ACTIVE_OBJECTS,8,TCM_TOUCH_END};
 memcpy(report_config,zero,sizeof(zero));report_config_size=sizeof(zero);report_buf[0]=0;
 ovt_tcm_report_touch();assert(posts==2 && !objects_down[0]);
 /* A no-count active loop ends at the payload boundary. The four-byte
  * frame prefix remains on lift; there is no object index to read. */
 const uint8_t prefix_only[]={0x05,16,0x10,16,TCM_TOUCH_FOREACH_ACTIVE_OBJECT,
 TCM_TOUCH_OBJECT_N_INDEX,4,TCM_TOUCH_OBJECT_N_CLASSIFICATION,4,
 TCM_TOUCH_OBJECT_N_X_POSITION,16,TCM_TOUCH_OBJECT_N_Y_POSITION,16,
 TCM_TOUCH_FOREACH_END,TCM_TOUCH_END};
 memcpy(report_config,prefix_only,sizeof(prefix_only));report_config_size=sizeof(prefix_only);
 memset(report_buf,0,sizeof(report_buf));report_buf_len=4;objects_down[0]=true;
 ovt_tcm_report_touch();assert(posts==3 && !objects_down[0]);
 /* Missing foreach terminator is malformed, even on an empty frame. */
 report_config_size=sizeof(prefix_only)-2;objects_down[0]=true;
 ovt_tcm_report_touch();assert(posts==3 && objects_down[0]);
 /* Exact opcode stream recovered from physical run 2280. Five objects
  * occupy 4 prefix bytes + 5*11 bytes, not the advertised 256-byte maximum. */
 const uint8_t physical[]={0x0f,1,0x17,8,0x12,16,0x16,4,4,1,6,4,7,4,8,12,9,12,
 0x0a,8,0x0b,8,0x0c,8,0x0d,16,0x0e,16,3,0};
 memcpy(report_config,physical,sizeof(physical));report_config_size=sizeof(physical);
 memset(report_buf,0,sizeof(report_buf));memset(objects_down,0,sizeof(objects_down));
 report_buf[4]=0x10;report_buf[5]=0xcf;report_buf[6]=0xf2;report_buf[7]=0x63;report_buf_len=15;
 assert(ovt_tcm_parse_touch_report() && objects[0].x==719 && objects[0].y==1599);
 ovt_tcm_report_touch();assert(objects_down[0]);
 report_buf_len=4;ovt_tcm_report_touch();assert(!objects_down[0]);
 report_buf_len=59;
 for(unsigned i=0;i<5;i++){report_buf[4+11*i]=0x10|i;report_buf[5+11*i]=100+i;}
 ovt_tcm_report_touch();for(unsigned i=0;i<5;i++)assert(objects_down[i]);
 report_buf_len=4;ovt_tcm_report_touch();for(unsigned i=0;i<5;i++)assert(!objects_down[i]);
}
'''.replace('DEFS',defs).replace('BODY',s[a:b])
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.c').write_text(program)
 subprocess.run(['clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=3)
print('TCM reports: valid contact, truncated payload retained state, zero-progress loop bounded, empty frame lifts: PASS')
