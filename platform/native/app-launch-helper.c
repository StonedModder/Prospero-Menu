// SPDX-License-Identifier: GPL-3.0-or-later
// Launch ABI follows PS Neighborhood / Itemzflow. Runs outside ShellUI and
// outside the coordinator so temporary credentials and a watchdog stay local.
#include "app-launch.h"
#include <ps5/kernel.h>
#include <sys/file.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
// sceSystemServiceLaunchApp context, exactly as ps5-payload-dev/websrv defines
// it (sys_launch_title): zeroed, with only the foreground user id set.
typedef struct{uint32_t structsize,user_id,app_opt;uint64_t crash_report;uint32_t check_flag;} LaunchCtx;
_Static_assert(sizeof(LaunchCtx)==32,"Launch context ABI");
static int publish(PmAppLaunch*r,unsigned stage,const char*message){r->stage=stage;snprintf(r->message,sizeof r->message,"%s",message);return pm_app_save(r,0);}
// What the launch services return, in words a user can act on. Mirrors
// ps5-app-dumper's table (source/app_launch.c) so both tools say the same thing.
static const char*launch_reason(int rc){
 switch((unsigned)rc){
  case 0x8094000cu:return "The console still has this title open; close it and try again";
  case 0x80940005u:return "The console refused permission to start this title";
  case 0x80940003u:return "The console does not know this title";
  case 0x80020060u:return "The console rejected this title; it will not start from the home screen either";
  case 0x80a40009u:return "This game needs its disc; insert it and try again";
  case 0x80a40027u:return "Another disc is in the drive; swap in this game's disc";
  case 0x80a40028u:return "The console is still reading the disc; try again in a moment";
 }
 return NULL;
}
int main(void){
 signal(SIGALRM,SIG_DFL);alarm(20);
 int lock=open("/data/ProsperoMenu/launches/active.lock",O_CREAT|O_RDWR|O_NOFOLLOW,0600);if(lock<0||flock(lock,LOCK_EX|LOCK_NB))return 1;
 int fd=open("/data/ProsperoMenu/launches/current",O_RDONLY|O_NOFOLLOW);char id[33]={0};if(fd<0)return 2;ssize_t n=read(fd,id,33);close(fd);if(n!=32||!pm_app_id_valid(id))return 2;
 PmAppLaunch r={0};if(pm_app_load(id,&r)||pm_app_boot(&r,1)||r.stage!=1)return 3;r.pid=getpid();
 void*sys=dlopen("libSceSystemService.sprx",RTLD_LAZY),*usr=dlopen("libSceUserService.sprx",RTLD_LAZY);
 if(!sys||!usr){publish(&r,4,"Game-launch services could not be loaded");return 4;}
 int(*appid)(const char*)=dlsym(sys,"sceLncUtilGetAppId");int(*init)(void)=dlsym(sys,"sceLncUtilInitialize");
 int(*userinit)(const void*)=dlsym(usr,"sceUserServiceInitialize");int(*user)(uint32_t*)=dlsym(usr,"sceUserServiceGetForegroundUser");
 // Title launch follows ps5-payload-dev/websrv and ps5-app-dumper: the console
 // runs one big app at a time, so close whatever holds the slot and then launch
 // the requested title.
 int(*active)(void)=dlsym(sys,"sceSystemServiceGetAppIdOfRunningBigApp");
 int(*killapp)(int,int,int,int)=dlsym(sys,"sceSystemServiceKillApp");
 int(*launchapp)(const char*,char**,LaunchCtx*)=dlsym(sys,"sceSystemServiceLaunchApp");
 // The shell's own entry point. sceSystemServiceLaunchApp answers 0x80940005
 // for sideloaded titles; sceLncUtilLaunchApp does not, so it is tried first.
 int(*lnclaunch)(const char*,char**,LaunchCtx*)=dlsym(sys,"sceLncUtilLaunchApp");
 if(!appid||!init||!active||!killapp||(!lnclaunch&&!launchapp)){publish(&r,4,"Game-launch functions are unavailable");return 4;}
 uint64_t saved=kernel_get_ucred_authid(getpid());if(!saved||saved==UINT64_MAX){publish(&r,4,"Could not read launch credentials");return 4;}
 if(kernel_set_ucred_authid(getpid(),UINT64_C(0x4801000000000013))||kernel_get_ucred_authid(getpid())!=UINT64_C(0x4801000000000013)){kernel_set_ucred_authid(getpid(),saved);publish(&r,4,"Could not establish launch credentials");return 4;}
 uint32_t uid=0;const char*reason="Game launch was not submitted";unsigned stage=4;
 init();if(userinit)userinit(0);r.appId=appid(r.title);
 if(r.appId>=0){stage=3;reason="Game is already running";goto done;}
 if(r.appId!=-1){r.result=r.appId;reason="Could not check the selected game";goto done;}
 // A foreground user is preferred but not required: without one the service
 // falls back to its own default, and the launch context must then be NULL.
 int haveUser=user&&!user(&uid)&&uid!=0;
 int current=active();
 if(current>0){
  if(publish(&r,2,"Closing the running game"))goto done;
  if(killapp(current,-1,0,0)){reason="Could not close the running game";goto done;}
  for(unsigned i=0;i<40;i++){usleep(250000);if(active()<=0)break;}
 }
 if(publish(&r,2,"Starting game"))goto done;
 LaunchCtx ctx={0};ctx.structsize=(uint32_t)sizeof(LaunchCtx);ctx.user_id=uid;
 int result=-1;
 if(lnclaunch)result=lnclaunch(r.title,0,haveUser?&ctx:NULL);
 if(result<0&&launchapp)result=launchapp(r.title,0,haveUser?&ctx:NULL);
 r.result=result;
 if(result<0){
  // The launch service refused, so nothing was started: report a clean failure
  // with no side effects. That lets the coordinator end the operation instead of
  // reserving the game-library resource for an outcome nothing will resolve.
  const char*known=launch_reason(result);
  if(known)snprintf(r.message,sizeof r.message,"%s",known);
  else snprintf(r.message,sizeof r.message,"Console rejected the game launch (0x%x)",(unsigned)result);
  stage=4;reason=r.message;goto done;
 }
 stage=5;reason="Launch submitted; waiting for the game to appear";
 for(unsigned i=0;i<40;i++){r.appId=appid(r.title);if(r.appId>=0){stage=3;reason="Game started";break;}usleep(250000);}
done:
 {int restored=0;for(unsigned i=0;i<3;i++)if(!kernel_set_ucred_authid(getpid(),saved)&&kernel_get_ucred_authid(getpid())==saved){restored=1;break;}if(!restored){stage=5;reason="Launch helper could not verify restored credentials";}publish(&r,stage,reason);}
 return stage==3?0:1;
}
