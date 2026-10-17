// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "firmware-profiles.h"
#include "kstuff-state.h"
#include <ps5/kernel.h>
#include <string.h>
static inline PmKstuffSnapshot pm_kstuff_read(uint64_t base,const SnipersFirmwareProfile *p){
  PmKstuffSnapshot s={0};
  if(!p || base>>48!=0xffff)return s;
  s.expected_native=base+p->sysents;s.expected_compat=base+p->sysentsPs4;
  s.readable=!kernel_copyout(base+p->sysentvec+8,&s.native,8) &&
    !kernel_copyout(base+p->sysentvecPs4+8,&s.compat,8) &&
    !kernel_copyout(base+p->cryptSingletonArray+11*8+2*8+6,&s.xts,2) &&
    !kernel_copyout(base+p->cryptSingletonArray+11*8+9*8+6,&s.hmac,2);
  return s;
}
static inline int pm_kstuff_snapshot_equal(const PmKstuffSnapshot *a,const PmKstuffSnapshot *b){
  // Compare fields, not implementation-defined structure padding.
  return a->native==b->native && a->compat==b->compat && a->expected_native==b->expected_native &&
    a->expected_compat==b->expected_compat && a->xts==b->xts && a->hmac==b->hmac && a->readable==b->readable;
}
