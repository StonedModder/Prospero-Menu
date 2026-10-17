// SPDX-License-Identifier: GPL-3.0-or-later
// PFS mount/key ABI: n0llptr/Playstation-5-Save-Mounter; see vendor/save-provenance.json.
// Isolated process: it never writes or mounts an installed save container.
#include "save-io.h"
#include "save-flow.h"
#include "firmware-profiles.h"
#include "sha256.h"
#include <ps5/kernel.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>
#include <stdarg.h>
typedef struct { uint8_t reserved;char* budget; } MountOpt;
_Static_assert(sizeof(MountOpt)==16,"PFS mount option ABI");
typedef struct {
 PmSaveReceipt receipt;PmSaveSource source;
 int root,stage,archive,manifest,destination;int elevated,mountCalled;
 uint64_t auth,lastCheck;unsigned char caps[16];
 char stagePath[160],mountPath[192],targetPath[256],reason[160];
 uint64_t manifestBytes,archiveBytes;SHA256_CTX archiveHash;
 int (*appId)(const char*);
 void (*initMount)(MountOpt*);int (*mount)(MountOpt*,const char*,const char*,unsigned char*);
 void (*initUnmount)(unsigned char*);int (*unmount)(unsigned char*,const char*,int,int);
} Context;
static void wipe(void* data,size_t bytes){volatile unsigned char* p=data;while(bytes--)*p++=0;}
static int reason(Context* c,const char* text){snprintf(c->reason,sizeof c->reason,"%s",text);return -1;}
static int message(int fd,const char* format,...){
 char text[1024];va_list args;va_start(args,format);int n=vsnprintf(text,sizeof text,format,args);va_end(args);
 return n<0||(size_t)n>=sizeof text?-1:pm_save_write_all(&fd,text,(size_t)n);
}
static int record(void* context,unsigned stage,int clean,int mounted,const char* text){
 Context* c=context;c->receipt.stage=stage;c->receipt.clean=clean;c->receipt.mounted=mounted;
 c->receipt.error=c->reason[0]?errno:0;
 snprintf(c->receipt.message,sizeof c->receipt.message,"%s",c->reason[0]&&stage==PM_SAVE_REJECTED?c->reason:text);
 alarm(60);return pm_save_receipt_save(&c->receipt);
}
static int available(int fd,uint64_t bytes){
 struct statfs fs;if(fstatfs(fd,&fs)||fs.f_bsize<=0||fs.f_bavail<0)return 0;
 uint64_t blocks=(uint64_t)fs.f_bavail,unit=(uint64_t)fs.f_bsize;
 // Leave 16 MiB for receipts and the runtime's own state, not a source-size multiplier.
 return bytes<=UINT64_MAX-16*1024*1024&&blocks>=(bytes+16*1024*1024+unit-1)/unit;
}
static int volume_current(Context* c){
 if(!c->receipt.input.destination)return 1;char path[64],stamp[65];
 return !pm_save_volume(c->receipt.input.destination,path,stamp)&&!strcmp(stamp,c->receipt.input.volume);
}
static int target_closed(Context* c){
 if(!c->appId)return 0;alarm(12);int app=c->appId(c->receipt.input.title);alarm(60);return app==-1;
}
static int progress(void* context,uint64_t bytes){
 Context* c=context;alarm(60);c->receipt.completed=bytes;
 uint64_t now=pm_save_millis();if(!now||now<c->lastCheck)return reason(c,"Monotonic clock unavailable during export");
 if(now-c->lastCheck<1000)return 0;c->lastCheck=now;
 if(!target_closed(c))return reason(c,"Close the game before exporting its save");
 if(!volume_current(c))return reason(c,"The selected USB drive changed or disconnected");
 return pm_save_receipt_save(&c->receipt);
}
static int auth_begin(Context* c){
 alarm(12);if(kernel_get_ucred_uid(getpid())!=0||kernel_get_ucred_caps(getpid(),c->caps))return reason(c,"Save export requires root process credentials");
 c->auth=kernel_get_ucred_authid(getpid());if(!c->auth||c->auth==UINT64_MAX)return reason(c,"Could not read save-service credentials");
 c->elevated=1;unsigned char caps[16],verify[16];memcpy(caps,c->caps,16);caps[7]|=0x40;
 if(kernel_set_ucred_authid(getpid(),UINT64_C(0x4800000000000010))||kernel_set_ucred_caps(getpid(),caps)||
   kernel_get_ucred_authid(getpid())!=UINT64_C(0x4800000000000010)||kernel_get_ucred_caps(getpid(),verify)||memcmp(caps,verify,16))return reason(c,"Could not establish save-service credentials");
 return 0;
}
static int auth_end(Context* c){
 unsigned char verify[16];
 if(c->elevated){for(unsigned i=0;i<3;i++){
  int caps=kernel_set_ucred_caps(getpid(),c->caps),auth=kernel_set_ucred_authid(getpid(),c->auth);
  if(!caps&&!auth&&!kernel_get_ucred_caps(getpid(),verify)&&!memcmp(verify,c->caps,16)&&kernel_get_ucred_authid(getpid())==c->auth){c->elevated=0;break;}
 }}
 alarm(60);return c->elevated?reason(c,"Save-service credentials could not be restored"):0;
}
static int fresh_file(int parent,const char* name){return openat(parent,name,O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);}
static int prepare(void* context){
 Context* c=context;PmSaveRequest* r=&c->receipt.input;
 // The ELF process does not inherit ShellUI's service modules. Resolve each
 // function from its own loaded module, keeping handles alive until exit.
 void* system=dlopen("libSceSystemService.sprx",RTLD_LAZY);
 void* files=dlopen("libSceFsInternalForVsh.sprx",RTLD_LAZY);
 if(!system||!files)return reason(c,"Save-service modules could not be loaded");
 c->appId=dlsym(system,"sceLncUtilGetAppId");
 c->initMount=dlsym(files,"sceFsInitMountSaveDataOpt");c->mount=dlsym(files,"sceFsMountSaveData");
 c->initUnmount=dlsym(files,"sceFsInitUmountSaveDataOpt");c->unmount=dlsym(files,"sceFsUmountSaveData");
 if(!c->appId||!c->initMount||!c->mount||!c->initUnmount||!c->unmount)return reason(c,"Console save-service symbols are unavailable");
 if(!target_closed(c))return reason(c,"Close the game before exporting its save");
 if(pm_save_source_open(r,&c->source)||strcmp(r->stamp,c->source.stamp))return reason(c,"Save selection changed; refresh the save list");
 if(!volume_current(c))return reason(c,"Selected USB drive is unavailable or changed");
 if(!available(c->root,(uint64_t)c->source.imageStat.st_size))return reason(c,"Not enough console storage for a verified encrypted copy");
 if(mkdirat(c->root,r->request,0700))return reason(c,"This export already has staging data; it will not be overwritten");
 if(fsync(c->root))return reason(c,"Private staging directory could not be committed");
 c->stage=pm_save_child_directory(c->root,r->request,0);if(c->stage<0)return -1;
 snprintf(c->stagePath,sizeof c->stagePath,PM_SAVE_ROOT "/%s",r->request);
 snprintf(c->mountPath,sizeof c->mountPath,"%s/mount",c->stagePath);
 int mount=pm_save_child_directory(c->stage,"mount",1);if(mount<0)return -1;close(mount);
 c->receipt.total=(uint64_t)c->source.imageStat.st_size;return 0;
}
static int copy_image(void* context){
 Context* c=context;int fd=fresh_file(c->stage,"image");if(fd<0)return reason(c,"Cannot create encrypted staging copy");
 char copied[65],verified[65];uint64_t bytes=(uint64_t)c->source.imageStat.st_size;
 int bad=pm_save_copy(c->source.image,fd,bytes,copied,progress,c)||fsync(fd)||pm_save_hash_file(fd,bytes,verified,progress,c)||strcmp(copied,verified);
 close(fd);if(bad)return reason(c,"Encrypted staging copy failed its full checksum check");
 if(!pm_save_source_current(&c->receipt.input,&c->source)||!target_closed(c))return reason(c,"Installed save or game state changed during the copy");
 return 0;
}
static int mount_image(void* context){
 Context* c=context;unsigned char* key=calloc(1,0x100);if(!key)return -1;
 int fd=-1,bad=1;
 if(!pm_save_source_current(&c->receipt.input,&c->source)||!target_closed(c)){reason(c,"Save changed before mounting its copy");goto done;}
 if(pm_save_read_all(c->receipt.input.platform==4?c->source.key:c->source.image,c->receipt.input.platform==4?0:0x800,key,0x60))goto done;
 if(auth_begin(c))goto done;
 fd=open("/dev/pfsmgr",O_RDWR|O_CLOEXEC);if(fd<0)goto done;
 if(ioctl(fd,0xc0845302,key)<0){reason(c,"Console could not decrypt this save's sealed key");goto done;}
 close(fd);fd=-1;MountOpt opt={0};c->initMount(&opt);opt.budget="system";
 char image[192];snprintf(image,sizeof image,"%s/image",c->stagePath);
 c->mountCalled=1;int rc=c->mount(&opt,image,c->mountPath,key+0x60);c->receipt.error=rc;
 if(rc<0){reason(c,"Console could not mount the disposable save copy");goto done;}bad=0;
done:
 if(fd>=0)close(fd);wipe(key,0x100);free(key);if(auth_end(c))bad=1;return bad?-1:0;
}
static int archive_write(void* context,const void* data,size_t bytes){
 Context* c=context;
 if(c->archiveBytes>PM_SAVE_MAX_BYTES+32*1024*1024-bytes||pm_save_write_all(&c->archive,data,bytes))return -1;
 sha256_update(&c->archiveHash,data,bytes);c->archiveBytes+=bytes;return 0;
}
static int manifest_write(void* context,const void* data,size_t bytes){
 Context* c=context;if(c->manifestBytes+bytes>16*1024*1024||pm_save_write_all(&c->manifest,data,bytes))return -1;c->manifestBytes+=bytes;return 0;
}
static int unchanged(const struct stat* a,const struct stat* b){
 return a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&a->st_mode==b->st_mode&&a->st_size==b->st_size&&
 a->st_mtim.tv_sec==b->st_mtim.tv_sec&&a->st_mtim.tv_nsec==b->st_mtim.tv_nsec&&a->st_ctim.tv_sec==b->st_ctim.tv_sec&&a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;
}
static int manifest_entry(Context* c,const char* path,int directory,uint64_t bytes,const char* hash){
 char data[192];int n=snprintf(data,sizeof data,"%s{\"path\":",c->receipt.files>1?",":"");
 if(manifest_write(c,data,(size_t)n)||pm_save_json_string(manifest_write,c,path))return -1;
 n=snprintf(data,sizeof data,",\"directory\":%s,\"bytes\":%llu,\"sha256\":\"%s\"}",directory?"true":"false",(unsigned long long)bytes,hash);
 return n<0||(size_t)n>=sizeof data?-1:manifest_write(c,data,(size_t)n);
}
static int tree(Context* c,int fd,const char* prefix,unsigned depth,dev_t device){
 if(depth>32)return reason(c,"Save contains more than 32 directory levels");
 int duplicate=dup(fd);if(duplicate<0)return -1;DIR* dir=fdopendir(duplicate);if(!dir){close(duplicate);return -1;}
 struct stat before,after;int bad=fstat(fd,&before);struct dirent* entry;
 while(!bad){
  errno=0;entry=readdir(dir);if(!entry){if(errno)bad=-1;break;}
  if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
  char path[1024];int n=snprintf(path,sizeof path,"%s/%s",prefix,entry->d_name);
  if(n<0||(size_t)n>=sizeof path||!pm_save_relative(path)||c->receipt.files>=PM_SAVE_MAX_FILES){bad=reason(c,"Save contains unsupported paths or too many files");break;}
  struct stat st,opened;int child=-1;
  if(fstatat(fd,entry->d_name,&st,AT_SYMLINK_NOFOLLOW)||st.st_dev!=device||(!S_ISREG(st.st_mode)&&!S_ISDIR(st.st_mode))){bad=reason(c,"Save contains a link, special file or nested mount");break;}
  int directory=S_ISDIR(st.st_mode);uint64_t bytes=directory?0:(uint64_t)st.st_size;
  if(st.st_size<0||bytes>PM_SAVE_MAX_BYTES-c->receipt.total||!available(c->stage,bytes+8192)){bad=reason(c,"Save exceeds the export limit or available console storage");break;}
  child=openat(fd,entry->d_name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC|(directory?O_DIRECTORY:0));
  if(child<0||fstat(child,&opened)||!unchanged(&st,&opened)){if(child>=0)close(child);bad=-1;break;}
  c->receipt.files++;c->receipt.total+=bytes;char hash[65]={0};
  bad=pm_save_tar_header(archive_write,c,path,bytes,directory);
  if(!bad&&!directory){
   SHA256_CTX h;sha256_init(&h);unsigned char* buffer=malloc(256*1024);if(!buffer)bad=-1;
   for(uint64_t at=0;!bad&&at<bytes;){
    size_t take=bytes-at>256*1024?256*1024:(size_t)(bytes-at);
    if(pm_save_read_all(child,at,buffer,take)||archive_write(c,buffer,take)){bad=-1;break;}
    sha256_update(&h,buffer,take);at+=take;if(progress(c,c->receipt.completed+take))bad=-1;
   }
   free(buffer);if(!bad){unsigned char digest[32];sha256_final(&h,digest);pm_save_hash_text(digest,hash);bad=pm_save_tar_padding(archive_write,c,bytes);}
  }
  if(!bad)bad=manifest_entry(c,path,directory,bytes,hash);
  if(!bad&&directory)bad=tree(c,child,path,depth+1,device);
  if(!bad&&(fstat(child,&opened)||!unchanged(&st,&opened)))bad=reason(c,"Mounted save changed during extraction");
  close(child);if(!bad)bad=progress(c,c->receipt.completed);
 }
 if(!bad&&(fstat(fd,&after)||!unchanged(&before,&after)))bad=reason(c,"Mounted directory changed during extraction");
 closedir(dir);return bad;
}
static int extract(void* context){
 Context* c=context;c->receipt.total=c->receipt.completed=0;c->archiveBytes=c->manifestBytes=0;sha256_init(&c->archiveHash);
 c->archive=fresh_file(c->stage,"archive.tar");c->manifest=fresh_file(c->stage,"manifest.json");
 if(c->archive<0||c->manifest<0)return reason(c,"Cannot create private export archive");
 char head[384];int n=snprintf(head,sizeof head,"{\"format\":\"ProsperoMenu decrypted save\",\"version\":1,\"platform\":%u,\"titleId\":\"%s\",\"slot\":\"%s\",\"entries\":[",c->receipt.input.platform,c->receipt.input.title,c->receipt.input.slot);
 if(n<0||(size_t)n>=sizeof head||manifest_write(c,head,(size_t)n)||pm_save_tar_header(archive_write,c,"files",0,1))return -1;
 int mount=pm_save_directory(c->mountPath,0);struct statfs fs;struct stat st;
 if(mount<0)return -1;int bad=fstatfs(mount,&fs)||fstat(mount,&st)||!memchr(fs.f_mntonname,0,sizeof fs.f_mntonname)||strcmp(fs.f_mntonname,c->mountPath);
 if(!bad)bad=tree(c,mount,"files",0,st.st_dev);close(mount);if(bad)return -1;
 if(manifest_write(c,"]}\n",3)||fsync(c->manifest)||pm_save_tar_header(archive_write,c,"manifest.json",c->manifestBytes,0))return -1;
 unsigned char buffer[16384];for(uint64_t at=0;at<c->manifestBytes;){size_t take=c->manifestBytes-at>sizeof buffer?sizeof buffer:(size_t)(c->manifestBytes-at);
  if(pm_save_read_all(c->manifest,at,buffer,take)||archive_write(c,buffer,take))return -1;at+=take;
 }
 if(pm_save_tar_padding(archive_write,c,c->manifestBytes)||pm_save_tar_end(archive_write,c)||fsync(c->archive))return -1;
 unsigned char digest[32];sha256_final(&c->archiveHash,digest);char expected[65],actual[65];pm_save_hash_text(digest,expected);
 c->receipt.total=c->archiveBytes;c->receipt.completed=0;
 // Archive overhead is bounded independently from the 32-GiB file-data limit.
 if(pm_save_hash_file(c->archive,c->archiveBytes,actual,progress,c)||strcmp(actual,expected)||!pm_save_source_current(&c->receipt.input,&c->source))return reason(c,"Archive checksum or original save identity changed");
 c->receipt.archiveBytes=c->archiveBytes;strcpy(c->receipt.sha256,actual);return 0;
}
static int unmount_image(void* context){
 Context* c=context;
 if(!c->mountCalled)return c->elevated?auth_end(c):0;
 int fd=pm_save_directory(c->mountPath,0);struct statfs fs;
 if(fd<0)return -1;int bad=fstatfs(fd,&fs)||!memchr(fs.f_mntonname,0,sizeof fs.f_mntonname);close(fd);if(bad)return -1;
 if(strcmp(fs.f_mntonname,c->mountPath))return c->elevated?auth_end(c):0;
 if(c->elevated&&auth_end(c))return -1;
 if(auth_begin(c)){auth_end(c);return -1;}
 unsigned char opt[16]={0};c->initUnmount(opt);int rc=c->unmount(opt,c->mountPath,0,0),restored=auth_end(c);c->receipt.error=rc;
 if(rc<0||restored)return -1;
 fd=pm_save_directory(c->mountPath,0);if(fd<0)return -1;bad=fstatfs(fd,&fs)||!memchr(fs.f_mntonname,0,sizeof fs.f_mntonname)||!strcmp(fs.f_mntonname,c->mountPath);close(fd);return bad?-1:0;
}
static int publish_archive(void* context){
 Context* c=context;PmSaveRequest* r=&c->receipt.input;
 if(!target_closed(c)||!pm_save_source_current(r,&c->source)||!volume_current(c))return reason(c,"Source or destination changed before export publication");
 // The copied encrypted image is no longer mounted. Free that space before
 // copying to USB; the installed source was never opened for writing.
 if(unlinkat(c->stage,"image",0)||fsync(c->stage))return -1;
 char base[160];int parent=-1;
 if(r->destination){
  char usb[64],stamp[65];int volume=pm_save_volume_open(r->destination,usb,stamp);
  if(volume<0)return -1;if(strcmp(stamp,r->volume)){close(volume);return -1;}
  // Hold the checked mount descriptor while creating children. A disconnect
  // cannot redirect mkdir to an unmounted /mnt/usbN placeholder on root.
  int folder=pm_save_child_directory(volume,"ProsperoMenu",1);close(volume);if(folder<0)return -1;
  parent=pm_save_child_directory(folder,"Saves",1);close(folder);snprintf(base,sizeof base,"%s/ProsperoMenu/Saves",usb);
 }else{strcpy(base,"/data/ProsperoMenu/share/saves");parent=pm_save_directory(base,1);}
 if(parent<0)return -1;
 if(!volume_current(c)||mkdirat(parent,r->request,0700)||fsync(parent)){close(parent);return -1;}
 c->destination=pm_save_child_directory(parent,r->request,0);close(parent);if(c->destination<0)return -1;
 snprintf(c->targetPath,sizeof c->targetPath,"%s/%s/save.tar",base,r->request);
 strcpy(c->receipt.output,c->targetPath);if(pm_save_receipt_save(&c->receipt))return -1;
 if(!r->destination){
  // Both locations are under /data; rename publishes the already read-back
  // verified archive without an unnecessary second full-sized local copy.
  if(renameat(c->stage,"archive.tar",c->destination,"save.tar")||fsync(c->destination))return -1;
 }else{
  if(!available(c->destination,c->archiveBytes))return reason(c,"Not enough space on the selected USB drive");
  int out=fresh_file(c->destination,"save.partial");if(out<0)return -1;char copied[65],verified[65];
  c->receipt.total=c->archiveBytes;c->receipt.completed=0;
  int bad=pm_save_copy(c->archive,out,c->archiveBytes,copied,progress,c)||fsync(out)||pm_save_hash_file(out,c->archiveBytes,verified,progress,c)||strcmp(copied,c->receipt.sha256)||strcmp(verified,c->receipt.sha256)||!volume_current(c);
  close(out);if(bad)return -1;
  if(renameat(c->destination,"save.partial",c->destination,"save.tar")||fsync(c->destination))return -1;
 }
 if(!volume_current(c))return -1;
 char published[256];snprintf(published,sizeof published,"%s/%s",base,r->request);
 int named=pm_save_directory(published,0);struct stat held,observed,archive;
 if(named<0)return -1;int bad=fstat(c->destination,&held)||fstat(named,&observed)||held.st_dev!=observed.st_dev||held.st_ino!=observed.st_ino||
  fstatat(named,"save.tar",&archive,AT_SYMLINK_NOFOLLOW)||!S_ISREG(archive.st_mode)||(uint64_t)archive.st_size!=c->archiveBytes;
 close(named);return bad?-1:0;
}
static void discard(void* context){
 Context* c=context;if(c->stage<0)return;
 // Fixed names only, after proven unmount. Never recursively delete a path
 // that may still contain a mounted filesystem or caller-selected content.
 unlinkat(c->stage,"image",0);unlinkat(c->stage,"archive.tar",0);unlinkat(c->stage,"manifest.json",0);unlinkat(c->stage,"mount",AT_REMOVEDIR);
 fsync(c->stage);unlinkat(c->root,c->receipt.input.request,AT_REMOVEDIR);
}
static int catalog(Context* c){
 char name[64];snprintf(name,sizeof name,"%s.catalog.json",c->receipt.input.request);int out=fresh_file(c->root,name);
 if(out<0)return -1;int home=pm_save_directory("/user/home",0);DIR* users=home>=0?fdopendir(home):NULL;
 if(!users){if(home>=0)close(home);close(out);return -1;}
 unsigned count=0,skipped=0,visited=0;int bad=message(out,"{\"version\":1,\"saves\":[");struct dirent* user;
 while(!bad){
  errno=0;user=readdir(users);if(!user){if(errno)bad=-1;break;}if(!pm_save_user(user->d_name))continue;
  if(++visited>65536){bad=-1;break;}
  for(unsigned platform=4;platform<=5&&!bad;platform++){
   char path[128];snprintf(path,sizeof path,"/user/home/%s/%s",user->d_name,platform==4?"savedata":"savedata_prospero");
   int fd=pm_save_directory(path,0);if(fd<0){if(errno!=ENOENT)skipped++;continue;}DIR* titles=fdopendir(fd);if(!titles){close(fd);skipped++;continue;}struct dirent* title;
   while(!bad){
    errno=0;title=readdir(titles);if(!title){if(errno)bad=-1;break;}if(pm_save_platform(title->d_name)!=(int)platform)continue;
    int t=pm_save_child_directory(dirfd(titles),title->d_name,0);DIR* slots=t>=0?fdopendir(t):NULL;
    if(!slots){if(t>=0)close(t);skipped++;continue;}struct dirent* slot;
    while(!bad){
     errno=0;slot=readdir(slots);if(!slot){if(errno)bad=-1;break;}
     if(++visited>65536){bad=-1;break;}if(!pm_save_slot(slot->d_name))continue;
     PmSaveRequest r={.platform=platform};strcpy(r.user,user->d_name);strcpy(r.title,title->d_name);strcpy(r.slot,slot->d_name);
     PmSaveSource source;if(pm_save_source_open(&r,&source)){skipped++;continue;}
     if(count>=PM_SAVE_MAX_CATALOG)bad=reason(c,"Save catalog exceeds 2048 entries");
     else bad=message(out,"%s{\"user\":\"%s\",\"titleId\":\"%s\",\"slot\":\"%s\",\"platform\":%u,\"bytes\":%llu,\"stamp\":\"%s\"}",count?",":"",r.user,r.title,r.slot,platform,(unsigned long long)source.imageStat.st_size,source.stamp);
     pm_save_source_close(&source);count++;alarm(60);
    }closedir(slots);
   }closedir(titles);
  }
 }closedir(users);
 if(!bad)bad=message(out,"],\"skipped\":%u,\"volumes\":[",skipped);unsigned volumes=0;
 for(unsigned i=1;i<=8&&!bad;i++){
  char path[64],stamp[65];if(pm_save_volume(i,path,stamp))continue;
  int fd=pm_save_directory(path,0);struct statfs fs;if(fd<0)continue;int invalid=fstatfs(fd,&fs);close(fd);if(invalid||fs.f_bsize<=0||fs.f_bavail<0)continue;
  bad=message(out,"%s{\"destination\":%u,\"path\":\"%s\",\"stamp\":\"%s\",\"freeBytes\":%.0f}",volumes++?",":"",i,path,stamp,(double)fs.f_bavail*fs.f_bsize);
 }
 if(!bad)bad=message(out,"]}\n");if(!bad)bad=fsync(out);
 struct stat st;if(!bad)bad=fstat(out,&st);if(!bad)bad=pm_save_hash_file(out,(uint64_t)st.st_size,c->receipt.sha256,NULL,NULL);
 close(out);if(bad){unlinkat(c->root,name,0);return -1;}
 snprintf(c->receipt.output,sizeof c->receipt.output,PM_SAVE_ROOT "/%s",name);c->receipt.archiveBytes=(uint64_t)st.st_size;c->receipt.files=count;
 return record(c,PM_SAVE_COMPLETE,1,0,"Save list refreshed");
}
int main(void){
 signal(SIGPIPE,SIG_IGN);signal(SIGALRM,SIG_DFL);alarm(60);
 Context c={.root=-1,.stage=-1,.archive=-1,.manifest=-1,.destination=-1,.source={.directory=-1,.image=-1,.key=-1}};
 c.root=pm_save_directory(PM_SAVE_ROOT,0);if(c.root<0)return 1;
 int lock=openat(c.root,"active.lock",O_CREAT|O_RDWR|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600);struct stat st;
 if(lock<0||fstat(lock,&st)||!S_ISREG(st.st_mode)||flock(lock,LOCK_EX|LOCK_NB))return 2;
 PmSaveRequest input;if(pm_save_request_load(&input)||pm_save_boot(&input,1)||pm_save_receipt_load(input.request,&c.receipt)||memcmp(&input,&c.receipt.input,sizeof input)||c.receipt.stage!=PM_SAVE_QUEUED||c.receipt.clean)return 3;
 c.receipt.pid=getpid();int result=-1;
 if(!snipers_firmware_profile(kernel_get_fw_version())||kernel_get_ucred_uid(getpid())!=0){record(&c,PM_SAVE_REJECTED,1,0,"Save helper requires supported firmware and root credentials");goto done;}
 if(input.mode==PM_SAVE_CATALOG){
  if(record(&c,PM_SAVE_PREPARING,1,0,"Reading local save containers"))goto done;
  result=catalog(&c);if(result)record(&c,PM_SAVE_REJECTED,1,0,"Save catalog could not be read completely");
 }else{
  PmSaveFlow flow={&c,record,prepare,copy_image,mount_image,extract,unmount_image,publish_archive,discard};result=pm_save_export_flow(&flow);
 }
done:
 if(c.elevated)auth_end(&c);pm_save_source_close(&c.source);
 if(c.archive>=0)close(c.archive);if(c.manifest>=0)close(c.manifest);if(c.destination>=0)close(c.destination);if(c.stage>=0)close(c.stage);close(lock);close(c.root);return result?1:0;
}
