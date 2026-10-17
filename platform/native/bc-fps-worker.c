// SPDX-License-Identifier: GPL-3.0-or-later
#include "quickjs.h"
#include "bc-fps.h"
#include "native-operation.h"
#include "game-source.h"
#include "game-info.h"
#include "kstuff-probe.h"
#include "sha256.h"
#include <pthread.h>
#include <sys/stat.h>
#include <ps5/kernel.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define ROOT "/data/ProsperoMenu/frame-counters"
#define MAGIC UINT64_C(0x5441434243465031)
enum {QUEUED=1,INSTALLED=2,REJECTED=3,UNKNOWN=4};
typedef struct {
 uint64_t magic;uint32_t version,stage;int pid;
 char request[33],generation[128],title[10],metadata[65];PmFpsInjection injection;unsigned char digest[32];
} Receipt;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;static Receipt current;static int running;
static void hash(const void *p,size_t n,unsigned char out[32]){SHA256_CTX h;sha256_init(&h);sha256_update(&h,p,n);sha256_final(&h,out);}
static void hexhash(const void *p,size_t n,char out[65]){unsigned char d[32];hash(p,n,d);for(unsigned i=0;i<32;i++)snprintf(out+2*i,3,"%02x",d[i]);}
static int hex(const char *s,size_t n){if(!s||strlen(s)!=n)return 0;for(size_t i=0;i<n;i++)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return 0;return 1;}
static int ps4(const char *title){return pm_game_title(title)&&strncmp(title,"PPS",3)&&strncmp(title,"MOUU",4);}
static int directory(void){if(mkdir(ROOT,0700)&&errno!=EEXIST)return -1;return open(ROOT,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);}
static void name(const char *generation,char out[80]){char h[65];hexhash(generation,strlen(generation),h);snprintf(out,80,"%s.receipt",h);}
static int transfer(int fd,void *buffer,size_t length,int writing){unsigned char *p=buffer;while(length){ssize_t n=writing?write(fd,p,length):read(fd,p,length);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;p+=n;length-=(size_t)n;}return 0;}
static int save(Receipt *r){
 memset(r->digest,0,32);unsigned char d[32];hash(r,sizeof *r,d);memcpy(r->digest,d,32);
 int dir=directory();if(dir<0)return -1;char file[80],temp[88];name(r->generation,file);snprintf(temp,sizeof temp,"%s.tmp",file);
 int fd=openat(dir,temp,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);if(fd<0){close(dir);return -1;}
 int bad=transfer(fd,r,sizeof *r,1)||fsync(fd);if(close(fd))bad=1;if(!bad)bad=renameat(dir,temp,dir,file)||fsync(dir);close(dir);return bad?-1:0;
}
static int decode(Receipt *r,const char *generation){
 if(r->magic!=MAGIC||r->version!=1||r->stage<QUEUED||r->stage>UNKNOWN||r->pid<=1||r->request[32]||r->generation[127]||r->title[9]||r->metadata[64]||r->injection.message[191]||
    !hex(r->request,32)||!hex(r->metadata,64)||strcmp(r->generation,generation)||!ps4(r->title))return -1;
 unsigned char expected[32],actual[32];memcpy(expected,r->digest,32);memset(r->digest,0,32);hash(r,sizeof *r,actual);memcpy(r->digest,expected,32);
 if(memcmp(expected,actual,32))return -1;
 if(r->stage==INSTALLED&&(!r->injection.installed||r->injection.uncertain||!r->injection.resumed||!r->injection.cookie||
    r->injection.arena<0x10000||r->injection.arena>UINT64_C(0x7fffffffffff)-0x8000||r->injection.control!=r->injection.arena+0x4000||
    r->injection.target<0x10000||r->injection.target>UINT64_C(0x7fffffffffff)-64))return -1;
 return 0;
}
static int load(const char *generation,Receipt *r){
 int dir=directory();if(dir<0)return -1;char file[80];name(generation,file);int fd=openat(dir,file,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);int saved=errno;close(dir);errno=saved;if(fd<0)return -1;
 struct stat st;int bad=fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size!=sizeof *r||transfer(fd,r,sizeof *r,0);close(fd);
 if(bad||decode(r,generation)){errno=EINVAL;return -1;}return 0;
}
static int identity(void *p){Receipt *r=p;return pm_game_identity(r->pid,r->generation,r->title);}
static int metadata(Receipt *r){unsigned char *bytes=NULL;size_t n=0;char reason[192],digest[65];
 if(pm_game_metadata_read(r->pid,r->generation,r->title,&bytes,&n,reason,sizeof reason))return 0;hexhash(bytes,n,digest);free(bytes);return !strcmp(digest,r->metadata);
}
static void *worker(void *p){
 Receipt *r=p;int rc=1;snprintf(r->injection.message,sizeof r->injection.message,"PS4 counter dependencies or game identity changed before installation");r->injection.resumed=1;
 const SnipersFirmwareProfile *profile=snipers_firmware_profile(kernel_get_fw_version());uint64_t base=KERNEL_ADDRESS_DATA_BASE;
 PmKstuffSnapshot a=pm_kstuff_read(base,profile),b=pm_kstuff_read(base,profile);
 if(profile&&pm_kstuff_snapshot_equal(&a,&b)&&pm_kstuff_injection_ready(base,&b)&&metadata(r)&&identity(r)){
  uint64_t cookie=0;arc4random_buf(&cookie,sizeof cookie);if(!cookie)cookie=1;
  rc=pm_fps_inject(r->pid,cookie,identity,r,&r->injection);
 }
 r->stage=rc<0?UNKNOWN:rc>0?REJECTED:INSTALLED;
 // Injection always returns after attempting resume; never fsync a receipt
 // while deliberately holding the game stopped.
 if(!r->injection.resumed||save(r)){r->stage=UNKNOWN;r->injection.uncertain=1;snprintf(r->injection.message,sizeof r->injection.message,"PS4 counter resume or durable outcome is unconfirmed; original intent retained");}
 pthread_mutex_lock(&mutex);current=*r;running=0;pm_native_release();pthread_mutex_unlock(&mutex);free(r);return NULL;
}
int pm_bc_fps_lookup(int pid,const char *generation,const char *title,PmFpsInjection *out){
 if(!generation||!title||!out)return -1;pthread_mutex_lock(&mutex);
 int ok=!running&&current.stage==INSTALLED&&current.pid==pid&&!strcmp(current.generation,generation)&&!strcmp(current.title,title);
 if(ok)*out=current.injection;pthread_mutex_unlock(&mutex);return ok?0:-1;
}
static JSValue error(JSContext *c,const char *code,const char *message){JSValue e=JS_NewError(c);JS_SetPropertyStr(c,e,"code",JS_NewString(c,code));JS_SetPropertyStr(c,e,"message",JS_NewString(c,message));return JS_Throw(c,e);}
static JSValue start_js(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
 (void)self;if(argc!=5)return JS_ThrowTypeError(c,"Expected PS4 counter request and exact game identity");
 int32_t pid=0;const char *id=JS_ToCString(c,argv[0]),*generation=JS_ToCString(c,argv[2]),*title=JS_ToCString(c,argv[3]),*meta=JS_ToCString(c,argv[4]);
 int valid=!JS_ToInt32(c,&pid,argv[1])&&pid>1&&hex(id,32)&&generation&&generation[0]&&strlen(generation)<128&&ps4(title)&&hex(meta,64);
 Receipt *r=calloc(1,sizeof *r);if(valid&&r){r->magic=MAGIC;r->version=1;r->stage=QUEUED;r->pid=pid;strcpy(r->request,id);strcpy(r->generation,generation);strcpy(r->title,title);strcpy(r->metadata,meta);}
 if(id)JS_FreeCString(c,id);if(generation)JS_FreeCString(c,generation);if(title)JS_FreeCString(c,title);if(meta)JS_FreeCString(c,meta);
 if(!r)return JS_ThrowOutOfMemory(c);if(!valid){free(r);return JS_ThrowTypeError(c,"Invalid PS4 counter identity");}
 pthread_mutex_lock(&mutex);if(running||!pm_native_claim()){pthread_mutex_unlock(&mutex);free(r);return error(c,"NATIVE_BUSY","A native operation is active");}
 Receipt old;int existed=!load(r->generation,&old),bad=0;
 if(existed){
  if(old.pid!=r->pid||strcmp(old.title,r->title)||strcmp(old.metadata,r->metadata))bad=1;
  else {current=old;if(current.stage==QUEUED){current.stage=UNKNOWN;snprintf(current.injection.message,sizeof current.injection.message,"Previous PS4 counter installation was interrupted; no replay");}}
 }else if(errno!=ENOENT||!identity(r)||save(r))bad=1;
 if(!bad&&!existed){
  current=*r;pthread_t thread;running=1;int rc=pthread_create(&thread,NULL,worker,r);
  if(rc){running=0;current.stage=REJECTED;current.injection.resumed=1;snprintf(current.injection.message,sizeof current.injection.message,"PS4 counter worker could not start");if(save(&current))current.stage=UNKNOWN;bad=1;}
  else pthread_detach(thread);
 }
 if(!running){pm_native_release();free(r);}pthread_mutex_unlock(&mutex);
 return bad?error(c,"FPS_NATIVE","PS4 counter intent or worker could not be prepared"):JS_UNDEFINED;
}
static JSValue poll_js(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
 (void)self;if(argc!=2)return JS_ThrowTypeError(c,"Expected counter request and generation");
 const char *id=JS_ToCString(c,argv[0]),*generation=JS_ToCString(c,argv[1]);
 if(!hex(id,32)||!generation||!*generation||strlen(generation)>=128){if(id)JS_FreeCString(c,id);if(generation)JS_FreeCString(c,generation);return JS_ThrowTypeError(c,"Invalid counter receipt identity");}
 Receipt r;pthread_mutex_lock(&mutex);int busy=running,known=current.magic==MAGIC&&!strcmp(current.generation,generation);if(known)r=current;pthread_mutex_unlock(&mutex);
 // Recover the previous installed receipt into RAM without reinjecting. Live
 // identity, entry bytes and the cookie are checked by the sampling worker.
 if(!known&&!busy&&!load(generation,&r)){
  if(r.stage==QUEUED){r.stage=UNKNOWN;snprintf(r.injection.message,sizeof r.injection.message,"Interrupted PS4 counter installation requires review");}
  pthread_mutex_lock(&mutex);current=r;pthread_mutex_unlock(&mutex);known=1;
 }
 JSValue out=JS_NULL;
 if(known){
  out=JS_NewObject(c);JS_SetPropertyStr(c,out,"request",JS_NewString(c,id));JS_SetPropertyStr(c,out,"installationRequest",JS_NewString(c,r.request));
  JS_SetPropertyStr(c,out,"pid",JS_NewInt32(c,r.pid));JS_SetPropertyStr(c,out,"generation",JS_NewString(c,r.generation));JS_SetPropertyStr(c,out,"metadataHash",JS_NewString(c,r.metadata));
  JS_SetPropertyStr(c,out,"stage",JS_NewUint32(c,r.stage));JS_SetPropertyStr(c,out,"busy",JS_NewBool(c,busy));
  JS_SetPropertyStr(c,out,"resumed",JS_NewBool(c,r.injection.resumed));JS_SetPropertyStr(c,out,"uncertain",JS_NewBool(c,r.injection.uncertain));
  JS_SetPropertyStr(c,out,"message",JS_NewString(c,r.injection.message));
 }
 JS_FreeCString(c,id);JS_FreeCString(c,generation);return out;
}
void pm_bc_fps_bind(JSContext *c,JSValue api){
 JS_SetPropertyStr(c,api,"bcFpsStart",JS_NewCFunction(c,start_js,"bcFpsStart",5));
 JS_SetPropertyStr(c,api,"bcFpsPoll",JS_NewCFunction(c,poll_js,"bcFpsPoll",2));
}
