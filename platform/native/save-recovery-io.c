// SPDX-License-Identifier: GPL-3.0-or-later
#include "save-recovery.h"
#ifdef PM_SAVE_RECOVERY_TEST
#include "save-recovery-test.h"
#else
#include "save-io.h"
#include <sys/file.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#endif
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef struct {
 const char* request;const unsigned char* expected;char* message;
 int root,lock;PmSaveRecoveryProgress progress;void* progressContext;
} Recover;
typedef struct {Recover* c;uint64_t total;} RecoveryProgress;
static int fail(Recover* c,const char* message){snprintf(c->message,192,"%s",message);return -1;}
static int acquire(void* ctx,PmSaveReceipt* r){
 Recover* c=ctx;if(!pm_save_hex(c->request,32))return fail(c,"Invalid save recovery request");
 c->root=pm_save_directory(PM_SAVE_ROOT,0);if(c->root<0)return fail(c,"Save operation records are unavailable");
 c->lock=openat(c->root,"active.lock",O_RDWR|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);struct stat st;
 if(c->lock<0||fstat(c->lock,&st)||!S_ISREG(st.st_mode)||flock(c->lock,LOCK_EX|LOCK_NB))return fail(c,"Save helper is still active or its lock is unavailable");
 if(pm_save_receipt_load(c->request,r)||memcmp(r->checksum,c->expected,32))return fail(c,"Save receipt changed; check the result again");
 PmSaveRequest active;
 if(pm_save_request_load(&active)||memcmp(&active,&r->input,sizeof active))return fail(c,"Active save request does not match this export");
 return 0;
}
static int quiescent(void* ctx,const PmSaveReceipt* r){
 Recover* c=ctx;PmSaveRequest current={0};
 if(pm_save_boot(&current,0))return fail(c,"Console boot identity is unavailable; save state remains held");
 if(current.bootSec!=r->input.bootSec||current.bootUsec!=r->input.bootUsec)return 0;
 // A queued helper without a recorded PID might still start later. Never
 // release it merely because a lock or a receipt heartbeat has gone quiet.
 if(r->pid<=0)return fail(c,"Save helper launch is unresolved; restart the console before checking this export again");
 errno=0;if(kill(r->pid,0)==0||errno!=ESRCH)return fail(c,"The save helper process may still be running; check again after it exits");
 return 0;
}
static int unmounted(void* ctx,const PmSaveReceipt* r){
 Recover* c=ctx;int stage=pm_save_child_directory(c->root,r->input.request,0);
 if(stage<0)return errno==ENOENT?0:fail(c,"Private save staging could not be inspected");
 struct stat rootStat,stageStat,mountStat;struct statfs stageFs,mountFs;
 int bad=fstat(c->root,&rootStat)||fstat(stage,&stageStat)||rootStat.st_dev!=stageStat.st_dev||fstatfs(stage,&stageFs);
 if(bad){close(stage);return fail(c,"Private staging is not on its original filesystem");}
 int mount=pm_save_child_directory(stage,"mount",0);int missing=mount<0&&errno==ENOENT;close(stage);
 if(missing)return 0;if(mount<0)return fail(c,"Staged save mount state is unavailable");
 bad=fstat(mount,&mountStat)||fstatfs(mount,&mountFs)||mountStat.st_dev!=stageStat.st_dev||
  memcmp(&mountFs.f_fsid,&stageFs.f_fsid,sizeof mountFs.f_fsid)||
  !memchr(mountFs.f_mntonname,0,sizeof mountFs.f_mntonname)||!memchr(stageFs.f_mntonname,0,sizeof stageFs.f_mntonname)||strcmp(mountFs.f_mntonname,stageFs.f_mntonname);
 close(mount);return bad?fail(c,"The staged save may still be mounted; restart the console before checking it again"):0;
}
static int progress(void* ctx,uint64_t bytes){
 RecoveryProgress* p=ctx;if(p->c->progress)p->c->progress(p->c->progressContext,bytes,p->total);return 0;
}
static int unchanged(const struct stat* a,const struct stat* b){
 return a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&a->st_size==b->st_size&&a->st_mode==b->st_mode&&
  a->st_mtim.tv_sec==b->st_mtim.tv_sec&&a->st_mtim.tv_nsec==b->st_mtim.tv_nsec&&
  a->st_ctim.tv_sec==b->st_ctim.tv_sec&&a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;
}
static int published(void* ctx,PmSaveReceipt* r){
 Recover* c=ctx;int parent=-1,fd=-1,result=-1;char name[64],base[192],expected[256];
 if(r->input.mode==PM_SAVE_CATALOG){
  parent=dup(c->root);snprintf(name,sizeof name,"%s.catalog.json",r->input.request);strcpy(base,PM_SAVE_ROOT);
 }else{
  if(r->input.destination){
   char usb[64],stamp[65];int volume=pm_save_volume_open(r->input.destination,usb,stamp);
   if(volume<0)return fail(c,"Reconnect the original USB drive before checking this export");
   if(strcmp(stamp,r->input.volume)){close(volume);return fail(c,"USB identity differs from the selected export destination");}
   int folder=pm_save_child_directory(volume,"ProsperoMenu",0);int error=errno;close(volume);
   if(folder<0)return error==ENOENT?0:fail(c,"USB export directory is unavailable");
   parent=pm_save_child_directory(folder,"Saves",0);error=errno;close(folder);
   if(parent<0)return error==ENOENT?0:fail(c,"USB save directory is unavailable");
   snprintf(base,sizeof base,"%s/ProsperoMenu/Saves/%s",usb,r->input.request);
  }else{parent=pm_save_directory("/data/ProsperoMenu/share/saves",0);snprintf(base,sizeof base,"/data/ProsperoMenu/share/saves/%s",r->input.request);}
  if(parent<0)return errno==ENOENT?0:fail(c,"Published save directory is unavailable");
  int child=pm_save_child_directory(parent,r->input.request,0);int error=errno;close(parent);parent=child;
  if(parent<0)return error==ENOENT?0:fail(c,"Export destination cannot be inspected");strcpy(name,"save.tar");
 }
 if(parent<0)return fail(c,"Save publication directory is unavailable");
 snprintf(expected,sizeof expected,"%s/%s",base,name);
 fd=openat(parent,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
 if(fd<0){if(errno==ENOENT)result=0;else fail(c,"Published archive could not be opened safely");goto done;}
 // Catalog generation may have been interrupted before recording its digest.
 // Presence of that file is never enough to claim successful completion.
 struct stat before,after,named;char digest[65];
 if(r->input.mode==PM_SAVE_CATALOG&&(!r->archiveBytes||!pm_save_hex(r->sha256,64))){result=0;goto done;}
 if(!r->archiveBytes||!pm_save_hex(r->sha256,64)||strcmp(r->output,expected)||
  fstat(fd,&before)||!S_ISREG(before.st_mode)||before.st_size<0||(uint64_t)before.st_size!=r->archiveBytes){fail(c,"Existing export lacks matching size, path or checksum evidence; files retained");goto done;}
 RecoveryProgress p={c,r->archiveBytes};
 if(c->progress)c->progress(c->progressContext,0,r->archiveBytes);
 if(pm_save_hash_file(fd,r->archiveBytes,digest,progress,&p)||strcmp(digest,r->sha256)||fstat(fd,&after)||!unchanged(&before,&after)||
  fstatat(parent,name,&named,AT_SYMLINK_NOFOLLOW)||!unchanged(&after,&named)||fsync(fd)||fsync(parent)){
  fail(c,"Published archive did not pass its full checksum, identity and storage verification");goto done;
 }
 int current=pm_save_directory(base,0);struct stat heldDirectory,namedDirectory;
 if(current<0){fail(c,"Export destination disappeared during verification");goto done;}
 int moved=fstat(parent,&heldDirectory)||fstat(current,&namedDirectory)||heldDirectory.st_dev!=namedDirectory.st_dev||heldDirectory.st_ino!=namedDirectory.st_ino;
 close(current);if(moved){fail(c,"Export destination changed during verification");goto done;}
 if(r->input.destination){char usb[64],stamp[65];if(pm_save_volume(r->input.destination,usb,stamp)||strcmp(stamp,r->input.volume)){fail(c,"USB changed during archive verification");goto done;}}
 result=1;
done:
 if(fd>=0&&close(fd)){fail(c,"Archive close failed after verification");result=-1;}
 if(close(parent)){fail(c,"Export directory close failed after verification");result=-1;}return result;
}
static int finish(void* ctx,PmSaveReceipt* r,int complete){
 Recover* c=ctx;PmSaveReceipt latest;
 if(r->input.destination){char usb[64],stamp[65];if(pm_save_volume(r->input.destination,usb,stamp)||strcmp(stamp,r->input.volume))return fail(c,"Reconnect the original USB destination before finishing recovery");}
 if(pm_save_receipt_load(c->request,&latest)||memcmp(latest.checksum,c->expected,32))return fail(c,"Save receipt changed during recovery; no result was overwritten");
 r->stage=complete?PM_SAVE_COMPLETE:PM_SAVE_REJECTED;r->clean=1;r->mounted=0;
 snprintf(r->message,sizeof r->message,"%s",complete?"Published save recovered and fully verified":"No completed export found; private staging retained. Refresh saves to start a new export.");
 if(pm_save_receipt_save(r))return fail(c,"Verified save recovery could not be committed; check the result again");
 snprintf(c->message,192,"%s",r->message);return 0;
}
static void release(void* ctx){Recover* c=ctx;if(c->lock>=0)close(c->lock);if(c->root>=0)close(c->root);}
int pm_save_recover(const char* request,const unsigned char expected[32],char message[192],PmSaveRecoveryProgress p,void* context){
 Recover c={.request=request,.expected=expected,.message=message,.root=-1,.lock=-1,.progress=p,.progressContext=context};
 message[0]=0;PmSaveRecovery flow={&c,acquire,quiescent,unmounted,published,finish,release};return pm_save_recovery_flow(&flow);
}
