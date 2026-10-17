// SPDX-License-Identifier: GPL-3.0-or-later
#include "quickjs.h"
#include "save-io.h"
#include "save-recovery.h"
#include "native-operation.h"
#include "kstuff-probe.h"
#include "elf-image.h"
#include "elfldr.h"
#include <sys/file.h>
#include <sys/wait.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
extern const unsigned char prosperomenu_save_start[],prosperomenu_save_end[];
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static PmSaveRequest current;static int running;static pid_t child;
static struct {char request[33],message[192];unsigned char expected[32];int active,finished,ok;uint64_t completed,total;} recovery;
static void reap(void){if(child>0){int status;pid_t got=waitpid(child,&status,WNOHANG);if(got==child||(got<0&&errno==ECHILD))child=0;}}
static void* worker(void* unused){
 (void)unused;PmSaveReceipt receipt={.stage=PM_SAVE_REJECTED,.clean=1};
 pthread_mutex_lock(&mutex);receipt.input=current;pthread_mutex_unlock(&mutex);const char* failure="Save helper launch preflight failed";
 const SnipersFirmwareProfile* profile=snipers_firmware_profile(kernel_get_fw_version());uint64_t base=KERNEL_ADDRESS_DATA_BASE;
 PmKstuffSnapshot a=pm_kstuff_read(base,profile),b=pm_kstuff_read(base,profile);
 if(!pm_kstuff_snapshot_equal(&a,&b)||pm_kstuff_classify(base,&b)!=PM_KSTUFF_INSTALLED||!pm_kstuff_injection_ready(base,&b)){failure="Verified kstuff is required before starting the save helper";goto rejected;}
 PmElfImage image;char detail[192];size_t bytes=(size_t)(prosperomenu_save_end-prosperomenu_save_start);
 if(pm_elf_inspect(prosperomenu_save_start,bytes,&image,detail,sizeof detail)){failure="Embedded save helper image is invalid";goto rejected;}
 receipt.stage=PM_SAVE_QUEUED;receipt.clean=0;snprintf(receipt.message,sizeof receipt.message,"Starting isolated save helper");
 if(pm_save_receipt_save(&receipt)){failure="Could not persist save helper launch intent";goto rejected;}
 // The helper owns the receipt after this point. A spawn error is not proof
 // that no process started; it must not cause a second launch or overwrite.
 pid_t pid=elfldr_spawn("/data/ProsperoMenu",-1,(void*)prosperomenu_save_start,"ProsperoMenu_Saves",bytes);
 pthread_mutex_lock(&mutex);if(pid>0)child=pid;pthread_mutex_unlock(&mutex);goto done;
rejected:
 receipt.stage=PM_SAVE_REJECTED;receipt.clean=1;snprintf(receipt.message,sizeof receipt.message,"%s",failure);pm_save_receipt_save(&receipt);
done:
 pthread_mutex_lock(&mutex);running=0;pm_native_release();pthread_mutex_unlock(&mutex);return NULL;
}
static int start(PmSaveRequest* input){
 if(pm_native_busy()){errno=EBUSY;return -1;}
 if(pm_save_boot(input,0)||!pm_save_request_valid(input)||!snipers_firmware_profile(kernel_get_fw_version())){errno=EINVAL;return -1;}
 pthread_mutex_lock(&mutex);reap();int root=-1,lock=-1,claimed=0,result=-1;
 if(running||child>0){errno=EBUSY;goto done;}
 root=pm_save_directory(PM_SAVE_ROOT,1);if(root<0)goto done;
 PmSaveReceipt previous;
 if(!pm_save_receipt_load(input->request,&previous)){
  PmSaveRequest compare=*input;memcpy(compare.checksum,previous.input.checksum,32);
  if(!memcmp(&compare,&previous.input,sizeof compare))result=0;else errno=EEXIST;goto done;
 }else if(errno!=ENOENT)goto done;
 PmSaveRequest prior;if(!pm_save_request_load(&prior)){
  if(pm_save_receipt_load(prior.request,&previous)||!(previous.stage==PM_SAVE_COMPLETE||(previous.stage==PM_SAVE_REJECTED&&previous.clean))){errno=EALREADY;goto done;}
 }else if(errno!=ENOENT)goto done;
 lock=openat(root,"active.lock",O_CREAT|O_RDWR|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600);struct stat st;
 if(lock<0||fstat(lock,&st)||!S_ISREG(st.st_mode)||flock(lock,LOCK_EX|LOCK_NB)){errno=EBUSY;goto done;}
 if(!pm_native_claim()){errno=EBUSY;goto done;}claimed=1;
 if(pm_save_request_save(input))goto done;
 PmSaveReceipt receipt={.input=*input,.stage=PM_SAVE_QUEUED,.clean=1};snprintf(receipt.message,sizeof receipt.message,"Save helper queued");
 if(pm_save_receipt_save(&receipt))goto done;
 current=*input;running=1;pthread_t thread;int rc=pthread_create(&thread,NULL,worker,NULL);
 if(!rc){pthread_detach(thread);claimed=0;result=0;}
 else{running=0;receipt.stage=PM_SAVE_REJECTED;snprintf(receipt.message,sizeof receipt.message,"Save helper worker could not start");pm_save_receipt_save(&receipt);errno=rc;}
done:
 {int error=errno;if(claimed)pm_native_release();if(lock>=0)close(lock);if(root>=0)close(root);pthread_mutex_unlock(&mutex);errno=error;return result;}
}
static JSValue failure(JSContext* c,int clean){
 int error=errno;JSValue out=JS_NewError(c);
 JS_SetPropertyStr(c,out,"code",JS_NewString(c,error==EBUSY?"NATIVE_BUSY":error==EALREADY?"SAVE_UNRESOLVED":"SAVE_NATIVE"));
 JS_SetPropertyStr(c,out,"message",JS_NewString(c,error==EALREADY?"A previous save operation needs reconciliation; no new helper was started":"Save helper or its verified receipt is unavailable"));
 if(clean)JS_SetPropertyStr(c,out,"noSideEffects",JS_TRUE);return JS_Throw(c,out);
}
static JSValue start_js(JSContext* c,JSValueConst self,int argc,JSValueConst* argv){
 (void)self;if(argc!=9)return JS_ThrowTypeError(c,"Expected request, mode, user, title, slot, stamp, destination, volume and platform");
 int indices[]={0,2,3,4,5,7};const char* texts[6]={0};PmSaveRequest r={.magic=PM_SAVE_MAGIC,.version=1};
 char* fields[]={r.request,r.user,r.title,r.slot,r.stamp,r.volume};size_t caps[]={sizeof r.request,sizeof r.user,sizeof r.title,sizeof r.slot,sizeof r.stamp,sizeof r.volume};
 int valid=1,result=-1;errno=EINVAL;
 for(unsigned i=0;i<6;i++){texts[i]=JS_ToCString(c,argv[indices[i]]);if(!texts[i]||strlen(texts[i])>=caps[i])valid=0;else strcpy(fields[i],texts[i]);}
 if(valid&&!JS_ToUint32(c,&r.mode,argv[1])&&!JS_ToUint32(c,&r.destination,argv[6])&&!JS_ToUint32(c,&r.platform,argv[8]))result=start(&r);
 for(unsigned i=0;i<6;i++)if(texts[i])JS_FreeCString(c,texts[i]);return result?failure(c,1):JS_UNDEFINED;
}
static JSValue poll_js(JSContext* c,JSValueConst self,int argc,JSValueConst* argv){
 (void)self;if(argc!=1)return JS_ThrowTypeError(c,"Expected save request ID");const char* id=JS_ToCString(c,argv[0]);if(!id)return JS_EXCEPTION;
 PmSaveReceipt r;int rc=pm_save_receipt_load(id,&r);JS_FreeCString(c,id);if(rc)return errno==ENOENT?JS_NULL:failure(c,0);
 pthread_mutex_lock(&mutex);reap();pthread_mutex_unlock(&mutex);JSValue out=JS_NewObject(c);
#define STR(key,value) JS_SetPropertyStr(c,out,key,JS_NewString(c,value))
#define NUM(key,value) JS_SetPropertyStr(c,out,key,JS_NewFloat64(c,(double)(value)))
 STR("request",r.input.request);STR("user",r.input.user);STR("titleId",r.input.title);STR("slot",r.input.slot);STR("stamp",r.input.stamp);STR("volume",r.input.volume);
 STR("message",r.message);STR("output",r.output);STR("sha256",r.sha256);
 char receiptHash[65];pm_save_hash_text(r.checksum,receiptHash);STR("receiptHash",receiptHash);
 NUM("mode",r.input.mode);NUM("destination",r.input.destination);NUM("platform",r.input.platform);NUM("stage",r.stage);NUM("pid",r.pid);NUM("error",r.error);
 NUM("completedBytes",r.completed);NUM("totalBytes",r.total);NUM("archiveBytes",r.archiveBytes);NUM("files",r.files);
 uint64_t now=pm_save_millis();NUM("ageMs",now&&now>=r.updated?now-r.updated:UINT64_MAX);
 JS_SetPropertyStr(c,out,"sameBoot",JS_NewBool(c,!pm_save_boot(&r.input,1)));JS_SetPropertyStr(c,out,"noSideEffects",JS_NewBool(c,r.clean));JS_SetPropertyStr(c,out,"mounted",JS_NewBool(c,r.mounted));
#undef STR
#undef NUM
 return out;
}
static void recovery_progress(void* unused,uint64_t completed,uint64_t total){
 (void)unused;pthread_mutex_lock(&mutex);recovery.completed=completed;recovery.total=total;pthread_mutex_unlock(&mutex);
}
static void* recover_worker(void* unused){
 (void)unused;char message[192];int rc=pm_save_recover(recovery.request,recovery.expected,message,recovery_progress,NULL);
 pthread_mutex_lock(&mutex);snprintf(recovery.message,sizeof recovery.message,"%s",message);
 recovery.ok=!rc;recovery.finished=1;recovery.active=0;running=0;pm_native_release();pthread_mutex_unlock(&mutex);return NULL;
}
static JSValue recover_start_js(JSContext* c,JSValueConst self,int argc,JSValueConst* argv){
 (void)self;if(argc!=2)return JS_ThrowTypeError(c,"Expected save request and receipt hash");
 const char* id=JS_ToCString(c,argv[0]);const char* hash=JS_ToCString(c,argv[1]);PmSaveReceipt r;char actual[65];int bad=1;
 if(!id||!hash||!pm_save_hex(id,32)||!pm_save_hex(hash,64)){errno=EINVAL;goto done;}
 pthread_mutex_lock(&mutex);reap();
 if(recovery.active&&!strcmp(recovery.request,id)){pm_save_hash_text(recovery.expected,actual);if(!strcmp(actual,hash)){bad=0;goto unlock;}}
 if(running||child>0||pm_native_busy()){errno=EBUSY;goto unlock;}
 if(pm_save_receipt_load(id,&r)){goto unlock;}pm_save_hash_text(r.checksum,actual);
 if(strcmp(actual,hash)){errno=EALREADY;goto unlock;}
 if(!pm_native_claim()){errno=EBUSY;goto unlock;}
 memset(&recovery,0,sizeof recovery);strcpy(recovery.request,id);memcpy(recovery.expected,r.checksum,32);
 recovery.active=1;running=1;pthread_t thread;int rc=pthread_create(&thread,NULL,recover_worker,NULL);
 if(rc){running=0;recovery.active=0;recovery.finished=1;pm_native_release();errno=rc;}
 else{pthread_detach(thread);bad=0;}
unlock:pthread_mutex_unlock(&mutex);
done:if(id)JS_FreeCString(c,id);if(hash)JS_FreeCString(c,hash);return bad?failure(c,1):JS_UNDEFINED;
}
static JSValue recover_poll_js(JSContext* c,JSValueConst self,int argc,JSValueConst* argv){
 (void)self;if(argc!=1)return JS_ThrowTypeError(c,"Expected save request ID");const char* id=JS_ToCString(c,argv[0]);if(!id)return JS_EXCEPTION;
 pthread_mutex_lock(&mutex);JSValue result=JS_NULL;
 if(!strcmp(recovery.request,id)){
  result=JS_NewObject(c);JS_SetPropertyStr(c,result,"request",JS_NewString(c,recovery.request));
  JS_SetPropertyStr(c,result,"running",JS_NewBool(c,recovery.active));JS_SetPropertyStr(c,result,"finished",JS_NewBool(c,recovery.finished));
  JS_SetPropertyStr(c,result,"ok",JS_NewBool(c,recovery.ok));JS_SetPropertyStr(c,result,"message",JS_NewString(c,recovery.message));
  JS_SetPropertyStr(c,result,"completed",JS_NewFloat64(c,(double)recovery.completed));JS_SetPropertyStr(c,result,"total",JS_NewFloat64(c,(double)recovery.total));
 }
 pthread_mutex_unlock(&mutex);JS_FreeCString(c,id);return result;
}
static JSValue catalog_js(JSContext* c,JSValueConst self,int argc,JSValueConst* argv){
 (void)self;if(argc!=1)return JS_ThrowTypeError(c,"Expected completed catalog request");const char* id=JS_ToCString(c,argv[0]);if(!id)return JS_EXCEPTION;
 PmSaveReceipt r;int bad=pm_save_receipt_load(id,&r);JS_FreeCString(c,id);
 if(bad||r.input.mode!=PM_SAVE_CATALOG||r.stage!=PM_SAVE_COMPLETE||!r.clean||r.mounted||r.archiveBytes>2*1024*1024||!r.archiveBytes){errno=EINVAL;return failure(c,0);}
 char name[64];snprintf(name,sizeof name,"%s.catalog.json",r.input.request);int root=pm_save_directory(PM_SAVE_ROOT,0);
 int fd=root<0?-1:openat(root,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);if(root>=0)close(root);if(fd<0)return failure(c,0);
 struct stat st;unsigned char* data=NULL;JSValue out=JS_UNDEFINED;
 if(fstat(fd,&st)||!S_ISREG(st.st_mode)||(uint64_t)st.st_size!=r.archiveBytes)goto fail;
 data=malloc((size_t)r.archiveBytes+1);if(!data||pm_save_read_all(fd,0,data,(size_t)r.archiveBytes))goto fail;
 unsigned char digest[32];char hash[65];pm_save_digest(data,(size_t)r.archiveBytes,digest);pm_save_hash_text(digest,hash);
 if(strcmp(hash,r.sha256))goto fail;data[r.archiveBytes]=0;out=JS_NewStringLen(c,(char*)data,(size_t)r.archiveBytes);goto done;
fail:errno=EINVAL;out=failure(c,0);
done:free(data);close(fd);return out;
}
void pm_save_bind(JSContext* c,JSValue api){
 JS_SetPropertyStr(c,api,"saveStart",JS_NewCFunction(c,start_js,"saveStart",9));
 JS_SetPropertyStr(c,api,"savePoll",JS_NewCFunction(c,poll_js,"savePoll",1));
 JS_SetPropertyStr(c,api,"saveCatalog",JS_NewCFunction(c,catalog_js,"saveCatalog",1));
 JS_SetPropertyStr(c,api,"saveRecoverStart",JS_NewCFunction(c,recover_start_js,"saveRecoverStart",2));
 JS_SetPropertyStr(c,api,"saveRecoverPoll",JS_NewCFunction(c,recover_poll_js,"saveRecoverPoll",1));
}
