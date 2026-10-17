// SPDX-License-Identifier: GPL-3.0-or-later
#include "quickjs.h"
#include "cheat-transaction.h"
#include "memory-map.h"
#include "memory-inspect.h"
#include "game-source.h"
#include "game-info.h"
#include "native-operation.h"
#include "kstuff-probe.h"
#include "sha256.h"
#include "pt.h"
#include <ps5/kernel.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <time.h>
#include <pthread.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#define ROOT "/data/ProsperoMenu/cheat-operations"
#define MAGIC UINT64_C(0x5441434348454154)
enum {QUEUED=1,ARMED=2,APPLIED=3,REJECTED=4,UNKNOWN=5};
extern const intptr_t KERNEL_OFFSET_VMSPACE_P_ROOT;
extern int pt_getlwps(pid_t,int*,size_t);
typedef struct {
  uint64_t magic;uint32_t version,phase;int32_t pid,resumed,restored;uint32_t writes;
  char request[33],planHash[65],generation[128],title[10],metadataHash[65],module[128],message[192];
  unsigned char digest[32];
} Receipt;
typedef struct {
  Receipt record;unsigned char *wire;size_t wireSize;PmCheatPlan plan;
  uint64_t proc,start;int attached,dirty;uint64_t pcs[2048];unsigned pcCount;
  int ownerPid,credentialsSaved,intentSaved;uint64_t ownerAuth;unsigned char ownerCaps[16];
  PmMemoryMapping changes[16];size_t changed;
  int inspect;uint64_t inspectOffset;size_t inspectLength;
} Work;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static Receipt current;static int running;
static unsigned char inspected[PM_INSPECT_MAX_BYTES];
static size_t inspectedLength;static uint64_t inspectedBase,inspectedSize;
static int inspectedReady;
static void digest(const void *bytes,size_t size,unsigned char out[32]){SHA256_CTX h;sha256_init(&h);sha256_update(&h,bytes,size);sha256_final(&h,out);}
static void hash(const void *bytes,size_t size,char out[65]){unsigned char d[32];digest(bytes,size,d);for(unsigned i=0;i<32;i++)snprintf(out+i*2,3,"%02x",d[i]);}
static int hex(const char *s,size_t size){if(!s||strlen(s)!=size)return 0;for(size_t i=0;i<size;i++)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return 0;return 1;}
static int transfer(int fd,void *bytes,size_t size,int writing){unsigned char *p=bytes;while(size){ssize_t n=writing?write(fd,p,size):read(fd,p,size);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;p+=n;size-=(size_t)n;}return 0;}
static int folder(void){if(mkdir(ROOT,0700)&&errno!=EEXIST)return -1;return open(ROOT,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);}
static void name(const char *id,char out[48]){snprintf(out,48,"%s.receipt",id);}
static int decode(Receipt *r){
  if(r->magic!=MAGIC||r->version!=1||r->phase<QUEUED||r->phase>UNKNOWN||r->pid<=1||
    r->request[32]||r->planHash[64]||r->generation[127]||r->title[9]||r->metadataHash[64]||r->module[127]||r->message[191]||
    r->resumed<0||r->resumed>1||r->restored<0||r->restored>1||r->writes>PM_CHEAT_MAX_PATCHES||
    !hex(r->request,32)||!hex(r->planHash,64)||!hex(r->metadataHash,64)||!pm_game_title(r->title)||!r->generation[0]||!r->module[0])return -1;
  unsigned char expected[32],actual[32];memcpy(expected,r->digest,32);memset(r->digest,0,32);digest(r,sizeof *r,actual);memcpy(r->digest,expected,32);
  return memcmp(actual,expected,32)?-1:0;
}
static int load_at(int dir,const char *file,Receipt *r){
  int fd=openat(dir,file,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return -1;struct stat st;
  int bad=fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size!=sizeof *r||transfer(fd,r,sizeof *r,0);close(fd);
  if(bad||decode(r)){errno=EINVAL;return -1;}return 0;
}
static int load(const char *id,Receipt *r){int dir=folder();if(dir<0)return -1;char file[48];name(id,file);int result=load_at(dir,file,r),saved=errno;close(dir);errno=saved;return result;}
static int save(Receipt *r){
  memset(r->digest,0,32);unsigned char d[32];digest(r,sizeof *r,d);memcpy(r->digest,d,32);
  int dir=folder();if(dir<0)return -1;char file[48],temp[56];name(r->request,file);snprintf(temp,sizeof temp,"%s.tmp",file);
  int fd=openat(dir,temp,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);if(fd<0){close(dir);return -1;}
  int bad=transfer(fd,r,sizeof *r,1)||fsync(fd);if(close(fd))bad=1;
  if(!bad)bad=renameat(dir,temp,dir,file)||fsync(dir);close(dir);return bad?-1:0;
}
static int unresolved(const char *generation){
  int fd=folder();if(fd<0)return -1;DIR *dir=fdopendir(fd);if(!dir){close(fd);return -1;}
  struct dirent *entry;unsigned count=0;int result=0;errno=0;
  while((entry=readdir(dir))){
    if(++count>4096){result=-1;break;}
    size_t length=strlen(entry->d_name);if(length!=40||strcmp(entry->d_name+32,".receipt"))continue;
    char id[33];memcpy(id,entry->d_name,32);id[32]=0;if(!hex(id,32)){result=-1;break;}
    Receipt r;if(load_at(fd,entry->d_name,&r)){result=-1;break;}
    if(!strcmp(generation,r.generation)&&(r.phase==QUEUED||r.phase==ARMED||r.phase==UNKNOWN)){result=1;break;}
    errno=0;
  }
  if(!entry&&errno)result=-1;closedir(dir);return result;
}
static void publish(Receipt *r){pthread_mutex_lock(&mutex);current=*r;pthread_mutex_unlock(&mutex);}
static int identity(void *arg){Work *w=arg;return pm_game_identity(w->record.pid,w->record.generation,w->record.title)&&
  (!w->proc||(uint64_t)kernel_get_proc(w->record.pid)==w->proc);}
static uint64_t now(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static int deadline(void *arg){Work *w=arg;uint64_t t=now();return t&&w->start&&t>=w->start&&t-w->start<2000;}
static int kread(void *unused,uint64_t at,void *bytes,size_t length){(void)unused;return kernel_copyout(at,bytes,length);}
static const PmMemoryReader reader={NULL,kread};
static int attach(void *arg){
  Work *w=arg;w->start=now();if(!w->start||!identity(w))return -1;
  w->ownerPid=getpid();w->ownerAuth=kernel_get_ucred_authid(w->ownerPid);
  if(!w->ownerAuth||kernel_get_ucred_caps(w->ownerPid,w->ownerCaps))return -1;w->credentialsSaved=1;
  if(pt_attach_owned(w->record.pid,&w->attached)||!identity(w))return -1;
  w->proc=(uint64_t)kernel_get_proc(w->record.pid);if(!w->proc)return -1;
  int ids[2048],n=pt_getlwps(w->record.pid,ids,2048);if(n<=0||n>2048)return -1;
  for(int i=0;i<n;i++){struct reg r;if(pt_getregs(ids[i],&r))return -1;w->pcs[i]=r.r_rip;}
  w->pcCount=(unsigned)n;return 0;
}
static int detach(void *arg){Work *w=arg;if(!w->attached)return 0;if(pt_detach(w->record.pid,0))return -1;w->attached=0;return 0;}
static int module(void *arg,uint64_t *base,uint64_t *size){Work *w=arg;PmMemoryModule m;
  int status=pm_memory_module(&reader,w->proc,w->record.module,&m);if(status)return -1;*base=m.base;*size=m.size;return 0;}
static int span(Work *w,uint64_t address,size_t size,PmMemoryMapping *maps,size_t *count){
  return !identity(w)||pm_memory_span(&reader,w->proc,KERNEL_OFFSET_VMSPACE_P_ROOT,address,size,maps,16,count)?-1:0;
}
static int range(void *arg,uint64_t address,size_t size){
  Work *w=arg;PmMemoryMapping maps[16];size_t count=0;if(span(w,address,size,maps,&count))return -1;
  int executable=0;for(size_t i=0;i<count;i++){if(!(maps[i].protection&5))return -1;executable|=maps[i].protection&4;}
  if(executable)for(unsigned i=0;i<w->pcCount;i++)if(w->pcs[i]>=address-14&&w->pcs[i]<address+size)return -1;
  return 0;
}
static int readable(void *arg,uint64_t address,size_t size){
  Work *w=arg;PmMemoryMapping maps[16];size_t count=0;if(span(w,address,size,maps,&count))return -1;
  for(size_t i=0;i<count;i++)if(!(maps[i].protection&1))return -1;
  return 0;
}
static int readbytes(void *arg,uint64_t at,void *bytes,size_t length){Work *w=arg;return !identity(w)?-1:pt_copyout(w->record.pid,at,bytes,length);}
static int unchanged(Work *w,const PmMemoryMapping *m){
  uint64_t limits[2];return identity(w)&&!kernel_copyout(m->entry+0x20,limits,sizeof limits)&&limits[0]==m->start&&limits[1]==m->end;
}
static int restore(Work *w){
  int failed=0;
  for(size_t i=w->changed;i>0;i--){PmMemoryMapping *m=&w->changes[i-1];unsigned char actual=0;
    if(!unchanged(w,m)||kernel_copyin(&m->protection,m->entry+0x64,1)||kernel_copyout(m->entry+0x64,&actual,1)||actual!=m->protection)failed=1;
  }
  if(!failed){w->changed=0;w->dirty=0;}return failed?-1:0;
}
static int writebytes(void *arg,uint64_t at,const void *bytes,size_t length){
  Work *w=arg;if(!w->attached||!identity(w)||(w->dirty&&restore(w)))return -1;
  PmMemoryMapping maps[16];size_t count=0;if(span(w,at,length,maps,&count))return -1;
  int failed=0;
  for(size_t i=0;i<count;i++){
    unsigned char observed=0,desired=maps[i].protection|3;
    if(!unchanged(w,&maps[i])||kernel_copyout(maps[i].entry+0x64,&observed,1)||observed!=maps[i].protection){failed=1;break;}
    w->changes[w->changed++]=maps[i];w->dirty=1;
    if(kernel_copyin(&desired,maps[i].entry+0x64,1)||kernel_copyout(maps[i].entry+0x64,&observed,1)||observed!=desired){failed=1;break;}
  }
  if(!failed&&pt_copyin(w->record.pid,bytes,at,length))failed=1;
  if(restore(w))failed=1;__atomic_thread_fence(__ATOMIC_SEQ_CST);return failed?-1:0;
}
static int journal(void *arg){
  Work *w=arg;if(!w->intentSaved)return -1;
  // QUEUED was durably committed before attach and is treated as unknown after
  // interruption. Do not introduce filesystem writes/fsync while the game is
  // stopped. Publish progress in RAM; save the final outcome after detach.
  w->record.phase=ARMED;snprintf(w->record.message,sizeof w->record.message,"Expected bytes verified; applying cheat transaction");publish(&w->record);return 0;
}
static int healthy(void *arg){
  Work *w=arg;int good=!w->dirty;
  if(w->credentialsSaved){
    unsigned char caps[16];uint64_t auth=kernel_get_ucred_authid(w->ownerPid);
    if(auth!=w->ownerAuth||kernel_get_ucred_caps(w->ownerPid,caps)||memcmp(caps,w->ownerCaps,16)){
      int c=kernel_set_ucred_caps(w->ownerPid,w->ownerCaps),a=kernel_set_ucred_authid(w->ownerPid,w->ownerAuth);
      if(c||a||kernel_get_ucred_authid(w->ownerPid)!=w->ownerAuth||kernel_get_ucred_caps(w->ownerPid,caps)||memcmp(caps,w->ownerCaps,16))good=0;
    }
  }
  return good;
}
static int metadata(Work *w){unsigned char *bytes=NULL;size_t size=0;char reason[192];
  if(pm_game_metadata_read(w->record.pid,w->record.generation,w->record.title,&bytes,&size,reason,sizeof reason))return 0;
  char actual[65];hash(bytes,size,actual);free(bytes);return !strcmp(actual,w->record.metadataHash);
}
static void *worker(void *arg){
  Work *w=arg;PmCheatResult result={.status=PM_CHEAT_REJECTED,.resumed=1,.restored=1};
  unsigned char *readBuffer=w->inspect?calloc(1,PM_INSPECT_MAX_BYTES):NULL;
  uint64_t readBase=0,readSize=0;
  snprintf(result.reason,sizeof result.reason,"Game metadata or process identity changed before native execution");
  const SnipersFirmwareProfile *profile=snipers_firmware_profile(kernel_get_fw_version());uint64_t base=KERNEL_ADDRESS_DATA_BASE;
  PmKstuffSnapshot a=pm_kstuff_read(base,profile),b=pm_kstuff_read(base,profile);
  int ready=profile&&pm_kstuff_snapshot_equal(&a,&b)&&pm_kstuff_injection_ready(base,&b);
  if(!ready)snprintf(result.reason,sizeof result.reason,"Verified kernel services are not ready for native game memory operations");
  if(ready&&metadata(w)&&identity(w)){
    PmCheatOps ops={w,attach,detach,identity,deadline,module,range,readbytes,writebytes,journal,healthy};
    if(w->inspect){
      ops.range=readable;
      if(readBuffer)result=pm_memory_inspect(&ops,w->inspectOffset,w->inspectLength,readBuffer,&readBase,&readSize);
      else snprintf(result.reason,sizeof result.reason,"Could not allocate bounded inspection buffer");
    } else result=pm_cheat_apply(&w->plan,&ops);
  }
  w->record.phase=result.status==PM_CHEAT_APPLIED?APPLIED:result.status==PM_CHEAT_REJECTED?REJECTED:UNKNOWN;
  w->record.resumed=result.resumed;w->record.restored=result.restored;w->record.writes=result.writes;
  snprintf(w->record.message,sizeof w->record.message,"%s",result.reason);
  if(save(&w->record)){w->record.phase=UNKNOWN;snprintf(w->record.message,sizeof w->record.message,"Cheat outcome could not be saved; no replay is allowed for this game instance");}
  pthread_mutex_lock(&mutex);current=w->record;
  if(w->inspect&&w->record.phase==APPLIED){
    memcpy(inspected,readBuffer,w->inspectLength);inspectedLength=w->inspectLength;
    inspectedBase=readBase;inspectedSize=readSize;inspectedReady=1;
  }
  running=0;pm_native_release();pthread_mutex_unlock(&mutex);
  if(readBuffer){memset(readBuffer,0,PM_INSPECT_MAX_BYTES);free(readBuffer);}
  free(w->wire);free(w);return NULL;
}
static int same(const Receipt *a,const Receipt *b){return a->pid==b->pid&&!strcmp(a->request,b->request)&&!strcmp(a->generation,b->generation)&&
  !strcmp(a->title,b->title)&&!strcmp(a->metadataHash,b->metadataHash)&&!strcmp(a->module,b->module)&&!strcmp(a->planHash,b->planHash);}
static int start(Work *w){
  pthread_mutex_lock(&mutex);if(running||!pm_native_claim()){pthread_mutex_unlock(&mutex);errno=EBUSY;return -1;}
  int result=-1;Receipt previous;
  if(!load(w->record.request,&previous)){
    if(!same(&previous,&w->record)){errno=EEXIST;goto done;}
    if(strcmp(current.request,previous.request)){inspectedReady=0;memset(inspected,0,sizeof inspected);}
    current=previous;result=0;goto done;
  }
  if(errno!=ENOENT)goto done;
  int held=unresolved(w->record.generation);if(held){errno=held>0?EALREADY:EIO;goto done;}
  if(!identity(w)){errno=ESRCH;goto done;}
  if(save(&w->record))goto done;
  w->intentSaved=1;
  inspectedReady=0;memset(inspected,0,sizeof inspected);
  current=w->record;pthread_t thread;pthread_attr_t attr;int err=pthread_attr_init(&attr);
  if(!err){err=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);if(!err)err=pthread_create(&thread,&attr,worker,w);pthread_attr_destroy(&attr);}
  if(err){w->record.phase=REJECTED;w->record.resumed=w->record.restored=1;snprintf(w->record.message,sizeof w->record.message,"Cheat worker could not start");save(&w->record);current=w->record;errno=err;goto done;}
  running=1;result=1;
done:
  if(!running)pm_native_release();pthread_mutex_unlock(&mutex);return result;
}
static JSValue failure(JSContext *c,int error){
  JSValue e=JS_NewError(c);const char *code=error==EBUSY?"NATIVE_BUSY":error==EALREADY?"CHEAT_RECOVERY_REQUIRED":"CHEAT_NATIVE";
  JS_SetPropertyStr(c,e,"code",JS_NewString(c,code));JS_SetPropertyStr(c,e,"message",JS_NewString(c,
    error==EBUSY?"A native operation is in progress":error==EALREADY?"This game has an unresolved cheat operation; no new memory writes were started":"Native cheat operation could not be accepted"));return JS_Throw(c,e);
}
static JSValue start_js(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
  (void)self;if(argc!=7)return JS_ThrowTypeError(c,"Expected request, PID, generation, title, metadata hash, module and patch packet");
  Work *w=calloc(1,sizeof *w);if(!w)return JS_ThrowOutOfMemory(c);const char *s[7]={0};int32_t pid=0;size_t length=0;
  int valid=!JS_ToInt32(c,&pid,argv[1]);for(unsigned i=0;i<6;i++)if(i!=1){s[i]=JS_ToCString(c,argv[i]);if(!s[i])valid=0;}
  unsigned char *wire=JS_GetArrayBuffer(c,&length,argv[6]);
  valid=valid&&wire&&length>=16&&length<=16+PM_CHEAT_MAX_PATCHES*20+PM_CHEAT_MAX_BYTES*2&&hex(s[0],32)&&pid>1&&strlen(s[2])>0&&strlen(s[2])<128&&pm_game_title(s[3])&&hex(s[4],64)&&strlen(s[5])>0&&strlen(s[5])<128;
  if(valid){
    w->wire=malloc(length);if(w->wire){memcpy(w->wire,wire,length);w->wireSize=length;valid=!pm_cheat_plan(w->wire,length,&w->plan);}else valid=0;
  }
  int result=-1,saved=EINVAL;
  if(valid){
    Receipt *r=&w->record;r->magic=MAGIC;r->version=1;r->phase=QUEUED;r->pid=pid;r->resumed=r->restored=1;
    strcpy(r->request,s[0]);strcpy(r->generation,s[2]);strcpy(r->title,s[3]);strcpy(r->metadataHash,s[4]);strcpy(r->module,s[5]);hash(w->wire,length,r->planHash);
    snprintf(r->message,sizeof r->message,"Cheat transaction queued");result=start(w);saved=errno;
  }
  for(unsigned i=0;i<6;i++)if(s[i])JS_FreeCString(c,s[i]);
  if(result!=1){free(w->wire);free(w);}return result<0?failure(c,saved):JS_UNDEFINED;
}
static JSValue poll_js(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
  (void)self;if(argc!=1)return JS_ThrowTypeError(c,"Expected cheat operation request");const char *id=JS_ToCString(c,argv[0]);
  if(!id||!hex(id,32)){if(id)JS_FreeCString(c,id);return JS_ThrowTypeError(c,"Invalid cheat receipt identity");}
  Receipt r;pthread_mutex_lock(&mutex);int live=!strcmp(id,current.request),busy=live&&running;if(live)r=current;pthread_mutex_unlock(&mutex);
  if(!live&&load(id,&r)){JS_FreeCString(c,id);return JS_NULL;}JS_FreeCString(c,id);
  if(!busy&&(r.phase==QUEUED||r.phase==ARMED)){r.phase=UNKNOWN;r.restored=0;r.resumed=0;snprintf(r.message,sizeof r.message,"Cheat operation was interrupted; its existing attempt was retained");}
  JSValue out=JS_NewObject(c);
  JS_SetPropertyStr(c,out,"request",JS_NewString(c,r.request));JS_SetPropertyStr(c,out,"planHash",JS_NewString(c,r.planHash));
  JS_SetPropertyStr(c,out,"pid",JS_NewInt32(c,r.pid));JS_SetPropertyStr(c,out,"generation",JS_NewString(c,r.generation));
  JS_SetPropertyStr(c,out,"stage",JS_NewInt32(c,r.phase));JS_SetPropertyStr(c,out,"busy",JS_NewBool(c,busy));
  JS_SetPropertyStr(c,out,"resumed",JS_NewBool(c,r.resumed));JS_SetPropertyStr(c,out,"restored",JS_NewBool(c,r.restored));
  JS_SetPropertyStr(c,out,"writes",JS_NewUint32(c,r.writes));JS_SetPropertyStr(c,out,"message",JS_NewString(c,r.message));return out;
}
static JSValue inspect_start_js(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
  (void)self;if(argc!=7)return JS_ThrowTypeError(c,"Expected game identity, module and inspection packet");
  Work *w=calloc(1,sizeof *w);if(!w)return JS_ThrowOutOfMemory(c);
  const char *s[6]={0};int32_t pid=0;size_t length=0;int valid=!JS_ToInt32(c,&pid,argv[1]);
  for(unsigned i=0;i<6;i++)if(i!=1){s[i]=JS_ToCString(c,argv[i]);if(!s[i])valid=0;}
  unsigned char *wire=JS_GetArrayBuffer(c,&length,argv[6]);
  valid=valid&&wire&&length==16&&hex(s[0],32)&&pid>1&&strlen(s[2])>0&&strlen(s[2])<128&&
    pm_game_title(s[3])&&hex(s[4],64)&&strlen(s[5])>0&&strlen(s[5])<128;
  uint64_t offset=0;uint32_t size=0,magic=0;
  if(valid){memcpy(&offset,wire,8);memcpy(&size,wire+8,4);memcpy(&magic,wire+12,4);
    valid=magic==0x544d5231&&size<=PM_INSPECT_MAX_BYTES&&(size||!offset);}
  int result=-1,saved=EINVAL;
  if(valid){
    Receipt *r=&w->record;r->magic=MAGIC;r->version=1;r->phase=QUEUED;r->pid=pid;r->resumed=r->restored=1;
    strcpy(r->request,s[0]);strcpy(r->generation,s[2]);strcpy(r->title,s[3]);strcpy(r->metadataHash,s[4]);strcpy(r->module,s[5]);hash(wire,length,r->planHash);
    w->inspect=1;w->inspectOffset=offset;w->inspectLength=size;
    snprintf(r->message,sizeof r->message,"Memory inspection queued");result=start(w);saved=errno;
  }
  for(unsigned i=0;i<6;i++)if(s[i])JS_FreeCString(c,s[i]);if(result!=1)free(w);
  return result<0?failure(c,saved):JS_UNDEFINED;
}
static JSValue inspect_poll_js(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
  JSValue out=poll_js(c,self,argc,argv);if(JS_IsException(out)||JS_IsNull(out))return out;
  const char *id=JS_ToCString(c,argv[0]);if(!id){JS_FreeValue(c,out);return JS_EXCEPTION;}
  pthread_mutex_lock(&mutex);
  if(!running&&inspectedReady&&!strcmp(id,current.request)&&current.phase==APPLIED){
    char base[24],size[24];snprintf(base,sizeof base,"0x%llx",(unsigned long long)inspectedBase);snprintf(size,sizeof size,"0x%llx",(unsigned long long)inspectedSize);
    JS_SetPropertyStr(c,out,"base",JS_NewString(c,base));JS_SetPropertyStr(c,out,"size",JS_NewString(c,size));
    JS_SetPropertyStr(c,out,"bytes",JS_NewArrayBufferCopy(c,inspected,inspectedLength));
  }
  pthread_mutex_unlock(&mutex);JS_FreeCString(c,id);return out;
}
void pm_cheat_bind(JSContext *c,JSValue api){
  JS_SetPropertyStr(c,api,"cheatStart",JS_NewCFunction(c,start_js,"cheatStart",7));
  JS_SetPropertyStr(c,api,"cheatPoll",JS_NewCFunction(c,poll_js,"cheatPoll",1));
  JS_SetPropertyStr(c,api,"inspectStart",JS_NewCFunction(c,inspect_start_js,"inspectStart",7));
  JS_SetPropertyStr(c,api,"inspectPoll",JS_NewCFunction(c,inspect_poll_js,"inspectPoll",1));
}
