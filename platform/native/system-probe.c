// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only native service evidence; no kernel pointers leave this adapter.
#include "quickjs.h"
#include "loader-worker.h"
#include "firmware-profiles.h"
#include "kstuff-state.h"
#include "kstuff-probe.h"
#include "game-info.h"
#include <ps5/kernel.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
_Static_assert(offsetof(struct kinfo_proc,ki_pid)==72,"Unexpected SDK process PID layout");
_Static_assert(offsetof(struct kinfo_proc,ki_comm)==447,"Unexpected SDK process name layout");
static void string(JSContext *c,JSValue o,const char *key,const char *s){JS_SetPropertyStr(c,o,key,JS_NewString(c,s));}
static void integer(JSContext *c,JSValue o,const char *key,int n){JS_SetPropertyStr(c,o,key,JS_NewInt32(c,n));}
static int port_state(unsigned short port){
  // Binding avoids sending arbitrary probe bytes into an unknown listener.
  int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return -1;
  struct sockaddr_in a={0};a.sin_len=sizeof a;a.sin_family=AF_INET;a.sin_port=htons(port);
  a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  int r=bind(fd,(struct sockaddr*)&a,sizeof a),e=errno;close(fd);
  return r==0?0:e==EADDRINUSE?1:-1;
}
static JSValue probe(JSContext *c,JSValueConst self,int argc,JSValueConst *args){
  (void)self;(void)argc;(void)args;
  JSValue out=JS_NewObject(c);
  // All calls originate on the JS thread; its loaderStart cannot race this
  // guard. The SDK kernel-copy primitive is not shared with the worker here.
  if(pm_loader_busy()){JS_SetPropertyStr(c,out,"busy",JS_TRUE);return out;}
  const SnipersFirmwareProfile *p=snipers_firmware_profile(kernel_get_fw_version());
  if(!p){JS_FreeValue(c,out);return JS_ThrowInternalError(c,"Unsupported native firmware profile");}
  string(c,out,"firmware",p->name);
  JS_SetPropertyStr(c,out,"appInfoAvailable",JS_NewBool(c,pm_game_info_available()));
  uint64_t base=KERNEL_ADDRESS_DATA_BASE;
  PmKstuffSnapshot first=pm_kstuff_read(base,p),second=pm_kstuff_read(base,p);
  int consistent=pm_kstuff_snapshot_equal(&first,&second);
  int state=consistent?pm_kstuff_classify(base,&second):PM_KSTUFF_UNKNOWN;
  JSValue kstuff=JS_NewObject(c);
  string(c,kstuff,"state",state==PM_KSTUFF_INSTALLED?"installed":state==PM_KSTUFF_UNMODIFIED?"unmodified":"unknown");
  JS_SetPropertyStr(c,kstuff,"injectionReady",JS_NewBool(c,consistent&&pm_kstuff_injection_ready(base,&second)));
  JS_SetPropertyStr(c,out,"kstuff",kstuff);
  struct timeval boot={0};size_t boot_size=sizeof boot;int bm[]={CTL_KERN,KERN_BOOTTIME};
  if(sysctl(bm,2,&boot,&boot_size,NULL,0)||boot_size!=sizeof boot){JS_FreeValue(c,out);return JS_ThrowInternalError(c,"Console boot identity unavailable");}
  char boot_id[80];snprintf(boot_id,sizeof boot_id,"%lld:%lld",(long long)boot.tv_sec,(long long)boot.tv_usec);string(c,out,"boot",boot_id);
  int mib[]={CTL_KERN,KERN_PROC,KERN_PROC_PROC,0};size_t needed=0;
  if(sysctl(mib,4,NULL,&needed,NULL,0)||needed<sizeof(int)||needed>16*1024*1024){JS_FreeValue(c,out);return JS_ThrowInternalError(c,"Process table unavailable");}
  size_t capacity=needed+needed/4;unsigned char *buffer=malloc(capacity);
  if(!buffer){JS_FreeValue(c,out);return JS_ThrowOutOfMemory(c);}
  if(sysctl(mib,4,buffer,&capacity,NULL,0)){free(buffer);JS_FreeValue(c,out);return JS_ThrowInternalError(c,"Process table changed during inspection");}
  JSValue processes=JS_NewArray(c);unsigned count=0;size_t offset=0;
  while(offset<capacity){
    int length=0;if(capacity-offset<sizeof length)goto bad;
    memcpy(&length,buffer+offset,sizeof length);
    if(length<(int)(offsetof(struct kinfo_proc,ki_comm)+COMMLEN+1)||(size_t)length>capacity-offset||count>=4096)goto bad;
    struct kinfo_proc info={0};memcpy(&info,buffer+offset,(size_t)length<sizeof info?(size_t)length:sizeof info);
    if(info.ki_pid>0){
      JSValue item=JS_NewObject(c);integer(c,item,"pid",info.ki_pid);
      info.ki_comm[COMMLEN]=0;info.ki_tdname[TDNAMLEN]=0;
      string(c,item,"name",info.ki_comm);string(c,item,"thread",info.ki_tdname);
      PmAppInfo app;
      if(!pm_game_app_info(info.ki_pid,&app)){
        string(c,item,"titleId",app.title);integer(c,item,"appId",app.appId);integer(c,item,"appInfoTitleOffset",app.titleOffset);
      }
      char generation[128];snprintf(generation,sizeof generation,"%s/%d/%lld:%lld",boot_id,info.ki_pid,(long long)info.ki_start.tv_sec,(long long)info.ki_start.tv_usec);
      string(c,item,"generation",info.ki_start.tv_sec>0 && info.ki_start.tv_usec>=0 && info.ki_start.tv_usec<1000000?generation:"");JS_SetPropertyUint32(c,processes,count++,item);
    }
    offset+=(size_t)length;
  }
  free(buffer);JS_SetPropertyStr(c,out,"processes",processes);
  JSValue ports=JS_NewObject(c);integer(c,ports,"shadowmount",port_state(10101));integer(c,ports,"payload-manager",port_state(8084));
  JS_SetPropertyStr(c,out,"ports",ports);return out;
bad:
  free(buffer);JS_FreeValue(c,processes);JS_FreeValue(c,out);return JS_ThrowInternalError(c,"Unsupported or truncated process table layout");
}
void pm_system_bind(JSContext *c,JSValue api){JS_SetPropertyStr(c,api,"systemProbe",JS_NewCFunction(c,probe,"systemProbe",0));}
