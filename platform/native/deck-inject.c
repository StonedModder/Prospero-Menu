// SPDX-License-Identifier: GPL-3.0-or-later
// Thread stager adapted from libNineS / John Törnblom and the supported etaHEN
// port. Original publication/checking is isolated from ShellUI's UI thread.
#include "deck-inject.h"
#include "deck-launch.h"
#include "deck-bootstrap.h"
#include "publication.h"
#include "deck-protection.h"
#include "pt.h"
#include <ps5/kernel.h>
#include <sys/mman.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#ifdef PROSPEROMENU_BOOTSTRAP_TRACE
#include <signal.h>
#endif
extern intptr_t pm_elf_map(pid_t,const uint8_t*,size_t);
extern intptr_t pm_payload_args(pid_t);
extern int pt_getlwps(pid_t,int*,size_t);
typedef struct { pid_t pid;intptr_t address;int(*identity)(void*);void* cookie;uint64_t pages[PM_DECK_MAX_PATCHES*2];int protections[PM_DECK_MAX_PATCHES*2];unsigned pageCount,patchCount;int attached,allowFallback;PmDeckPatch patches[PM_DECK_MAX_PATCHES]; } Publication;
static int attach(void* p){Publication* c=p;
 if(!c->identity(c->cookie)||(!c->attached&&pt_attach_owned(c->pid,&c->attached)))return -1;
 return c->identity(c->cookie)?0:-1;}
static int detach(void* p){Publication* c=p;if(!c->attached)return 0;
 if(pt_detach(c->pid,0))return -1;c->attached=0;return 0;}
static int threads(void* p,uint64_t* pcs,size_t capacity){
 Publication* c=p;int ids[2048];int n=pt_getlwps(c->pid,ids,capacity);if(n<=0)return -1;
 for(int i=0;i<n;i++){struct reg r;if(pt_getregs(ids[i],&r))return -1;pcs[i]=r.r_rip;}return n;
}
static int readbytes(void* p,uint64_t a,void* b,size_t n){return pt_copyout(((Publication*)p)->pid,a,b,n);}
static int writebytes(void* p,uint64_t a,const void* b,size_t n){
 Publication* c=p;unsigned char check[PM_DECK_PATCH_MAX];
 const PmDeckPatch* patch=NULL;
 for(unsigned i=0;i<c->patchCount;i++)if(c->patches[i].address==a)patch=&c->patches[i];
 if(!patch||n!=pm_deck_patch_length(patch))return -1;
 for(size_t offset=0;offset<n;){
  uint64_t at=a+offset,page=at&~UINT64_C(0x3fff);size_t length=0x4000-(at-page);if(length>n-offset)length=n-offset;
  unsigned slot=0;while(slot<c->pageCount&&c->pages[slot]!=page)slot++;
  if(slot==c->pageCount){if(slot==PM_DECK_MAX_PATCHES*2)return -1;int original=kernel_get_vmem_protection(c->pid,at,length);
   if(original<0||!(original&PROT_EXEC))return -1;c->pages[slot]=page;c->protections[slot]=original;c->pageCount++;}
  int protection=c->protections[slot];
  int final=pm_deck_final_protection(protection,patch,a,b,n);if(final<0)return -1;
  if(kernel_mprotect(c->pid,at,length,protection|PROT_READ|PROT_WRITE))return -1;
  // Only PT_IO writes, never the SDK's physical fallback on shared code pages.
  int written=pt_copyin(c->pid,(const unsigned char*)b+offset,at,length),restored=kernel_mprotect(c->pid,at,length,final);
  if(restored)restored=kernel_mprotect(c->pid,at,length,final);
  if(written||restored)return -1;offset+=length;
 }
 if(pt_copyout(c->pid,a,check,n)||memcmp(check,b,n))return -1;return 0;
}
static int ack(void* p){Publication* c=p;uint32_t state=2,count=c->patchCount;
 if(pt_copyin(c->pid,&count,c->address+offsetof(PmDeckBoot,count),4))return -1;
 return pt_copyin(c->pid,&state,c->address+offsetof(PmDeckBoot,state),4);}
