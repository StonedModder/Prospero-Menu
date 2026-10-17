// SPDX-License-Identifier: GPL-3.0-or-later
// Native, read-only point samples. No game injection, ptrace or disk publication.
// DCE/AGC source interpretation follows OnionHEN / LightningMods and PHU/ArkSama.
#include "quickjs.h"
#include "native-operation.h"
#include "game-source.h"
#include "game-info.h"
#include "firmware-profiles.h"
#include "fps-counters.h"
#include "memory-map.h"
#include "user-read.h"
#include "bc-fps.h"
#include <ps5/kernel.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include "process-info.h"
#include <sys/ioctl.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stddef.h>
// Pinned SDK v0.43 CRT interfaces; that SDK's public header omits these exports.
extern unsigned long KERNEL_ADDRESS_DMAP_BASE;
extern int kernel_proc_getpaddr(int,unsigned long,unsigned long*,unsigned long*);
typedef struct {
  uint32_t serial,flags;int pid,valid,resources,ringOk,globalOk,scanoutOk,bcOk,cpus;
  char generation[128],title[10];uint64_t proc,time,ring,global,scanout,bc,cpuUsec,rss;
} Sample;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static Sample latest;static uint32_t serial;static int running;
static uint64_t millis(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static int current(Sample *s){return pm_game_identity(s->pid,s->generation,s->title)&&(!s->proc||(uint64_t)kernel_get_proc(s->pid)==s->proc);}
static int user_current(void *p){return current(p);}
static int translate(void *p,uint64_t at,uint64_t *physical,uint64_t *available){
  Sample *s=p;unsigned long address=0,length=0;int rc=kernel_proc_getpaddr(s->pid,at,&address,&length);
  *physical=address;*available=length;return rc;
}
static int physical(void *p,uint64_t at,void *out,size_t n){
  (void)p;uint64_t dmap=KERNEL_ADDRESS_DMAP_BASE;
  if(dmap<UINT64_C(0xffff800000000000)||at>UINT64_MAX-dmap||n>UINT64_MAX-(dmap+at))return -1;
  return kernel_copyout(dmap+at,out,n);
}
static int read_user(void *p,uint64_t at,void *out,size_t n){
  const PmUserReader reader={p,user_current,translate,physical};return pm_user_read(&reader,at,out,n);
}
static int read_kernel(void *p,uint64_t at,void *out,size_t n){
  (void)p;return kernel_copyout(at,out,n);
}
static void resource_sample(Sample *s){
  struct kinfo_proc info={0};
  if(pm_process_info(s->pid,&info)||info.ki_rssize<0)return;
  int pages=getpagesize(),cpus=0;size_t n=sizeof cpus;int cpuMib[]={CTL_HW,HW_NCPU};
  if(pages<4096||pages>65536||(pages&(pages-1))||sysctl(cpuMib,2,&cpus,&n,NULL,0)||n!=sizeof cpus||cpus<1||cpus>64)return;
  if((uint64_t)info.ki_rssize>UINT64_C(128)*1024*1024*1024/(unsigned)pages)return;
  uint64_t rss=(uint64_t)info.ki_rssize*(unsigned)pages;
  s->rss=rss;s->cpuUsec=info.ki_runtime;s->cpus=cpus;s->resources=1;
}
static void dce_sample(Sample *s){
  int fd=open("/dev/dce",O_RDWR|O_NONBLOCK);if(fd<0)return;
  _Alignas(8) unsigned char out[0x60]={0};
  struct {uint64_t selector,mask,output,reserved[3];} arg={UINT64_C(0x10000000a),UINT64_C(0x8000000000),(uint64_t)out,{0}};
  _Static_assert(sizeof arg==0x30,"DCE query ABI");
  int result=ioctl(fd,UINT64_C(0x80308217),&arg);
  if(result<0&&errno==EINVAL){memset(out,0,sizeof out);result=ioctl(fd,UINT64_C(0xffffffff80308217),&arg);}
  close(fd);if(result<0)return;memcpy(&s->scanout,out+8,8);s->scanoutOk=1;
}
static void *worker(void *p){
  Sample *s=p;
  if(snipers_firmware_profile(kernel_get_fw_version())&&current(s)){
    s->proc=(uint64_t)kernel_get_proc(s->pid);
    if(s->proc){
      if(s->flags&1)resource_sample(s);
      if((s->flags&2)&&(!strncmp(s->title,"PPSA",4)||!strncmp(s->title,"PPSB",4))){
        PmCounterReader reader={s,read_user};s->ringOk=!pm_fps_ring(&reader,&s->ring);
        PmMemoryReader modules={NULL,read_kernel};PmMemoryModule driver;
        // This is a live observational read, not a stopped transaction. Recheck
        // module identity after reading; never cache this object across samples.
        if(!pm_memory_module(&modules,s->proc,"libSceAgcDriver.sprx",&driver)&&driver.size>=0x1aac8){
          PmMemoryModule after;
          if(!read_user(s,driver.base+0x1aac0,&s->global,8)&&
             !pm_memory_module(&modules,s->proc,"libSceAgcDriver.sprx",&after)&&
             after.object==driver.object&&after.base==driver.base&&after.size==driver.size)s->globalOk=1;
        }
        dce_sample(s);
      }else if(s->flags&2){
        PmFpsInjection injection;PmFpsGate gate;unsigned char entry[14];
        if(!pm_bc_fps_lookup(s->pid,s->generation,s->title,&injection)&&
           !read_user(s,injection.control,&gate,sizeof gate)&&gate.magic==PM_FPS_MAGIC&&gate.cookie==injection.cookie&&gate.enabled==1&&
           !read_user(s,injection.target,entry,sizeof entry)&&!memcmp(entry,injection.after,sizeof entry)){
          s->bc=gate.frames;s->bcOk=1;
        }
      }
      s->time=millis();s->valid=s->time&&current(s);
    }
  }
  pthread_mutex_lock(&mutex);latest=*s;running=0;pm_native_release();pthread_mutex_unlock(&mutex);free(s);return NULL;
}
static JSValue start_js(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
  (void)self;if(argc!=4)return JS_ThrowTypeError(c,"Expected telemetry PID, generation, title and flags");
  Sample *s=calloc(1,sizeof *s);if(!s)return JS_ThrowOutOfMemory(c);int32_t flags=0,pid=0;
  const char *generation=JS_ToCString(c,argv[1]),*title=JS_ToCString(c,argv[2]);
  int valid=!JS_ToInt32(c,&pid,argv[0])&&!JS_ToInt32(c,&flags,argv[3])&&pid>1&&flags>0&&flags<=3&&
    generation&&generation[0]&&strlen(generation)<128&&pm_game_title(title);
  if(valid){s->pid=pid;s->flags=flags;strcpy(s->generation,generation);strcpy(s->title,title);}
  if(generation)JS_FreeCString(c,generation);if(title)JS_FreeCString(c,title);
  if(!valid){free(s);return JS_ThrowTypeError(c,"Invalid telemetry target");}
  pthread_mutex_lock(&mutex);
  if(running||!pm_native_claim()){pthread_mutex_unlock(&mutex);free(s);return JS_NULL;}
  if(!++serial)++serial;s->serial=serial;pthread_t thread;int error=pthread_create(&thread,NULL,worker,s);
  if(error){pm_native_release();pthread_mutex_unlock(&mutex);free(s);return JS_ThrowInternalError(c,"Telemetry worker could not start");}
  running=1;pthread_detach(thread);pthread_mutex_unlock(&mutex);return JS_NewUint32(c,serial);
}
static void decimal(JSContext *c,JSValue out,const char *name,uint64_t value){char text[32];snprintf(text,sizeof text,"%llu",(unsigned long long)value);JS_SetPropertyStr(c,out,name,JS_NewString(c,text));}
static JSValue poll_js(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
  (void)self;uint32_t request=0;if(argc!=1||JS_ToUint32(c,&request,argv[0])||!request)return JS_ThrowTypeError(c,"Expected telemetry serial");
  pthread_mutex_lock(&mutex);Sample s=latest;int busy=running&&request==serial;pthread_mutex_unlock(&mutex);
  if(busy)return JS_NULL;if(s.serial!=request)return JS_NULL;JSValue out=JS_NewObject(c);
  JS_SetPropertyStr(c,out,"serial",JS_NewUint32(c,s.serial));JS_SetPropertyStr(c,out,"pid",JS_NewInt32(c,s.pid));
  JS_SetPropertyStr(c,out,"generation",JS_NewString(c,s.generation));JS_SetPropertyStr(c,out,"valid",JS_NewBool(c,s.valid));
  JS_SetPropertyStr(c,out,"time",JS_NewFloat64(c,(double)s.time));
  if(s.valid){
    if(s.resources){decimal(c,out,"cpuUsec",s.cpuUsec);JS_SetPropertyStr(c,out,"rssBytes",JS_NewFloat64(c,(double)s.rss));JS_SetPropertyStr(c,out,"cpus",JS_NewInt32(c,s.cpus));}
    if(s.ringOk)decimal(c,out,"ring",s.ring);if(s.globalOk)decimal(c,out,"global",s.global);if(s.scanoutOk)decimal(c,out,"scanout",s.scanout);
    if(s.bcOk)decimal(c,out,"bc",s.bc);
  }
  return out;
}
void pm_telemetry_bind(JSContext *c,JSValue api){
  JS_SetPropertyStr(c,api,"telemetryStart",JS_NewCFunction(c,start_js,"telemetryStart",4));
  JS_SetPropertyStr(c,api,"telemetryPoll",JS_NewCFunction(c,poll_js,"telemetryPoll",1));
}
