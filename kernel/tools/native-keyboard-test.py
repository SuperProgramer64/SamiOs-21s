#!/usr/bin/env python3
"""Exercise the real portrait keyboard size wrapper with the measured 320/360 mismatch."""
from pathlib import Path
import subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/'userland/src/display_native.c').read_text()
assert 'static CGSize native_keyboard_size(' in s,'native keyboard width correction missing'
start=s.index('static CGSize native_keyboard_size(');end=s.index('\n}',start)+2
program=r'''
#include <assert.h>
typedef void *id;typedef void *SEL;
typedef struct {double width,height;} CGSize;
static CGSize supplied;static unsigned keyboard_said;
static CGSize orig(id a,SEL b,long long o){(void)a;(void)b;(void)o;return supplied;}
static CGSize (*original_keyboard_size)(id,SEL,long long)=orig;
static void say(const char *s,...){(void)s;}
BODY
int main(void){
 supplied=(CGSize){320,233};
 for(int o=1;o<=2;o++){CGSize v=native_keyboard_size(0,0,o);assert(v.width==360 && v.height==233);}
 for(int o=0;o<=4;o+=3){CGSize v=native_keyboard_size(0,0,o);assert(v.width==320 && v.height==233);}
 double widths[]={360,375,414,0,-1};
 for(unsigned i=0;i<5;i++){supplied=(CGSize){widths[i],233};CGSize v=native_keyboard_size(0,0,1);assert(v.width==widths[i] && v.height==233);}
 supplied=(CGSize){320,0};assert(native_keyboard_size(0,0,1).width==320);
 supplied=(CGSize){320,800};assert(native_keyboard_size(0,0,1).width==320);
}
'''.replace('BODY',s[start:end])
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'check.c').write_text(program)
 subprocess.run(['clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'check.c'),'-o',str(p/'check')],check=True)
 subprocess.run([str(p/'check')],check=True)
assert '0x18911ca08' in s and '"{CGSize=dd}24@0:8q16"' in s
print('Native keyboard: measured portrait fallback 320 -> 360; height preserved; other sizes/orientations unchanged: PASS')
