// SPDX-License-Identifier: GPL-3.0-or-later
#include "update-io.h"
#ifdef PM_UPDATE_IO_TEST
#include "update-io-test.h"
#else
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int directory(const char *path){struct stat st;if(mkdir(path,0700)&&errno!=EEXIST)return -1;return lstat(path,&st)||!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode)?-1:0;}
static int read_all(int fd,void *p,size_t n){uint8_t *b=p;while(n){ssize_t count=read(fd,b,n);if(count<0&&errno==EINTR)continue;if(count<=0)return -1;b+=count;n-=(size_t)count;}return 0;}
static int write_all(int fd,const void *p,size_t n){const uint8_t *b=p;while(n){ssize_t count=write(fd,b,n);if(count<0&&errno==EINTR)continue;if(count<=0)return -1;b+=count;n-=(size_t)count;}return 0;}
static int sync_dir(void){int fd=open(PM_UPDATE_ROOT,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(fd<0)return -1;int rc=fsync(fd);if(close(fd))rc=-1;return rc;}
int pm_update_lock(void){
  if(directory("/data/ProsperoMenu")||directory(PM_UPDATE_ROOT))return -1;
  int fd=open(PM_UPDATE_ROOT "/selection.lock",O_CREAT|O_RDWR|O_NOFOLLOW,0600);if(fd<0)return -1;
  struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode)||flock(fd,LOCK_EX|LOCK_NB)){close(fd);return -1;}return fd;
}
int pm_update_read(PmUpdateState *out,int *recovered){
  unsigned valid=0,present=0;PmUpdateState best;pm_update_init(&best);
  for(unsigned i=0;i<2;i++){
    char path[256];snprintf(path,sizeof path,PM_UPDATE_ROOT "/selection-%u.bin",i);
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);if(fd<0){if(errno==ENOENT)continue;return -1;}present++;
    PmUpdateState candidate;struct stat st;
    int bad=fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size!=sizeof candidate||read_all(fd,&candidate,sizeof candidate);close(fd);
    if(bad||pm_update_decode(&candidate,sizeof candidate,&candidate)||candidate.revision%2!=i)continue;
    valid++;if(candidate.revision>best.revision)best=candidate;
  }
  if(present&&!valid){errno=EINVAL;return -1;}
  *out=best;*recovered=present!=valid;return 0;
}
int pm_update_write(PmUpdateState *state){
  if(state->revision>=INT64_MAX)return -1;PmUpdateState next=*state;next.revision++;pm_update_seal(&next);
  PmUpdateState check;if(pm_update_decode(&next,sizeof next,&check))return -1;
  char final[256],part[256];snprintf(final,sizeof final,PM_UPDATE_ROOT "/selection-%u.bin",(unsigned)(next.revision%2));snprintf(part,sizeof part,"%s.part",final);
  int fd=open(part,O_CREAT|O_TRUNC|O_WRONLY|O_NOFOLLOW|O_NONBLOCK,0600);if(fd<0)return -1;
  struct stat st;int bad=fstat(fd,&st)||!S_ISREG(st.st_mode)||write_all(fd,&next,sizeof next)||fsync(fd);if(close(fd))bad=1;
  if(bad||rename(part,final)||sync_dir())return -1;*state=next;return 0;
}
int pm_update_boot(int64_t *sec,int64_t *usec){struct timeval tv;size_t size=sizeof tv;int mib[]={CTL_KERN,KERN_BOOTTIME};if(sysctl(mib,2,&tv,&size,NULL,0)||size!=sizeof tv||tv.tv_sec<=0||tv.tv_usec<0||tv.tv_usec>=1000000)return -1;*sec=tv.tv_sec;*usec=tv.tv_usec;return 0;}
int pm_update_read_image(const PmUpdateHeader *header,uint32_t fw,uint8_t **data,size_t *size){
  PmUpdateHeader h;if(pm_update_header(header,sizeof *header,&h))return -1;char hash[65],path[256];pm_update_hex(h.payload_hash,hash);snprintf(path,sizeof path,PM_UPDATE_ROOT "/%s.prosperomenu",hash);
  int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return -1;struct stat st;
  if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size!=(int64_t)(sizeof h+h.bytes)){close(fd);return -1;}
  size_t n=(size_t)st.st_size;uint8_t *bytes=malloc(n);if(!bytes){close(fd);return -1;}int bad=read_all(fd,bytes,n);close(fd);
  PmUpdateHeader observed;if(bad||pm_update_image(bytes,n,fw,&observed)||memcmp(&h,&observed,sizeof h)){free(bytes);return -1;}
  *data=bytes;*size=n;return 0;
}
int pm_update_save_image(const void *data,size_t size,uint32_t fw,PmUpdateHeader *out){
  PmUpdateHeader h;if(pm_update_image(data,size,fw,&h))return -1;char hash[65],path[256],part[256];pm_update_hex(h.payload_hash,hash);snprintf(path,sizeof path,PM_UPDATE_ROOT "/%s.prosperomenu",hash);snprintf(part,sizeof part,PM_UPDATE_ROOT "/%s.part",hash);
  struct stat st;if(!lstat(path,&st)){
    uint8_t *existing=NULL;size_t count=0;int rc=pm_update_read_image(&h,fw,&existing,&count);free(existing);if(rc)return -1;
    // The prior save may have renamed successfully but failed its directory
    // barrier. Matching visible bytes alone do not prove durable publication.
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return -1;
    rc=fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size!=(int64_t)size||fsync(fd);if(close(fd))rc=-1;
    if(rc||sync_dir())return -1;*out=h;return 0;
  }if(errno!=ENOENT)return -1;
  int fd=open(part,O_CREAT|O_TRUNC|O_WRONLY|O_NOFOLLOW|O_NONBLOCK,0600);if(fd<0)return -1;int bad=fstat(fd,&st)||!S_ISREG(st.st_mode)||write_all(fd,data,size)||fsync(fd);if(close(fd))bad=1;
  if(bad||rename(part,path)||sync_dir())return -1;*out=h;return 0;
}
int pm_update_retire_image(const PmUpdateState *s,const char *hash,uint64_t revision){
  uint8_t digest[32];if(pm_update_unhex(hash,digest)||pm_update_retirable(s,digest,revision))return -1;
  char path[256];snprintf(path,sizeof path,PM_UPDATE_ROOT "/%s.prosperomenu",hash);
  int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return errno==ENOENT?sync_dir():-1;
  struct stat st;PmUpdateHeader h,checked;
  int bad=fstat(fd,&st)||!S_ISREG(st.st_mode)||read_all(fd,&h,sizeof h);if(close(fd))bad=1;
  if(bad||pm_update_header(&h,sizeof h,&checked)||st.st_size!=(int64_t)(sizeof h+h.bytes)||memcmp(h.payload_hash,digest,32))return -1;
  // This only removes an explicitly selected content-addressed package. Other
  // uploads, journal/lock files and interrupted staging files are never swept.
  if(unlink(path)&&errno!=ENOENT)return -1;return sync_dir();
}
