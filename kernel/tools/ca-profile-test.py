from pathlib import Path
import subprocess,tempfile
h=Path(__file__).resolve().parents[1]/'userland/src/ca_profile.h'
assert h.exists(),'missing all-or-nothing CoreAnimation profile selection'
src=r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include "ca_profile.h"
static uintptr_t values[12];
static int read_slot(void *c,uintptr_t p,uintptr_t *v) {
 (void)c;if(p>=12)return 0;*v=values[p];return 1;
}
int main(void) {
 struct hakios_ca_profile p[2]={0};
 p[0].name="old";p[1].name="current";
 for(unsigned i=0;i<6;i++) {
  p[0].hooks[i]=(struct hakios_ca_hook){i,100+i};
  p[1].hooks[i]=(struct hakios_ca_hook){i+6,200+i};values[i+6]=200+i;
 }
 assert(hakios_ca_choose(p,2,read_slot,0)==p+1);
 values[11]=999;assert(hakios_ca_choose(p,2,read_slot,0)==0);
 values[11]=205;for(unsigned i=0;i<6;i++)values[i]=100+i;
 assert(hakios_ca_choose(p,2,read_slot,0)==p);
 p[0].hooks[0].slot=99;assert(hakios_ca_choose(p,2,read_slot,0)==p+1);
 /* The OpenGL pair must be verified independently of the six window hooks:
  * one wrong or unreadable method leaves both untouched. */
 p[1].gl_hooks[0]=(struct hakios_ca_hook){6,200};
 p[1].gl_hooks[1]=(struct hakios_ca_hook){7,201};
 assert(hakios_ca_hooks_match(p[1].gl_hooks,2,read_slot,0));
 values[7]=999;assert(!hakios_ca_hooks_match(p[1].gl_hooks,2,read_slot,0));
 values[7]=201;p[1].gl_hooks[0].slot=99;
 assert(!hakios_ca_hooks_match(p[1].gl_hooks,2,read_slot,0));
 assert(hakios_ca_choose(p,2,read_slot,0)==p+1);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'test.c').write_text(src)
 subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(h.parent),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('CoreAnimation profile: current and legacy, one bad target refuses all, unreadable slot rejected PASS')
