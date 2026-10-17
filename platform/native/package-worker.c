// SPDX-License-Identifier: GPL-3.0-or-later
#include "quickjs.h"
#include "package-io.h"
#include "native-operation.h"
#include "kstuff-probe.h"
#include "elf-image.h"
#include "elfldr.h"
#include <sys/file.h>
#include <sys/wait.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
extern const unsigned char prosperomenu_package_start[],prosperomenu_package_end[];
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static PmPackageRequest current;static int running;static pid_t child;
static void reap(void){
 if(child<=0)return;int status;pid_t result=waitpid(child,&status,WNOHANG);
 if(result==child||(result<0&&errno==ECHILD))child=0;
}
static void* worker(void* unused){
 (void)unused;PmPackageReceipt r={0};pthread_mutex_lock(&mutex);r.input=current;pthread_mutex_unlock(&mutex);
 r.stage=PM_PKG_REJECTED;r.clean=1;const char* reason="Package helper launch preflight failed";
 const SnipersFirmwareProfile* profile=snipers_firmware_profile(kernel_get_fw_version());uint64_t base=KERNEL_ADDRESS_DATA_BASE;
 PmKstuffSnapshot a=pm_kstuff_read(base,profile),b=pm_kstuff_read(base,profile);
 if(!pm_kstuff_snapshot_equal(&a,&b)||pm_kstuff_classify(base,&b)!=PM_KSTUFF_INSTALLED||!pm_kstuff_injection_ready(base,&b)){
  reason="Verified kstuff must be ready before installing a package";goto rejected;
 }
 PmElfImage image;char detail[192];size_t bytes=(size_t)(prosperomenu_package_end-prosperomenu_package_start);
 if(pm_elf_inspect(prosperomenu_package_start,bytes,&image,detail,sizeof detail)){reason="Embedded installer helper image is invalid";goto rejected;}
 r.stage=PM_PKG_QUEUED;r.clean=0;snprintf(r.message,sizeof r.message,"Starting the isolated package installer");
 if(pm_package_receipt_save(&r)){reason="Could not persist installer launch intent";goto rejected;}
 // From this point the helper owns receipt updates. Even if spawn reports an
 // error, it could have started: never overwrite its receipt or launch again.
 pid_t pid=elfldr_spawn("/data/ProsperoMenu",-1,(void*)prosperomenu_package_start,"ProsperoMenu_Package",bytes);
 pthread_mutex_lock(&mutex);if(pid>0)child=pid;pthread_mutex_unlock(&mutex);
 goto done;
rejected:
 r.stage=PM_PKG_REJECTED;r.clean=1;snprintf(r.message,sizeof r.message,"%s",reason);pm_package_receipt_save(&r);
done:
 pthread_mutex_lock(&mutex);running=0;pm_native_release();pthread_mutex_unlock(&mutex);return NULL;
}
static int start(PmPackageRequest* input){
 if(pm_native_busy()){errno=EBUSY;return -1;}
 if(pm_package_boot(input,0)||!pm_package_request_valid(input)||!snipers_firmware_profile(kernel_get_fw_version())){errno=EINVAL;return -1;}
 pthread_mutex_lock(&mutex);
 reap();
 if(running||child>0){pthread_mutex_unlock(&mutex);errno=EBUSY;return -1;}
 if(pm_package_directory()){pthread_mutex_unlock(&mutex);return -1;}
 PmPackageReceipt previous;
 if(!pm_package_receipt_load(input->request,&previous)){
  // Check the complete input, not just the identifier, on duplicate requests.
  PmPackageRequest compare=*input;memcpy(compare.checksum,previous.input.checksum,32);
  int same=!memcmp(&compare,&previous.input,sizeof compare);pthread_mutex_unlock(&mutex);if(same)return 0;errno=EEXIST;return -1;
 }else if(errno!=ENOENT){pthread_mutex_unlock(&mutex);return -1;}
 PmPackageRequest prior;
 if(!pm_package_request_load(&prior)){
  if(pm_package_receipt_load(prior.request,&previous)||
    !(previous.stage==PM_PKG_COMPLETED||(previous.stage==PM_PKG_REJECTED&&previous.clean)||
      (previous.input.bootSec>0 && (previous.input.bootSec!=input->bootSec||previous.input.bootUsec!=input->bootUsec)))){pthread_mutex_unlock(&mutex);errno=EALREADY;return -1;}
 }else if(errno!=ENOENT){pthread_mutex_unlock(&mutex);return -1;}
 int lock=open(PM_PACKAGE_ROOT "/active.lock",O_CREAT|O_RDWR|O_NOFOLLOW,0600);
 if(lock<0||flock(lock,LOCK_EX|LOCK_NB)){if(lock>=0)close(lock);pthread_mutex_unlock(&mutex);errno=EBUSY;return -1;}
 if(!pm_native_claim()){close(lock);pthread_mutex_unlock(&mutex);errno=EBUSY;return -1;}
 PmPackageReceipt receipt={.stage=PM_PKG_QUEUED,.clean=1};
 int failed=pm_package_request_save(input);receipt.input=*input;
 snprintf(receipt.message,sizeof receipt.message,"Installer helper queued");
 if(!failed)failed=pm_package_receipt_save(&receipt);
 close(lock);
 if(failed){pm_native_release();pthread_mutex_unlock(&mutex);return -1;}
 current=*input;running=1;pthread_t thread;int rc=pthread_create(&thread,NULL,worker,NULL);
 if(!rc)pthread_detach(thread);
 else{running=0;pm_native_release();receipt.stage=PM_PKG_REJECTED;snprintf(receipt.message,sizeof receipt.message,"Installer helper worker could not start");pm_package_receipt_save(&receipt);}
 pthread_mutex_unlock(&mutex);if(rc){errno=rc;return -1;}return 0;
}
static JSValue failure(JSContext* c,int clean){
 JSValue error=JS_NewError(c);JS_SetPropertyStr(c,error,"code",JS_NewString(c,errno==EBUSY?"NATIVE_BUSY":errno==EALREADY?"PACKAGE_UNRESOLVED":"PACKAGE_NATIVE"));
 JS_SetPropertyStr(c,error,"message",JS_NewString(c,errno==EALREADY?"A previous package install needs reconciliation before another can start":"Native package installer could not start or its receipt could not be read"));
 if(clean)JS_SetPropertyStr(c,error,"noSideEffects",JS_TRUE);return JS_Throw(c,error);
}
static JSValue start_js(JSContext* c,JSValueConst self,int argc,JSValueConst* argv){
 (void)self;if(argc!=9&&argc!=10)return JS_ThrowTypeError(c,"Expected package request and optional verification mode");
 PmPackageRequest request={.magic=PM_PACKAGE_MAGIC,.version=1};int64_t size;int rc=-1;errno=EINVAL;
 if(argc==10&&(JS_ToUint32(c,&request.reserved,argv[9])||request.reserved>1))return JS_ThrowTypeError(c,"Invalid package verification mode");
 if(request.reserved)request.version=2;
 const char* text[5]={JS_ToCString(c,argv[0]),JS_ToCString(c,argv[1]),JS_ToCString(c,argv[2]),JS_ToCString(c,argv[3]),JS_ToCString(c,argv[8])};
 if(text[0]&&text[1]&&text[2]&&text[3]&&text[4]&&strlen(text[0])==32&&strlen(text[1])==32&&strlen(text[2])==64&&strlen(text[3])==36&&strlen(text[4])<128&&
   !JS_ToUint32(c,&request.type,argv[4])&&!JS_ToUint32(c,&request.platform,argv[5])&&!JS_ToUint32(c,&request.patch,argv[6])&&!JS_ToInt64(c,&size,argv[7])&&size>0){
  strcpy(request.request,text[0]);strcpy(request.upload,text[1]);strcpy(request.sha256,text[2]);strcpy(request.content,text[3]);strcpy(request.name,text[4]);request.bytes=(uint64_t)size;rc=start(&request);
 }
 for(unsigned i=0;i<5;i++)if(text[i])JS_FreeCString(c,text[i]);return rc?failure(c,1):JS_UNDEFINED;
}
static JSValue poll_js(JSContext* c,JSValueConst self,int argc,JSValueConst* argv){
 (void)self;if(argc!=1)return JS_ThrowTypeError(c,"Expected package install request");const char* id=JS_ToCString(c,argv[0]);if(!id)return JS_EXCEPTION;
 PmPackageReceipt r;int rc=pm_package_receipt_load(id,&r);JS_FreeCString(c,id);if(rc)return errno==ENOENT?JS_NULL:failure(c,0);
 pthread_mutex_lock(&mutex);reap();pthread_mutex_unlock(&mutex);
 JSValue out=JS_NewObject(c);
#define STR(key,value) JS_SetPropertyStr(c,out,key,JS_NewString(c,value))
#define NUM(key,value) JS_SetPropertyStr(c,out,key,JS_NewFloat64(c,(double)(value)))
 STR("request",r.input.request);STR("upload",r.input.upload);STR("sha256",r.input.sha256);STR("contentId",r.input.content);STR("message",r.message);STR("state",r.state);
 STR("verification",r.input.reserved==1?"header":"file");
 NUM("stage",r.stage);NUM("pid",r.pid);NUM("error",r.error);NUM("bytes",r.input.bytes);NUM("completedBytes",r.downloaded);NUM("totalBytes",r.total);
 uint64_t now=pm_package_millis();NUM("ageMs",now&&now>=r.updated?now-r.updated:UINT64_MAX);
 PmPackageRequest boot={0};int bootKnown=!pm_package_boot(&boot,0);
 JS_SetPropertyStr(c,out,"sameBoot",bootKnown?JS_NewBool(c,boot.bootSec==r.input.bootSec&&boot.bootUsec==r.input.bootUsec):JS_NULL);
 JS_SetPropertyStr(c,out,"noSideEffects",JS_NewBool(c,r.clean));JS_SetPropertyStr(c,out,"accepted",JS_NewBool(c,r.accepted));JS_SetPropertyStr(c,out,"observed",JS_NewBool(c,r.observed));
#undef STR
#undef NUM
 return out;
}
void pm_package_bind(JSContext* c,JSValue api){JS_SetPropertyStr(c,api,"packageStart",JS_NewCFunction(c,start_js,"packageStart",9));JS_SetPropertyStr(c,api,"packagePoll",JS_NewCFunction(c,poll_js,"packagePoll",1));}
