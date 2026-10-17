// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stddef.h>
// Layout follows ps5-payload-sdk v0.43's dynlib and VM walkers. The caller must
// own the kernel-copy guard and verify generation. Transactions require a stopped
// target. A live observational module lookup is not a coherent snapshot: its
// consumer must recheck generation/module identity and never use it for writes.
// These bounded readers do not establish process identity or acquire a lease.
typedef struct {
  void *context;
  int (*read)(void *context,uint64_t kernelAddress,void *bytes,size_t length);
} PmMemoryReader;
typedef struct {uint64_t object,base,size;uint32_t handle;} PmMemoryModule;
typedef struct {uint64_t entry,start,end;unsigned char protection;} PmMemoryMapping;
enum {PM_MEMORY_OK=0,PM_MEMORY_INVALID=1,PM_MEMORY_READ=2,PM_MEMORY_LIMIT=3,
      PM_MEMORY_MISSING=4,PM_MEMORY_AMBIGUOUS=5,PM_MEMORY_GAP=6};
int pm_memory_module(const PmMemoryReader*,uint64_t proc,const char *basename,PmMemoryModule*);
// rootOffset comes from the initialized SDK, not a guessed firmware profile.
// Output covers the entire requested interval with contiguous mapped ranges.
// On failure *count is zero and output must not be used.
int pm_memory_span(const PmMemoryReader*,uint64_t proc,uint64_t rootOffset,
  uint64_t address,size_t length,PmMemoryMapping *output,size_t capacity,size_t *count);
