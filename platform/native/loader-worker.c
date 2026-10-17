// SPDX-License-Identifier: GPL-3.0-or-later
// Worker owns all file bytes and never calls QuickJS. A durable spawn intent
// precedes native effects. Receipts prove dispatch, not payload initialization.
#include "loader-worker.h"
#include "elf-image.h"
#include "elfldr.h"
#include "sha256.h"
#include "kstuff-probe.h"
#include "native-operation.h"
#include "plugin-image.h"
#include "dispatch-receipt.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/user.h>
#include "process-info.h"
#include <ps5/kernel.h>
#define DISPATCH_ROOT "/data/ProsperoMenu/dispatch"
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static int busy;
static PmDispatchReceipt current;
static int hex(const char *s,size_t length){if(!s||strlen(s)!=length)return 0;for(size_t i=0;i<length;i++)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return 0;return 1;}
static void location(char path[256],const char *request,const char *suffix){snprintf(path,256,DISPATCH_ROOT "/%s%s",request,suffix);}
static int write_all(int fd,const void *data,size_t size){const uint8_t *p=data;while(size){ssize_t n=write(fd,p,size);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;p+=n;size-=n;}return 0;}
static int read_all(int fd,void *data,size_t size){uint8_t *p=data;while(size){ssize_t n=read(fd,p,size);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;p+=n;size-=n;}return 0;}
static int receipt_read(const char *request,PmDispatchReceipt *r){
  char path[256];location(path,request,".receipt");int fd=open(path,O_RDONLY|O_NOFOLLOW);if(fd<0)return -1;
  unsigned char raw[sizeof(PmDispatchReceipt)];struct stat st;
  int bad=fstat(fd,&st)||!S_ISREG(st.st_mode)||(st.st_size!=sizeof(PmDispatchReceipt)&&st.st_size!=sizeof(PmDispatchReceiptV1));
  if(!bad)bad=read_all(fd,raw,(size_t)st.st_size);close(fd);
  if(bad||pm_dispatch_decode(raw,(size_t)st.st_size,request,r)){errno=EINVAL;return -1;}
  return 0;
}
static int receipt_write(const PmDispatchReceipt *r){
  PmDispatchReceipt sealed=*r;pm_dispatch_seal(&sealed);
  char final[256],part[256];location(final,r->request,".receipt");location(part,r->request,".tmp");
  int fd=open(part,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);if(fd<0)return -1;
  int bad=write_all(fd,&sealed,sizeof sealed)||fsync(fd);if(close(fd))bad=1;
  if(!bad)bad=rename(part,final);if(bad)return -1;
  int dir=open(DISPATCH_ROOT,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(dir<0)return -1;
  bad=fsync(dir);close(dir);return bad;
}
static void publish(PmDispatchReceipt *r){pthread_mutex_lock(&mutex);current=*r;pthread_mutex_unlock(&mutex);}
static int transition(PmDispatchReceipt *r,int stage,int clean,const char *message){
  r->stage=stage;r->no_side_effects=clean;snprintf(r->message,sizeof r->message,"%s",message);
  int result=receipt_write(r);publish(r);return result;
}
static int duplicate(const PmDispatchReceipt *r){
  DIR *dir=opendir(DISPATCH_ROOT);if(!dir)return -1;struct dirent *entry;unsigned count=0;int found=0;
  while((entry=readdir(dir))){
    if(strlen(entry->d_name)!=40||strcmp(entry->d_name+32,".receipt"))continue;
    if(++count>1024){found=-1;break;}
    char id[33];memcpy(id,entry->d_name,32);id[32]=0;if(!hex(id,32)||!strcmp(id,r->request))continue;
    PmDispatchReceipt prior;
    if(receipt_read(id,&prior)){found=-1;break;}
    if(pm_dispatch_conflicts(r,&prior)){found=1;break;}
  }
  closedir(dir);return found;
}
static int supported(void){
  return snipers_firmware_profile(kernel_get_fw_version())!=NULL;
}
static int plugin_present(const char *name){
  int mib[]={CTL_KERN,KERN_PROC,KERN_PROC_PROC,0};size_t size=0;
  if(sysctl(mib,4,NULL,&size,NULL,0)||size<sizeof(int)||size>16*1024*1024)return -1;
  size+=size/4;unsigned char *table=malloc(size);if(!table)return -1;
  if(sysctl(mib,4,table,&size,NULL,0)){free(table);return -1;}
  size_t offset=0;int result=0;
  while(offset<size){
    int length=0;if(size-offset<sizeof length){result=-1;break;}memcpy(&length,table+offset,sizeof length);
    if(length<(int)(offsetof(struct kinfo_proc,ki_comm)+COMMLEN+1)||(size_t)length>size-offset){result=-1;break;}
    struct kinfo_proc info={0};memcpy(&info,table+offset,(size_t)length<sizeof info?(size_t)length:sizeof info);info.ki_comm[COMMLEN]=0;
    if(info.ki_pid>0&&!strcmp(info.ki_comm,name)){result=1;break;}offset+=(size_t)length;
  }
  free(table);return result;
}
static int capture_generation(PmDispatchReceipt *r){
  struct kinfo_proc info={0};
  if(pm_process_info(r->pid,&info)||
     info.ki_start.tv_sec<=0||info.ki_start.tv_usec<0||info.ki_start.tv_usec>=1000000)return -1;
  info.ki_comm[COMMLEN]=0;if(strcmp(info.ki_comm,r->plugin_id))return -1;
  snprintf(r->generation,sizeof r->generation,"%lld:%lld/%d/%lld:%lld",(long long)r->boot_sec,(long long)r->boot_usec,r->pid,
    (long long)info.ki_start.tv_sec,(long long)info.ki_start.tv_usec);return 0;
}
static void *run(void *unused){
  (void)unused;PmDispatchReceipt r;pthread_mutex_lock(&mutex);r=current;pthread_mutex_unlock(&mutex);
  uint8_t *data=NULL;int fd=-1;char reason[192]="Could not open the verified inbox file";
  if(!supported()){snprintf(reason,sizeof reason,"Firmware is outside ProsperoMenu's supported profiles");goto rejected;}
  char path[256];snprintf(path,sizeof path,"/data/ProsperoMenu/inbox/%s.payload",r.upload);
  // The parent is private to ProsperoMenu; do not follow a replacement symlink.
  struct stat parent;if(lstat("/data/ProsperoMenu/inbox",&parent)||!S_ISDIR(parent.st_mode)||S_ISLNK(parent.st_mode))goto rejected;
  fd=open(path,O_RDONLY|O_NOFOLLOW);if(fd<0)goto rejected;
  struct stat st;
  if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<64||st.st_size>128*1024*1024){snprintf(reason,sizeof reason,"Invalid inbox file size or type");goto rejected;}
  size_t size=(size_t)st.st_size;data=malloc(size);if(!data){snprintf(reason,sizeof reason,"Not enough memory to prepare this payload");goto rejected;}
  if(read_all(fd,data,size)){snprintf(reason,sizeof reason,"Payload could not be read completely");goto rejected;}
  close(fd);fd=-1;
  // Hash the private byte buffer that will actually execute, not a pathname.
  SHA256_CTX sha;uint8_t digest[32];char actual[65];sha256_init(&sha);sha256_update(&sha,data,size);sha256_final(&sha,digest);
  for(unsigned i=0;i<32;i++)snprintf(actual+i*2,3,"%02x",digest[i]);
  if(strcmp(actual,r.sha256)){snprintf(reason,sizeof reason,"Payload changed after inbox verification");goto rejected;}
  size_t elf_offset=0;
  if(r.plugin){
    PmPluginImage plugin;if(pm_plugin_inspect(data,size,&plugin,reason,sizeof reason))goto rejected;
    strcpy(r.plugin_id,plugin.id);strcpy(r.plugin_version,plugin.version);elf_offset=plugin.offset;
    int present=plugin_present(r.plugin_id);
    if(present){snprintf(reason,sizeof reason,present>0?"A process already owns this plugin identity":"Plugin process ownership could not be checked");goto rejected;}
  }else{
    PmElfImage image;if(pm_elf_inspect(data,size,&image,reason,sizeof reason))goto rejected;
  }
  // A payload may be dispatched again in the same boot: re-running it is the
  // point. Only unreadable dispatch history still withholds a launch.
  int collision=duplicate(&r);
  if(collision<0){snprintf(reason,sizeof reason,"Dispatch history cannot be safely reconciled");goto rejected;}
  const SnipersFirmwareProfile *profile=snipers_firmware_profile(kernel_get_fw_version());
  uint64_t base=KERNEL_ADDRESS_DATA_BASE;
  PmKstuffSnapshot hooks1=pm_kstuff_read(base,profile),hooks2=pm_kstuff_read(base,profile);
  if(!pm_kstuff_snapshot_equal(&hooks1,&hooks2)||!pm_kstuff_injection_ready(base,&hooks2)){
    snprintf(reason,sizeof reason,"Kernel hook state is unknown or changing; native dispatch was withheld");goto rejected;
  }
  if(transition(&r,PM_DISPATCH_SPAWNING,0,"Starting the verified payload in its own process")){
    snprintf(reason,sizeof reason,"Could not persist the spawn intent; nothing was launched");goto rejected;
  }
  char name[32];if(r.plugin)snprintf(name,sizeof name,"%s",r.plugin_id);else snprintf(name,sizeof name,"Tac_%.*s",16,r.request);
  r.pid=elfldr_spawn("/data/ProsperoMenu",-1,data+elf_offset,name,size-elf_offset);
  free(data);data=NULL;
  if(r.pid<=0){transition(&r,PM_DISPATCH_UNKNOWN,0,"Native launch failed after spawning began; no automatic retry was issued");goto done;}
  if(r.plugin&&capture_generation(&r)){
    transition(&r,PM_DISPATCH_UNKNOWN,0,"Plugin was dispatched but its process identity could not be confirmed; no retry was issued");goto done;
  }
  if(transition(&r,PM_DISPATCH_SPAWNED,0,"Payload dispatched; initialization is not confirmed")){
    r.stage=PM_DISPATCH_UNKNOWN;snprintf(r.message,sizeof r.message,"Payload launched but its durable receipt could not be saved");publish(&r);
  }
  goto done;
rejected:
  if(fd>=0)close(fd);free(data);transition(&r,PM_DISPATCH_FAILED,1,reason);
done:
  pthread_mutex_lock(&mutex);busy=0;pm_native_release();pthread_mutex_unlock(&mutex);return NULL;
}
int pm_loader_busy(void){return pm_native_busy();}
static int start(const char *request,const char *upload,const char *sha256,int plugin){
  if(!hex(request,32)||!hex(upload,32)||!hex(sha256,64)){errno=EINVAL;return -1;}
  pthread_mutex_lock(&mutex);
  if(busy){int same=current.plugin==(unsigned)plugin&&!strcmp(current.request,request)&&!strcmp(current.upload,upload)&&!strcmp(current.sha256,sha256);pthread_mutex_unlock(&mutex);if(same)return 0;errno=EBUSY;return -1;}
  if(mkdir(DISPATCH_ROOT,0700)&&errno!=EEXIST){pthread_mutex_unlock(&mutex);return -1;}
  struct stat st;if(lstat(DISPATCH_ROOT,&st)||!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode)){pthread_mutex_unlock(&mutex);errno=EACCES;return -1;}
  PmDispatchReceipt prior;
  if(!receipt_read(request,&prior)){
    int same=prior.plugin==(unsigned)plugin&&!strcmp(prior.upload,upload)&&!strcmp(prior.sha256,sha256);pthread_mutex_unlock(&mutex);if(same)return 0;errno=EEXIST;return -1;
  }
  if(errno!=ENOENT){pthread_mutex_unlock(&mutex);return -1;}
  memset(&current,0,sizeof current);current.magic=PM_DISPATCH_MAGIC;current.version=2;current.plugin=plugin;current.stage=PM_DISPATCH_QUEUED;current.no_side_effects=1;
  strcpy(current.request,request);strcpy(current.upload,upload);strcpy(current.sha256,sha256);
  struct timeval boot;size_t boot_size=sizeof boot;int mib[]={CTL_KERN,KERN_BOOTTIME};
  if(sysctl(mib,2,&boot,&boot_size,NULL,0)||boot_size!=sizeof boot){pthread_mutex_unlock(&mutex);return -1;}
  current.boot_sec=boot.tv_sec;current.boot_usec=boot.tv_usec;
  if(!pm_native_claim()){pthread_mutex_unlock(&mutex);errno=EBUSY;return -1;}
  if(receipt_write(&current)){pm_native_release();pthread_mutex_unlock(&mutex);return -1;}
  busy=1;pthread_t thread;pthread_attr_t attr;int result=pthread_attr_init(&attr);
  if(!result){result=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);if(!result)result=pthread_create(&thread,&attr,run,NULL);pthread_attr_destroy(&attr);}
  if(result){busy=0;pm_native_release();current.stage=PM_DISPATCH_FAILED;snprintf(current.message,sizeof current.message,"Native loader worker could not start");receipt_write(&current);}
  pthread_mutex_unlock(&mutex);if(result){errno=result;return -1;}return 0;
}
int pm_loader_start(const char *request,const char *upload,const char *sha256){return start(request,upload,sha256,0);}
int pm_plugin_start(const char *request,const char *upload,const char *sha256){return start(request,upload,sha256,1);}
int pm_loader_poll(const char *request,PmDispatchReceipt *receipt){
  if(!hex(request,32)){errno=EINVAL;return -1;}
  pthread_mutex_lock(&mutex);
  if(!strcmp(current.request,request)){*receipt=current;int active=busy;pthread_mutex_unlock(&mutex);
    if(active&&receipt->stage>PM_DISPATCH_SPAWNING)receipt->stage=PM_DISPATCH_SPAWNING;
    if(!active&&(receipt->stage==PM_DISPATCH_QUEUED||receipt->stage==PM_DISPATCH_SPAWNING))receipt->stage=PM_DISPATCH_UNKNOWN;
    return 0;
  }
  pthread_mutex_unlock(&mutex);int result=receipt_read(request,receipt);
  if(!result&&(receipt->stage==PM_DISPATCH_QUEUED||receipt->stage==PM_DISPATCH_SPAWNING)){
    receipt->stage=PM_DISPATCH_UNKNOWN;snprintf(receipt->message,sizeof receipt->message,"Previous dispatch was interrupted; its result is unknown");
  }
  return result;
}
