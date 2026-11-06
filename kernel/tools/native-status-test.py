#!/usr/bin/env python3
"""Exercise the actual native-status selection wrapper, with ObjC runtime at its boundary."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'userland/src/display_native.c').read_text()
start=s.index('static Class native_provider(');end=s.index('\n}',start)+2
body=s[start:end]
program=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
typedef void *id; typedef void *Class; typedef void *SEL;
static Class supplied;static unsigned said;
static Class legacy=(Class)3, fallback=(Class)2;
static Class orig(id a,SEL b,id c,id d){(void)a;(void)b;(void)c;(void)d;return supplied;}
static Class (*original_provider)(id,SEL,id,id)=orig;
static void say(const char *s,...){(void)s;}
BODY
int main(void){
 supplied=(Class)1;assert(native_provider(0,0,0,0)==supplied && said==0);
 supplied=0;assert(native_provider(0,0,0,0)==legacy && said==1);
 supplied=fallback;assert(native_provider(0,0,0,0)==legacy && said==1);
 legacy=0;assert(native_provider(0,0,0,0)==fallback);
}
'''.replace('BODY',body)
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'check.c').write_text(program)
 subprocess.run(['clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'check.c'),'-o',str(p/'check')],check=True)
 subprocess.run([str(p/'check')],check=True)
assert '0x189abe338' in s and '"#32@0:8@16@24"' in s
assert '"hakios_native_ui=1"' in s
print('Native status: preserve valid provider; unknown -> flexible phone provider; missing class -> unchanged: PASS')
