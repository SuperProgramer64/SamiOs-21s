#!/usr/bin/env python3
"""The production sampler must resume a target on every exit, before symbol lookup."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'userland/src/observe.c').read_text()
start=source.index('static kern_return_t capture_stack(')
end=source.index('\nstatic void snapshot(',start)
body=source[start:end]
test=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
typedef int kern_return_t; typedef unsigned thread_t; typedef unsigned mach_msg_type_number_t;
typedef unsigned long vm_size_t; typedef unsigned long vm_address_t; typedef void *thread_state_t;
typedef struct { uint64_t pc,lr,fp; } arm_thread_state64_t;
#define ARM_THREAD_STATE64_COUNT 3
#define ARM_THREAD_STATE64 1
#define KERN_SUCCESS 0
#define arm_thread_state64_get_pc(s) ((s).pc)
#define arm_thread_state64_get_lr(s) ((s).lr)
#define arm_thread_state64_get_fp(s) ((s).fp)
static int suspended,resumed,fail_suspend,fail_state,fail_read;
static kern_return_t thread_suspend(thread_t t){(void)t;if(fail_suspend)return 2;assert(!suspended);suspended=1;return 0;}
static kern_return_t thread_resume(thread_t t){(void)t;assert(suspended);suspended=0;resumed++;return 0;}
static kern_return_t thread_get_state(thread_t t,int f,thread_state_t v,mach_msg_type_number_t *n){(void)t;(void)f;(void)n;if(fail_state)return 3;*(arm_thread_state64_t *)v=(arm_thread_state64_t){0x100,0x200,0x1000};return 0;}
static unsigned mach_task_self(void){return 1;}
static kern_return_t vm_read_overwrite(unsigned t,uintptr_t fp,vm_size_t sz,vm_address_t out,vm_size_t *got){(void)t;(void)sz;if(fail_read)return 4;uint64_t *v=(void *)out;v[0]=fp<0x1020?fp+16:0;v[1]=0x300+fp;*got=16;return 0;}
static uintptr_t canonical(uintptr_t p){return p;}
BODY
int main(void){uint64_t frames[14];unsigned count;
 assert(capture_stack(2,true,frames,&count)==0 && resumed==1 && !suspended && count==5);
 fail_state=1;assert(capture_stack(2,true,frames,&count)==3 && resumed==2 && !suspended && count==0);
 fail_state=0;fail_read=1;assert(capture_stack(2,true,frames,&count)==0 && resumed==3 && !suspended && count==2);
 fail_suspend=1;assert(capture_stack(2,true,frames,&count)==2 && resumed==3 && !suspended && count==0);
 fail_suspend=0;assert(capture_stack(2,false,frames,&count)==0 && resumed==3 && !suspended && count==2);
}
'''.replace('BODY',body)
with tempfile.TemporaryDirectory() as td:
    p=Path(td);(p/'check.c').write_text(test)
    subprocess.run(['clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'check.c'),'-o',str(p/'check')],check=True)
    subprocess.run([str(p/'check')],check=True)
print('observer stack: success, register failure, stack failure, suspend failure, unsuspended mode: PASS')
