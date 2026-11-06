#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""No private image: preserve a running chip, otherwise refuse before reset/write."""
from pathlib import Path
import subprocess, tempfile, sys

s = (Path(__file__).resolve().parents[1] / 'drivers/input/hakios.goodix.c').read_text()
a = s.index('static bool\nfocaltech_probe(')
b = s.index('\n}', a) + 2
body = s[a:b]
if '--legacy' in sys.argv:
    start = body.index('\tif (FOCALTECH_FW_LENGTH == 0) {')
    end = body.index('\n\tif (!focaltech_enter_boot_rom())', start)
    body = body[:start] + body[end:]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define FOCALTECH_FW_LENGTH 0u
static const unsigned char focaltech_fw[1];
static bool running;
static unsigned resets, writes;
static bool focaltech_wait_valid(uint8_t *a,uint8_t *b){(void)a;(void)b;return running;}
static bool focaltech_enter_boot_rom(void){resets++;return true;}
static bool focaltech_write_pram(const unsigned char *p,unsigned n){(void)p;(void)n;writes++;return false;}
static bool focaltech_start_app(void){writes++;return false;}
static void sleep_ms(unsigned n){(void)n;}
'''
tests = r'''
int main(void){
 assert(!focaltech_probe(false));assert(resets==0 && writes==0);
 running=true;assert(focaltech_probe(false));assert(resets==0 && writes==0);
 puts("optional FocalTech firmware: no-image refusal before reset, running chip preserved PASS");
}
'''
with tempfile.TemporaryDirectory() as td:
    p = Path(td)
    (p / 'test.c').write_text(prefix + body + tests)
    subprocess.run(['clang', '-std=c11', '-fsanitize=address,undefined',
                    str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
