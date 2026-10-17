// SPDX-License-Identifier: GPL-3.0-or-later
#include "module-storage.h"
#include "game-source.h"
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
static int child(int parent,const char *name,int mode){
  if(mkdirat(parent,name,mode)&&errno!=EEXIST)return -1;
  int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(fd<0)return -1;
  struct stat s;if(fstat(fd,&s)){close(fd);return -1;}
  // Payloads can retain UID 1 while authorized kernel file access creates root
  // owned files. Both are normal; ownership need not equal getuid().
  if(!S_ISDIR(s.st_mode)||(s.st_mode&0022)||(s.st_uid!=0&&s.st_uid!=getuid())){close(fd);errno=EPERM;return -1;}
  if(fchmod(fd,mode)){close(fd);return -1;}
  return fd;
}
int pm_module_storage_open(int pid,const char *generation,const char *title,int platform,uint32_t sdk,char reason[192]){
  int home=-1,cache=-1,source=-1,root=-1,target=-1,mounted=0;
  char sandbox[96],from[160],to[160],variant[32];
  snprintf(variant,sizeof variant,"v1-p%d-%08x",platform,sdk);
  snprintf(from,sizeof from,"/data/ProsperoMenu/module-files/%s",variant);
  const char *error="Could not prepare plugin backing storage";
  home=open("/data/ProsperoMenu",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(home<0)goto fail;
  cache=child(home,"module-files",0700);if(cache<0)goto fail;
  source=child(cache,variant,0755);if(source<0)goto fail;
  root=pm_game_sandbox_open_path(pid,generation,title,sandbox,sizeof sandbox,reason,192);
  if(root<0){error=NULL;goto fail;}
  snprintf(to,sizeof to,"%s/prosperomenu-modules",sandbox);
  if(mkdirat(root,"prosperomenu-modules",0755)&&errno!=EEXIST)goto fail;
  target=openat(root,"prosperomenu-modules",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(target<0)goto fail;
  struct statfs fs={0};if(fstatfs(target,&fs))goto fail;
  if(strcmp(fs.f_mntonname,to)){
    if(!pm_game_identity(pid,generation,title)){error="Game ended before plugin storage was mounted";goto fail;}
    const char *v[]={"fstype","nullfs","from",from,"fspath",to};struct iovec iov[6];
    for(unsigned i=0;i<6;i++){iov[i].iov_base=(void*)v[i];iov[i].iov_len=strlen(v[i])+1;}
    int rc=(int)syscall(SYS_nmount,iov,6,MNT_RDONLY);
    if(rc){if(rc>0)errno=rc;error="Could not mount executable plugin storage";goto fail;}
    mounted=1;close(target);target=openat(root,"prosperomenu-modules",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(target<0||fstatfs(target,&fs))goto fail;
  }
  if(strcmp(fs.f_fstypename,"nullfs")||strcmp(fs.f_mntfromname,from)||strcmp(fs.f_mntonname,to)||!(fs.f_flags&MNT_RDONLY)||(fs.f_flags&MNT_NOEXEC)){
    error="Game plugin path is occupied by a different or non-executable mount";goto fail;
  }
  if(!pm_game_identity(pid,generation,title)){error="Game ended while preparing plugin storage";goto fail;}
  close(target);close(root);close(cache);close(home);return source;
fail:
  {int saved=errno;if(target>=0)close(target);if(root>=0)close(root);if(source>=0)close(source);if(cache>=0)close(cache);if(home>=0)close(home);
   if(mounted)syscall(SYS_unmount,to,0);
   if(error)snprintf(reason,192,"%s (errno %d)",error,saved);return -1;}
}
