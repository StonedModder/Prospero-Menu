// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime metadata layout follows the credited etaHEN rtld.hpp. Import-slot
// selection was cross-checked against ciss84's public Plugin-Loader-v1.13.
// This bounded reader and validation are ProsperoMenu's implementation.
#include "game-imports.h"
#include <stdlib.h>
#include <string.h>
static uint64_t u64(const void *p){uint64_t v;memcpy(&v,p,8);return v;}
static uint32_t u32(const void *p){uint32_t v;memcpy(&v,p,4);return v;}
static int kernel_span(uint64_t p,uint64_t n){return (p>>48)==0xffff&&!(p&7)&&n&&n<=UINT64_MAX-p;}
int pm_game_pad_imports(const PmMemoryReader *r,const PmMemoryModule *m,uint64_t slots[2]){
  if(!slots)return -1;slots[0]=slots[1]=0;
  if(!r||!r->read||!m||!kernel_span(m->object,0x150)||m->base<0x10000||m->base>UINT64_C(0x7fffffffffff)-8)return -1;
  uint64_t meta=0;unsigned char header[0x58];
  if(r->read(r->context,m->object+0x148,&meta,8)||!kernel_span(meta,sizeof header)||r->read(r->context,meta,header,sizeof header))return -1;
  uint64_t sy=u64(header+0x28),syn=u64(header+0x30),st=u64(header+0x38),stn=u64(header+0x40),pl=u64(header+0x48),pln=u64(header+0x50);
  if(!syn||syn%24||syn>24*65536||!stn||stn>4*1024*1024||pln%24||pln>24*65536||
     !kernel_span(sy,syn)||!kernel_span(st,stn)||(pln&&!kernel_span(pl,pln)))return -1;
  unsigned char *symbols=malloc(syn),*strings=malloc(stn),*relocs=pln?malloc(pln):NULL;int result=-1;uint64_t found[2]={0};
  if(!symbols||!strings||(pln&&!relocs))goto done;
  if(r->read(r->context,sy,symbols,syn)||r->read(r->context,st,strings,stn)||(pln&&r->read(r->context,pl,relocs,pln)))goto done;
  const char *names[2]={"YndgXqQVV7c","q1cHNfGycLI"};
  for(size_t i=0;i<pln;i+=24){
    uint64_t info=u64(relocs+i+8),index=info>>32;if((uint32_t)info!=7)continue; // R_X86_64_JUMP_SLOT
    if(index>=syn/24)goto done;const unsigned char *sym=symbols+index*24;uint32_t name=u32(sym);
    if(name>=stn||!memchr(strings+name,0,stn-name))goto done;
    if(sym[6]||sym[7])continue; // Only imported (undefined) symbols.
    for(unsigned lane=0;lane<2;lane++){
      if(stn-name<12||memcmp(strings+name,names[lane],11)||(strings[name+11]&&strings[name+11]!='#'))continue;
      uint64_t offset=u64(relocs+i);
      if(offset>UINT64_C(0x7fffffffffff)-m->base-8||((m->base+offset)&7)||found[lane])goto done;
      found[lane]=m->base+offset;
    }
  }
  slots[0]=found[0];slots[1]=found[1];result=0;
done:
  free(symbols);free(strings);free(relocs);return result;
}
