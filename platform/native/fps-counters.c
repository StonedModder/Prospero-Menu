// SPDX-License-Identifier: GPL-3.0-or-later
// Counter interpretation adapted from OnionHEN / LightningMods fps_agc.cpp
// (2026), following PHU Games Tools by ArkSama. Fresh virtual reads; no cached
// physical mappings survive a sample or process-generation change.
#include "fps-counters.h"
#include <string.h>
int pm_fps_ring(const PmCounterReader *r,uint64_t *count){
  const uint64_t base=UINT64_C(0xfe03b72ec);uint32_t header[2],after[2];
  if(!r||!r->read||!count)return -1;*count=0;
  if(r->read(r->context,base,header,sizeof header)||!header[0]||header[0]>4096||header[1]>=header[0])return -1;
  uint64_t best=0;unsigned slots=header[0];
  if(slots<=100){
    unsigned char entries[3200];if(r->read(r->context,base+12,entries,slots*32))return -1;
    for(unsigned i=0;i<slots;i++){uint64_t n;memcpy(&n,entries+i*32+8,8);if(n>best)best=n;}
  }else{
    for(unsigned back=1;back<=4;back++){
      unsigned slot=(header[1]+slots-back)%slots;uint64_t n=0;
      if(r->read(r->context,base+12+(uint64_t)slot*32+8,&n,8))return -1;
      if(n){best=n;break;}
    }
  }
  if(!best||r->read(r->context,base,after,sizeof after)||after[0]!=header[0]||after[1]>=after[0])return -1;
  *count=best;return 0;
}
