// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stddef.h>
#if defined(__x86_64__) && defined(_WIN32)
#define PM_GAME_SYSV __attribute__((sysv_abi))
#else
#define PM_GAME_SYSV
#endif
#define PM_GAME_GATE_MAGIC UINT64_C(0x54414347414d4531)
#define PM_GAME_MODULE_SLOTS 8
#define PM_GAME_GATE_VERSION 2
#define PM_GAME_MODULE_PREFIX "/prosperomenu-modules/"
enum { PM_MODULE_PENDING=0,PM_MODULE_STARTING=1,PM_MODULE_STARTED=2,PM_MODULE_FAILED=3,PM_MODULE_REJECTED=4 };
enum { PM_MODULE_PHASE_IDLE=0,PM_MODULE_PHASE_THREAD=1,PM_MODULE_PHASE_LOAD=2,
  PM_MODULE_PHASE_EXPORTS=3,PM_MODULE_PHASE_ACTIVATE=4,PM_MODULE_PHASE_COMPLETE=5 };
typedef struct {
  uint32_t enabled,state;
  int32_t handle,startResult;
  char path[256];
  uint32_t phase,activation;
  int32_t threadResult,detachResult,activationResult,loadSymbolResult,unloadSymbolResult;
} PmGameModule;
typedef struct {
  uint64_t magic,cookie,loadStartModule;
  uint32_t version,bytes,enabled,busy;
  uint64_t callbacks[2];
  uint64_t threadCreate,threadDetach,dlsym;
  PmGameModule modules[PM_GAME_MODULE_SLOTS];
} PmGameGate;
typedef int (PM_GAME_SYSV *PmGameLoadModule)(const char*,uint64_t,const void*,unsigned,const void*,int*);
typedef void* (PM_GAME_SYSV *PmGameThreadEntry)(void*);
typedef int (PM_GAME_SYSV *PmGameThreadCreate)(uintptr_t*,const void*,PmGameThreadEntry,void*,const char*);
typedef int (PM_GAME_SYSV *PmGameThreadDetach)(uintptr_t);
typedef int (PM_GAME_SYSV *PmGameDlsym)(int,const char*,void**);
typedef int (PM_GAME_SYSV *PmGamePluginLoad)(int,const char**);
void PM_GAME_SYSV pm_game_gate(PmGameGate *control,uint64_t cookie,unsigned lane);
typedef struct { uint8_t bytes[512];size_t size,stolen; } PmGameTrampoline;
#ifdef __cplusplus
extern "C" {
#endif
int pm_game_trampoline(const uint8_t *before,size_t available,uint64_t target,uint64_t stub,
  uint64_t control,uint64_t cookie,uint64_t gate,unsigned lane,PmGameTrampoline *out);
int pm_game_import_trampoline(uint64_t original,uint64_t control,uint64_t cookie,uint64_t gate,unsigned lane,PmGameTrampoline *out);
#ifdef __cplusplus
}
#endif
