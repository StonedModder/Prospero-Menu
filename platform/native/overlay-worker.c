// SPDX-License-Identifier: GPL-3.0-or-later
// ProsperoMenu overlay launcher (docs/OVERLAYS.md).
//
// Overlays are ordinary payload ELFs that live in
// /data/ProsperoMenu/overlays/<id>/. Unlike ProsperoMenu's payload dispatch, an
// overlay may be started again in the same boot (after it exits, crashes or is
// stopped), because its whole contract is "draw a page through the menu". The
// worker still validates the image, shares the global native-operation lock
// with every other ptrace-based worker and refuses to start while kernel hook
// state is changing. Overlay stdout/stderr go to <dir>/overlay.log.
#include "quickjs.h"
#include "elf-image.h"
#include "elfldr.h"
#include "kstuff-probe.h"
#include "firmware-profiles.h"
#include "native-operation.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <ps5/kernel.h>

#define PM_OVERLAY_ROOT "/data/ProsperoMenu/overlays/"
#define PM_OVERLAY_MAX (32u*1024u*1024u)
enum { PM_SPAWN_IDLE, PM_SPAWN_RUNNING, PM_SPAWN_DONE, PM_SPAWN_FAILED };
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static struct { int state; int pid; unsigned serial; char id[16], path[256], message[160]; } job;

