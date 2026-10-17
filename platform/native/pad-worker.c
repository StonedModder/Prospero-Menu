// SPDX-License-Identifier: GPL-3.0-or-later
// A single bounded exchange off the JavaScript thread. No ptrace, file I/O or
// credential changes; writes are restricted to the receipt-owned pad protocol.
#include "pad-worker.h"
#include "pad-exchange.h"
#include "module-session.h"
#include "user-read.h"
#include "user-write.h"
#include "native-operation.h"
#include "game-source.h"
#include "firmware-profiles.h"
#include <ps5/kernel.h>
#include <pthread.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
extern unsigned long KERNEL_ADDRESS_DMAP_BASE;
extern int kernel_proc_getpaddr(int,unsigned long,unsigned long*,unsigned long*);
typedef struct {
 PmModuleSession session;PmPadRequest request;PmPadBatch batch;
 uint64_t proc,started,finished;uint32_t serial;int result;
} Exchange;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static Exchange latest;static int running;static uint32_t serial;
static uint64_t monotonic(void *ignored){
 (void)ignored;struct timespec t;
 if(clock_gettime(CLOCK_MONOTONIC,&t)||t.tv_sec<0||t.tv_sec>INT64_C(18000000000)||t.tv_nsec<0||t.tv_nsec>=1000000000)return 0;
 return (uint64_t)t.tv_sec*UINT64_C(1000000000)+t.tv_nsec;
}
static int identity(void *p){
 Exchange *x=p;return pm_game_identity(x->session.pid,x->session.generation,x->session.title)&&
  x->proc&&(uint64_t)kernel_get_proc(x->session.pid)==x->proc;
}
static int translate(void *p,uint64_t at,uint64_t *physical,uint64_t *available){
 Exchange *x=p;unsigned long address=0,length=0;int result=kernel_proc_getpaddr(x->session.pid,at,&address,&length);
 *physical=address;*available=length;return result;
}
static uint64_t direct_address(uint64_t physical,size_t n){
 uint64_t base=KERNEL_ADDRESS_DMAP_BASE;
 if(base<UINT64_C(0xffff800000000000)||physical>UINT64_MAX-base||n>UINT64_MAX-(base+physical))return 0;
 return base+physical;
}
static int read_physical(void *p,uint64_t at,void *out,size_t n){
 (void)p;uint64_t address=direct_address(at,n);return address?kernel_copyout(address,out,n):-1;
}
static int write_physical(void *p,uint64_t at,const void *bytes,size_t n){
 (void)p;uint64_t address=direct_address(at,n);return address?kernel_copyin(bytes,address,n):-1;
}
static int user_read(Exchange *x,uint64_t at,void *out,size_t n){
 const PmUserReader reader={x,identity,translate,read_physical};return pm_user_read(&reader,at,out,n);
}
static int read_pad(void *p,size_t offset,void *out,size_t n){
 Exchange *x=p;if(offset>sizeof(PmPadBridge)||n>sizeof(PmPadBridge)-offset)return -1;
 return user_read(x,x->session.arena+0x5000+offset,out,n);
}
static int write_pad(void *p,size_t offset,const void *bytes,size_t n){
 Exchange *x=p;const size_t lease=offsetof(PmPadBridge,lease);
 int allowed=(offset>=lease&&offset<lease+sizeof(PmPadLease)&&n<=lease+sizeof(PmPadLease)-offset)||
  (offset==offsetof(PmPadBridge,consumed)&&n==4)||(offset==offsetof(PmPadBridge,enabled)&&n==4);
 if(!allowed)return -1;
 const PmUserWriter writer={x,identity,translate,write_physical};uint64_t base=x->session.arena+0x5000;
 return pm_user_write(&writer,base,sizeof(PmPadBridge),base+offset,bytes,n);
}
static void *worker(void *argument){
 Exchange *x=argument;x->result=1;x->started=monotonic(NULL);
 if(x->started&&snipers_firmware_profile(kernel_get_fw_version())){
  x->proc=(uint64_t)kernel_get_proc(x->session.pid);PmGameGate gate;
  if(!user_read(x,x->session.control,&gate,sizeof gate)&&pm_module_gate_valid(&x->session,&gate)){
   PmPadExchangeOps ops={x,read_pad,write_pad,monotonic};
   x->result=pm_pad_exchange(&ops,x->session.arena,x->session.cookie,&x->request,x->started,&x->batch);
  }
 }
 x->finished=monotonic(NULL);
 if(!identity(x)){x->result=-1;memset(&x->batch,0,sizeof x->batch);}
 pthread_mutex_lock(&mutex);latest=*x;running=0;pm_native_release();pthread_mutex_unlock(&mutex);free(x);return NULL;
}
static JSValue fail(JSContext *c,const char *code,const char *message){
 JSValue e=JS_NewError(c);JS_SetPropertyStr(c,e,"code",JS_NewString(c,code));JS_SetPropertyStr(c,e,"message",JS_NewString(c,message));return JS_Throw(c,e);
}
static JSValue start_js(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
 (void)self;if(argc!=7)return JS_ThrowTypeError(c,"Expected PID, generation, metadata hash, epoch, mode, owner and connection");
 const char *generation=JS_ToCString(c,argv[1]),*metadata=JS_ToCString(c,argv[2]);int32_t pid=0;PmPadRequest request={0};
 int invalid=!generation||!*generation||strlen(generation)>=128||!pm_hex_string(metadata,64)||
  JS_ToInt32(c,&pid,argv[0])||JS_ToUint32(c,&request.epoch,argv[3])||JS_ToUint32(c,&request.mode,argv[4])||
  JS_ToInt32(c,&request.owner,argv[5])||JS_ToUint32(c,&request.connection,argv[6])||
  pid<=1||!request.epoch||request.mode>PM_PAD_CAPTURE||request.connection>255||(request.mode==PM_PAD_CAPTURE&&request.owner<0);
 JSValue result=JS_UNDEFINED;
 if(invalid){result=JS_ThrowTypeError(c,"Invalid private game input exchange identity");goto done;}
 pthread_mutex_lock(&mutex);
 if(running||!pm_native_claim()){
  pthread_mutex_unlock(&mutex);result=fail(c,"NATIVE_BUSY","Native operation in progress");goto done;
 }
 Exchange *x=calloc(1,sizeof *x);
 if(!x||pm_module_live_session(pid,generation,metadata,&x->session)){
  free(x);pm_native_release();pthread_mutex_unlock(&mutex);result=fail(c,"INPUT_NOT_PREPARED","The matching game callback is not ready for input exchange");goto done;
 }
 x->request=request;x->serial=++serial;if(!x->serial)x->serial=++serial;
 latest=*x;running=1;pthread_t thread;pthread_attr_t attr;int error=pthread_attr_init(&attr);
 if(!error){error=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);if(!error)error=pthread_create(&thread,&attr,worker,x);pthread_attr_destroy(&attr);}
 if(error){free(x);running=0;memset(&latest,0,sizeof latest);pm_native_release();result=fail(c,"INPUT_WORKER","Input exchange worker could not start");}
 else result=JS_NewUint32(c,latest.serial);
 pthread_mutex_unlock(&mutex);
