// SPDX-License-Identifier: GPL-3.0-or-later
#include "quickjs.h"
#include "deck-inject.h"
#include "deck-bootstrap.h"
#include "native-operation.h"
#include "kstuff-probe.h"
#include "elf-image.h"
#include "sha256.h"
#include <pthread.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include "process-info.h"
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stddef.h>
extern const unsigned char prosperomenu_deck_start[],prosperomenu_deck_end[];
#define RECEIPT "/data/ProsperoMenu/deck.receipt"
#define MAGIC UINT32_C(0x54444431)
typedef struct {
 uint32_t magic,version,stage,clean;int32_t pid;uint32_t error;
 int64_t bootSec,bootUsec;uint64_t control;
 char request[33],generation[128],message[192];unsigned char checksum[32];
} Receipt;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static Receipt current;static char secret[33];static int running;
static int input_trace_pid;static PmDeckBoot input_trace;
static const char* start_check="arguments";
static int hex(const char* s){if(!s||strlen(s)!=32)return 0;for(unsigned i=0;i<32;i++)if(!((s[i]>='a'&&s[i]<='f')||(s[i]>='0'&&s[i]<='9')))return 0;return 1;}
static uint64_t millis(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static void checksum(const Receipt* r,unsigned char* digest){SHA256_CTX s;sha256_init(&s);sha256_update(&s,(const unsigned char*)r,offsetof(Receipt,checksum));sha256_final(&s,digest);}
static int load(Receipt* r){
 int fd=open(RECEIPT,O_RDONLY|O_NOFOLLOW);if(fd<0)return -1;struct stat st;
 int bad=fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size!=sizeof *r||read(fd,r,sizeof *r)!=sizeof *r;close(fd);
 unsigned char digest[32];if(!bad)checksum(r,digest);
 if(bad||r->magic!=MAGIC||r->version!=1||r->stage<1||r->stage>5||r->request[32]||r->generation[127]||r->message[191]||
   !hex(r->request)||memcmp(r->checksum,digest,32)){errno=EINVAL;return -1;}return 0;
}
static int save(Receipt* r){
 checksum(r,r->checksum);int fd=open(RECEIPT ".tmp",O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);if(fd<0)return -1;
 int bad=write(fd,r,sizeof *r)!=sizeof *r||fsync(fd);if(close(fd))bad=1;
 if(!bad)bad=rename(RECEIPT ".tmp",RECEIPT);
 if(!bad){fd=open("/data/ProsperoMenu",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(fd<0)return -1;bad=fsync(fd);close(fd);}return bad?-1:0;
}
static int transition(Receipt* r,unsigned stage,int clean,const char* message){
 r->stage=stage;r->clean=clean;snprintf(r->message,sizeof r->message,"%s",message);int result=save(r);
 pthread_mutex_lock(&mutex);current=*r;pthread_mutex_unlock(&mutex);return result;
}
static int identity(Receipt* r){
 struct timeval boot;size_t size=sizeof boot;int bm[]={CTL_KERN,KERN_BOOTTIME};
 if(sysctl(bm,2,&boot,&size,NULL,0)||size!=sizeof boot)return 0;
 if(r->bootSec!=boot.tv_sec||r->bootUsec!=boot.tv_usec)return 0;
 struct kinfo_proc info={0};
 if(pm_process_info(r->pid,&info)){
  int error=errno;fprintf(stderr,"ProsperoMenu deck identity: process record read failed pid=%d errno=%d\n",r->pid,error);errno=error;return 0;
 }
 info.ki_comm[COMMLEN]=0;if(strcmp(info.ki_comm,"SceShellUI")||info.ki_start.tv_sec<=0||info.ki_start.tv_usec<0||info.ki_start.tv_usec>=1000000)return 0;
 char generation[128];snprintf(generation,sizeof generation,"%lld:%lld/%d/%lld:%lld",(long long)boot.tv_sec,(long long)boot.tv_usec,r->pid,(long long)info.ki_start.tv_sec,(long long)info.ki_start.tv_usec);
 return !strcmp(generation,r->generation);
}
static int verify_identity(void* receipt){return identity(receipt);}
static void* worker(void* ignored){
 (void)ignored;Receipt r;char credential[33];pthread_mutex_lock(&mutex);r=current;memcpy(credential,secret,33);pthread_mutex_unlock(&mutex);
 const SnipersFirmwareProfile* profile=snipers_firmware_profile(kernel_get_fw_version());uint64_t base=KERNEL_ADDRESS_DATA_BASE;
 PmKstuffSnapshot a=pm_kstuff_read(base,profile),b=pm_kstuff_read(base,profile);
 if(!identity(&r)||!pm_kstuff_snapshot_equal(&a,&b)||pm_kstuff_classify(base,&b)!=PM_KSTUFF_INSTALLED||!pm_kstuff_injection_ready(base,&b)){
  transition(&r,4,1,"ShellUI identity or kstuff readiness changed before injection");goto done;
 }
 PmElfImage image;char reason[192];size_t bytes=(size_t)(prosperomenu_deck_end-prosperomenu_deck_start);
 if(pm_elf_inspect(prosperomenu_deck_start,bytes,&image,reason,sizeof reason)||image.header.type!=3){transition(&r,4,1,"Embedded deck image validation failed");goto done;}
 if(transition(&r,2,0,"Preparing the native deck inside the verified ShellUI process")){
  transition(&r,4,1,"Deck intent could not be saved; injection withheld");goto done;
 }
 intptr_t control=0;int resumed=0;int injected=pm_deck_inject(r.pid,prosperomenu_deck_start,bytes,credential,&control,verify_identity,&r,&resumed);r.control=control;
 if(!resumed){
  // Retain the durable intent. Do not wait on filesystem operations while
  // ShellUI may still be stopped by this coordinator.
  r.stage=5;r.clean=0;snprintf(r.message,sizeof r.message,"ShellUI resume is unconfirmed; original deck intent retained");
  pthread_mutex_lock(&mutex);current=r;pthread_mutex_unlock(&mutex);
 }else if(injected){
  // A bootstrap rejected before any hook publication (state 3, count 0) leaves
  // ShellUI pristine. Record a clean rejection, which the next attempt may take;
  // only published hooks make a second attempt in this boot unsafe.
  PmDeckBoot seen={0};
  if(r.control&&!kernel_proc_copyout(r.pid,r.control,&seen,sizeof seen)&&
     seen.magic==PM_DECK_BOOT_MAGIC&&seen.version==PM_DECK_BOOT_VERSION&&
     seen.state==3&&seen.count==0)
   transition(&r,4,1,"Deck bootstrap rejected before any hooks were published; a retry is safe");
  else
   transition(&r,5,0,"Deck initialization or hook publication was not confirmed; no reinjection was issued");
 }
 else if(transition(&r,3,0,"Deck hooks published; waiting for actual renderer heartbeat")){
  r.stage=5;snprintf(r.message,sizeof r.message,"Deck initialized but its durable receipt could not be saved");pthread_mutex_lock(&mutex);current=r;pthread_mutex_unlock(&mutex);
 }
done:
 memset(credential,0,sizeof credential);pthread_mutex_lock(&mutex);memset(secret,0,sizeof secret);running=0;pm_native_release();pthread_mutex_unlock(&mutex);return NULL;
}
static int start(const char* request,int pid,const char* generation,const char* credential){
 start_check="arguments";
 if(!hex(request)||!hex(credential)||pid<=1||!generation||strlen(generation)>=128){errno=EINVAL;return -1;}
 pthread_mutex_lock(&mutex);
 if(running){int same=!strcmp(request,current.request)&&pid==current.pid&&!strcmp(generation,current.generation);pthread_mutex_unlock(&mutex);if(same)return 0;errno=EBUSY;return -1;}
 if(pm_native_busy()){start_check="native operation busy";pthread_mutex_unlock(&mutex);errno=EBUSY;return -1;}
 Receipt r={0};r.magic=MAGIC;r.version=1;r.stage=1;r.clean=1;r.pid=pid;strcpy(r.request,request);strcpy(r.generation,generation);
 struct timeval boot;size_t size=sizeof boot;int mib[]={CTL_KERN,KERN_BOOTTIME};
 start_check="boot identity";
 if(sysctl(mib,2,&boot,&size,NULL,0)||size!=sizeof boot){pthread_mutex_unlock(&mutex);return -1;}r.bootSec=boot.tv_sec;r.bootUsec=boot.tv_usec;
 Receipt prior;
 start_check="previous deck receipt";
 if(!load(&prior)){
  if(!strcmp(request,prior.request)){
   int same=prior.pid==r.pid&&!strcmp(prior.generation,r.generation)&&prior.bootSec==r.bootSec&&prior.bootUsec==r.bootUsec;
   if(same)current=prior;pthread_mutex_unlock(&mutex);if(same)return 0;errno=EINVAL;return -1;
  }
  if(prior.bootSec==r.bootSec&&prior.bootUsec==r.bootUsec&&!(prior.stage==4&&prior.clean)){
   pthread_mutex_unlock(&mutex);errno=EALREADY;return -1;
  }
 }else if(errno!=ENOENT){pthread_mutex_unlock(&mutex);return -1;}
 start_check="ShellUI identity";
 if(!identity(&r)){int e=errno;pthread_mutex_unlock(&mutex);errno=e?e:ESRCH;return -1;}
 start_check="save deck intent";
 if(save(&r)){int e=errno;pthread_mutex_unlock(&mutex);errno=e;return -1;}
 start_check="claim native operation";
 if(!pm_native_claim()){pthread_mutex_unlock(&mutex);errno=EBUSY;return -1;}
 start_check="create deck worker";
 current=r;memcpy(secret,credential,33);running=1;pthread_t thread;int result=pthread_create(&thread,NULL,worker,NULL);
 if(!result)pthread_detach(thread);
 else{running=0;pm_native_release();current.stage=4;current.clean=1;snprintf(current.message,sizeof current.message,"Deck worker could not start");save(&current);}
 pthread_mutex_unlock(&mutex);if(result){errno=result;return -1;}return 0;
}
static JSValue failure(JSContext* c){
 const int e=errno;char message[256];snprintf(message,sizeof message,"Native deck stopped at %s (errno %d). No automatic reinjection.",start_check,e);
 fprintf(stderr,"ProsperoMenu deck start: check=%s errno=%d\n",start_check,e);
 JSValue error=JS_NewError(c);JS_SetPropertyStr(c,error,"code",JS_NewString(c,e==EALREADY?"DECK_ALREADY_ATTEMPTED":e==EBUSY?"NATIVE_BUSY":"DECK_NATIVE"));
 JS_SetPropertyStr(c,error,"errno",JS_NewInt32(c,e));
 JS_SetPropertyStr(c,error,"message",JS_NewString(c,message));return JS_Throw(c,error);
}
static JSValue start_js(JSContext* c,JSValueConst self,int argc,JSValueConst* argv){
 (void)self;if(argc!=4)return JS_ThrowTypeError(c,"Expected request, PID, generation and private renderer credential");
 int32_t pid;const char* request=JS_ToCString(c,argv[0]),*generation=JS_ToCString(c,argv[2]),*credential=JS_ToCString(c,argv[3]);
 int result=-1;if(request&&generation&&credential&&!JS_ToInt32(c,&pid,argv[1]))result=start(request,pid,generation,credential);
 if(request)JS_FreeCString(c,request);if(generation)JS_FreeCString(c,generation);if(credential)JS_FreeCString(c,credential);return result?failure(c):JS_UNDEFINED;
}
static JSValue poll_js(JSContext* c,JSValueConst self,int argc,JSValueConst* argv){
 (void)self;if(argc!=1)return JS_ThrowTypeError(c,"Expected deck request");const char* request=JS_ToCString(c,argv[0]);if(!request)return JS_EXCEPTION;
 if(!hex(request)){JS_FreeCString(c,request);return JS_ThrowTypeError(c,"Invalid deck request");}
 Receipt r;pthread_mutex_lock(&mutex);int live=running,found=current.magic==MAGIC&&!strcmp(current.request,request);if(found)r=current;pthread_mutex_unlock(&mutex);
 if(!found&&(load(&r)||strcmp(r.request,request))){JS_FreeCString(c,request);return JS_NULL;}JS_FreeCString(c,request);
 int ready=0,traced=0;PmDeckBoot status;uint64_t now=0; // ProsperoMenu: traced/status/now feed the debug trace below
 if(!live&&(r.stage==1||r.stage==2))r.stage=5;
 // Sample "busy" once: a poll skipped because another native operation was
 // running must report busy, or the caller reads it as a lost renderer.
 const int busy=pm_native_busy();
 if(!busy&&r.stage==3&&r.control&&identity(&r)){
  fprintf(stderr,"ProsperoMenu deck poll: stage=%d control=%llu busy=%d\n",r.stage,(unsigned long long)r.control,busy);fflush(stderr);
  if(!kernel_proc_copyout(r.pid,r.control,&status,sizeof status)&&status.magic==PM_DECK_BOOT_MAGIC&&status.version==PM_DECK_BOOT_VERSION&&status.state==4){
   traced=1;
   // Read the clock after the copy: ShellUI can stamp a heartbeat between an
   // earlier clock read and the copy, which looked like a future heartbeat.
   now=millis();
   if(status.renderedAt&&now>=status.renderedAt&&now-status.renderedAt<=3000)ready=1;fprintf(stderr,"ProsperoMenu deck heartbeat: now=%llu renderedAt=%llu age=%lld ready=%d state=%u renderPhase=%u\n",(unsigned long long)now,(unsigned long long)status.renderedAt,(long long)(now-status.renderedAt),ready,status.state,status.renderPhase);fflush(stderr);
   if(input_trace.menuUpdateWaits!=status.menuUpdateWaits||input_trace.menuUpdateExpired!=status.menuUpdateExpired){
    fprintf(stderr,"ProsperoMenu menu updates: waits=%u expired=%u\n",status.menuUpdateWaits,status.menuUpdateExpired);
    input_trace.menuUpdateWaits=status.menuUpdateWaits;input_trace.menuUpdateExpired=status.menuUpdateExpired;
   }
   if(input_trace.cheatsStage!=status.cheatsStage||input_trace.cheatsError!=status.cheatsError||input_trace.cheatsActions!=status.cheatsActions||input_trace.dashboardRefreshes!=status.dashboardRefreshes){
    fprintf(stderr,"ProsperoMenu native cheats: stage=%u error=%u actions=%u; homeRefresh=%u error=%u\n",status.cheatsStage,status.cheatsError,status.cheatsActions,status.dashboardRefreshes,status.dashboardRefreshError);
    input_trace.cheatsStage=status.cheatsStage;input_trace.cheatsError=status.cheatsError;input_trace.cheatsActions=status.cheatsActions;input_trace.dashboardRefreshes=status.dashboardRefreshes;
   }
   if(input_trace.dashboardVersionCalls!=status.dashboardVersionCalls||input_trace.dashboardVersionError!=status.dashboardVersionError){
    fprintf(stderr,"ProsperoMenu dashboard version: labels=%u decorated=%u preparationError=%u\n",status.dashboardVersionCalls,status.dashboardVersionDecorated,status.dashboardVersionError);
    input_trace.dashboardVersionCalls=status.dashboardVersionCalls;input_trace.dashboardVersionError=status.dashboardVersionError;
   }
   if(input_trace_pid!=r.pid||input_trace.dashboardTitleCalls!=status.dashboardTitleCalls||input_trace.dashboardOptionCalls!=status.dashboardOptionCalls||input_trace.dashboardUrlCalls!=status.dashboardUrlCalls){
    fprintf(stderr,"ProsperoMenu dashboard callbacks: titles=%u decorated=%u options=%u added=%u urls=%u\n",status.dashboardTitleCalls,status.dashboardTitleDecorated,status.dashboardOptionCalls,status.dashboardOptionAdded,status.dashboardUrlCalls);
    input_trace.dashboardTitleCalls=status.dashboardTitleCalls;input_trace.dashboardOptionCalls=status.dashboardOptionCalls;input_trace.dashboardUrlCalls=status.dashboardUrlCalls;
   }
   if(input_trace_pid!=r.pid||input_trace.inputDevice!=status.inputDevice||input_trace.inputButtonsSeen!=status.inputButtonsSeen||
      input_trace.inputRoute!=status.inputRoute||input_trace.inputReady!=status.inputReady||input_trace.inputOpen!=status.inputOpen||input_trace.reservedTrace!=status.reservedTrace||
      input_trace.inputFocus!=status.inputFocus||input_trace.inputRequestedRoute!=status.inputRequestedRoute){
    fprintf(stderr,"ProsperoMenu deck input: calls=%u device=%d seen=0x%x skipped=%u route=%u ready=%u open=%u scene=%u focus=%d requestedRoute=%u\n",
      status.inputCalls,status.inputDevice,status.inputButtonsSeen,status.inputSkipped,status.inputRoute,status.inputReady,status.inputOpen,status.reservedTrace,status.inputFocus,status.inputRequestedRoute);
    input_trace_pid=r.pid;input_trace=status;
   }
  }
 }
 JSValue out=JS_NewObject(c);JS_SetPropertyStr(c,out,"request",JS_NewString(c,r.request));JS_SetPropertyStr(c,out,"stage",JS_NewInt32(c,r.stage));
 char boot[80];snprintf(boot,sizeof boot,"%lld:%lld",(long long)r.bootSec,(long long)r.bootUsec);JS_SetPropertyStr(c,out,"boot",JS_NewString(c,boot));
 JS_SetPropertyStr(c,out,"busy",JS_NewBool(c,busy));
 JS_SetPropertyStr(c,out,"pid",JS_NewInt32(c,r.pid));JS_SetPropertyStr(c,out,"generation",JS_NewString(c,r.generation));
 JS_SetPropertyStr(c,out,"ready",JS_NewBool(c,ready));JS_SetPropertyStr(c,out,"noSideEffects",JS_NewBool(c,r.clean));JS_SetPropertyStr(c,out,"message",JS_NewString(c,r.message));
 if(traced){
  // ProsperoMenu diagnostics: renderer and input-hook counters from the live
  // ShellUI deck, so in-game behaviour can be inspected without a log file.
  JSValue t=JS_NewObject(c);
#define PM_TRACE(name,value) JS_SetPropertyStr(c,t,name,JS_NewFloat64(c,(double)(value)))
  PM_TRACE("renderedAgeMs",status.renderedAt&&now>=status.renderedAt?(double)(now-status.renderedAt):-1);
  PM_TRACE("phase",status.phase);PM_TRACE("renderPhase",status.renderPhase);PM_TRACE("inputPhase",status.inputPhase);
  PM_TRACE("scene",status.reservedTrace);PM_TRACE("inputCalls",status.inputCalls);PM_TRACE("inputDevice",status.inputDevice);
  PM_TRACE("inputButtonsSeen",status.inputButtonsSeen);PM_TRACE("inputSkipped",status.inputSkipped);PM_TRACE("inputRoute",status.inputRoute);
  PM_TRACE("inputReady",status.inputReady);PM_TRACE("inputOpen",status.inputOpen);PM_TRACE("inputFocus",status.inputFocus);
  PM_TRACE("inputRequestedRoute",status.inputRequestedRoute);PM_TRACE("menuUpdateWaits",status.menuUpdateWaits);PM_TRACE("menuUpdateExpired",status.menuUpdateExpired);
#undef PM_TRACE
  JS_SetPropertyStr(c,out,"trace",t);
 }
 return out;
}
void pm_deck_bind(JSContext* c,JSValue api){JS_SetPropertyStr(c,api,"deckStart",JS_NewCFunction(c,start_js,"deckStart",4));JS_SetPropertyStr(c,api,"deckPoll",JS_NewCFunction(c,poll_js,"deckPoll",1));}
