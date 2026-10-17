// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "game-gate.h"
#define PM_MODULE_SESSION_MAGIC UINT32_C(0x544d5331)
#define PM_MODULE_SESSION_VERSION 2u
enum { PM_ARENA_EMPTY=0,PM_ARENA_INTENT=1,PM_ARENA_READY=2,PM_ARENA_UNKNOWN=3 };
enum { PM_PRX_EMPTY=0,PM_PRX_QUEUED=1,PM_PRX_ARMING=2,PM_PRX_ARMED=3,PM_PRX_STARTED=4,
  PM_PRX_FAILED=5,PM_PRX_UNKNOWN=6,PM_PRX_REJECTED=7 };
typedef struct {
  char request[33],upload[33],sha256[65],extension[8];
  uint32_t stage,clean;int32_t handle,startResult;char message[192];
} PmModuleReceipt;
typedef struct {
  uint32_t magic,version,phase;int32_t pid;uint64_t arena,control,cookie;
  char generation[128],title[10],metadataHash[65];
  char arenaRequest[33],arenaMessage[192];uint32_t arenaStage;
  PmModuleReceipt slots[PM_GAME_MODULE_SLOTS];unsigned char checksum[32];
} PmModuleSession;
int pm_hex_string(const char *value,size_t bytes);
void pm_module_seal(PmModuleSession *session);
int pm_module_decode(const void *bytes,size_t length,const char *generation,PmModuleSession *session);
int pm_module_slot(const PmModuleSession *session,const char *request);
// New operations must not overtake an unresolved game/module publication.
int pm_module_session_held(const PmModuleSession *session);
// 0 claims a new arena request, 1 returns its existing receipt, -1 conflicts.
// Caller persists a new claim before starting a native worker.
int pm_module_arena_claim(PmModuleSession *session,const char *request);
// RAM-only snapshot for high-frequency input. Caller holds native-operation
// ownership; never loads a disk receipt or adopts another game implicitly.
int pm_module_live_session(int pid,const char *generation,const char *metadata,PmModuleSession*);
void pm_module_path(const PmModuleReceipt *receipt,char path[256]);
void pm_module_failure_message(int32_t handle,int32_t startResult,char *message,size_t size);
// Identity of the in-game control block; no pointer obtained from it is used.
int pm_module_gate_valid(const PmModuleSession *session,const PmGameGate *gate);
// Pure publication primitive. Caller must hold a stopped process lease through
// both writes, readback and rollback. 0=armed, 1=not applied, -1=uncertain,
// 2=callback busy with an untouched slot (resume the game before waiting).
typedef struct {void *context;int(*read)(void*,PmGameGate*);int(*write)(void*,size_t,const void*,size_t);} PmModuleOps;
int pm_module_stage(const PmModuleSession *session,unsigned slot,const PmModuleOps *ops);