done:if(generation)JS_FreeCString(c,generation);if(metadata)JS_FreeCString(c,metadata);return result;
}
static JSValue poll_js(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
 (void)self;uint32_t id=0;if(argc!=1||JS_ToUint32(c,&id,argv[0])||!id)return JS_ThrowTypeError(c,"Expected input exchange serial");
 Exchange x;pthread_mutex_lock(&mutex);int busy=running; x=latest;pthread_mutex_unlock(&mutex);if(x.serial!=id)return JS_NULL;
 JSValue out=JS_NewObject(c);
 JS_SetPropertyStr(c,out,"serial",JS_NewUint32(c,id));JS_SetPropertyStr(c,out,"busy",JS_NewBool(c,busy));
 JS_SetPropertyStr(c,out,"pid",JS_NewInt32(c,x.session.pid));JS_SetPropertyStr(c,out,"generation",JS_NewString(c,x.session.generation));
 JS_SetPropertyStr(c,out,"metadataHash",JS_NewString(c,x.session.metadataHash));JS_SetPropertyStr(c,out,"epoch",JS_NewUint32(c,x.request.epoch));
 if(busy)return out;
 JS_SetPropertyStr(c,out,"result",JS_NewInt32(c,x.result));JS_SetPropertyStr(c,out,"reset",JS_NewBool(c,x.batch.reset));
 JS_SetPropertyStr(c,out,"faults",JS_NewUint32(c,x.batch.faults));
 JS_SetPropertyStr(c,out,"time",JS_NewFloat64(c,(double)x.finished/1000000));
 JS_SetPropertyStr(c,out,"deadline",JS_NewFloat64(c,(double)x.batch.deadline/1000000));
 JSValue events=JS_NewArray(c);
 for(uint32_t i=0;i<x.batch.count;i++){
  const PmPadEvent *e=&x.batch.events[i];JSValue item=JS_NewObject(c),sticks=JS_NewArray(c),triggers=JS_NewArray(c);
  JS_SetPropertyStr(c,item,"sequence",JS_NewUint32(c,e->sequence));JS_SetPropertyStr(c,item,"buttons",JS_NewUint32(c,e->buttons));
  JS_SetPropertyStr(c,item,"handle",JS_NewInt32(c,e->handle));JS_SetPropertyStr(c,item,"connection",JS_NewUint32(c,e->connection));
  JS_SetPropertyStr(c,item,"time",JS_NewFloat64(c,(double)e->time/1000000));
  for(unsigned j=0;j<4;j++)JS_SetPropertyUint32(c,sticks,j,JS_NewUint32(c,e->sticks[j]));
  for(unsigned j=0;j<2;j++)JS_SetPropertyUint32(c,triggers,j,JS_NewUint32(c,e->triggers[j]));
  JS_SetPropertyStr(c,item,"sticks",sticks);JS_SetPropertyStr(c,item,"triggers",triggers);JS_SetPropertyUint32(c,events,i,item);
 }
 JS_SetPropertyStr(c,out,"events",events);return out;
}
static JSValue clock_js(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
 (void)self;(void)argc;(void)argv;return JS_NewFloat64(c,(double)(monotonic(NULL)/1000000));
}
void pm_pad_worker_bind(JSContext *c,JSValue api){
 JS_SetPropertyStr(c,api,"gameInputClock",JS_NewCFunction(c,clock_js,"gameInputClock",0));
 JS_SetPropertyStr(c,api,"gameInputExchange",JS_NewCFunction(c,start_js,"gameInputExchange",7));
 JS_SetPropertyStr(c,api,"gameInputExchangePoll",JS_NewCFunction(c,poll_js,"gameInputExchangePoll",1));
}
