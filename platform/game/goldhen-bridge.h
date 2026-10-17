// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "game-gate.h"
#define PM_GOLDHEN_MAGIC UINT64_C(0x54414347484e3031)
enum { PM_GH_IDLE=0,PM_GH_LOCKED=1,PM_GH_PENDING=2,PM_GH_DONE=3,PM_GH_EXPIRED=4 };
// GoldHEN SDK v1 process-info and process-RW wire layouts (packed).
typedef struct __attribute__((packed)) {
  int32_t pid; char name[40],path[64],title[16],content[64],version[6]; uint64_t base;
} PmGoldhenInfo;
typedef struct { uint64_t address,data,length,write; } PmGoldhenRw;
typedef struct {
  uint64_t magic,cookie,sleep;
  uint32_t state; int32_t result;
  uint64_t command,argument;
} PmGoldhenBridge;
typedef int (PM_GAME_SYSV *PmGoldhenSleep)(unsigned);
int PM_GAME_SYSV pm_goldhen_bridge(PmGoldhenBridge*,uint64_t,void*);
