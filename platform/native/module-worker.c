// SPDX-License-Identifier: GPL-3.0-or-later
// A PRX load is tied to a durable game-instance receipt. Never infer successful
// module initialization merely from hook publication or a positive handle.
#include "quickjs.h"
#include "module-session.h"
#include "pad-worker.h"
#include "pad-bridge.h"
#include "prx-image.h"
#include "game-inject.h"
#include "game-source.h"
#include "game-info.h"
#include "native-operation.h"
#include "process-current.h"
#include "kstuff-probe.h"
#include "sha256.h"
#include "pt.h"
#include <pthread.h>
#include "module-thread.h"
#include "module-storage.h"
#include "goldhen-worker.h"
#include "goldhen-compat.h"
#include <sys/mman.h>
#include <ps5/kernel.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ROOT "/data/ProsperoMenu/modules"
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static PmModuleSession current;static unsigned currentSlot;static int running;
int pm_module_live_session(int pid,const char *generation,const char *metadata,PmModuleSession *out){
  pthread_mutex_lock(&mutex);
  int valid=!running&&current.pid==pid&&!strcmp(current.generation,generation)&&!strcmp(current.metadataHash,metadata)&&
    current.phase==PM_ARENA_READY&&current.arenaStage==2&&!pm_module_session_held(&current);
  if(valid)*out=current;pthread_mutex_unlock(&mutex);return valid?0:-1;
}
static void sha_hex(const void *bytes,size_t length,char out[65]){
  SHA256_CTX h;unsigned char digest[32];sha256_init(&h);sha256_update(&h,bytes,length);sha256_final(&h,digest);
  for(unsigned i=0;i<32;i++)snprintf(out+i*2,3,"%02x",digest[i]);
}
static int directory(void){
  if(mkdir(ROOT,0700)&&errno!=EEXIST)return -1;
  return open(ROOT,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
}
static void receipt_name(const char *generation,char name[96]){char hash[65];sha_hex(generation,strlen(generation),hash);snprintf(name,96,"%s.session",hash);}
static int transfer(int fd,void *buffer,size_t size,int writing){
  unsigned char *p=buffer;while(size){ssize_t n=writing?write(fd,p,size):read(fd,p,size);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;p+=n;size-=(size_t)n;}return 0;
}
static int load(const char *generation,PmModuleSession *s){
  char name[96];receipt_name(generation,name);int dir=directory();if(dir<0)return -1;
  int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);int saved=errno;close(dir);errno=saved;if(fd<0)return -1;
  PmModuleSession raw;struct stat st;int bad=fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size!=sizeof raw||transfer(fd,&raw,sizeof raw,0);close(fd);
  if(bad||pm_module_decode(&raw,sizeof raw,generation,s)){errno=EINVAL;return -1;}return 0;
}
static int save(PmModuleSession *s){
  pm_module_seal(s);char name[96],temp[104];receipt_name(s->generation,name);snprintf(temp,sizeof temp,"%s.tmp",name);
  int dir=directory();if(dir<0)return -1;int fd=openat(dir,temp,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
  if(fd<0){close(dir);return -1;}int bad=transfer(fd,s,sizeof *s,1)||fsync(fd);if(close(fd))bad=1;
  if(!bad)bad=renameat(dir,temp,dir,name)||fsync(dir);close(dir);return bad?-1:0;
}
static void publish(const PmModuleSession *s){pthread_mutex_lock(&mutex);current=*s;pthread_mutex_unlock(&mutex);}
static int transition(PmModuleSession *s,unsigned slot,unsigned stage,int clean,const char *message){
  PmModuleReceipt *r=&s->slots[slot];r->stage=stage;r->clean=clean;snprintf(r->message,sizeof r->message,"%s",message);
  int result=save(s);
  if(result){r->stage=PM_PRX_UNKNOWN;r->clean=0;snprintf(r->message,sizeof r->message,"Module operation receipt could not be saved; no retry was issued");}
  printf("ProsperoMenu game module: title=%s pid=%d stage=%u clean=%u handle=0x%08x startResult=0x%08x message=%s\n",
    s->title,s->pid,r->stage,r->clean,(unsigned)r->handle,(unsigned)r->startResult,r->message);fflush(stdout);
  publish(s);return result;
}
static int identity(void *arg){PmModuleSession *s=arg;return pm_game_identity(s->pid,s->generation,s->title);}
static int metadata_matches(PmModuleSession *s,char *reason){
  unsigned char *bytes=NULL;size_t size=0;
  if(pm_game_metadata_read(s->pid,s->generation,s->title,&bytes,&size,reason,192))return 0;
  char hash[65];sha_hex(bytes,size,hash);free(bytes);
  if(strcmp(hash,s->metadataHash)){snprintf(reason,192,"Mounted game version changed before the native operation");return 0;}return 1;
}
// Read the process parameter through the kernel API, without calling game
// functions on a suspended thread or changing its SDK/credentials.
static uint32_t host_sdk(PmModuleSession *s,int *attached){
  uint32_t sdk=0;intptr_t scratch=0;uint64_t address=0,length=0;unsigned char param[32];
  if(!identity(s)||pt_attach_owned(s->pid,attached))return 0;
  if(!identity(s))goto done;
  scratch=pt_mmap(s->pid,0,0x4000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(scratch<=0){scratch=0;goto done;}
  if(pt_syscall(s->pid,598,scratch,scratch+8,0,0,0,0)||pt_copyout(s->pid,scratch,&address,8)||
    pt_copyout(s->pid,scratch+8,&length,8)||address<0x10000||address>UINT64_C(0x7fffffffffff)-32||length<32||length>4096||
    pt_copyout(s->pid,address,param,sizeof param))goto done;
  if(identity(s))sdk=pm_ps5_process_sdk(param,sizeof param);
done:
  if(scratch)pt_munmap(s->pid,scratch,0x4000);
  if(*attached&&!pt_detach(s->pid,0))*attached=0;
  return sdk;
}
static int sealed_file(PmModuleSession *s,PmModuleReceipt *r,int platform,uint32_t sdk,int *attached,char *reason){
  int inbox=open("/data/ProsperoMenu/inbox",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(inbox<0){snprintf(reason,192,"Cannot open plugin storage (errno %d)",errno);return -1;}
  char name[64];snprintf(name,sizeof name,"%s.payload",r->upload);int fd=openat(inbox,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);close(inbox);
  if(fd<0){snprintf(reason,192,"Cannot open the registered plugin file (errno %d)",errno);return -1;}struct stat st;unsigned char *bytes=NULL;int bad=1;
  if(fstat(fd,&st)){snprintf(reason,192,"Cannot inspect the registered plugin file (errno %d)",errno);goto done;}
  if(!S_ISREG(st.st_mode)||st.st_size<64||st.st_size>128*1024*1024){snprintf(reason,192,"Registered plugin has an invalid file type or size");goto done;}
  size_t size=(size_t)st.st_size;bytes=malloc(size);
  if(!bytes){snprintf(reason,192,"Not enough memory to verify the plugin (%zu bytes)",size);goto done;}
  if(transfer(fd,bytes,size,0)){snprintf(reason,192,"Could not read the complete plugin file (errno %d)",errno);goto done;}
  char hash[65];sha_hex(bytes,size,hash);if(strcmp(hash,r->sha256)){snprintf(reason,192,"Module changed after inbox verification");goto done;}
  int actual;if(pm_prx_inspect(bytes,size,&actual,reason,192))goto done;
  if(actual&&actual!=platform){snprintf(reason,192,"Module container targets a different console platform than this game");goto done;}
  uint32_t originalSdk=0;int adapted=platform==5?pm_prx_match_host_sdk(bytes,size,sdk,kernel_get_fw_version(),&originalSdk):0;
  if(adapted<0){snprintf(reason,192,"Development module SDK metadata is malformed");goto done;}
  if(pm_goldhen_supported(r->sha256)){
    if(platform!=4){snprintf(reason,192,"This Havana file targets PS4 RDR2 1.32");goto done;}
    if(pm_goldhen_prepare(s,bytes,size,attached,reason))goto done;
  }
  char stagedHash[65];sha_hex(bytes,size,stagedHash);
  printf("ProsperoMenu game module SDK: host=0x%08x module=0x%08x adapted=%d source=%s staged=%s originalUploadPreserved=1\n",sdk,originalSdk,adapted,r->sha256,stagedHash);fflush(stdout);
  // The backing copy is isolated by SDK and never modifies the verified upload.
  // The console still checks imports and runs the module's normal initializer.
  int dir=pm_module_storage_open(s->pid,s->generation,s->title,platform,sdk,reason);if(dir<0)goto done;
  char final[80],temp[80],modulePath[256];pm_module_path(r,modulePath);snprintf(final,sizeof final,"%s",modulePath+strlen(PM_GAME_MODULE_PREFIX));snprintf(temp,sizeof temp,"%s.part",r->request);
  int existing=openat(dir,final,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
  if(existing>=0){
    struct stat old;bad=fstat(existing,&old)||!S_ISREG(old.st_mode)||old.st_size!=st.st_size||(old.st_mode&0222);
    unsigned char chunk[4096];size_t offset=0;
    while(!bad&&offset<size){size_t count=size-offset;if(count>sizeof chunk)count=sizeof chunk;bad=transfer(existing,chunk,count,0)||memcmp(chunk,bytes+offset,count);offset+=count;}
    // Repair execute permissions only after comparing the complete expected
    // staged copy, including its explicit host-SDK adaptation if applicable.
    if(!bad)bad=fchmod(existing,0555)||fsync(existing);
    if(bad)snprintf(reason,192,"Existing staged plugin differs from its verified file or cannot be made executable (errno %d)",errno);
    close(existing);close(dir);goto done;
  }
  if(errno!=ENOENT){snprintf(reason,192,"Cannot inspect the staged plugin (errno %d)",errno);close(dir);goto done;}
  int out=openat(dir,temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(out<0){snprintf(reason,192,"Cannot create the game's staged plugin (errno %d)",errno);close(dir);goto done;}
  bad=transfer(out,bytes,size,1)||fchmod(out,0555)||fsync(out);if(close(out))bad=1;
  if(!bad)bad=renameat(dir,temp,dir,final)||fsync(dir);
  if(bad){snprintf(reason,192,"Could not write or finalize the staged plugin (errno %d)",errno);unlinkat(dir,temp,0);}close(dir);
done:
  close(fd);free(bytes);return bad?-1:0;
}
static int read_gate(void *arg,PmGameGate *g){PmModuleSession *s=arg;return !identity(s)||!pm_process_current(s->pid)?-1:pt_copyout(s->pid,s->control,g,sizeof *g);}
static int write_gate(void *arg,size_t offset,const void *bytes,size_t size){
  PmModuleSession *s=arg;if(offset>sizeof(PmGameGate)||size>sizeof(PmGameGate)-offset||!identity(s)||!pm_process_current(s->pid))return -1;
  return pt_copyin(s->pid,bytes,s->control+offset,size);
}
static void *worker(void *ignored){
  (void)ignored;PmModuleSession s;unsigned slot;pthread_mutex_lock(&mutex);s=current;slot=currentSlot;pthread_mutex_unlock(&mutex);
  PmModuleReceipt *r=&s.slots[slot];char reason[192]="Module file could not be prepared";int attached=0;
  int pendingStage=0;const char *pendingMessage=NULL;
  int platform=!memcmp(s.title,"PPS",3)||!memcmp(s.title,"MOUU",4)?5:4;
  uint32_t sdk=platform==5?host_sdk(&s,&attached):0;
  if(attached){pendingStage=PM_PRX_UNKNOWN;pendingMessage="Game resume was not confirmed after SDK inspection";goto done;}
  int fileResult=sealed_file(&s,r,platform,sdk,&attached,reason);
  if(attached){pendingStage=PM_PRX_UNKNOWN;pendingMessage="Game resume was not confirmed after compatibility preparation";goto done;}
  if(fileResult||!metadata_matches(&s,reason))goto rejected;
  const SnipersFirmwareProfile *profile=snipers_firmware_profile(kernel_get_fw_version());uint64_t base=KERNEL_ADDRESS_DATA_BASE;
  PmKstuffSnapshot a=pm_kstuff_read(base,profile),b=pm_kstuff_read(base,profile);
  if(!profile||!pm_kstuff_snapshot_equal(&a,&b)||!pm_kstuff_injection_ready(base,&b)){
    snprintf(reason,sizeof reason,"Kernel hook state is not ready for a game module");goto rejected;
  }
  if(s.phase==PM_ARENA_EMPTY){
    arc4random_buf(&s.cookie,sizeof s.cookie);if(!s.cookie)s.cookie=1;s.phase=PM_ARENA_INTENT;
    if(transition(&s,slot,PM_PRX_ARMING,0,"Preparing a verified game callback"))goto done;
    PmGameInjection injected;int result=pm_game_inject(s.pid,s.cookie,0,identity,&s,&injected);
    s.arena=injected.arena;s.control=injected.control;
    if(!injected.resumed){
      s.phase=PM_ARENA_UNKNOWN;r->stage=PM_PRX_UNKNOWN;r->clean=0;
      snprintf(r->message,sizeof r->message,"Game callback resume is unconfirmed; original intent retained");publish(&s);goto done;
    }
    if(result){s.phase=result<0?PM_ARENA_UNKNOWN:PM_ARENA_EMPTY;
      transition(&s,slot,result<0?PM_PRX_UNKNOWN:PM_PRX_REJECTED,result>0,injected.message);goto done;}
    s.phase=PM_ARENA_READY;if(transition(&s,slot,PM_PRX_ARMING,0,"Game callback ready; preparing module slot"))goto done;
  }else if(s.phase!=PM_ARENA_READY){snprintf(reason,sizeof reason,"Previous game callback ownership is unresolved");goto rejected;}
  if(!metadata_matches(&s,reason))goto rejected;
  if(transition(&s,slot,PM_PRX_ARMING,0,"Publishing the module slot in the verified game"))goto done;
  PmModuleOps ops={&s,read_gate,write_gate};int staged=2;
  for(unsigned attempt=0;attempt<40;attempt++){
    if(!identity(&s)){snprintf(reason,sizeof reason,"Game ended while waiting for its module callback");goto rejected;}
    if(pt_attach_owned(s.pid,&attached)){pendingStage=PM_PRX_UNKNOWN;pendingMessage="Could not acquire a confirmed stopped game lease";goto done;}
    staged=pm_module_stage(&s,slot,&ops);
    if(pt_detach(s.pid,0)){s.phase=PM_ARENA_UNKNOWN;pendingStage=PM_PRX_UNKNOWN;pendingMessage="Game resume was not confirmed; no retry was issued";goto done;}attached=0;
    if(staged!=2)break;
    // Retry only a proven untouched slot. The game must run between samples
    // so its controller callback (or another initializer) can release busy.
    if(!attempt){printf("ProsperoMenu game module: callback busy; waiting with game resumed\n");fflush(stdout);}
    usleep(25000);
  }
  if(staged<0){s.phase=PM_ARENA_UNKNOWN;transition(&s,slot,PM_PRX_UNKNOWN,0,"Module slot rollback could not be confirmed");}
  else if(staged>0)transition(&s,slot,PM_PRX_REJECTED,1,staged==2?
    "Game callback is still busy; module was not loaded. Wait for the game to finish loading, then try again.":
    "Module slot was not published; game callback ownership changed");
  else transition(&s,slot,PM_PRX_ARMED,0,"Module staged; waiting for the game's module initializer");
  goto done;
rejected:
  transition(&s,slot,PM_PRX_REJECTED,1,reason);
done:
  // Retry only the resume operation if the first detach failed, never inject or
  // run a module again. An uncertain durable receipt remains uncertain.
  if(attached&&!pt_detach(s.pid,0))attached=0;
  if(pendingStage){
    if(!attached)transition(&s,slot,pendingStage,0,pendingMessage);
    else {s.phase=PM_ARENA_UNKNOWN;r->stage=PM_PRX_UNKNOWN;r->clean=0;
      snprintf(r->message,sizeof r->message,"Game resume is unconfirmed; original module intent retained");publish(&s);}
  }
  pthread_mutex_lock(&mutex);running=0;pm_native_release();pthread_mutex_unlock(&mutex);return NULL;
}
static int start(const char *request,const char *upload,const char *hash,int pid,const char *generation,const char *title,const char *metadata,const char *extension){
  if(!pm_hex_string(request,32)||!pm_hex_string(upload,32)||!pm_hex_string(hash,64)||!pm_hex_string(metadata,64)||pid<=1||
    !generation||!*generation||strlen(generation)>=128||!title||!pm_game_title(title)||!extension||(strcmp(extension,"prx")&&strcmp(extension,"sprx"))){errno=EINVAL;return -1;}
  pthread_mutex_lock(&mutex);if(running||!pm_native_claim()){pthread_mutex_unlock(&mutex);errno=EBUSY;return -1;}
  int result=-1;PmModuleSession s;int loaded=load(generation,&s);
  if(loaded&&errno!=ENOENT)goto done;
  if(loaded){memset(&s,0,sizeof s);s.magic=PM_MODULE_SESSION_MAGIC;s.version=PM_MODULE_SESSION_VERSION;s.pid=pid;strcpy(s.generation,generation);strcpy(s.title,title);strcpy(s.metadataHash,metadata);}
  if(s.pid!=pid||strcmp(s.title,title)||strcmp(s.metadataHash,metadata)){errno=EINVAL;goto done;}
  int slot=pm_module_slot(&s,request);
  if(slot>=0){PmModuleReceipt *r=&s.slots[slot];if(strcmp(r->upload,upload)||strcmp(r->sha256,hash)||strcmp(r->extension,extension)){errno=EEXIST;goto done;}
    current=s;currentSlot=(unsigned)slot;result=0;goto done;}
  if(pm_module_session_held(&s)){errno=EALREADY;goto done;}
  for(unsigned i=0;i<PM_GAME_MODULE_SLOTS;i++)if(s.slots[i].stage&&!(s.slots[i].stage==PM_PRX_REJECTED&&s.slots[i].clean)&&!strcmp(s.slots[i].sha256,hash)){errno=EALREADY;goto done;}
  for(unsigned i=0;i<PM_GAME_MODULE_SLOTS;i++)if(!s.slots[i].stage){slot=(int)i;break;}
  if(slot<0){errno=ENOSPC;goto done;}
  if(!identity(&s)){errno=ESRCH;goto done;}
  PmModuleReceipt *r=&s.slots[slot];strcpy(r->request,request);strcpy(r->upload,upload);strcpy(r->sha256,hash);strcpy(r->extension,extension);
  r->stage=PM_PRX_QUEUED;r->clean=1;if(save(&s))goto done;
  current=s;currentSlot=(unsigned)slot;running=1;pthread_t thread;int err=pm_module_thread_start(&thread,worker,NULL);
  if(err){running=0;r->stage=PM_PRX_REJECTED;snprintf(r->message,sizeof r->message,"Module worker could not start");save(&s);current=s;errno=err;goto done;}
  result=0;
done:
  if(!running)pm_native_release();pthread_mutex_unlock(&mutex);return result;
}
static JSValue failure(JSContext *c){
  const char *code=errno==EBUSY?"NATIVE_BUSY":errno==EALREADY?"MODULE_ALREADY_ATTEMPTED":errno==ENOSPC?"MODULE_LIMIT":"MODULE_NATIVE";
  JSValue e=JS_NewError(c);JS_SetPropertyStr(c,e,"code",JS_NewString(c,code));JS_SetPropertyStr(c,e,"message",JS_NewString(c,
    errno==EBUSY?"Native operation in progress":errno==ENOSPC?"This game instance has used its eight module slots; restart the game to load more":
    errno==EALREADY?"This module or game callback has an existing or unresolved attempt":"Native game module operation could not be completed"));return JS_Throw(c,e);
}
static JSValue start_js(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
  (void)self;if(argc!=8)return JS_ThrowTypeError(c,"Expected request, upload, hash, PID, generation, title, metadata hash and module extension");
  const char *v[8]={0};int32_t pid=0;int valid=!JS_ToInt32(c,&pid,argv[3]);
  for(unsigned i=0;i<8;i++)if(i!=3){v[i]=JS_ToCString(c,argv[i]);if(!v[i])valid=0;}
  int result=valid?start(v[0],v[1],v[2],pid,v[4],v[5],v[6],v[7]):-1,saved=errno;
  for(unsigned i=0;i<8;i++)if(v[i])JS_FreeCString(c,v[i]);errno=saved;return result?failure(c):JS_UNDEFINED;
}
static void inspect(PmModuleSession *s,unsigned slot){
  PmModuleReceipt *r=&s->slots[slot];
  if(r->stage!=PM_PRX_ARMED&&r->stage!=PM_PRX_UNKNOWN&&r->stage!=PM_PRX_ARMING)return;
  if(!identity(s)||!pm_process_current(s->pid))return;
  PmGameGate a,b;
  if(kernel_proc_copyout(s->pid,s->control,&a,sizeof a)||kernel_proc_copyout(s->pid,s->control,&b,sizeof b)||
    !identity(s)||!pm_module_gate_valid(s,&a)||!pm_module_gate_valid(s,&b))return;
  PmGameModule *m=&b.modules[slot];char expected[256];pm_module_path(r,expected);
  if(memcmp(&a.modules[slot],m,sizeof *m)||m->path[255]||strcmp(m->path,expected)||!m->enabled)return;
  if(m->state==PM_MODULE_STARTING){
    const char *phase=m->phase==PM_MODULE_PHASE_THREAD?"Waiting for game module worker":
      m->phase==PM_MODULE_PHASE_LOAD?"Running module_start on game worker":
      m->phase==PM_MODULE_PHASE_EXPORTS?"Checking optional plugin entry points":
      m->phase==PM_MODULE_PHASE_ACTIVATE?"Running plugin_load on game worker":"Game module worker is finishing";
    if(strcmp(r->message,phase)){
      transition(s,slot,PM_PRX_ARMED,0,phase);
      printf("ProsperoMenu game module progress: phase=%u thread=0x%08x handle=0x%08x start=0x%08x activation=%u\n",
        m->phase,(unsigned)m->threadResult,(unsigned)m->handle,(unsigned)m->startResult,m->activation);fflush(stdout);
    }
  }
  if(m->state==PM_MODULE_STARTED||m->state==PM_MODULE_FAILED||m->state==PM_MODULE_REJECTED){
    r->handle=m->handle;r->startResult=m->startResult;
    if(m->state==PM_MODULE_STARTED&&m->handle>=0&&m->startResult>=0)
      transition(s,slot,PM_PRX_STARTED,0,m->activation?"Module initializer and plugin_load completed successfully":"Module initializer completed; no paired plugin activation exports");
    else {
      char message[192];
      if(m->state==PM_MODULE_REJECTED){
        if(m->phase==PM_MODULE_PHASE_THREAD)snprintf(message,sizeof message,"Game worker could not start (0x%08x); module was not loaded.",(unsigned)m->threadResult);
        else snprintf(message,sizeof message,"Game worker rejected the staged module path; module was not loaded.");
        transition(s,slot,PM_PRX_REJECTED,1,message);
      }else {
        if(m->activation)snprintf(message,sizeof message,"plugin_load failed: 0x%08x (handle %d); close this game before retrying.",(unsigned)m->activationResult,m->handle);
        else pm_module_failure_message(m->handle,m->startResult,message,sizeof message);
        transition(s,slot,PM_PRX_FAILED,0,message);
      }
    }
    printf("ProsperoMenu game module: stage=%u phase=%u thread=0x%08x detach=0x%08x handle=0x%08x startResult=0x%08x activation=%u activationResult=0x%08x loadSymbol=0x%08x unloadSymbol=0x%08x\n",
      r->stage,m->phase,(unsigned)m->threadResult,(unsigned)m->detachResult,(unsigned)r->handle,(unsigned)r->startResult,
      m->activation,(unsigned)m->activationResult,(unsigned)m->loadSymbolResult,(unsigned)m->unloadSymbolResult);fflush(stdout);
  }
}
static JSValue poll_js(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
  (void)self;if(argc!=3)return JS_ThrowTypeError(c,"Expected request, PID and game generation");
  const char *request=JS_ToCString(c,argv[0]),*generation=JS_ToCString(c,argv[2]);int32_t pid=0;
  if(!request||!generation||!pm_hex_string(request,32)||strlen(generation)>=128||JS_ToInt32(c,&pid,argv[1])){
    if(request)JS_FreeCString(c,request);if(generation)JS_FreeCString(c,generation);return JS_ThrowTypeError(c,"Invalid module receipt identity");
  }
  PmModuleSession s;pthread_mutex_lock(&mutex);int live=running,found=!strcmp(current.generation,generation)&&pm_module_slot(&current,request)>=0;if(found)s=current;pthread_mutex_unlock(&mutex);
  if(!found&&(load(generation,&s)||pm_module_slot(&s,request)<0)){JS_FreeCString(c,request);JS_FreeCString(c,generation);return JS_NULL;}
  int slot=pm_module_slot(&s,request);JS_FreeCString(c,request);JS_FreeCString(c,generation);if(s.pid!=pid){errno=EINVAL;return failure(c);}
  int busy=live||pm_native_busy();
  if(!busy&&pm_native_claim()){inspect(&s,(unsigned)slot);pm_native_release();}
  PmModuleReceipt r=s.slots[slot];if(!live&&(r.stage==PM_PRX_QUEUED||r.stage==PM_PRX_ARMING)){r.stage=PM_PRX_UNKNOWN;r.clean=0;snprintf(r.message,sizeof r.message,"Previous module attempt was interrupted; no replay was issued");}
  JSValue out=JS_NewObject(c);
  JS_SetPropertyStr(c,out,"request",JS_NewString(c,r.request));JS_SetPropertyStr(c,out,"sha256",JS_NewString(c,r.sha256));
  JS_SetPropertyStr(c,out,"generation",JS_NewString(c,s.generation));JS_SetPropertyStr(c,out,"pid",JS_NewInt32(c,s.pid));
  JS_SetPropertyStr(c,out,"stage",JS_NewInt32(c,r.stage));JS_SetPropertyStr(c,out,"busy",JS_NewBool(c,busy));
  JS_SetPropertyStr(c,out,"handle",JS_NewInt32(c,r.handle));JS_SetPropertyStr(c,out,"startResult",JS_NewInt32(c,r.startResult));
  JS_SetPropertyStr(c,out,"noSideEffects",JS_NewBool(c,r.clean));JS_SetPropertyStr(c,out,"message",JS_NewString(c,r.message));return out;
}
// Arena preparation has its own durable request but shares the game session,
// native guard and hook owner with PRX loading. It consumes no module slot.
static int arena_outcome(PmModuleSession *s,unsigned stage,const char *message,int durable){
  s->arenaStage=stage;snprintf(s->arenaMessage,sizeof s->arenaMessage,"%s",message);
  int bad=durable?save(s):0;
  if(bad){s->arenaStage=4;snprintf(s->arenaMessage,sizeof s->arenaMessage,"Game input outcome could not be saved; original intent retained");}
  publish(s);return bad?-1:0;
}
static void *arena_worker(void *ignored){
  (void)ignored;PmModuleSession s;pthread_mutex_lock(&mutex);s=current;pthread_mutex_unlock(&mutex);
  char reason[192]="Game input dependencies changed before preparation";
  const SnipersFirmwareProfile *profile=snipers_firmware_profile(kernel_get_fw_version());uint64_t base=KERNEL_ADDRESS_DATA_BASE;
  PmKstuffSnapshot a=pm_kstuff_read(base,profile),b=pm_kstuff_read(base,profile);
  if(!profile||!pm_kstuff_snapshot_equal(&a,&b)||!pm_kstuff_injection_ready(base,&b)||!identity(&s)||!metadata_matches(&s,reason)){
    arena_outcome(&s,3,reason,1);goto done;
  }
  if(s.phase==PM_ARENA_READY){
    PmPadBridge pad={0};
    int compatible=!kernel_proc_copyout(s.pid,s.arena+0x5000,&pad,sizeof pad)&&identity(&s)&&
      pad.magic==PM_PAD_MAGIC&&pad.cookie==s.cookie&&pad.version==1&&pad.bytes==sizeof pad&&
      pad.moduleEntry==s.arena&&pad.moduleControl==s.control;
    arena_outcome(&s,compatible?2:3,compatible?"Existing game callback adopted; input capture remains disabled":
      "This game uses the independent plugin loader. In-game menu preparation was not applied.",1);goto done;
  }
  arc4random_buf(&s.cookie,sizeof s.cookie);if(!s.cookie)s.cookie=1;s.phase=PM_ARENA_INTENT;
  if(arena_outcome(&s,1,"Preparing the shared game input callback",1))goto done;
  PmGameInjection injected;int result=pm_game_inject(s.pid,s.cookie,1,identity,&s,&injected);
  s.arena=injected.arena;s.control=injected.control;
  s.phase=result<0?PM_ARENA_UNKNOWN:result>0?PM_ARENA_EMPTY:PM_ARENA_READY;
  if(!injected.resumed){s.phase=PM_ARENA_UNKNOWN;arena_outcome(&s,4,"Game resume is unconfirmed; original input intent retained",0);}
  else arena_outcome(&s,result<0?4:result>0?3:2,injected.message,1);
done:
  pthread_mutex_lock(&mutex);running=0;pm_native_release();pthread_mutex_unlock(&mutex);return NULL;
}
static int arena_start(const char *request,int pid,const char *generation,const char *title,const char *metadata){
  if(!pm_hex_string(request,32)||pid<=1||!generation||!*generation||strlen(generation)>=128||!pm_game_title(title)||!pm_hex_string(metadata,64)){errno=EINVAL;return -1;}
  pthread_mutex_lock(&mutex);if(running||!pm_native_claim()){pthread_mutex_unlock(&mutex);errno=EBUSY;return -1;}
  int result=-1;PmModuleSession s;int missing=load(generation,&s);
  if(missing&&errno!=ENOENT)goto done;
  if(missing){memset(&s,0,sizeof s);s.magic=PM_MODULE_SESSION_MAGIC;s.version=PM_MODULE_SESSION_VERSION;s.pid=pid;
    strcpy(s.generation,generation);strcpy(s.title,title);strcpy(s.metadataHash,metadata);}
  if(s.pid!=pid||strcmp(s.title,title)||strcmp(s.metadataHash,metadata)){errno=EINVAL;goto done;}
  int claim=pm_module_arena_claim(&s,request);
  if(claim<0){errno=EALREADY;goto done;}
  if(claim>0){current=s;result=0;goto done;}
  if(!identity(&s)){errno=ESRCH;goto done;}
  if(save(&s))goto done;
  current=s;running=1;pthread_t thread;int err=pm_module_thread_start(&thread,arena_worker,NULL);
  if(err){running=0;s.arenaStage=3;snprintf(s.arenaMessage,sizeof s.arenaMessage,"Game input worker could not start");
    if(save(&s)){s.arenaStage=4;snprintf(s.arenaMessage,sizeof s.arenaMessage,"Worker startup receipt is unconfirmed");}
    current=s;errno=err;goto done;
  }
  result=0;
done:
  if(!running)pm_native_release();pthread_mutex_unlock(&mutex);return result;
}
static JSValue arena_start_js(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
  (void)self;if(argc!=5)return JS_ThrowTypeError(c,"Expected request, PID, generation, title and metadata hash");
  const char *v[5]={0};int32_t pid=0;int valid=!JS_ToInt32(c,&pid,argv[1]);
  for(unsigned i=0;i<5;i++)if(i!=1){v[i]=JS_ToCString(c,argv[i]);if(!v[i])valid=0;}
  int result=valid?arena_start(v[0],pid,v[2],v[3],v[4]):-1,saved=errno;
  for(unsigned i=0;i<5;i++)if(v[i])JS_FreeCString(c,v[i]);errno=saved;return result?failure(c):JS_UNDEFINED;
}
static JSValue arena_poll_js(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
  (void)self;if(argc!=3)return JS_ThrowTypeError(c,"Expected input request, PID and generation");
  const char *request=JS_ToCString(c,argv[0]),*generation=JS_ToCString(c,argv[2]);int32_t pid=0;
  if(!pm_hex_string(request,32)||!generation||!*generation||strlen(generation)>=128||JS_ToInt32(c,&pid,argv[1])||pid<=1){
    if(request)JS_FreeCString(c,request);if(generation)JS_FreeCString(c,generation);return JS_ThrowTypeError(c,"Invalid input receipt identity");
  }
  PmModuleSession s;pthread_mutex_lock(&mutex);int busy=running,found=!strcmp(current.generation,generation)&&!strcmp(current.arenaRequest,request);
  if(found)s=current;pthread_mutex_unlock(&mutex);
  if(!found){if(!load(generation,&s)&&!strcmp(s.arenaRequest,request))found=1;}
  JSValue out=JS_NULL;
  if(found&&s.pid==pid){
    unsigned stage=s.arenaStage;
    if(s.phase==PM_ARENA_UNKNOWN||(!busy&&stage==1))stage=4;
    out=JS_NewObject(c);JS_SetPropertyStr(c,out,"request",JS_NewString(c,request));JS_SetPropertyStr(c,out,"pid",JS_NewInt32(c,s.pid));
    JS_SetPropertyStr(c,out,"generation",JS_NewString(c,s.generation));JS_SetPropertyStr(c,out,"metadataHash",JS_NewString(c,s.metadataHash));
    JS_SetPropertyStr(c,out,"stage",JS_NewUint32(c,stage));JS_SetPropertyStr(c,out,"phase",JS_NewUint32(c,s.phase));
    JS_SetPropertyStr(c,out,"busy",JS_NewBool(c,busy));JS_SetPropertyStr(c,out,"captureEnabled",JS_FALSE);
    JS_SetPropertyStr(c,out,"message",JS_NewString(c,stage==4&&s.arenaStage==1?"Previous input preparation was interrupted; no replay":s.arenaMessage));
  }
  JS_FreeCString(c,request);JS_FreeCString(c,generation);return out;
}
void pm_module_bind(JSContext *c,JSValue api){
  pm_pad_worker_bind(c,api);
  JS_SetPropertyStr(c,api,"gameModuleStart",JS_NewCFunction(c,start_js,"gameModuleStart",8));
  JS_SetPropertyStr(c,api,"gameModulePoll",JS_NewCFunction(c,poll_js,"gameModulePoll",3));
  JS_SetPropertyStr(c,api,"gameInputStart",JS_NewCFunction(c,arena_start_js,"gameInputStart",5));
  JS_SetPropertyStr(c,api,"gameInputPoll",JS_NewCFunction(c,arena_poll_js,"gameInputPoll",3));
}
