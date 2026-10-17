// SPDX-License-Identifier: GPL-3.0-or-later
#include "cheat-transaction.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
static uint32_t u32(const unsigned char *p){uint32_t n;memcpy(&n,p,4);return n;}
static uint64_t u64(const unsigned char *p){uint64_t n;memcpy(&n,p,8);return n;}
int pm_cheat_plan(const void *input,size_t length,PmCheatPlan *out){
  if(!out)return -1;memset(out,0,sizeof *out);
  if(!input||length<16||length>16+PM_CHEAT_MAX_PATCHES*20+PM_CHEAT_MAX_BYTES*2)return -1;
  const unsigned char *p=input;unsigned count=u32(p+8),version=u32(p+4);
  if(u32(p)!=PM_CHEAT_PLAN_MAGIC||(version<1||version>3)||!count||count>PM_CHEAT_MAX_PATCHES||u32(p+12)!=length)return -1;
  size_t at=16,total=0;
  for(unsigned i=0;i<count;i++){
    unsigned header=version==3?20:16;if(length-at<header)return -1;
    uint64_t offset=u64(p+at);uint32_t before=u32(p+at+8),after=version==3?u32(p+at+12):before,mode=u32(p+at+(version==3?16:12));at+=header;
    uint32_t n=before>after?before:after;
    if(mode>(version>=2?1U:0U)||!before||!after||n>4096||length-at<(size_t)before+after||offset>UINT64_MAX-n||total>PM_CHEAT_MAX_BYTES-n)return -1;
    if(mode&&(offset<0x10000||offset>=UINT64_C(0x800000000000)||n>UINT64_C(0x800000000000)-offset))return -1;
    out->patches[i]=(PmCheatPatch){.offset=offset,.size=n,.absolute=mode,.beforeSize=before,.afterSize=after,.before=p+at,.after=p+at+before};total+=n;at+=(size_t)before+after;
  }
  if(at!=length)return -1;
  // Only offsets in the same addressing mode can be compared before the module
  // is resolved. Mixed-mode aliases are checked again after verified attach.
  for(unsigned i=1;i<count;i++){PmCheatPatch value=out->patches[i];unsigned j=i;
    while(j&&(out->patches[j-1].absolute>value.absolute||(out->patches[j-1].absolute==value.absolute&&out->patches[j-1].offset>value.offset))){out->patches[j]=out->patches[j-1];j--;}out->patches[j]=value;}
  for(unsigned i=1;i<count;i++)if(out->patches[i-1].absolute==out->patches[i].absolute&&out->patches[i-1].offset+out->patches[i-1].size>out->patches[i].offset)return -1;
  out->count=count;out->bytes=total;return 0;
}
PmCheatResult pm_cheat_apply(PmCheatPlan *plan,const PmCheatOps *o){
  PmCheatResult result={.status=PM_CHEAT_REJECTED,.restored=1};unsigned touched=0;unsigned char check[4096],*original=NULL;size_t snapshotBytes=0;
  const char *reason="Invalid native cheat transaction";int attempted=0;
  if(!plan||!plan->count||plan->count>PM_CHEAT_MAX_PATCHES||!o||!o->attach||!o->detach||!o->identity||!o->deadline||
     !o->module||!o->range||!o->read||!o->write||!o->journal||!o->healthy)goto done;
  if(!plan->bytes||plan->bytes>PM_CHEAT_MAX_BYTES)goto done;
  original=malloc(plan->bytes);if(!original){reason="Not enough memory for complete cheat rollback snapshots";goto done;}
  if(!o->identity(o->context)){reason="The target game instance changed before patching";goto done;}
  attempted=1;if(o->attach(o->context)){reason="The game could not be held in a verified stopped state";goto finish;}
  if(!o->identity(o->context)){reason="Game identity changed while stopping the process";goto finish;}
  uint64_t base=0,size=0;
  if(o->module(o->context,&base,&size)||base<0x10000||base>=UINT64_C(0x800000000000)||!size||size>UINT64_C(0x800000000000)-base){
    reason="The target module could not be uniquely resolved";goto finish;}
  for(unsigned i=0;i<plan->count;i++){
    PmCheatPatch *p=&plan->patches[i];
    if(!p->before||!p->after||!p->size||p->size>sizeof check||p->absolute>1||!p->beforeSize||!p->afterSize||p->size!=(p->beforeSize>p->afterSize?p->beforeSize:p->afterSize)){reason="Invalid patch record";goto finish;}
    if(p->absolute){
      if(p->offset<0x10000||p->offset>=UINT64_C(0x800000000000)||p->size>UINT64_C(0x800000000000)-p->offset){reason="Absolute patch is outside user process memory";goto finish;}
      p->address=p->offset;
    }else{
      if(p->offset>size||p->size>size-p->offset){reason="A cheat patch extends outside its loaded module";goto finish;}
      p->address=base+p->offset;
    }
  }
  for(unsigned i=1;i<plan->count;i++){PmCheatPatch value=plan->patches[i];unsigned j=i;
    while(j&&plan->patches[j-1].address>value.address){plan->patches[j]=plan->patches[j-1];j--;}plan->patches[j]=value;}
  for(unsigned i=1;i<plan->count;i++)if(plan->patches[i-1].address+plan->patches[i-1].size>plan->patches[i].address){reason="Resolved cheat patches overlap; no bytes written";goto finish;}
  for(unsigned i=0;i<plan->count;i++){
    PmCheatPatch *p=&plan->patches[i];
    if(!o->deadline(o->context)){reason="Cheat preflight exceeded its stopped-game time budget";goto finish;}
    if(o->range(o->context,p->address,p->size)){reason="A patch crosses an unavailable mapping or a stopped thread's current instruction";goto finish;}
    if(p->size>plan->bytes-snapshotBytes){reason="Invalid snapshot storage size";goto finish;}
    p->originalAt=snapshotBytes;snapshotBytes+=p->size;
    if(o->read(o->context,p->address,original+p->originalAt,p->size)||memcmp(original+p->originalAt,p->before,p->beforeSize)){
      reason="Game bytes do not match the cheat's expected state; check version or conflicting mods";goto finish;}
  }
  if(!o->identity(o->context)||!o->deadline(o->context)){reason="Game context or stopped-game time budget changed before writing";goto finish;}
  if(o->journal(o->context)){reason="Cheat intent could not be saved; no patch was written";goto finish;}
  for(unsigned i=0;i<plan->count;i++){
    PmCheatPatch *p=&plan->patches[i];
    if(!o->identity(o->context)||!o->deadline(o->context)){reason="Cheat execution context expired; restoring original bytes";goto rollback;}
    touched=i+1;result.writes++;
    if(o->write(o->context,p->address,p->after,p->afterSize)||o->read(o->context,p->address,check,p->afterSize)||memcmp(check,p->after,p->afterSize)){
      reason="A cheat write failed verification; restoring original bytes";goto rollback;}
  }
  if(!o->identity(o->context)||!o->healthy(o->context)){reason="Cheat completion context could not be verified";goto rollback;}
  result.status=PM_CHEAT_APPLIED;result.restored=0;reason="Cheat bytes applied and verified";goto finish;
rollback:
  result.restored=1;
  for(unsigned i=touched;i>0;i--){
    PmCheatPatch *p=&plan->patches[i-1];
    if(!o->identity(o->context)){result.restored=0;break;}
    if(o->write(o->context,p->address,original+p->originalAt,p->afterSize)||o->read(o->context,p->address,check,p->afterSize)||memcmp(check,original+p->originalAt,p->afterSize))result.restored=0;
  }
  if(!result.restored){result.status=PM_CHEAT_UNCERTAIN;reason="Original cheat bytes could not be completely restored; close the game before further changes";}
finish:
  if(!o->healthy(o->context)){result.status=PM_CHEAT_UNCERTAIN;result.restored=0;reason="Temporary memory permissions could not be completely restored";}
  if(!o->detach(o->context))result.resumed=1;
  else {result.status=PM_CHEAT_UNCERTAIN;reason="Game resume was not confirmed; check the console before further changes";}
  if(!o->healthy(o->context)){result.status=PM_CHEAT_UNCERTAIN;result.restored=0;reason="Temporary native permissions or credentials could not be completely restored";}
done:
  free(original);if(!attempted)result.resumed=1;
  snprintf(result.reason,sizeof result.reason,"%s",reason);return result;
}