static int reject(void* p){Publication* c=p;uint32_t state=c->allowFallback?1:3;return pt_copyin(c->pid,&state,c->address+offsetof(PmDeckBoot,state),4);}
static void pause_thread(void* p){(void)p;usleep(1000);}
#ifdef PROSPEROMENU_BOOTSTRAP_TRACE
static void log_kernel_scan(const PmDeckBoot* s){
 fprintf(stderr,"ProsperoMenu bootstrap kernel snapshot: firmware=0x%08x initResult=%d data=0x%llx allproc=0x%llx dmap=0x%llx prison=0x%llx iommu=0x%llx reads=%u\n",
  s->bootstrapFirmware,s->kernelInitResult,(unsigned long long)s->kernelDataBase,(unsigned long long)s->kernelAllproc,
  (unsigned long long)s->kernelDmap,(unsigned long long)s->kernelPrison,(unsigned long long)s->kernelIommu,s->kernelReadCount);
 uint32_t count=s->kernelReadCount<PM_DECK_READ_HISTORY?s->kernelReadCount:PM_DECK_READ_HISTORY;
 for(uint32_t i=0;i<count;i++){
  uint32_t sequence=s->kernelReadCount-count+i+1;const PmDeckKernelRead* r=&s->kernelReads[(sequence-1)%PM_DECK_READ_HISTORY];
  if(r->sequence!=sequence)continue;
  fprintf(stderr,"ProsperoMenu bootstrap read: seq=%u address=0x%llx length=%llu complete=%u result=%d\n",r->sequence,
   (unsigned long long)r->address,(unsigned long long)r->length,r->complete,r->result);
 }
}
// Diagnostic-only: observe the first signal before Mono can proxy/re-raise it.
// A known, address-checked checkpoint ends tracing after kernel initialization.
// Other signals are delivered unchanged. No filesystem I/O while stopped.
static int trace_bootstrap(pid_t pid,intptr_t control,int* attached){
 uint32_t release=0x5443;int timedOut=0,tid=0,signal=0,code=0,stopped=0;
 uintptr_t fault=0;struct reg regs={0};PmDeckBoot state={0};uint8_t bytes[32]={0};
 int haveRegs=0,haveBytes=0,haveState=0,ownTrap=0,haveEarlyPointer=0;uint64_t earlyPointer=0;
 uint8_t stack[8192];size_t stackBytes=0;int controlProtection=-1;
 if(pt_copyin(pid,&release,control+offsetof(PmDeckBoot,count),sizeof release)||pt_continue(pid,0))return -1;
 int waitSignal=0;
 if(!pt_wait_signal(pid,8000,&waitSignal))stopped=1;
 else if(errno==ETIMEDOUT){
  timedOut=1;if(!kill(pid,SIGSTOP)&&!pt_wait_signal(pid,1000,&waitSignal))stopped=1;
 }
 if(!stopped)return -1;
 int haveInfo=!pt_stop_info(pid,&tid,&signal,&code,&fault);
 if(!signal)signal=waitSignal;
 haveRegs=haveInfo&&tid>0&&!pt_getregs(tid,&regs);
 haveState=!pt_copyout(pid,control,&state,sizeof state);
 if(haveState&&(state.magic!=PM_DECK_BOOT_MAGIC||state.version!=PM_DECK_BOOT_VERSION))haveState=0;
 if(haveState&&state.phase==201&&state.kernelReadLength==(uint64_t)control&&state.kernelReadAddress)
  haveEarlyPointer=!pt_copyout(pid,state.kernelReadAddress,&earlyPointer,sizeof earlyPointer);
 if(haveRegs)haveBytes=!pt_copyout(pid,regs.r_rip,bytes,sizeof bytes);
 ownTrap=haveInfo&&haveRegs&&haveState&&signal==SIGTRAP&&state.phase==204&&state.reservedTrace==3&&regs.r_rip==state.kernelReadAddress;
 if(!ownTrap&&haveRegs){
  size_t available=0x4000-(regs.r_rsp&0x3fff);if(available>sizeof stack)available=sizeof stack;
  if(!pt_copyout(pid,regs.r_rsp,stack,available))stackBytes=available;
  controlProtection=kernel_get_vmem_protection(pid,control,sizeof state);
 }
 // If timeout raced a real fault, preserve that fault rather than swallowing it.
 int deliver=ownTrap||(timedOut&&signal==SIGSTOP)?0:signal;
 if(pt_detach(pid,deliver))return -1;*attached=0;
 fprintf(stderr,"ProsperoMenu bootstrap first stop: tid=%d signal=%d code=%d fault=0x%llx timeout=%d checkpoint=%d phase=%u regs=%d\n",
  tid,signal,code,(unsigned long long)fault,timedOut,ownTrap,haveState?state.phase:0,haveRegs);
 if(haveState)log_kernel_scan(&state);
 if(haveEarlyPointer)fprintf(stderr,"ProsperoMenu CRT trace pointer: slot=0x%llx expected=0x%llx observed=0x%llx\n",
  (unsigned long long)state.kernelReadAddress,(unsigned long long)control,(unsigned long long)earlyPointer);
 if(!ownTrap){fprintf(stderr,"ProsperoMenu bootstrap fault capture: controlProtection=0x%x stackAddress=0x%llx stackBytes=%zu\n",controlProtection,(unsigned long long)regs.r_rsp,stackBytes);
  if(stackBytes){FILE* capture=fopen("/data/ProsperoMenu/bootstrap-stack.bin","wb");int saved=0;if(capture){size_t wrote=fwrite(stack,1,stackBytes,capture);int closed=fclose(capture);saved=wrote==stackBytes&&!closed;}
   fprintf(stderr,"ProsperoMenu bootstrap stack saved: ok=%d bytes=%zu\n",saved,stackBytes);}
 }
 if(haveRegs)fprintf(stderr,"ProsperoMenu bootstrap registers: rip=0x%llx rsp=0x%llx rbp=0x%llx rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx rsi=0x%llx rdi=0x%llx r12=0x%llx r13=0x%llx r14=0x%llx r15=0x%llx\n",
  (unsigned long long)regs.r_rip,(unsigned long long)regs.r_rsp,(unsigned long long)regs.r_rbp,(unsigned long long)regs.r_rax,(unsigned long long)regs.r_rbx,(unsigned long long)regs.r_rcx,(unsigned long long)regs.r_rdx,(unsigned long long)regs.r_rsi,(unsigned long long)regs.r_rdi,(unsigned long long)regs.r_r12,(unsigned long long)regs.r_r13,(unsigned long long)regs.r_r14,(unsigned long long)regs.r_r15);
 if(haveBytes){fputs("ProsperoMenu bootstrap instruction:",stderr);for(unsigned i=0;i<sizeof bytes;i++)fprintf(stderr," %02x",bytes[i]);fputc('\n',stderr);}
 fflush(stderr);return ownTrap?0:-1;
}
#endif
int pm_deck_inject(pid_t pid,const uint8_t* image,size_t bytes,const char credential[33],intptr_t* control,int(*identity)(void*),void* cookie,int* resumed){
 // First thing: prove the injector was entered at all. The end of this function
 // overwrites this with the full outcome, so a file that still says "entered"
 // means the injection hung or crashed inside.
 {FILE* entered=fopen("/data/ProsperoMenu/deck-status.txt","w");if(entered){fputs("deck injector entered\n",entered);fclose(entered);}}
 *control=0;*resumed=1;int attached=0,ok=0;intptr_t code=0,params=0,entry=0;PmDeckLaunch launch={0};
 Publication context={.pid=pid,.identity=identity,.cookie=cookie};
 const char* check="attach";PmDeckBoot last={0};int haveStatus=0;
 if(pt_attach_owned(pid,&attached))goto done;
 check="attached process identity";if(!identity||!identity(cookie))goto done;
 check="map deck image";entry=pm_elf_map(pid,image,bytes);if(!entry)goto done;
 check="create payload arguments";intptr_t args=pm_payload_args(pid);if(!args)goto done;
 *control=args+PM_DECK_BOOT_OFFSET;
 PmDeckBoot initial={0};initial.magic=PM_DECK_BOOT_MAGIC;initial.version=PM_DECK_BOOT_VERSION;memcpy(initial.credential,credential,33);
 check="initialize bootstrap record";if(pt_copyin(pid,&initial,*control,sizeof initial))goto done;
 check="resolve pthread_create";intptr_t create=kernel_dynlib_dlsym(pid,0x1,"pthread_create");if(!create)create=kernel_dynlib_dlsym(pid,0x2001,"pthread_create");
 if(!create)goto done;
 // SysV pthread_create(&thread,NULL,entry,args), followed by an observed int3.
 const unsigned char stager[]={0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x10,0x49,0x89,0xf8,
  0x48,0x8d,0x7d,0xf8,0x31,0xf6,0x49,0x8b,0x10,0x49,0x8b,0x48,0x08,0x41,0xff,0x50,0x10,0xcc,0xc9,0xc3};
 uint64_t pointers[3]={(uintptr_t)entry,(uintptr_t)args,(uintptr_t)create};
 check="stage bootstrap thread";code=pt_mmap(pid,0,0x4000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 params=pt_mmap(pid,0,0x4000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(code<=0||params<=0||pt_copyin(pid,stager,code,sizeof stager)||pt_copyin(pid,pointers,params,sizeof pointers))goto done;
 // Adjacent anonymous RW allocations can share one VM entry. The SDK's
 // kernel_mprotect changes that entire entry, without splitting at our range.
 // Use native mprotect first to isolate this code page from args and state.
 check="isolate bootstrap code";if(pt_mprotect(pid,code,0x4000,PROT_READ))goto done;
 check="protect bootstrap code";if(kernel_mprotect(pid,code,0x4000,PROT_READ|PROT_EXEC))goto done;
 check="verify bootstrap memory permissions";
 int controlProt=kernel_get_vmem_protection(pid,*control,sizeof initial);
 int paramsProt=kernel_get_vmem_protection(pid,params,sizeof pointers);
 int codeProt=kernel_get_vmem_protection(pid,code,sizeof stager);
 if(controlProt<0||paramsProt<0||codeProt<0||
    (controlProt&(PROT_READ|PROT_WRITE))!=(PROT_READ|PROT_WRITE)||
    (paramsProt&(PROT_READ|PROT_WRITE))!=(PROT_READ|PROT_WRITE)||
    (codeProt&(PROT_READ|PROT_WRITE|PROT_EXEC))!=(PROT_READ|PROT_EXEC))goto done;
 check="create bootstrap thread";if(pm_deck_launch(pid,code,code+sizeof stager-2,(uintptr_t)params,&launch))goto done;
#ifdef PROSPEROMENU_BOOTSTRAP_TRACE
 check="trace bootstrap first signal";if(trace_bootstrap(pid,*control,&attached))goto done;
#else
 // The new thread can already be inside CRT initialization. Detach directly,
 // matching the working libNineS resident path: do not commandeer the host
 // thread for cleanup syscalls while its new sibling is starting. These two
 // small mappings remain owned by this ShellUI generation until process exit.
 // The durable receipt prevents repeated allocations/reinjection this boot.
 check="resume ShellUI";if(pt_detach(pid,0))goto done;attached=0;
#endif
 for(unsigned tick=0;tick<180;tick++){
  check="bootstrap process identity";
  if(!identity(cookie))goto done;
  check="read bootstrap record";PmDeckBoot status;if(kernel_proc_copyout(pid,*control,&status,sizeof status))goto done;
  check="bootstrap record identity";if(status.magic!=PM_DECK_BOOT_MAGIC||status.version!=PM_DECK_BOOT_VERSION)goto done;
  if(!haveStatus||status.state!=last.state||status.phase!=last.phase||status.error!=last.error||
     (status.renderPhase&&!last.renderPhase)||(status.inputPhase&&!last.inputPhase)){
   fprintf(stderr,"ProsperoMenu deck bootstrap: state=%u phase=%u error=%u render=%u input=%u\n",status.state,status.phase,status.error,status.renderPhase,status.inputPhase);fflush(stderr);
  }
  if(status.kernelReadCount&&(!haveStatus||status.kernelReadCount!=last.kernelReadCount||status.reservedTrace!=last.reservedTrace)){
   fprintf(stderr,"ProsperoMenu kernel init: reads=%u readPhase=%u address=0x%llx length=%llu result=%d\n",
    status.kernelReadCount,status.reservedTrace,(unsigned long long)status.kernelReadAddress,(unsigned long long)status.kernelReadLength,status.kernelReadResult);fflush(stderr);
  }
  last=status;haveStatus=1;
  if(status.state==3){fprintf(stderr,"ProsperoMenu deck bootstrap rejected: check=%s error=%u count=%u\n",pm_deck_prepare_error(status.error),status.error,status.count);goto done;}
  if(status.state==1){
   if(status.count!=2&&status.count!=5&&status.count!=9&&status.count!=PM_DECK_MAX_PATCHES)goto done;
   context.patchCount=status.count;
   memcpy(context.patches,status.patches,sizeof context.patches);
   context.address=*control;PmPublicationOps ops={&context,attach,detach,threads,readbytes,writebytes,ack,reject,pause_thread};
   context.allowFallback=status.count>2;
   check="publish hook batch";int published=pm_publish_hooks(status.patches,status.count,&ops);
   // Only a verified complete rollback permits this fallback. The bootstrap
   // stays waiting at state 1; an uncertain write/detach never retries.
   if(published==1&&context.allowFallback){
    context.allowFallback=0;context.patchCount=2;
    published=pm_publish_hooks(status.patches,2,&ops);
   }
   if(published){
    uint32_t failed=3;kernel_proc_copyin(pid,&failed,*control+offsetof(PmDeckBoot,state),4);goto done;
   }
  }
  if(status.state==4){fprintf(stderr,"ProsperoMenu dashboard integration: hooks=%u available=%d preparationError=%u\n",status.count,status.count>=5,status.dashboardError);ok=1;break;}check="bootstrap wait";usleep(100000);
 }
done:
 if(attached){if(pt_detach(pid,launch.deliver))ok=0;else attached=0;}
 if(detach(&context))ok=0;
 *resumed=!attached&&!context.attached;
 // Filesystem logging only after both trace owners confirmed detach.
 if(*resumed){fprintf(stderr,"ProsperoMenu launch stop: caller=%d thread=%d signal=%d rip=0x%llx restored=%d\n",launch.caller,launch.stoppedThread,launch.signal,(unsigned long long)launch.stoppedRip,launch.restored);
#ifdef PROSPEROMENU_BOOTSTRAP_TRACE
 uint64_t imageEntry=0;if(bytes>=32)memcpy(&imageEntry,image+24,sizeof imageEntry);
 fprintf(stderr,"ProsperoMenu diagnostic deck mapping: pid=%d entry=0x%llx imageEntry=0x%llx loadBias=0x%llx control=0x%llx\n",(int)pid,
  (unsigned long long)entry,(unsigned long long)imageEntry,(unsigned long long)((uint64_t)entry-imageEntry),(unsigned long long)*control);
#endif
 fprintf(stderr,"ProsperoMenu deck injection: check=%s ok=%d observed=%d state=%u phase=%u error=%u render=%u input=%u\n",check,ok,haveStatus,last.state,last.phase,last.error,last.renderPhase,last.inputPhase);fflush(stderr);}
 // The console cannot show this, and stderr goes nowhere the owner can read:
 // write the outcome to a small file on the data folder, readable over FTP.
 {
  FILE* outcome=fopen("/data/ProsperoMenu/deck-status.txt","w");
  if(outcome){
   fprintf(outcome,"deck injection check=%s ok=%d observed=%d state=%u phase=%u error=%u(%s) render=%u input=%u hooks=%u\n",
    check,ok,haveStatus,haveStatus?last.state:0,haveStatus?last.phase:0,haveStatus?last.error:0,
    haveStatus?pm_deck_prepare_error(last.error):"none",haveStatus?last.renderPhase:0,haveStatus?last.inputPhase:0,
    haveStatus?last.count:0);
   fclose(outcome);
  }
 }
 return ok?0:-1;
}
