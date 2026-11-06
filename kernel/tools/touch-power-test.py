#!/usr/bin/env python3
"""Real bounded PMIC protocol: power bits only, stable reads and failure behavior."""
import pathlib,tempfile,subprocess
root=pathlib.Path(__file__).resolve().parents[1]
code=r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include "hakios.touch_power.h"
static uint32_t status,words[4],writes,requests,mode,badreply;static uint8_t reg=0x12;static uint64_t ticks;
static uint32_t rd(void *c,unsigned space,uint32_t o){(void)c;if(!space)return o==0x28?status:0;return words[o/4];}
static void wr(void *c,unsigned space,uint32_t o,uint32_t v){(void)c;if(space){assert(o<8);words[o/4]=v;return;}if(o==0x20){status&=~v;return;}assert(o==8&&v==(1u<<26));requests++;assert(words[0]==0x146);if(words[1]&1){writes++;reg=(words[1]>>8)&255;}if(mode!=1){words[3]=badreply?0x01000000:((uint32_t)reg<<8);status=1u<<10;}}
static uint64_t now(void *c){(void)c;return ticks+=1000000;}
static void wait(void *c){(void)c;}
int main(void){struct hakios_touch_power_io io={0,rd,wr,now,wait};uint8_t before,after;
assert(hakios_touch_power(&io,1,&before,&after)==0&&reg==0x12&&!writes);
assert(hakios_touch_power(&io,2,&before,&after)==0&&before==0x12&&after==0xd2&&reg==0xd2&&writes==1);
assert(hakios_touch_power(&io,2,&before,&after)==0&&writes==1);
reg=0x12;badreply=1;assert(hakios_touch_power(&io,2,&before,&after)!=0&&reg==0x12&&writes==1);badreply=0;
status=1u<<10;unsigned n=requests;assert(hakios_touch_power(&io,2,&before,&after)!=0&&requests==n);status=0;
mode=1;assert(hakios_touch_power(&io,2,&before,&after)!=0&&writes==1);
puts("touch power: LDO28 only, voltage bits preserved, read-only mode, pending reply, firmware error and deadline PASS");}
'''
with tempfile.TemporaryDirectory() as t:
 p=pathlib.Path(t);(p/'t.c').write_text(code);subprocess.run(['clang','-std=c11','-fsanitize=address,undefined','-I',str(root/'drivers/input'),str(p/'t.c'),'-o',str(p/'t')],check=True);subprocess.run([str(p/'t')],check=True)
