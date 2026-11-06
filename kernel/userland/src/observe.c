/* SPDX-License-Identifier: MIT
 * Optional self-observer for bring-up: bounded thread/stack samples, no hooks
 * or policy changes. Enable insertion with OBSERVE=1 and hakios_observe=1.
 */
#include <mach/mach.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <mach/arm/thread_status.h>
#include <pthread.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
extern const void *_dyld_get_shared_cache_range(size_t *);
static uintptr_t slide;
static char who[64];
static bool stable_stacks;
static void say(const char *fmt,...) {
 char b[1200]; va_list ap;va_start(ap,fmt);int n=vsnprintf(b,sizeof(b),fmt,ap);va_end(ap);
 if(n>0)write(2,b,(size_t)n<sizeof(b)?(size_t)n:sizeof(b)-1);
}
static uintptr_t canonical(uintptr_t p) { return p & 0x0000007fffffffffULL; }
static void frame(unsigned tid,unsigned n,uintptr_t raw) {
 uintptr_t p=canonical(raw); Dl_info info={0};dladdr((void *)p,&info);
 say("HOBS %s[%d] T%u F%u cache=%#llx image=%s offset=%#llx symbol=%s\n",who,getpid(),tid,n,
 (unsigned long long)(p-slide),info.dli_fname?info.dli_fname:"?",
 (unsigned long long)(p-(uintptr_t)info.dli_fbase),info.dli_sname?info.dli_sname:"?");
}
/* No symbol lookup, printing or allocation while another thread is suspended.
 * Always resume before returning, including when reading its registers fails. */
static kern_return_t capture_stack(thread_t thread, bool stabilize, uint64_t frames[14], unsigned *count) {
 *count=0;
 kern_return_t e=KERN_SUCCESS;
 if(stabilize && (e=thread_suspend(thread))!=KERN_SUCCESS)return e;
 arm_thread_state64_t st={0};mach_msg_type_number_t sc=ARM_THREAD_STATE64_COUNT;
 e=thread_get_state(thread,ARM_THREAD_STATE64,(thread_state_t)&st,&sc);
 if(!e) {
  frames[(*count)++]=arm_thread_state64_get_pc(st);
  frames[(*count)++]=arm_thread_state64_get_lr(st);
  uintptr_t fp=canonical(arm_thread_state64_get_fp(st));
  while(*count<14 && fp && !(fp&15)) {
   uint64_t pair[2];vm_size_t got=0;
   if(vm_read_overwrite(mach_task_self(),fp,sizeof(pair),(vm_address_t)pair,&got)||got!=sizeof(pair))break;
   frames[(*count)++]=pair[1];uintptr_t next=canonical(pair[0]);
   if(next<=fp||next-fp>1024*1024)break;
   fp=next;
  }
 }
 if(stabilize) {kern_return_t resumed=thread_resume(thread);if(!e)e=resumed;}
 return e;
}
static void snapshot(unsigned sample) {
 thread_act_array_t threads=NULL;mach_msg_type_number_t count=0;
 kern_return_t e=task_threads(mach_task_self(),&threads,&count);
 say("HOBS %s[%d] sample=%u task_threads=%d count=%u slide=%#llx\n",who,getpid(),sample,e,count,(unsigned long long)slide);
 if(e)return;
 thread_t self=mach_thread_self();
 for(unsigned i=0;i<count;i++) {
  if(i<16) {
   thread_basic_info_data_t bi={0};mach_msg_type_number_t bc=THREAD_BASIC_INFO_COUNT;
   kern_return_t be=thread_info(threads[i],THREAD_BASIC_INFO,(thread_info_t)&bi,&bc);
   uint64_t frames[14];unsigned nframes=0;
   bool stable=stable_stacks && threads[i]!=self;
   kern_return_t se=capture_stack(threads[i],stable,frames,&nframes);
   char name[64]="";pthread_t pt=pthread_from_mach_thread_np(threads[i]);if(pt)pthread_getname_np(pt,name,sizeof(name));
   say("HOBS %s[%d] T%u name=%s state=%d cpu=%d user=%d.%06d system=%d.%06d info=%d regs=%d stable=%d\n",who,getpid(),i,name,bi.run_state,bi.cpu_usage,bi.user_time.seconds,bi.user_time.microseconds,bi.system_time.seconds,bi.system_time.microseconds,be,se,stable);
   if(!se) {
    for(unsigned j=0;j<nframes;j++)frame(i,j,frames[j]);
   }
  }
  mach_port_deallocate(mach_task_self(),threads[i]);
 }
 mach_port_deallocate(mach_task_self(),self);
 vm_deallocate(mach_task_self(),(vm_address_t)threads,count*sizeof(*threads));
}
static void *observer(void *unused) {
 (void)unused;pthread_setname_np("hakios-observer");
 for(unsigned n=1;n<=40;n++){sleep(n==1?90:120);snapshot(n);}return NULL;
}
__attribute__((constructor))static void begin(void) {
 char args[2048]={0};size_t n=sizeof(args)-1;
 if(sysctlbyname("kern.bootargs",args,&n,NULL,0)!=0||strstr(args,"hakios_observe=1")==NULL)return;
 stable_stacks=strstr(args,"hakios_observe_suspend=1")!=NULL;
 const char *p=getprogname();snprintf(who,sizeof(who),"%s",p?p:"?");
 size_t length=0;slide=(uintptr_t)_dyld_get_shared_cache_range(&length)-0x180000000ULL;
 say("HOBS %s[%d] observer loaded cache_slide=%#llx\n",who,getpid(),(unsigned long long)slide);
 pthread_t t;int e=pthread_create(&t,NULL,observer,NULL);if(!e)pthread_detach(t);else say("HOBS thread error=%d\n",e);
}
