// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "goldhen-bridge.h"
// Private trial: only the reversed upstream Havana 1.2 / RDR2 1.32 binary.
// Unknown SDK implementations are passed through, not guessed or patched.
#define PM_HAVANA132_SHA "b465b1f7aaa7d4a523db3b00618613b1e1d3ab9984d59461710fc4749356e36b"
static inline int pm_goldhen_supported(const char *sha){return sha&&!strcmp(sha,PM_HAVANA132_SHA);}
int pm_goldhen_patch(void *bytes,size_t size,uint64_t entry,uint64_t control);
int pm_goldhen_sfo(const void*,size_t,const char*,char*,size_t);
typedef struct {
  void *context;
  int (*read)(void*,uint64_t,void*,size_t);
  int (*write)(void*,uint64_t,const void*,size_t);
} PmGoldhenOps;
int pm_goldhen_request(const PmGoldhenOps*,const PmGoldhenInfo*,uint64_t,uint64_t);
