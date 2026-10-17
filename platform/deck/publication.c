// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from the etaHEN port's stopped-process publication design. No writes
// while controller/render threads can execute partially replaced instructions.
#include "publication.h"
#include <string.h>
#include <stdlib.h>
// width 0: each patch's own length (instruction hooks); otherwise fixed.
static size_t span(const PmDeckPatch* p,size_t width){return width?width:pm_deck_patch_length(p);}
static int code_busy(const PmDeckPatch* patches,size_t count,const PmPublicationOps* o,size_t width){
 // Only instruction patches need a PC collision check. Keep the potentially
 // large thread snapshot off the caller's native worker stack.
 uint64_t* pcs=malloc(2048*sizeof *pcs);if(!pcs)return -1;
 int n=o->threads(o->context,pcs,2048),busy=0;
 if(n<=0||n>2048)busy=-1;
 else for(int i=0;i<n;i++)for(size_t j=0;j<count;j++)if(pcs[i]>=patches[j].address&&pcs[i]<patches[j].address+span(&patches[j],width))busy=1;
 free(pcs);return busy;
}
static int publish(const PmDeckPatch* patches,size_t count,const PmPublicationOps* o,size_t width,int code){
 if(!patches||count<1||count>PM_DECK_MAX_PATCHES||!o||!o->attach||!o->detach||(code&&!o->threads)||!o->read||!o->write||!o->ack||!o->reject||!o->pause)return 1;
 for(size_t i=0;i<count;i++){
  if(patches[i].address<0x10000||patches[i].address>UINT64_C(0x7fffffffffff)-64)return 1;
  if(span(&patches[i],width)<8||span(&patches[i],width)>PM_DECK_PATCH_MAX)return 1;
  for(size_t j=0;j<i;j++)if(patches[i].address<patches[j].address+span(&patches[j],width)&&patches[j].address<patches[i].address+span(&patches[i],width))return 1;
 }
 for(unsigned attempt=0;attempt<20;attempt++){
  // Attach can acquire ownership before a stop wait or credential check fails.
  // The adapter must make detach a no-op when ownership was never acquired.
  if(o->attach(o->context)){o->detach(o->context);return -1;}
  int busy=code?code_busy(patches,count,o,width):0,ok=1;
  if(busy<0){o->detach(o->context);return -1;}
  if(busy){if(o->detach(o->context))return -1;o->pause(o->context);continue;}
  for(size_t i=0;i<count;i++){
   unsigned char bytes[PM_DECK_PATCH_MAX];size_t n=span(&patches[i],width);if(o->read(o->context,patches[i].address,bytes,n)||memcmp(bytes,patches[i].before,n)){ok=0;break;}
  }
  size_t attempted=0;
  if(ok)for(size_t i=0;i<count;i++){
   attempted=i+1; // a failed write may still have modified part of this patch
   if(o->write(o->context,patches[i].address,patches[i].after,span(&patches[i],width))){ok=0;break;}
  }
  if(ok&&o->ack(o->context))ok=0;
  int restored=1;
  if(!ok)while(attempted){size_t i=--attempted;if(o->write(o->context,patches[i].address,patches[i].before,span(&patches[i],width)))restored=0;}
  // A partial acknowledgement must not let a resumed initializer observe
  // success after the hooks were restored. Reject before resuming any thread.
  if(!ok&&o->reject(o->context))restored=0;
  if(o->detach(o->context)||!restored)return -1;
  return ok?0:1;
 }
 return 1;
}

int pm_publish_hooks(const PmDeckPatch* patches,size_t count,const PmPublicationOps* o){return publish(patches,count,o,0,1);}
int pm_publish_imports(const PmDeckPatch* patches,size_t count,const PmPublicationOps* o){
 if(!patches||!count||count>PM_DECK_MAX_PATCHES)return 1;
 for(size_t i=0;i<count;i++)if(patches[i].address&7)return 1;
 return publish(patches,count,o,8,0);
}
