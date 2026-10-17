// SPDX-License-Identifier: GPL-3.0-or-later
#include "quickjs.h"
#include "app-launch.h"
#include "native-operation.h"
#include "elfldr.h"
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
extern const unsigned char prosperomenu_launch_start[],prosperomenu_launch_end[];
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;static int running;static pid_t child;static PmAppLaunch current;
static void*worker(void*unused){(void)unused;pid_t pid=elfldr_spawn("/data/ProsperoMenu",-1,(void*)prosperomenu_launch_start,"ProsperoMenu_Launch",prosperomenu_launch_end-prosperomenu_launch_start);pthread_mutex_lock(&mutex);if(pid>0)child=pid;running=0;pm_native_release();pthread_mutex_unlock(&mutex);return 0;}
static JSValue start(JSContext*c,JSValueConst self,int argc,JSValueConst*argv){
 (void)self;if(argc!=2)return JS_ThrowTypeError(c,"Expected request and title");const char*id=JS_ToCString(c,argv[0]),*title=JS_ToCString(c,argv[1]);
 if(!pm_app_id_valid(id)||!pm_app_title_valid(title)){if(id)JS_FreeCString(c,id);if(title)JS_FreeCString(c,title);return JS_ThrowTypeError(c,"Invalid game launch identity");}
 pthread_mutex_lock(&mutex);int ok=0,claimed=0;if(child>0){int status;pid_t r=waitpid(child,&status,WNOHANG);if(r==child||(r<0&&errno==ECHILD))child=0;}
 if(running||child||pm_native_busy())goto done;
 PmAppLaunch prior={0};if(!pm_app_load(id,&prior)){ok=!strcmp(prior.title,title)&&!pm_app_boot(&prior,1);goto done;}
 if(!pm_native_claim())goto done;claimed=1;memset(&current,0,sizeof current);current.magic=0x54414c31;current.stage=1;strcpy(current.request,id);strcpy(current.title,title);strcpy(current.message,"Preparing game launch");
 if(pm_app_boot(&current,0)||pm_app_save(&current,1))goto done;
 int fd=open("/data/ProsperoMenu/launches/current",O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);if(fd<0)goto done;int bad=write(fd,id,32)!=32||fsync(fd);close(fd);if(bad)goto done;
 pthread_t thread;running=1;if(pthread_create(&thread,0,worker,0)){running=0;goto done;}pthread_detach(thread);claimed=0;ok=1;
done:
 if(claimed)pm_native_release();pthread_mutex_unlock(&mutex);JS_FreeCString(c,id);JS_FreeCString(c,title);return ok?JS_UNDEFINED:JS_ThrowInternalError(c,"Game launch is busy or its receipt could not be saved");
}
static JSValue poll(JSContext*c,JSValueConst self,int argc,JSValueConst*argv){(void)self;if(argc!=1)return JS_ThrowTypeError(c,"Expected request");const char*id=JS_ToCString(c,argv[0]);PmAppLaunch r={0};int rc=pm_app_load(id,&r);if(id)JS_FreeCString(c,id);if(rc)return JS_NULL;JSValue o=JS_NewObject(c);JS_SetPropertyStr(c,o,"request",JS_NewString(c,r.request));JS_SetPropertyStr(c,o,"titleId",JS_NewString(c,r.title));JS_SetPropertyStr(c,o,"stage",JS_NewUint32(c,r.stage));JS_SetPropertyStr(c,o,"result",JS_NewInt32(c,r.result));JS_SetPropertyStr(c,o,"appId",JS_NewInt32(c,r.appId));JS_SetPropertyStr(c,o,"currentBoot",JS_NewBool(c,!pm_app_boot(&r,1)));JS_SetPropertyStr(c,o,"message",JS_NewString(c,r.message));return o;}
void pm_app_launch_bind(JSContext*c,JSValue api){JS_SetPropertyStr(c,api,"appLaunch",JS_NewCFunction(c,start,"appLaunch",2));JS_SetPropertyStr(c,api,"appLaunchPoll",JS_NewCFunction(c,poll,"appLaunchPoll",1));}
