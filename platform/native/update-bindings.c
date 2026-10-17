// SPDX-License-Identifier: GPL-3.0-or-later
#include "quickjs.h"
#include "update-io.h"
#include "firmware-profiles.h"
#include "update-source.h"
#include <ps5/kernel.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static JSValue error(JSContext *c,const char *code,const char *message){JSValue e=JS_NewError(c);JS_SetPropertyStr(c,e,"code",JS_NewString(c,code));JS_SetPropertyStr(c,e,"message",JS_NewString(c,message));return JS_Throw(c,e);}
static JSValue image(JSContext *c,const PmUpdateHeader *h){if(!h->bytes)return JS_NULL;JSValue v=JS_NewObject(c);char hash[65];pm_update_hex(h->payload_hash,hash);JS_SetPropertyStr(c,v,"sha256",JS_NewString(c,hash));pm_update_hex(h->source_hash,hash);JS_SetPropertyStr(c,v,"source",JS_NewString(c,hash));JS_SetPropertyStr(c,v,"label",JS_NewString(c,h->label));JS_SetPropertyStr(c,v,"bytes",JS_NewInt64(c,h->bytes));JS_SetPropertyStr(c,v,"firmwareMask",JS_NewUint32(c,h->firmware_mask));
  JS_SetPropertyStr(c,v,"formatVersion",JS_NewUint32(c,h->version));JSValue profiles=JS_NewArray(c);uint32_t index=0;
  for(unsigned i=0;i<sizeof snipers_firmware_profiles/sizeof snipers_firmware_profiles[0];i++)
    if(pm_update_supported(h,snipers_firmware_profiles[i].code))JS_SetPropertyUint32(c,profiles,index++,JS_NewString(c,snipers_firmware_profiles[i].name));
  JS_SetPropertyStr(c,v,"profiles",profiles);return v;}
static JSValue snapshot(JSContext *c,const PmUpdateState *s,int recovered){
  static const char *phases[]={"idle","staged","armed","trial"};char revision[32];snprintf(revision,sizeof revision,"%llu",(unsigned long long)s->revision);JSValue v=JS_NewObject(c);
  int64_t sec=0,usec=0;int managed=!pm_update_boot(&sec,&usec)&&s->boot_sec==sec&&s->boot_usec==usec&&s->pid==getpid();
  JS_SetPropertyStr(c,v,"revision",JS_NewString(c,revision));JS_SetPropertyStr(c,v,"phase",JS_NewString(c,phases[s->phase]));JS_SetPropertyStr(c,v,"recovered",JS_NewBool(c,recovered));JS_SetPropertyStr(c,v,"managed",JS_NewBool(c,managed));
  JS_SetPropertyStr(c,v,"good",image(c,&s->good));JS_SetPropertyStr(c,v,"previous",image(c,&s->previous));JS_SetPropertyStr(c,v,"candidate",image(c,&s->candidate));JS_SetPropertyStr(c,v,"attempt",image(c,&s->attempt));
  JS_SetPropertyStr(c,v,"rolledBack",JS_NewBool(c,s->attempt_kind==PM_BOOT_ROLLBACK));return v;
}
static int read_header(const char *hash,PmUpdateHeader *out){
  uint8_t digest[32];if(pm_update_unhex(hash,digest))return -1;char path[256];snprintf(path,sizeof path,PM_UPDATE_ROOT "/%s.prosperomenu",hash);int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return -1;
  struct stat st;PmUpdateHeader h;size_t at=0;int bad=fstat(fd,&st)||!S_ISREG(st.st_mode);
  while(!bad&&at<sizeof h){ssize_t n=read(fd,(uint8_t*)&h+at,sizeof h-at);if(n<0&&errno==EINTR)continue;if(n<=0){bad=1;break;}at+=(size_t)n;}close(fd);
  if(bad||pm_update_header(&h,sizeof h,out)||st.st_size!=(int64_t)(sizeof h+h.bytes)||memcmp(h.payload_hash,digest,32)||!pm_update_supported(&h,kernel_get_fw_version()))return -1;return 0;
}
static JSValue command(JSContext *c,JSValueConst self,int argc,JSValueConst *args,int op){
  (void)self;uint64_t revision=0;const char *text=NULL,*hash=NULL;
  if((op>=1&&op<=3)||op==5||op==6){
    if(argc!=(op==1||op==6?2:1))return JS_ThrowTypeError(c,"Expected update selection revision");
    text=JS_ToCString(c,args[op==1||op==6?1:0]);if(!text)return JS_EXCEPTION;
    int valid=*text!=0;for(const char *p=text;*p;p++)if(*p<'0'||*p>'9')valid=0;errno=0;char *end=NULL;revision=strtoull(text,&end,10);valid=valid&&!errno&&end&&!*end&&revision<=INT64_MAX;JS_FreeCString(c,text);
    if(!valid)return JS_ThrowTypeError(c,"Invalid update selection revision");
  }
  int lock=pm_update_lock();if(lock<0)return error(c,"UPDATE_BUSY","Update selection is busy or unavailable.");
  PmUpdateState s;int recovered=0;if(pm_update_read(&s,&recovered)){close(lock);return error(c,"UPDATE_STATE","Saved update selection is unreadable. Preserve its files before repair.");}
  if(op&&recovered){close(lock);return error(c,"UPDATE_STATE","An update snapshot is corrupt. No selection was changed.");}
  int result=0,changed=op!=0&&op!=6;
  if(op==1){hash=JS_ToCString(c,args[0]);PmUpdateHeader h;result=!hash||read_header(hash,&h)||pm_update_stage(&s,&h,revision);if(hash)JS_FreeCString(c,hash);}
  else if(op==2)result=pm_update_arm(&s,revision);
  else if(op==3)result=pm_update_revert(&s,revision);
  else if(op==5)result=pm_update_discard(&s,revision);
  else if(op==6){hash=JS_ToCString(c,args[0]);result=!hash||pm_update_retire_image(&s,hash,revision);if(hash)JS_FreeCString(c,hash);}
  else if(op==4){int64_t sec,usec;uint8_t source[32];result=pm_update_boot(&sec,&usec)||pm_update_unhex(PROSPEROMENU_SOURCE_HASH,source);
    if(!result&&s.pid!=getpid())changed=0;
    else if(!result)result=pm_update_ack(&s,sec,usec,getpid(),source);
  }
  if(!result&&changed)result=pm_update_write(&s);close(lock);
  if(result)return op==6?error(c,"UPDATE_RETIRE","Package removal could not be confirmed. It may be retained by startup or the selection may have changed. Refresh retained builds before retrying."):
    error(c,"UPDATE_SELECTION","The update selection changed or could not be saved. Refresh before retrying.");
  return snapshot(c,&s,recovered);
}
void pm_update_bind(JSContext *c,JSValue api){const char *names[]={"updateStatus","updateStage","updateArm","updateRevert","updateAcknowledge","updateDiscard","updateRetire"};for(int i=0;i<7;i++)JS_SetPropertyStr(c,api,names[i],JS_NewCFunctionMagic(c,command,names[i],i==1||i==6?2:i==2||i==3||i==5?1:0,JS_CFUNC_generic_magic,i));}
