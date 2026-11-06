#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the driver's startup policy against a scripted controller boundary.

Catch discarding an unsolicited identity, accepting a truncated identity, and
reusing a previous attempt's identity. No hardware or firmware is required.
"""
import pathlib, subprocess, tempfile

src = (pathlib.Path(__file__).resolve().parents[1] /
       'drivers/input/hakios.ovt_tcm.c').read_text()

def function(name):
    start = src.index('static int\n' + name + '(')
    brace = src.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (src[end] == '{') - (src[end] == '}')
        end += 1
    return src[start:end] + '\n'

prefix = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define TCM_MIN_READ_LEN 9
#define TCM_MSG_HEADER_SIZE 4
#define SPI_CHUNK 63
#define bzero(p,n) memset(p,0,n)
#define ID_LEN 24
#define ID_MODE 1
#define ID_MAX_WRITE 22
#define TCM_WR_CHUNK_SIZE 512
#define TCM_CMD_IDENTIFY 2
#define TCM_CMD_RESET 4
#define TCM_CMD_RUN_APPLICATION_FIRMWARE 0x14
#define TCM_MODE_SWITCH_DELAY_MS 200
#define TCM_MODE_ROMBOOTLOADER 4
#define TCM_IS_FW_MODE(m) ((m)==1 || (m)==2)
static uint8_t id_info[24], answer[24];
static bool id_info_valid;
static unsigned touch_probe_mode, wr_chunk_size;
static uint8_t app_info[38],report_config[64];
static unsigned touch_read_length=9,read_length=9,report_config_size;
static struct {const char *stage;} diag;
static unsigned identify_calls, reset_calls, read_calls, drains;
static bool unsolicited, read_error;
static unsigned answer_len = 24;
static unsigned le2_to_uint(const uint8_t *s){return s[0]|(s[1]<<8);}
static void wait_ms(unsigned n){(void)n;}
static int ovt_tcm_write_message(uint8_t c,const uint8_t *p,unsigned n,
 const uint8_t **r,unsigned *l){(void)p;(void)n;
 if(c==2){identify_calls++;*r=answer;*l=answer_len;return 0;}
 if(c==4){reset_calls++;return 0;}
 return -ENODEV;
}
static int ovt_tcm_read_message(void){read_calls++;
 if(read_error)return -ENXIO;
 if(unsolicited){memcpy(id_info,answer,24);id_info_valid=true;wr_chunk_size=512;}
 return 0;
}
static void ovt_tcm_drain(void){drains++;}
static int ovt_tcm_romboot_download(const uint8_t *p,unsigned n){(void)p;(void)n;return -ENODEV;}
static int ovt_tcm_get_app_info(void){return 0;}
static int ovt_tcm_get_report_config(void){return 0;}
static unsigned max_x=720,max_y=1600,max_objects=10;
static unsigned le4_to_uint(const uint8_t *p){(void)p;return 1;}
#define ID_BUILD 18
'''
helpers = ''
if 'static int\novt_tcm_accept_identify(' in src:
    helpers = function('ovt_tcm_accept_identify')
tests = r'''
static void fresh(void){memset(answer,0,24);answer[1]=2;answer[22]=0;answer[23]=2;
 answer_len=24;identify_calls=reset_calls=read_calls=drains=0;
 unsolicited=read_error=false;touch_probe_mode=1;id_info_valid=false;}
int main(void){
 fresh();unsolicited=true;app_info[14]=0;app_info[15]=1;read_length=133;
 assert(ovt_tcm_init_device(NULL,0,false)==0 && touch_read_length==63 && read_length==63);
 app_info[14]=20;app_info[15]=0;
 assert(ovt_tcm_init_device(NULL,0,false)==0 && touch_read_length==25 && read_length==25);
 app_info[14]=app_info[15]=0;
 fresh();unsolicited=true;
 assert(ovt_tcm_init_device(NULL,0,false)==0);
 assert(read_calls==1 && identify_calls==0); /* preserve startup report */
 fresh();read_error=true;
 assert(ovt_tcm_init_device(NULL,0,false)==0 && identify_calls==1);
 fresh();touch_probe_mode=0;
 assert(ovt_tcm_init_device(NULL,0,false)==0 && read_calls==0 && identify_calls==1);
 fresh();answer_len=2;
 assert(ovt_tcm_identify()<0 && !id_info_valid); /* no stale trailing fields */
 fresh();memset(answer,255,24);
 assert(ovt_tcm_identify()<0 && !id_info_valid); /* disconnected bus */
 fresh();id_info_valid=true;memset(id_info,255,24);read_error=true;
 assert(ovt_tcm_init_device(NULL,0,false)==0 && identify_calls==1 && id_info[1]==2);
 fresh();unsolicited=true;
 assert(ovt_tcm_init_device(NULL,0,true)==0 && reset_calls==1 && identify_calls==1);
 puts("touch startup: unsolicited identity, legacy path, malformed reply, stale state and reset PASS");
}
'''
with tempfile.TemporaryDirectory() as d:
    p = pathlib.Path(d)
    (p/'test.c').write_text(prefix + helpers + function('ovt_tcm_identify') +
                          function('ovt_tcm_init_device') + tests)
    subprocess.run(['clang','-std=c11','-fsanitize=address,undefined',
                    str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
