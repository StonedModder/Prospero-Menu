// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stddef.h>
#define PM_CHEAT_MAX_PATCHES 256
#define PM_CHEAT_MAX_BYTES 1048576
#define PM_CHEAT_PLAN_MAGIC UINT32_C(0x54435031)
typedef struct {uint64_t offset,address;uint32_t size,absolute,beforeSize,afterSize;size_t originalAt;const unsigned char *before,*after;} PmCheatPatch;
typedef struct {unsigned count;size_t bytes;PmCheatPatch patches[PM_CHEAT_MAX_PATCHES];} PmCheatPlan;
// Wire: LE magic, version=1, 2 or 3, patch count, total bytes (four uint32s).
// Each record: uint64 offset, uint32 byte count, uint32 address mode,
// mode=0 is module-relative. Version 2 additionally accepts mode=1 (absolute).
// Version 3: uint64 offset, uint32 expected count, desired count, address mode.
// Followed by expected/desired bytes at their respective lengths. Absolute targets are resolved only in the
// verified game, with user-address, mapping, overlap and expected-byte checks.
int pm_cheat_plan(const void*,size_t,PmCheatPlan*);
typedef struct {
  void *context;
  int (*attach)(void*); // zero only after verified stop; detach runs even on failure
  int (*detach)(void*); // zero only if no outstanding trace ownership remains
  int (*identity)(void*); // true only for this exact game instance
  int (*deadline)(void*); // true while forward execution is within its time budget
  int (*module)(void*,uint64_t *base,uint64_t *size);
  int (*range)(void*,uint64_t,size_t); // bounds, mappings and stopped instruction PCs
  int (*read)(void*,uint64_t,void*,size_t);
  int (*write)(void*,uint64_t,const void*,size_t); // exact permission restoration
  int (*journal)(void*); // durable intent, before first write
  int (*healthy)(void*); // all temporary permission changes restored
} PmCheatOps;
enum {PM_CHEAT_APPLIED=0,PM_CHEAT_REJECTED=1,PM_CHEAT_UNCERTAIN=2};
typedef struct {int status,resumed,restored;unsigned writes;char reason[192];} PmCheatResult;
// Every expected byte is checked before the first write. The in-flight write is
// included in rollback. Rollback is bounded by patch count, not forward deadline.
PmCheatResult pm_cheat_apply(PmCheatPlan*,const PmCheatOps*);
