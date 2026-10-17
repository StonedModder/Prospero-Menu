// SPDX-License-Identifier: GPL-3.0-or-later
#include "memory-inspect.h"
#include <string.h>
#include <stdio.h>
PmCheatResult pm_memory_inspect(const PmCheatOps *o,uint64_t offset,
  size_t length,unsigned char *bytes,uint64_t *base,uint64_t *size){
  PmCheatResult result={.status=PM_CHEAT_REJECTED,.restored=1,.resumed=1};
  const char *reason="Invalid memory inspection request";
  if(base)*base=0;if(size)*size=0;
  if(!o||!o->attach||!o->detach||!o->identity||!o->deadline||!o->module||
     !o->range||!o->read||!o->healthy||!base||!size||length>PM_INSPECT_MAX_BYTES||
     (length&&!bytes)||(!length&&offset))goto done;
  if(!o->identity(o->context)){reason="Game instance changed before inspection";goto done;}
  result.resumed=0;
  if(o->attach(o->context)){reason="Could not stop the verified game for inspection";goto release;}
  if(!o->identity(o->context)||!o->deadline(o->context)){
    reason="Inspection context or time budget expired";goto release;}
  if(o->module(o->context,base,size)||*base<0x10000||*base>=UINT64_C(0x800000000000)||
    !*size||*size>UINT64_C(0x800000000000)-*base||offset>*size||length>*size-offset){
    reason="Inspection extends outside a uniquely resolved module";goto release;}
  if(length&&o->range(o->context,*base+offset,length)){
    reason="Inspection crosses an unreadable or missing mapping";goto release;}
  for(size_t at=0;at<length;){
    size_t count=length-at;if(count>16384)count=16384;
    if(!o->identity(o->context)||!o->deadline(o->context)||o->read(o->context,*base+offset+at,bytes+at,count)){
      reason="Memory read did not complete within its verified game lease";goto release;}
    at+=count;
  }
  if(!o->identity(o->context)||!o->deadline(o->context)){
    reason="Game identity or inspection time budget changed";goto release;}
  result.status=PM_CHEAT_APPLIED;reason="Memory inspected and game resume confirmed";
release:
  if(!o->healthy(o->context)){result.status=PM_CHEAT_UNCERTAIN;reason="Native credentials could not be restored";}
  if(!o->detach(o->context))result.resumed=1;
  else {result.status=PM_CHEAT_UNCERTAIN;reason="Game resume was not confirmed after inspection";}
  if(!o->healthy(o->context)){result.status=PM_CHEAT_UNCERTAIN;reason="Native credentials could not be restored after resume";}
done:
  if(result.status!=PM_CHEAT_APPLIED){
    if(bytes&&length<=PM_INSPECT_MAX_BYTES)memset(bytes,0,length);
    if(base)*base=0;if(size)*size=0;
  }
  snprintf(result.reason,sizeof result.reason,"%s",reason);return result;
}
