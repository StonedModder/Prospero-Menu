// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "game-gate.h"
#define PM_PAD_MAGIC UINT64_C(0x5441435041443031)
#define PM_PAD_EVENTS 128u
#define PM_PAD_CONTROLLERS 8u
#define PM_PAD_LEASE_NS UINT64_C(500000000)
#define PM_PAD_DRAIN_NS UINT64_C(250000000)
// Normal scePadRead/scePadReadState interoperability layout. Other/extended
// input APIs are not interpreted as this structure.
typedef struct {
 uint32_t buttons;uint8_t sticks[4],triggers[2],reserved0[2];
 float orientation[4],acceleration[3],angular[3];
 uint8_t touches[24];int32_t connected;uint64_t timestamp;
 uint8_t extension[16],connection,reserved1[2],deviceLength,device[12];
} PmPadSample;
enum {PM_PAD_OFF=0,PM_PAD_OBSERVE=1,PM_PAD_CAPTURE=2};
typedef struct {
 uint32_t revision,epoch,mode;int32_t owner;uint32_t connection,reserved;
 uint64_t expires;
} PmPadLease;
typedef struct {
 uint32_t sequence,buttons;int32_t handle;uint32_t connection;
 uint64_t time;uint8_t sticks[4],triggers[2],reserved[2];
} PmPadEvent;
typedef struct {
 int32_t handle;uint32_t used,connection,buttons;uint64_t timestamp,lastEvent,drainUntil,captureUntil,lastSeen;
 uint8_t sticks[4],triggers[2],captured,reserved;
} PmPadController;
typedef struct {
 uint64_t magic,cookie,moduleControl,moduleEntry,clock;
 uint32_t version,bytes,enabled,lock;
 PmPadLease lease;
 uint32_t written,consumed,faultEpoch,faults;
 PmPadController controllers[PM_PAD_CONTROLLERS];
 PmPadEvent events[PM_PAD_EVENTS];
} PmPadBridge;
typedef struct {int64_t sec,nsec;} PmPadTime;
typedef int(PM_GAME_SYSV *PmPadClock)(int,PmPadTime*);
typedef int(PM_GAME_SYSV *PmPadRead)(int,PmPadSample*,int);
typedef int(PM_GAME_SYSV *PmPadReadState)(int,PmPadSample*);
// Called through a typed SysV entry stub. The original pad function and its
// return value are preserved even when capture is disabled or incompatible.
int PM_GAME_SYSV pm_pad_bridge(PmPadBridge*,uint64_t,unsigned,uint64_t,int,PmPadSample*,int);
#ifdef __cplusplus
extern "C" {
#endif
int pm_pad_trampoline(const uint8_t*,size_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,unsigned,PmGameTrampoline*);
#ifdef __cplusplus
}
#endif
