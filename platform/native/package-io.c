// SPDX-License-Identifier: GPL-3.0-or-later
#include "package-io.h"
#include "sha256.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
static void digest(const void* p,size_t n,unsigned char* out){SHA256_CTX s;sha256_init(&s);sha256_update(&s,p,n);sha256_final(&s,out);}
int pm_package_open_source(const char* upload,char* path,size_t capacity){
 if(!pm_package_hex(upload,32)||capacity<1024){errno=EINVAL;return -1;}
 char reference[160];snprintf(reference,sizeof reference,"/data/ProsperoMenu/packages/%s.source",upload);
 int ref=open(reference,O_RDONLY|O_NOFOLLOW);
 if(ref<0){if(errno!=ENOENT)return -1;snprintf(path,capacity,"/data/ProsperoMenu/packages/%s.payload",upload);}
 else {
  struct stat st;size_t at=0;int bad=fstat(ref,&st)||!S_ISREG(st.st_mode)||st.st_size<=0||st.st_size>=1024;
  while(!bad&&at<(size_t)st.st_size){ssize_t n=read(ref,path+at,(size_t)st.st_size-at);if(n<0&&errno==EINTR)continue;if(n<=0)bad=1;else at+=(size_t)n;}
  close(ref);if(bad||memchr(path,0,at)){errno=EINVAL;return -1;}path[at]=0;
 }
 if(!pm_package_source_path_valid(path)){errno=EINVAL;return -1;}
 // Walk from the root using directory descriptors: no symlink traversal or
 // pathname supplied by an HTTP client. The returned fd stays open throughout.
 char parts[1024];strcpy(parts,path+1);char* next=parts;int dir=open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
 if(dir<0)return -1;
 for(;;){char* slash=strchr(next,'/');if(slash)*slash=0;
  int fd=openat(dir,next,O_RDONLY|O_NOFOLLOW|(slash?O_DIRECTORY:0)),saved=errno;close(dir);
  if(fd<0){errno=saved;return -1;}if(!slash)return fd;dir=fd;next=slash+1;
 }
}
int pm_package_directory(void){
 struct stat st;
 if(lstat("/data/ProsperoMenu",&st)||!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode))return -1;
 if(mkdir(PM_PACKAGE_ROOT,0700)&&errno!=EEXIST)return -1;
 return lstat(PM_PACKAGE_ROOT,&st)||!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode)?-1:0;
}
static int load(const char* path,void* value,size_t bytes){
 int fd=open(path,O_RDONLY|O_NOFOLLOW);if(fd<0)return -1;struct stat st;size_t at=0;
 int bad=fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size!=(off_t)bytes;
 while(!bad&&at<bytes){ssize_t n=read(fd,(unsigned char*)value+at,bytes-at);if(n<0&&errno==EINTR)continue;if(n<=0)bad=1;else at+=(size_t)n;}
 close(fd);if(bad){errno=EINVAL;return -1;}return 0;
}
static int save(const char* path,const void* value,size_t bytes){
 char temporary[256];if(snprintf(temporary,sizeof temporary,"%s.tmp",path)>=(int)sizeof temporary){errno=EINVAL;return -1;}
 int fd=open(temporary,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);if(fd<0)return -1;size_t at=0;int bad=0;
 while(at<bytes){ssize_t n=write(fd,(const unsigned char*)value+at,bytes-at);if(n<0&&errno==EINTR)continue;if(n<=0){bad=1;break;}at+=(size_t)n;}
 if(!bad)bad=fsync(fd);if(close(fd))bad=1;if(!bad)bad=rename(temporary,path);if(bad)return -1;
 fd=open(PM_PACKAGE_ROOT,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(fd<0)return -1;bad=fsync(fd);close(fd);return bad?-1:0;
}
int pm_package_request_load(PmPackageRequest* r){
 if(load(PM_PACKAGE_ROOT "/request.bin",r,sizeof *r))return -1;unsigned char sum[32];digest(r,offsetof(PmPackageRequest,checksum),sum);
 if(!pm_package_request_valid(r)||memcmp(sum,r->checksum,32)){errno=EINVAL;return -1;}return 0;
}
int pm_package_request_save(PmPackageRequest* r){
 if(!pm_package_request_valid(r)){errno=EINVAL;return -1;}digest(r,offsetof(PmPackageRequest,checksum),r->checksum);
 return save(PM_PACKAGE_ROOT "/request.bin",r,sizeof *r);
}
int pm_package_receipt_load(const char* request,PmPackageReceipt* r){
 if(!pm_package_hex(request,32)){errno=EINVAL;return -1;}char path[256];snprintf(path,sizeof path,PM_PACKAGE_ROOT "/%s.receipt",request);
 if(load(path,r,sizeof *r))return -1;unsigned char sum[32];digest(r,offsetof(PmPackageReceipt,checksum),sum);
 if(!pm_package_request_valid(&r->input)||strcmp(request,r->input.request)||r->stage<1||r->stage>PM_PKG_UNKNOWN||r->clean>1||r->accepted>1||r->observed>1||
   r->state[15]||r->message[191]||r->total>PM_PACKAGE_MAX||r->downloaded>r->total||memcmp(sum,r->checksum,32)){errno=EINVAL;return -1;}
 return 0;
}
int pm_package_receipt_save(PmPackageReceipt* r){
 r->updated=pm_package_millis();digest(r,offsetof(PmPackageReceipt,checksum),r->checksum);
 char path[256];snprintf(path,sizeof path,PM_PACKAGE_ROOT "/%s.receipt",r->input.request);return save(path,r,sizeof *r);
}
int pm_package_boot(PmPackageRequest* r,int compare){
 struct timeval boot;size_t size=sizeof boot;int mib[]={CTL_KERN,KERN_BOOTTIME};
 if(sysctl(mib,2,&boot,&size,NULL,0)||size!=sizeof boot)return -1;
 if(compare)return r->bootSec==boot.tv_sec&&r->bootUsec==boot.tv_usec?0:-1;
 r->bootSec=boot.tv_sec;r->bootUsec=boot.tv_usec;return 0;
}
uint64_t pm_package_millis(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
