// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from GronedWaffel's verified etaHEN port_kstuff_state.hpp.
// Markers follow EchoStretch kstuff-lite v1.11. This classifier never writes hooks.
#pragma once
#include <stdint.h>
typedef struct {
  uint64_t native, compat, expected_native, expected_compat;
  uint16_t xts, hmac;
  int readable;
} PmKstuffSnapshot;
enum { PM_KSTUFF_UNKNOWN, PM_KSTUFF_UNMODIFIED, PM_KSTUFF_INSTALLED };
static inline int pm_kstuff_classify(uint64_t base, const PmKstuffSnapshot *s) {
  if (!s->readable || base>>48!=0xffff || s->expected_native>>48!=0xffff || s->expected_compat>>48!=0xffff) return PM_KSTUFF_UNKNOWN;
  uint64_t nt=s->native>>48, ct=s->compat>>48;
  if ((nt!=0xffff && nt!=0xdeb7) || (ct!=0xffff && ct!=0xdeb7)) return PM_KSTUFF_UNKNOWN;
  int n=(s->native|UINT64_C(0xffff000000000000))==s->expected_native;
  int c=(s->compat|UINT64_C(0xffff000000000000))==s->expected_compat;
  if (n && c && nt==0xffff && ct==0xffff && s->xts==0xffff && s->hmac==0xffff) return PM_KSTUFF_UNMODIFIED;
  if (!n && !c && s->xts==0xdeb7 && s->hmac==0xdeb7) return PM_KSTUFF_INSTALLED;
  return PM_KSTUFF_UNKNOWN;
}
static inline int pm_kstuff_injection_ready(uint64_t base, const PmKstuffSnapshot *s) {
  int state=pm_kstuff_classify(base,s);
  return state==PM_KSTUFF_UNMODIFIED || (state==PM_KSTUFF_INSTALLED && s->native>>48==0xffff && s->compat>>48==0xffff);
}
