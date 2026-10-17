// SPDX-License-Identifier: GPL-3.0-or-later
#include "save-io.h"
#include "sha256.h"
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
int pm_save_child_directory(int fd,const char* name,int create){
 if(!pm_save_relative(name)||strchr(name,'/')){errno=EINVAL;return -1;}
 if(create&&mkdirat(fd,name,0700)&&errno!=EEXIST)return -1;
 if(create&&fsync(fd))return -1;
 return openat(fd,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
}
int pm_save_directory(const char* path,int create){
 if(!path||path[0]!='/'||strlen(path)>1023){errno=EINVAL;return -1;}
 char copy[1024];strcpy(copy,path+1);if(!pm_save_relative(copy)){errno=EINVAL;return -1;}
 int fd=open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -1;
 char* cursor=copy;for(;;){char* slash=strchr(cursor,'/');if(slash)*slash=0;
  int next=pm_save_child_directory(fd,cursor,create);close(fd);if(next<0)return -1;fd=next;if(!slash)break;cursor=slash+1;
 }return fd;
}
int pm_save_read_all(int fd,uint64_t offset,void* data,size_t bytes){
 size_t at=0;while(at<bytes){ssize_t n=pread(fd,(unsigned char*)data+at,bytes-at,(off_t)(offset+at));if(n<0&&errno==EINTR)continue;if(n<=0)return -1;at+=(size_t)n;}return 0;
}
int pm_save_write_all(void* context,const void* data,size_t bytes){
 int fd=*(int*)context;size_t at=0;while(at<bytes){ssize_t n=write(fd,(const unsigned char*)data+at,bytes-at);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;at+=(size_t)n;}return 0;
}
static void stat_hash(SHA256_CTX* hash,const struct stat* st){
 uint64_t values[]={(uint64_t)st->st_dev,(uint64_t)st->st_ino,(uint64_t)st->st_size,(uint64_t)st->st_mode,
  (uint64_t)st->st_mtim.tv_sec,(uint64_t)st->st_mtim.tv_nsec,(uint64_t)st->st_ctim.tv_sec,(uint64_t)st->st_ctim.tv_nsec};
 sha256_update(hash,(const unsigned char*)values,sizeof values);
}
void pm_save_source_close(PmSaveSource* s){if(s->image>=0)close(s->image);if(s->key>=0)close(s->key);if(s->directory>=0)close(s->directory);s->image=s->key=s->directory=-1;}
int pm_save_source_open(const PmSaveRequest* input,PmSaveSource* s){
 memset(s,0,sizeof *s);s->directory=s->image=s->key=-1;
 if(!pm_save_user(input->user)||!pm_save_slot(input->slot)||pm_save_platform(input->title)!=(int)input->platform){errno=EINVAL;return -1;}
 char path[256];snprintf(path,sizeof path,"/user/home/%s/%s/%s",input->user,input->platform==4?"savedata":"savedata_prospero",input->title);
 s->directory=pm_save_directory(path,0);if(s->directory<0)return -1;
 s->image=openat(s->directory,input->slot,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
 if(s->image<0||fstat(s->image,&s->imageStat)||!S_ISREG(s->imageStat.st_mode)||s->imageStat.st_size<0x860||(uint64_t)s->imageStat.st_size>PM_SAVE_MAX_BYTES)goto fail;
 unsigned char header=0,key[0x60];
 if(pm_save_read_all(s->image,0,&header,1)||header!=(input->platform==4?1:2))goto fail;
 if(input->platform==4){char name[132];snprintf(name,sizeof name,"%s.bin",input->slot+6);
  s->key=openat(s->directory,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
  if(s->key<0||fstat(s->key,&s->keyStat)||!S_ISREG(s->keyStat.st_mode)||s->keyStat.st_size<0x60||s->keyStat.st_size>1024*1024||pm_save_read_all(s->key,0,key,sizeof key))goto fail;
 }else if(pm_save_read_all(s->image,0x800,key,sizeof key))goto fail;
 SHA256_CTX hash;sha256_init(&hash);stat_hash(&hash,&s->imageStat);if(input->platform==4)stat_hash(&hash,&s->keyStat);
 sha256_update(&hash,(const unsigned char*)input->user,strlen(input->user));sha256_update(&hash,(const unsigned char*)input->title,9);
 sha256_update(&hash,(const unsigned char*)input->slot,strlen(input->slot));sha256_update(&hash,&header,1);sha256_update(&hash,key,sizeof key);
 unsigned char digest[32];sha256_final(&hash,digest);pm_save_hash_text(digest,s->stamp);memset(key,0,sizeof key);return 0;
fail:pm_save_source_close(s);errno=EINVAL;return -1;
}
int pm_save_source_current(const PmSaveRequest* r,const PmSaveSource* s){
 PmSaveSource current;if(pm_save_source_open(r,&current))return 0;
 int same=!strcmp(current.stamp,s->stamp)&&!strcmp(current.stamp,r->stamp);
 struct stat actual;if(fstat(s->image,&actual)||actual.st_dev!=current.imageStat.st_dev||actual.st_ino!=current.imageStat.st_ino)same=0;
 pm_save_source_close(&current);return same;
}
int pm_save_volume_open(unsigned destination,char path[64],char stamp[65]){
 if(destination<1||destination>8){errno=EINVAL;return -1;}snprintf(path,64,"/mnt/usb%u",destination-1);
 int fd=pm_save_directory(path,0);if(fd<0)return -1;struct stat st;struct statfs fs;
 int bad=fstat(fd,&st)||fstatfs(fd,&fs);
 if(bad||!memchr(fs.f_mntonname,0,sizeof fs.f_mntonname)||strcmp(fs.f_mntonname,path)||(fs.f_flags&MNT_RDONLY)||!memchr(fs.f_mntfromname,0,sizeof fs.f_mntfromname)){close(fd);errno=EINVAL;return -1;}
 SHA256_CTX hash;sha256_init(&hash);uint64_t values[]={(uint64_t)st.st_dev,(uint64_t)st.st_ino};
 sha256_update(&hash,(const unsigned char*)values,sizeof values);sha256_update(&hash,(const unsigned char*)&fs.f_fsid,sizeof fs.f_fsid);
 sha256_update(&hash,(const unsigned char*)fs.f_mntfromname,strlen(fs.f_mntfromname));unsigned char digest[32];sha256_final(&hash,digest);pm_save_hash_text(digest,stamp);return fd;
}
int pm_save_volume(unsigned destination,char path[64],char stamp[65]){int fd=pm_save_volume_open(destination,path,stamp);if(fd<0)return -1;close(fd);return 0;}
static int blob_read(const char* name,void* data,size_t bytes){
 int root=pm_save_directory(PM_SAVE_ROOT,0);if(root<0)return -1;
 int fd=openat(root,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);close(root);if(fd<0)return -1;struct stat st;
 int bad=fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size!=(off_t)bytes||pm_save_read_all(fd,0,data,bytes);close(fd);
 if(bad){errno=EINVAL;return -1;}return 0;
}
static int blob_write(const char* name,const void* data,size_t bytes){
 int root=pm_save_directory(PM_SAVE_ROOT,1);if(root<0)return -1;char temp[64];snprintf(temp,sizeof temp,"%s.tmp",name);
 // A stale temporary is disposable. Create exclusively so even a locally
 // replaced FIFO or hard link cannot be opened/truncated as a receipt.
 if(unlinkat(root,temp,0)&&errno!=ENOENT){close(root);return -1;}
 int fd=openat(root,temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0){close(root);return -1;}
 int bad=pm_save_write_all(&fd,data,bytes)||fsync(fd);if(close(fd))bad=1;
 if(!bad)bad=renameat(root,temp,root,name);if(!bad)bad=fsync(root);close(root);return bad?-1:0;
}
int pm_save_request_save(PmSaveRequest* r){
 if(!pm_save_request_valid(r)){errno=EINVAL;return -1;}pm_save_digest(r,offsetof(PmSaveRequest,checksum),r->checksum);return blob_write("active.bin",r,sizeof *r);
}
int pm_save_request_load(PmSaveRequest* r){
 if(blob_read("active.bin",r,sizeof *r))return -1;unsigned char sum[32];pm_save_digest(r,offsetof(PmSaveRequest,checksum),sum);
 if(!pm_save_request_valid(r)||memcmp(sum,r->checksum,32)){errno=EINVAL;return -1;}return 0;
}
int pm_save_receipt_save(PmSaveReceipt* r){
 if(!pm_save_request_valid(&r->input)){errno=EINVAL;return -1;}r->updated=pm_save_millis();
 pm_save_digest(r,offsetof(PmSaveReceipt,checksum),r->checksum);char name[64];snprintf(name,sizeof name,"%s.receipt",r->input.request);return blob_write(name,r,sizeof *r);
}
int pm_save_receipt_load(const char* id,PmSaveReceipt* r){
 if(!pm_save_hex(id,32)){errno=EINVAL;return -1;}char name[64];snprintf(name,sizeof name,"%s.receipt",id);if(blob_read(name,r,sizeof *r))return -1;
 unsigned char sum[32];pm_save_digest(r,offsetof(PmSaveReceipt,checksum),sum);
 if(!pm_save_request_valid(&r->input)||strcmp(id,r->input.request)||memcmp(sum,r->checksum,32)||r->stage<1||r->stage>PM_SAVE_UNKNOWN||r->clean>1||r->mounted>1||
  r->files>PM_SAVE_MAX_FILES||r->total>PM_SAVE_MAX_ARCHIVE||r->archiveBytes>PM_SAVE_MAX_ARCHIVE||r->completed>r->total||r->output[255]||r->sha256[64]||r->message[191]){errno=EINVAL;return -1;}return 0;
}
int pm_save_boot(PmSaveRequest* r,int compare){struct timeval boot;size_t n=sizeof boot;int mib[]={CTL_KERN,KERN_BOOTTIME};if(sysctl(mib,2,&boot,&n,0,0)||n!=sizeof boot||boot.tv_sec<=0||boot.tv_usec<0||boot.tv_usec>=1000000)return -1;
 if(compare)return r->bootSec==boot.tv_sec&&r->bootUsec==boot.tv_usec?0:-1;r->bootSec=boot.tv_sec;r->bootUsec=boot.tv_usec;return 0;}
uint64_t pm_save_millis(void){struct timespec now;if(clock_gettime(CLOCK_MONOTONIC,&now))return 0;return (uint64_t)now.tv_sec*1000+now.tv_nsec/1000000;}
static int transfer(int source,int target,uint64_t bytes,char sha[65],PmSaveProgress progress,void* context){
 unsigned char* buffer=malloc(256*1024);if(!buffer)return -1;SHA256_CTX hash;sha256_init(&hash);int result=-1;
 for(uint64_t at=0;at<bytes;){size_t take=bytes-at>256*1024?256*1024:(size_t)(bytes-at);
  if(pm_save_read_all(source,at,buffer,take)||(target>=0&&pm_save_write_all(&target,buffer,take)))goto done;
  sha256_update(&hash,buffer,take);at+=take;if(progress&&progress(context,at))goto done;
 }unsigned char digest[32];sha256_final(&hash,digest);pm_save_hash_text(digest,sha);result=0;
done:free(buffer);return result;
}
int pm_save_copy(int source,int target,uint64_t bytes,char sha[65],PmSaveProgress progress,void* context){return transfer(source,target,bytes,sha,progress,context);}
int pm_save_hash_file(int file,uint64_t bytes,char sha[65],PmSaveProgress progress,void* context){return transfer(file,-1,bytes,sha,progress,context);}
