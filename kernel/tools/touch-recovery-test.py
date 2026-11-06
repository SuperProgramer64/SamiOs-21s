#!/usr/bin/env python3
"""Exercise the actual runtime error policy and contact cleanup after confirmed stream loss."""
from pathlib import Path
import subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/'drivers/input/hakios.ovt_tcm.c').read_text()
assert 'touch_recovery_needed(' in s,'runtime recovery policy missing'
a=s.index('static bool\ntouch_recovery_needed(');b=s.index('\n#ifndef A135F_TOUCH_TEST',a)
source=r'''
#include <stdbool.h>
#include <assert.h>
static bool msg_dispatched,objects_down[10],hk_down[16];
static unsigned touch_probe_mode=1,max_objects=5,releases;
#define TCM_MAX_OBJECTS 10
static void a135f_hid_touch(int i,int down,int x,int y){(void)x;(void)y;assert(!down);if(hk_down[i]){releases++;hk_down[i]=false;}}
BODY
int main(void){
 unsigned errors=0;
 for(int i=0;i<7;i++){msg_dispatched=false;assert(!touch_recovery_needed(-5,&errors));for(int j=0;j<30;j++)assert(!touch_recovery_needed(1,&errors));}
 assert(touch_recovery_needed(-5,&errors));
 errors=0;msg_dispatched=false;assert(!touch_recovery_needed(-5,&errors));
 msg_dispatched=true;assert(!touch_recovery_needed(0,&errors) && errors==0);
 msg_dispatched=false;assert(!touch_recovery_needed(-5,&errors));assert(errors==1);
 objects_down[0]=hk_down[0]=true;objects_down[2]=hk_down[2]=true;
 touch_release_contacts();assert(releases==2 && !objects_down[0] && !objects_down[2] && !hk_down[0] && !hk_down[2]);
 touch_release_contacts();assert(releases==2);
}
'''.replace('BODY',s[a:b])
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.c').write_text(source)
 subprocess.run(['clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Touch recovery: idle gaps preserve stream failures; valid message clears failures; recovery releases contacts exactly once: PASS')
