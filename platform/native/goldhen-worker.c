// SPDX-License-Identifier: GPL-3.0-or-later
#include "goldhen-worker.h"
#include "goldhen-compat.h"
#include "goldhen/bridge.h"
#include "game-source.h"
#include "memory-map.h"
#include "native-operation.h"
#include "process-current.h"
#include "process-info.h"
#include "pt.h"
#include <ps5/kernel.h>
#include <pthread.h>
#include "module-thread.h"
#include <sys/mman.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
extern const intptr_t KERNEL_OFFSET_VMSPACE_P_ROOT;
extern int pt_getlwps(pid_t,int*,size_t);
typedef struct {
  int pid,attached;uint64_t proc,arena,cookie,sleep;char generation[128],title[10];
  PmGoldhenInfo info;unsigned requests;int broken;
} Work;
static unsigned workers;
static int identity(Work *w){return pm_game_identity(w->pid,w->generation,w->title)&&pm_process_current(w->pid)&&(uint64_t)kernel_get_proc(w->pid)==w->proc;}
static int kread(void *unused,uint64_t at,void *bytes,size_t size){(void)unused;return kernel_copyout(at,bytes,size);}
static const PmMemoryReader reader={NULL,kread};
static int span(Work *w,uint64_t at,size_t size,PmMemoryMapping *maps,size_t *count){
  return !identity(w)||pm_memory_span(&reader,w->proc,KERNEL_OFFSET_VMSPACE_P_ROOT,at,size,maps,32,count)?-1:0;
}
static int readbytes(void *context,uint64_t at,void *bytes,size_t size){
  Work *w=context;PmMemoryMapping maps[32];size_t count=0;
  if(!w->attached||span(w,at,size,maps,&count))return -1;
  for(size_t i=0;i<count;i++)if(!(maps[i].protection&5))return -1;
  return pt_copyout(w->pid,at,bytes,size);
}
static int writebytes(void *context,uint64_t at,const void *bytes,size_t size){
  Work *w=context;PmMemoryMapping maps[32];size_t count=0,changed=0;
  if(!w->attached||w->broken||size>65536||span(w,at,size,maps,&count))return -1;
  int executable=0;
  for(size_t i=0;i<count;i++){if(!(maps[i].protection&5))return -1;executable|=maps[i].protection&4;}
  if(executable){
    int ids[2048],n=pt_getlwps(w->pid,ids,2048);if(n<=0||n>2048)return -1;
    for(int i=0;i<n;i++){struct reg r;if(pt_getregs(ids[i],&r)|| (r.r_rip>=at-14&&r.r_rip<at+size))return -1;}
  }
  unsigned char *original=malloc(size);if(!original)return -1;
  int result=pt_copyout(w->pid,at,original,size),written=0;
  for(size_t i=0;!result&&i<count;i++){
    unsigned char actual=0,desired=maps[i].protection|3;
    if(kernel_copyout(maps[i].entry+0x64,&actual,1)||actual!=maps[i].protection){result=-1;break;}
    changed=i+1;
    if(kernel_copyin(&desired,maps[i].entry+0x64,1)){result=-1;break;}
  }
  if(!result){written=1;result=pt_copyin(w->pid,bytes,at,size);}
  if(result&&written&&pt_copyin(w->pid,original,at,size))w->broken=1;
  for(size_t i=changed;i>0;i--){
    unsigned char actual=0;PmMemoryMapping *m=&maps[i-1];
    int rc=kernel_copyin(&m->protection,m->entry+0x64,1);
    if(rc)rc=kernel_copyin(&m->protection,m->entry+0x64,1);
    if(rc||kernel_copyout(m->entry+0x64,&actual,1)||actual!=m->protection){w->broken=1;result=-1;}
  }
  free(original);return result;
}
static int readbox(Work *w,PmGoldhenBridge *box){
  return kernel_proc_copyout(w->pid,w->arena+0x4000,box,sizeof *box)||box->magic!=PM_GOLDHEN_MAGIC||box->cookie!=w->cookie||box->sleep!=w->sleep?-1:0;
}
static void *service(void *context){
  Work *w=context;
  for(;;){
    // Havana's traced startup needs one info request and one combined write.
    // Keep the gate available, but back off after startup rather than polling
    // the kernel at controller frequency for the rest of the game session.
    usleep(w->requests>=2?500000:20000);
    if(!pm_native_claim())continue;
    if(!identity(w)){pm_native_release();break;}
    PmGoldhenBridge box;
    if(readbox(w,&box)||box.state==PM_GH_EXPIRED){pm_native_release();break;}
    if(box.state!=PM_GH_PENDING){pm_native_release();continue;}
    // Stop only to service a posted request, then recheck its identity/state.
    // No game function runs under this lease and no filesystem I/O is needed.
    int valid=!pt_attach_owned(w->pid,&w->attached)&&identity(w)&&!readbox(w,&box)&&box.state==PM_GH_PENDING;
    int result=-1,ack=0;
    if(valid){
      PmGoldhenOps ops={w,readbytes,writebytes};
      result=pm_goldhen_request(&ops,&w->info,box.command,box.argument);
      uint32_t state=w->broken?PM_GH_EXPIRED:PM_GH_DONE;
      ack=!pt_copyin(w->pid,&result,w->arena+0x4000+offsetof(PmGoldhenBridge,result),sizeof result)&&
          !pt_copyin(w->pid,&state,w->arena+0x4000+offsetof(PmGoldhenBridge,state),sizeof state);
      w->requests++;
    }
    if(w->attached&&!pt_detach(w->pid,0))w->attached=0;
    if(w->attached&&!pt_detach(w->pid,0))w->attached=0;
    printf("ProsperoMenu GoldHEN compatibility: pid=%d command=%llu result=%d acknowledged=%d resumed=%d broken=%d\n",w->pid,(unsigned long long)box.command,result,ack,!w->attached,w->broken);fflush(stdout);
    pm_native_release();
    if(!valid||!ack||w->attached||w->broken)break;
  }
  printf("ProsperoMenu GoldHEN compatibility ended: pid=%d requests=%u resumed=%d\n",w->pid,w->requests,!w->attached);fflush(stdout);
  // Published code/mailbox belongs to this process until it exits. Never free
  // memory an initialized plugin may still call, even on a failed request.
  free(w);__atomic_fetch_sub(&workers,1,__ATOMIC_RELEASE);return NULL;
}
int pm_goldhen_prepare(const PmModuleSession *s,void *bytes,size_t length,int *attached,char reason[192]){
  Work *w=calloc(1,sizeof *w);if(!w){snprintf(reason,192,"Cannot allocate Havana compatibility worker");return -1;}
  w->pid=s->pid;snprintf(w->generation,sizeof w->generation,"%s",s->generation);snprintf(w->title,sizeof w->title,"%s",s->title);
  w->proc=kernel_get_proc(s->pid);int result=-1;const char *error="Havana compatibility preflight failed";
  unsigned char *metadata=NULL;size_t size=0;
  if(!identity(w)||pm_game_metadata_read(s->pid,s->generation,s->title,&metadata,&size,reason,192))goto done;
  struct kinfo_proc process={0};if(pm_process_info(s->pid,&process))goto done;
  w->info.pid=s->pid;snprintf(w->info.name,sizeof w->info.name,"%.*s",(int)sizeof process.ki_comm,process.ki_comm);snprintf(w->info.path,sizeof w->info.path,"/app0/eboot.bin");snprintf(w->info.title,sizeof w->info.title,"%s",s->title);
  if(pm_goldhen_sfo(metadata,size,"APP_VER",w->info.version,sizeof w->info.version)||
    pm_goldhen_sfo(metadata,size,"CONTENT_ID",w->info.content,sizeof w->info.content)){error="Cannot read the PS4 game's version/content ID for Havana";goto done;}
  if(strcmp(w->info.version,"01.32")&&strcmp(w->info.version,"1.32")){error="This Havana compatibility build requires RDR2 update 1.32";goto done;}
  if(pt_attach_owned(s->pid,attached)||!identity(w))goto done;
  PmMemoryModule eboot;if(pm_memory_module(&reader,w->proc,"eboot.bin",&eboot))goto done;
  w->info.base=eboot.base;
  w->sleep=kernel_dynlib_dlsym(s->pid,1,"sceKernelUsleep");
  if(!w->sleep)w->sleep=kernel_dynlib_dlsym(s->pid,0x2001,"sceKernelUsleep");
  if(w->sleep<0x10000||w->sleep>UINT64_C(0x7fffffffffff)-16||kernel_get_vmem_protection(s->pid,w->sleep,16)<0||
    !(kernel_get_vmem_protection(s->pid,w->sleep,16)&PROT_EXEC)){error="Game sleep function is unavailable for Havana compatibility";goto done;}
  intptr_t arena=pt_mmap(s->pid,0,0x8000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(arena<=0){error="Cannot allocate Havana compatibility gate";goto done;}w->arena=arena;
  arc4random_buf(&w->cookie,sizeof w->cookie);if(!w->cookie)w->cookie=1;
  PmGoldhenBridge box={.magic=PM_GOLDHEN_MAGIC,.cookie=w->cookie,.sleep=w->sleep};
  if(pt_copyin(s->pid,pm_goldhen_bridge_bytes,arena,sizeof pm_goldhen_bridge_bytes)||pt_copyin(s->pid,&box,arena+0x4000,sizeof box)||
    pt_mprotect(s->pid,arena,0x4000,PROT_READ|PROT_EXEC)||kernel_get_vmem_protection(s->pid,arena,0x4000)!=(PROT_READ|PROT_EXEC)||
    kernel_get_vmem_protection(s->pid,arena+0x4000,0x4000)!=(PROT_READ|PROT_WRITE))goto done;
  if(pm_goldhen_patch(bytes,length,arena,arena+0x4000)){error="Havana SDK instructions differ from the verified release";goto done;}
  if(pt_detach(s->pid,0))goto done;*attached=0;
  unsigned count=__atomic_fetch_add(&workers,1,__ATOMIC_ACQ_REL);
  if(count>=8){__atomic_fetch_sub(&workers,1,__ATOMIC_RELEASE);error="Havana compatibility worker limit reached";goto done;}
  pthread_t thread;int started=pm_module_thread_start(&thread,service,w);
  if(started){__atomic_fetch_sub(&workers,1,__ATOMIC_RELEASE);error="Havana compatibility worker could not start";goto done;}
  printf("ProsperoMenu GoldHEN compatibility ready: pid=%d title=%s version=%s base=0x%llx commands=4,5 originalUploadPreserved=1\n",s->pid,s->title,w->info.version,(unsigned long long)w->info.base);fflush(stdout);
  free(metadata);return 0;
done:
  if(*attached&&w->arena)pt_munmap(s->pid,w->arena,0x8000);
  if(*attached&&!pt_detach(s->pid,0))*attached=0;
  if(*attached)result=-2;
  snprintf(reason,192,"%s%s",error,*attached?"; game resume is unconfirmed":"");free(metadata);free(w);return result;
}
