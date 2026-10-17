// SPDX-License-Identifier: GPL-3.0-or-later
#include "app-launch.h"
#include "sha256.h"
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stddef.h>
int pm_app_id_valid(const char*s){if(!s||strlen(s)!=32)return 0;for(unsigned i=0;i<32;i++)if(!((s[i]>='a'&&s[i]<='f')||(s[i]>='0'&&s[i]<='9')))return 0;return 1;}
int pm_app_title_valid(const char*s){if(!s||strlen(s)!=9||(memcmp(s,"PPSA",4)&&memcmp(s,"CUSA",4)&&memcmp(s,"MOUU",4)))return 0;for(unsigned i=4;i<9;i++)if(s[i]<'0'||s[i]>'9')return 0;return 1;}
int pm_app_boot(PmAppLaunch*r,int check){struct timeval v;size_t n=sizeof v;int mib[]={CTL_KERN,KERN_BOOTTIME};if(sysctl(mib,2,&v,&n,0,0)||n!=sizeof v)return -1;if(check)return r->bootSec!=v.tv_sec||r->bootUsec!=v.tv_usec?-1:0;r->bootSec=v.tv_sec;r->bootUsec=v.tv_usec;return 0;}
static int root(void){struct stat st;if(lstat("/data/ProsperoMenu",&st)||!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode))return -1;if(mkdir("/data/ProsperoMenu/launches",0700)&&errno!=EEXIST)return -1;return open("/data/ProsperoMenu/launches",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);}
static void digest(const PmAppLaunch*r,unsigned char out[32]){SHA256_CTX h;sha256_init(&h);sha256_update(&h,r,offsetof(PmAppLaunch,checksum));sha256_final(&h,out);}
int pm_app_load(const char*id,PmAppLaunch*r){if(!pm_app_id_valid(id))return -1;int dir=root();if(dir<0)return -1;int fd=openat(dir,id,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);close(dir);if(fd<0)return -1;struct stat st;int bad=fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size!=sizeof *r;size_t at=0;while(!bad&&at<sizeof *r){ssize_t n=read(fd,(char*)r+at,sizeof *r-at);if(n<0&&errno==EINTR)continue;if(n<=0){bad=1;break;}at+=(size_t)n;}close(fd);unsigned char sum[32];digest(r,sum);return bad||r->magic!=0x54414c31||r->stage>5||r->request[32]||strcmp(id,r->request)||r->title[9]||!pm_app_title_valid(r->title)||r->message[191]||memcmp(sum,r->checksum,32)?-1:0;}
int pm_app_save(PmAppLaunch*r,int fresh){if(!pm_app_id_valid(r->request)||!pm_app_title_valid(r->title))return -1;int dir=root();if(dir<0)return -1;digest(r,r->checksum);char name[48];snprintf(name,sizeof name,"%s%s",r->request,fresh?"":".next");int fd=openat(dir,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(fd<0){close(dir);return -1;}size_t at=0;int bad=0;while(at<sizeof *r){ssize_t n=write(fd,(char*)r+at,sizeof *r-at);if(n<0&&errno==EINTR)continue;if(n<=0){bad=1;break;}at+=(size_t)n;}if(!bad)bad=fsync(fd);close(fd);if(!bad&&!fresh)bad=renameat(dir,name,dir,r->request);if(!bad)bad=fsync(dir);if(bad)unlinkat(dir,name,0);close(dir);return bad?-1:0;}
