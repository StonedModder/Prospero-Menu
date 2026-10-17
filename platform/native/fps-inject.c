// SPDX-License-Identifier: GPL-3.0-or-later
#include "fps-inject.h"
#include "publication.h"
#include "memory-map.h"
#include "pt.h"
#include <ps5/kernel.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
extern int pt_getlwps(pid_t,int*,size_t);
extern const intptr_t KERNEL_OFFSET_VMSPACE_P_ROOT;
typedef struct {
 int pid,attached,owner,saved;uint64_t proc,auth;unsigned char caps[16];
 int (*identity)(void*);void *context;PmDeckPatch patch;PmMemoryMapping maps[4];size_t mapCount;
 uint64_t control;size_t stolen;
} Target;
static int current(Target *t){return t->identity(t->context)&&(!t->proc||t->proc==(uint64_t)kernel_get_proc(t->pid));}
static int kread(void *unused,uint64_t at,void *out,size_t n){(void)unused;return kernel_copyout(at,out,n);}
static const PmMemoryReader reader={NULL,kread};
static int attach(void *p){Target *t=p;if(!current(t))return -1;if(!t->attached&&pt_attach_owned(t->pid,&t->attached))return -1;return current(t)?0:-1;}
static int detach(void *p){Target *t=p;if(!t->attached)return 0;if(pt_detach(t->pid,0))return -1;t->attached=0;return 0;}
static int threads(void *p,uint64_t *pcs,size_t capacity){
 Target *t=p;int ids[2048],n=pt_getlwps(t->pid,ids,capacity);if(n<=0||n>2048)return -1;
 for(int i=0;i<n;i++){struct reg r;if(pt_getregs(ids[i],&r))return -1;pcs[i]=r.r_rip;
  if(pcs[i]>=t->patch.address&&pcs[i]<t->patch.address+t->stolen)return -1;
 }return n;
}
static int readbytes(void *p,uint64_t at,void *out,size_t n){Target *t=p;return current(t)?pt_copyout(t->pid,at,out,n):-1;}
static int protection(Target *t,const PmMemoryMapping *map,unsigned char desired){
 uint64_t bounds[2];unsigned char observed=0;
 if(!current(t)||kernel_copyout(map->entry+0x20,bounds,sizeof bounds)||bounds[0]!=map->start||bounds[1]!=map->end)return -1;
 if(kernel_copyin(&desired,map->entry+0x64,1)||kernel_copyout(map->entry+0x64,&observed,1)||observed!=desired)return -1;
 return 0;
}
static int writebytes(void *p,uint64_t at,const void *bytes,size_t n){
 Target *t=p;if(!t->attached||!current(t)||at!=t->patch.address||n!=14)return -1;
 int restoring=!memcmp(bytes,t->patch.before,14),bad=0;
 for(size_t i=0;i<t->mapCount;i++)if(protection(t,&t->maps[i],t->maps[i].protection|3)){bad=1;break;}
 if(!bad&&pt_copyin(t->pid,bytes,at,n))bad=1;
 for(size_t i=0;i<t->mapCount;i++){
  // FF 25 reads its inline target. Installed entries must remain readable,
  // including on games whose original GNM mapping was execute-only.
  unsigned char final=restoring?t->maps[i].protection:(t->maps[i].protection|1)&~2;
  if(protection(t,&t->maps[i],final)&&protection(t,&t->maps[i],final))bad=1;
 }
 unsigned char actual[14];return bad||readbytes(t,at,actual,n)||memcmp(actual,bytes,n)?-1:0;
}
static int enable(void *p,int on){Target *t=p;uint32_t value=on,actual=99;
 return !current(t)||pt_copyin(t->pid,&value,t->control+offsetof(PmFpsGate,enabled),4)||
  pt_copyout(t->pid,t->control+offsetof(PmFpsGate,enabled),&actual,4)||actual!=value?-1:0;
}
static int ack(void *p){return enable(p,1);}static int reject(void *p){return enable(p,0);}
static void pause_target(void *p){(void)p;usleep(1000);}
static int credentials(Target *t){
 if(!t->saved)return 0;unsigned char caps[16];
 if(kernel_get_ucred_authid(t->owner)==t->auth&&!kernel_get_ucred_caps(t->owner,caps)&&!memcmp(caps,t->caps,16))return 0;
 if(kernel_set_ucred_authid(t->owner,t->auth)||kernel_set_ucred_caps(t->owner,t->caps))return -1;
 return kernel_get_ucred_authid(t->owner)!=t->auth||kernel_get_ucred_caps(t->owner,caps)||memcmp(caps,t->caps,16)?-1:0;
}
static uint64_t resolve(Target *t){
 const char *names[]={"libSceGnmDriverForNeoMode.sprx","libSceGnmDriver.sprx"};
 const char *nids[]={"Ga6r7H6Y0RI","xbxNatawohc"};uint64_t chosen=0;
 for(unsigned i=0;i<2;i++){
  PmMemoryModule module;int rc=pm_memory_module(&reader,t->proc,names[i],&module);
  if(rc==PM_MEMORY_MISSING)continue;if(rc)return 0;
  uint64_t address=0;for(unsigned j=0;j<2&&!address;j++)address=kernel_dynlib_resolve(t->pid,module.handle,nids[j]);
  if(!address)continue;if(address<module.base||address-module.base>=module.size||64>module.size-(address-module.base))return 0;
  if(chosen&&chosen!=address)return 0;chosen=address;
 }return chosen;
}
int pm_fps_inject(int pid,uint64_t cookie,int(*identity)(void*),void *context,PmFpsInjection *out){
 if(!out)return -1;memset(out,0,sizeof *out);out->cookie=cookie;out->resumed=1;
 Target t={.pid=pid,.identity=identity,.context=context};intptr_t arena=0;int keep=0,result=1;
 const char *message="PS4 frame counter preflight failed";
 if(pid<=1||!cookie||!identity||!identity(context))goto done;
 t.owner=getpid();t.auth=kernel_get_ucred_authid(t.owner);if(!t.auth||kernel_get_ucred_caps(t.owner,t.caps))goto done;t.saved=1;
 if(attach(&t)){result=-1;message="PS4 frame counter could not confirm stopped game ownership";goto done;}
 t.proc=(uint64_t)kernel_get_proc(pid);if(!t.proc)goto done;
 uint64_t target=resolve(&t);if(!target){message="A unique supported GNM flip export was not found";goto done;}
 unsigned char before[64];if(readbytes(&t,target,before,sizeof before))goto done;
 if((before[0]==0xff&&before[1]==0x25)||before[0]==0xe9||before[0]==0xeb){message="GNM flip entry already has a hook; no competing hook was installed";goto done;}
 if(pm_memory_span(&reader,t.proc,KERNEL_OFFSET_VMSPACE_P_ROOT,target,14,t.maps,4,&t.mapCount))goto done;
 for(size_t i=0;i<t.mapCount;i++)if(!(t.maps[i].protection&PROT_EXEC))goto done;
 arena=pt_mmap(pid,0,0x8000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(arena<=0){arena=0;message="PS4 counter allocation failed";goto done;}t.control=arena+0x4000;
 PmFpsTrampoline stub;if(pm_fps_trampoline(before,sizeof before,target,arena,t.control,&stub)){message="GNM flip prologue could not be relocated";goto done;}
 PmFpsGate gate={.magic=PM_FPS_MAGIC,.cookie=cookie};
 if(pt_copyin(pid,stub.bytes,arena,stub.size)||pt_copyin(pid,&gate,t.control,sizeof gate)){message="PS4 counter staging failed";goto done;}
 if(pt_mprotect(pid,arena,0x4000,PROT_READ|PROT_EXEC)||kernel_get_vmem_protection(pid,arena,0x4000)!=5||kernel_get_vmem_protection(pid,t.control,0x4000)!=3){message="PS4 counter arena could not be sealed RX/RW";goto done;}
 t.patch.address=target;t.stolen=stub.stolen;memcpy(t.patch.before,before,14);t.patch.after[0]=0xff;t.patch.after[1]=0x25;memcpy(t.patch.after+6,&arena,8);
 PmPublicationOps ops={&t,attach,detach,threads,readbytes,writebytes,ack,reject,pause_target};
 result=pm_publish_hooks(&t.patch,1,&ops);keep=result<=0;
 if(!result){out->installed=1;message="GNM flip counter installed in this game instance";}
 else if(result<0)message="PS4 hook publication or resume is uncertain; no replay allowed";
 else message="PS4 counter was rejected; original entry restored";
done:
 if(arena&&!keep){if(attach(&t)||pt_munmap(pid,arena,0x8000)){keep=1;result=-1;message="PS4 counter allocation cleanup is unconfirmed";}}
 if(detach(&t)){result=-1;message="PS4 game resume was not confirmed";}
 out->resumed=!t.attached;
 if(credentials(&t)){result=-1;message="Coordinator credential restoration was not confirmed";}
 if(keep){out->arena=arena;out->control=t.control;out->target=t.patch.address;memcpy(out->before,t.patch.before,14);memcpy(out->after,t.patch.after,14);}
 out->uncertain=result<0;snprintf(out->message,sizeof out->message,"%s",message);return result;
}
