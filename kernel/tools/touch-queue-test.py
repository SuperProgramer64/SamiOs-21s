#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real producer queue under a slow reader: no stale single-finger MOVE backlog.

Merging across DOWN/UP, another finger, a button, consumed data or an explicit
gate-off must fail. Only scheduling/clock/lock boundaries are replaced.
"""
from pathlib import Path
import subprocess,tempfile
c=Path(__file__).resolve().parents[1]
s=(c/'drivers/input/hakios.input.c').read_text()
a=s.index('static int touch_coalesce') if 'static int touch_coalesce' in s else s.index('static void\ntouch_post(')
b=s.index('\nvoid\nhakios_touch_post(',a)
body=s[a:b]
prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "HEADER"
#define TOUCH_QUEUE 512
static struct hakios_touch touch_queue[TOUCH_QUEUE];
static uint32_t touch_head,touch_tail;
static int touch_lock;
static uint64_t ticks;
static bool enabled=true;
static void lck_mtx_lock(int *p){(void)p;}
static void lck_mtx_unlock(int *p){(void)p;}
static uint64_t mach_absolute_time(void){return ++ticks;}
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *n){*n=t;}
static bool latency_lines(void){return false;}
static void hakios_tl(const char *s,...){(void)s;}
static void count_fingers(uint32_t t,uint32_t f){(void)t;(void)f;}
static void wakeup(void *p){(void)p;}
static bool PE_parse_boot_argn(const char *n,void *v,size_t z){
 assert(!strcmp(n,"touch_coalesce") && z==sizeof(uint32_t));*(uint32_t *)v=enabled;return true;}
'''.replace('HEADER',str(c/'xnu/files/bsd/sys/hakios.input.h'))
tests=r'''
static void reset(void){touch_head=touch_tail=0;ticks=0;memset(touch_queue,0,sizeof(touch_queue));}
static void put(uint32_t t,uint32_t f,uint32_t x){touch_post(t,f,x,100,true);}
int main(void){
 reset();put(1,0,10);for(unsigned i=0;i<900;i++)put(2,0,i);put(3,0,899);
 assert(touch_tail-touch_head==3);
 assert(touch_queue[0].type==1 && touch_queue[0].x==10);
 assert(touch_queue[1].type==2 && touch_queue[1].x==899 && touch_queue[1].time==901);
 assert(touch_queue[2].type==3 && touch_queue[2].time==902);
 reset();put(1,0,0);put(2,0,1);put(2,1,2);put(2,0,3);put(4,2,0);put(2,0,4);put(3,0,4);put(1,0,5);put(2,0,6);
 assert(touch_tail==9);
 reset();touch_head=touch_tail=UINT32_MAX-1;put(1,0,1);put(2,0,2);put(2,0,3);put(3,0,3);
 assert(touch_tail-touch_head==3 && touch_queue[511].x==3);
 reset();put(2,0,1);touch_head=touch_tail;put(2,0,2);assert(touch_tail-touch_head==1 && touch_tail==2);
 reset();touch_post(2,0,1,1,false);touch_post(2,0,2,2,false);assert(touch_tail==2);
 touch_coalesce=-1;enabled=false;reset();put(1,0,1);put(2,0,2);put(2,0,3);put(3,0,3);assert(touch_tail==4);
 puts("touch queue: 900 moves keep latest sample, preserve transitions/buttons/fingers/wrap, gate-off and replay PASS");
}
'''
# Existing code lacks the gate too; supply only that test variable for the red run.
if 'static int touch_coalesce' not in body:prefix+='static int touch_coalesce;\n'
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'check.c').write_text(prefix+body+tests)
 subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Wno-unused-function','-fsanitize=address,undefined',str(p/'check.c'),'-o',str(p/'check')],check=True)
 subprocess.run([str(p/'check')],check=True)