static int id_ok(const char *id){
  size_t n=id?strlen(id):0;if(!n||n>15)return 0;
  for(size_t i=0;i<n;i++){char c=id[i];if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'))return 0;}
  return 1;
}
// <root><id>/<file> with a plain file name: no traversal, no nested folders.
static int path_ok(const char *id,const char *path){
  size_t root=strlen(PM_OVERLAY_ROOT),n=strlen(id);
  if(!path||strlen(path)>=sizeof job.path||strncmp(path,PM_OVERLAY_ROOT,root)||strncmp(path+root,id,n)||path[root+n]!='/')return 0;
  const char *file=path+root+n+1;
  if(!*file||strchr(file,'/')||!strcmp(file,".")||!strcmp(file,".."))return 0;
  return 1;
}
static void finish(int state,int pid,const char *message){
  pthread_mutex_lock(&mutex);job.state=state;job.pid=pid;snprintf(job.message,sizeof job.message,"%s",message);pthread_mutex_unlock(&mutex);
}
static void *run(void *arg){
  (void)arg;char path[256],id[16],reason[160]="";uint8_t *data=NULL;int fd=-1,log=-1,pid=-1;
  pthread_mutex_lock(&mutex);strcpy(path,job.path);strcpy(id,job.id);pthread_mutex_unlock(&mutex);
  fd=open(path,O_RDONLY|O_NOFOLLOW);
  struct stat st;
  if(fd<0||fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<64||st.st_size>PM_OVERLAY_MAX){snprintf(reason,sizeof reason,"Overlay ELF is missing or has an invalid size");goto out;}
  size_t size=(size_t)st.st_size;
  if(!(data=malloc(size))){snprintf(reason,sizeof reason,"Not enough memory to start this overlay");goto out;}
  for(size_t done=0;done<size;){ssize_t r=read(fd,data+done,size-done);if(r<=0){snprintf(reason,sizeof reason,"Overlay ELF could not be read");goto out;}done+=(size_t)r;}
  close(fd);fd=-1;
  PmElfImage image;
  if(pm_elf_inspect(data,size,&image,reason,sizeof reason))goto out;
  const SnipersFirmwareProfile *profile=snipers_firmware_profile(kernel_get_fw_version());
  uint64_t base=KERNEL_ADDRESS_DATA_BASE;
  PmKstuffSnapshot a=pm_kstuff_read(base,profile),b=pm_kstuff_read(base,profile);
  if(!profile||!pm_kstuff_snapshot_equal(&a,&b)||!pm_kstuff_injection_ready(base,&b)){
    snprintf(reason,sizeof reason,"Kernel hook state is unknown or changing; the overlay was not started");goto out;
  }
  // The log lives next to the overlay so its author can read it over FTP.
  char logPath[300];const char *slash=strrchr(path,'/');
  snprintf(logPath,sizeof logPath,"%.*s/overlay.log",(int)(slash-path),path);
  log=open(logPath,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0666);
  // The process name doubles as the overlay's identity for its SDK and for
  // overlayAlive/overlayKill: "pmo_" + id (at most 19 characters).
  char name[20];snprintf(name,sizeof name,"pmo_%s",id);
  pid=elfldr_spawn("/",log>0?log:-1,data,name,size);
  if(pid<=0){pid=-1;snprintf(reason,sizeof reason,"The overlay process could not be started");}
out:
  if(fd>=0)close(fd);if(log>=0)close(log);free(data);
  pm_native_release();
  finish(pid>0?PM_SPAWN_DONE:PM_SPAWN_FAILED,pid,pid>0?"Overlay started":reason);
  return NULL;
}
// Process table lookup by pid: 1 alive with our name, 0 gone or reused, -1 unknown.
static int alive(int pid,const char *id){
  if(pid<=0||!id_ok(id))return 0;
  char name[20];snprintf(name,sizeof name,"pmo_%s",id);
  int mib[]={CTL_KERN,KERN_PROC,KERN_PROC_PID,pid};struct kinfo_proc info;size_t size=sizeof info;memset(&info,0,sizeof info);
  if(sysctl(mib,4,&info,&size,NULL,0))return errno==ESRCH?0:-1;
  if(size<offsetof(struct kinfo_proc,ki_comm)+COMMLEN+1||info.ki_pid!=pid)return 0;
  info.ki_comm[COMMLEN]=0;info.ki_tdname[TDNAMLEN]=0;
  return !strncmp(info.ki_comm,name,COMMLEN)||!strncmp(info.ki_tdname,name,TDNAMLEN);
}
static JSValue throw_code(JSContext *c,const char *code,const char *message){
  JSValue e=JS_NewError(c);JS_SetPropertyStr(c,e,"code",JS_NewString(c,code));JS_SetPropertyStr(c,e,"message",JS_NewString(c,message));return JS_Throw(c,e);
}
static JSValue spawn(JSContext *c,JSValueConst self,int argc,JSValueConst *args){
  (void)self;if(argc!=2)return JS_ThrowTypeError(c,"Expected overlay id and ELF path");
  const char *id=JS_ToCString(c,args[0]),*path=JS_ToCString(c,args[1]);JSValue result=JS_UNDEFINED;
  if(!id||!path||!id_ok(id)||!path_ok(id,path))result=throw_code(c,"OVERLAY_PATH","Overlay ELF must be a file inside its own overlay folder");
  else{
    pthread_mutex_lock(&mutex);
    if(job.state==PM_SPAWN_RUNNING||!pm_native_claim()){pthread_mutex_unlock(&mutex);result=throw_code(c,"OVERLAY_BUSY","Another native operation is running");}
    else{
      job.state=PM_SPAWN_RUNNING;job.pid=-1;job.serial++;snprintf(job.id,sizeof job.id,"%s",id);snprintf(job.path,sizeof job.path,"%s",path);snprintf(job.message,sizeof job.message,"Starting overlay");
      pthread_t thread;pthread_attr_t attr;int r=pthread_attr_init(&attr);
      if(!r){r=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);if(!r)r=pthread_create(&thread,&attr,run,NULL);pthread_attr_destroy(&attr);}
      if(r){job.state=PM_SPAWN_FAILED;snprintf(job.message,sizeof job.message,"Overlay launcher thread could not start");pm_native_release();}
      unsigned serial=job.serial;pthread_mutex_unlock(&mutex);
      result=r?throw_code(c,"OVERLAY_NATIVE","Overlay launcher thread could not start"):JS_NewUint32(c,serial);
    }
  }
  if(id)JS_FreeCString(c,id);if(path)JS_FreeCString(c,path);return result;
}
static JSValue poll(JSContext *c,JSValueConst self,int argc,JSValueConst *args){
  (void)self;(void)argc;(void)args;JSValue o=JS_NewObject(c);
  pthread_mutex_lock(&mutex);
  static const char *names[]={"idle","spawning","spawned","failed"};
  JS_SetPropertyStr(c,o,"state",JS_NewString(c,names[job.state]));
  JS_SetPropertyStr(c,o,"serial",JS_NewUint32(c,job.serial));
  JS_SetPropertyStr(c,o,"id",JS_NewString(c,job.id));
  JS_SetPropertyStr(c,o,"pid",JS_NewInt32(c,job.pid));
  JS_SetPropertyStr(c,o,"message",JS_NewString(c,job.message));
  pthread_mutex_unlock(&mutex);return o;
}
static JSValue check(JSContext *c,JSValueConst self,int argc,JSValueConst *args,int magic){
  (void)self;if(argc!=2)return JS_ThrowTypeError(c,"Expected pid and overlay id");
  int pid=0;JS_ToInt32(c,&pid,args[0]);const char *id=JS_ToCString(c,args[1]);if(!id)return JS_EXCEPTION;
  int state=alive(pid,id);JS_FreeCString(c,id);
  if(magic&&state==1){if(kill(pid,SIGKILL))return JS_FALSE;return JS_TRUE;}
  return state<0?JS_NULL:JS_NewBool(c,state==1);
}
void pm_overlay_bind(JSContext *c,JSValue api){
  JS_SetPropertyStr(c,api,"overlaySpawn",JS_NewCFunction(c,spawn,"overlaySpawn",2));
  JS_SetPropertyStr(c,api,"overlayPoll",JS_NewCFunction(c,poll,"overlayPoll",0));
  JS_SetPropertyStr(c,api,"overlayAlive",JS_NewCFunctionMagic(c,check,"overlayAlive",2,JS_CFUNC_generic_magic,0));
  JS_SetPropertyStr(c,api,"overlayKill",JS_NewCFunctionMagic(c,check,"overlayKill",2,JS_CFUNC_generic_magic,1));
}
