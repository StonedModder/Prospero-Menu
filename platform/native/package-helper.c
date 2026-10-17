// SPDX-License-Identifier: GPL-3.0-or-later
// One submitted package per isolated process. Its watchdog can terminate this
// helper without stopping the phone/deck coordinator. No restart/retry loop.
#include "package-core.h"
#include "package-abi.h"
#include "package-io.h"
#include "package-http.h"
#include "package-fingerprint.h"
#include "firmware-profiles.h"
#include "sha256.h"
#include <ps5/kernel.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
static PmPackageReceipt receipt;
static atomic_uint_fast64_t watchdog_deadline;
static void arm_watchdog(unsigned seconds){atomic_store(&watchdog_deadline,seconds?pm_package_millis()+(uint64_t)seconds*1000:0);}
static void* watchdog(void* unused){
 (void)unused;for(;;){uint64_t deadline=atomic_load(&watchdog_deadline);if(deadline&&pm_package_millis()>=deadline)_exit(124);usleep(100000);}return NULL;
}
static int (*initialize)(void),(*install)(PmInstallMeta*,PmInstallPackage*,PmInstallPlaygo*);
static int (*status)(const char*,void*);
static uint64_t savedAuth;static int elevated;
static int publish(unsigned stage,int clean,const char* text){
 receipt.stage=stage;receipt.clean=clean;snprintf(receipt.message,sizeof receipt.message,"%s",text);return pm_package_receipt_save(&receipt);
}
static int read_at(void* context,uint64_t at,void* data,size_t bytes){
 int fd=*(int*)context;size_t got=0;
 while(got<bytes){ssize_t n=pread(fd,(unsigned char*)data+got,bytes-got,(off_t)(at+got));if(n<0&&errno==EINTR)continue;if(n<=0)return -1;got+=(size_t)n;}return 0;
}
static int auth_begin(void){
 arm_watchdog(12);savedAuth=kernel_get_ucred_authid(getpid());
 if(!savedAuth||savedAuth==UINT64_MAX){arm_watchdog(0);return -1;}
 elevated=1;
 return kernel_set_ucred_authid(getpid(),UINT64_C(0x4801000000000013))||kernel_get_ucred_authid(getpid())!=UINT64_C(0x4801000000000013)?-1:0;
}
static int auth_end(void){
 if(elevated)for(unsigned i=0;i<3;i++)if(!kernel_set_ucred_authid(getpid(),savedAuth)&&kernel_get_ucred_authid(getpid())==savedAuth){elevated=0;break;}
 arm_watchdog(0);return elevated?-1:0;
}
static int same_file(int fd,const char* path,const struct stat* before){
 struct stat after,named;
 return !fstat(fd,&after)&&!lstat(path,&named)&&S_ISREG(named.st_mode)&&!S_ISLNK(named.st_mode)&&
  before->st_dev==named.st_dev&&before->st_ino==named.st_ino&&before->st_size==after.st_size&&
  before->st_mtim.tv_sec==after.st_mtim.tv_sec&&before->st_mtim.tv_nsec==after.st_mtim.tv_nsec;
}
static int get_status(PmPackageProgress* result){
 PmInstallStatusStorage value={0};if(auth_begin()){auth_end();return -1;}
 int rc=status(receipt.input.content,&value),restore=auth_end();if(restore)return -2;if(rc)return rc;
 return pm_install_progress(&value,sizeof value,kernel_get_fw_version(),result)?-3:0;
}
int main(void){
 signal(SIGPIPE,SIG_IGN);signal(SIGALRM,SIG_DFL);
 if(pm_package_directory())return 1;
 int lock=open(PM_PACKAGE_ROOT "/active.lock",O_CREAT|O_RDWR|O_NOFOLLOW,0600);
 if(lock<0||flock(lock,LOCK_EX|LOCK_NB))return 2;
 PmPackageRequest request;
 if(pm_package_request_load(&request)||pm_package_boot(&request,1)||pm_package_receipt_load(request.request,&receipt)||
   memcmp(&request,&receipt.input,sizeof request)||receipt.stage!=PM_PKG_QUEUED||receipt.clean)return 3;
 receipt.pid=getpid();int fd=-1;unsigned char* chunk=NULL;PmPackageSource* source=NULL;int result=1;const char* reason="Package helper preflight failed";
 pthread_t guard;if(pthread_create(&guard,NULL,watchdog,NULL)){reason="Installer watchdog could not start";goto rejected;}pthread_detach(guard);
 if(!snipers_firmware_profile(kernel_get_fw_version())){reason="This firmware is outside ProsperoMenu's supported profiles";goto rejected;}
 if(publish(PM_PKG_HASHING,1,request.reserved?"Checking the selected package headers":"Checking the exact package bytes before installation"))goto done;
 struct stat parent;if(lstat("/data/ProsperoMenu/packages",&parent)||!S_ISDIR(parent.st_mode)||S_ISLNK(parent.st_mode))goto rejected;
 char path[1024];fd=pm_package_open_source(request.upload,path,sizeof path);struct stat before;
 if(fd<0||fstat(fd,&before)||!S_ISREG(before.st_mode)||(uint64_t)before.st_size!=request.bytes){reason="Package source is missing or changed. Keep USB connected, or select the file again";goto rejected;}
 PmPackageInfo info;unsigned char digest[32];
 if(pm_package_header_fingerprint(read_at,&fd,request.bytes,&info,digest)||info.type!=request.type||info.platform!=request.platform||info.patch!=request.patch||strcmp(info.content,request.content)){
  reason="Native package header differs from the approved upload";goto rejected;
 }
 if(!request.reserved){
 chunk=malloc(1024*1024);if(!chunk){reason="Not enough memory for package verification";goto rejected;}
 SHA256_CTX hash;sha256_init(&hash);uint64_t last=pm_package_millis();receipt.total=request.bytes;
 for(uint64_t at=0;at<request.bytes;){
  size_t n=request.bytes-at>1024*1024?1024*1024:(size_t)(request.bytes-at);
  if(read_at(&fd,at,chunk,n)){reason="Package read ended before its checksum was complete";goto rejected;}
  sha256_update(&hash,chunk,n);at+=n;receipt.downloaded=at;uint64_t now=pm_package_millis();
  if(!now||now<last){reason="Monotonic clock unavailable during verification";goto rejected;}
  if(now-last>=2000){if(pm_package_receipt_save(&receipt))goto done;last=now;}
 }
 free(chunk);chunk=NULL;sha256_final(&hash,digest);
 }
 char actual[65];
 for(unsigned i=0;i<32;i++)snprintf(actual+i*2,3,"%02x",digest[i]);
 if(strcmp(actual,request.sha256)||!same_file(fd,path,&before)){reason="Package changed after upload verification";goto rejected;}
 receipt.downloaded=receipt.total=0;if(publish(PM_PKG_PREFLIGHT,1,"Preparing the console package installer"))goto done;
 // Load the same explicit service dependencies used by the working Neighborhood
 // receiver during CRT startup. Late dlopen of AppInstUtil stalled on 13.60.
 arm_watchdog(12);
 initialize=dlsym(RTLD_DEFAULT,"sceAppInstUtilInitialize");install=dlsym(RTLD_DEFAULT,"sceAppInstUtilInstallByPackage");
 status=dlsym(RTLD_DEFAULT,request.patch?"sceAppInstUtilGetPatchInstallStatus":"sceAppInstUtilGetInstallStatus");arm_watchdog(0);
 if(!initialize||!install||!status){reason="Console installer symbols are unavailable";goto rejected;}
 if(publish(PM_PKG_PREFLIGHT,1,"Initializing the console package installer"))goto done;
 if(auth_begin()){auth_end();reason="Installer credentials could not be established";goto rejected;}
 int rc=initialize(),restored=auth_end();if(rc||restored){receipt.error=rc;reason="Console installer initialization failed";goto rejected;}
 if(publish(PM_PKG_PREFLIGHT,1,"Checking existing installation status"))goto done;
 PmPackageTracker tracker={0};rc=get_status(&tracker.baseline);if(elevated){reason="Installer credentials could not be restored";goto rejected;}tracker.baselineValid=rc==0;
 if(tracker.baselineValid&&tracker.baseline.total&&tracker.baseline.downloaded<tracker.baseline.total){reason="This content already has an unfinished installation";goto rejected;}
 if(!same_file(fd,path,&before)){reason="Package identity changed before submission";goto rejected;}
 char uri[160];
 if(publish(PM_PKG_PREFLIGHT,1,"Starting the verified local package source"))goto done;
 if(pm_package_source_start(&source,fd,request.request,request.bytes,uri,sizeof uri)){
  reason="The local package source could not start; nothing was submitted";goto rejected;
 }
 if(!same_file(fd,path,&before)||!pm_package_source_healthy(source)){reason="The local package source changed before submission";goto rejected;}
 // Everything the marshaller can retain stays alive through confirmed finish.
 PmInstallPackage output={0};PmInstallPlaygo playgo={0};PmInstallMeta meta={.uri=uri,.extra="",.scenario="",.content="",.name=request.name,.icon=""};
 if(publish(PM_PKG_SUBMITTING,0,"Submitting this package once to the console installer"))goto done;
 if(auth_begin()){auth_end();reason="Installer credential change failed before submission";goto rejected;}
 rc=install(&meta,&output,&playgo);restored=auth_end();receipt.error=rc;
 if(rc||restored){
  char detail[192];snprintf(detail,sizeof detail,"Installer returned 0x%08X; credentials %s. Package retained for inspection.",(unsigned)rc,restored?"could not be restored":"restored");
  publish(PM_PKG_UNKNOWN,0,detail);goto done;
 }
 receipt.accepted=1;
 if(!memchr(output.content,0,sizeof output.content)||strcmp(output.content,request.content)){
  publish(PM_PKG_UNKNOWN,0,"Installer returned a different content identity; no second submission was issued");goto done;
 }
 if(publish(PM_PKG_INSTALLING,0,"Package accepted; observing actual install progress"))goto done;
 uint64_t started=pm_package_millis(),lastPoll=started;unsigned errors=0;
 for(;;){
  if(!pm_package_source_healthy(source)){publish(PM_PKG_UNKNOWN,0,"The local package source stopped or changed; no resubmission was issued");break;}
  uint64_t now=pm_package_millis();if(!now||now<lastPoll||now-started>48ULL*60*60*1000){publish(PM_PKG_UNKNOWN,0,"Installation observation expired; uploaded package retained");break;}lastPoll=now;
  PmPackageProgress progress;rc=get_status(&progress);
  if(rc){receipt.error=rc;if(elevated||++errors>=5){publish(PM_PKG_UNKNOWN,0,"Console install status became unavailable; no resubmission was issued");break;}usleep(500000);continue;}
  errors=0;int complete=pm_package_track(&tracker,&progress,request.bytes,request.platform);
  memcpy(receipt.state,progress.state,16);receipt.state[15]=0;receipt.downloaded=progress.downloaded;receipt.total=progress.total;
  receipt.error=progress.error;receipt.observed=tracker.observed;
  if(complete<0){
   const char* detail=complete==-2?(!strcmp(progress.state,"none")?
    "The accepted installation never appeared; source retained and no resubmission issued":
    "Only a completed status was visible; a new install cannot be distinguished from the previous one"):
    "Console installer reported an error, lost task or invalid progress; package retained";
   publish(PM_PKG_UNKNOWN,0,detail);break;
  }
  if(complete){result=publish(PM_PKG_COMPLETED,0,"Console reported complete installation after observed transfer progress");break;}
  if(now-receipt.updated>=2000&&pm_package_receipt_save(&receipt))break;usleep(500000);
 }
 goto done;
rejected:
 publish(PM_PKG_REJECTED,1,reason);
done:
 if(elevated)auth_end();pm_package_source_stop(source);free(chunk);if(fd>=0)close(fd);close(lock);return result;
}
