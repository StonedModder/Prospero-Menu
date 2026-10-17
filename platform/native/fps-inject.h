// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stddef.h>
#define PM_FPS_MAGIC UINT64_C(0x5441434650533031)
typedef struct {uint64_t magic,cookie,frames;uint32_t enabled,reserved;} PmFpsGate;
typedef struct {uint8_t bytes[320];size_t size,stolen;} PmFpsTrampoline;
#ifdef __cplusplus
extern "C" {
#endif
int pm_fps_trampoline(const uint8_t*,size_t,uint64_t,uint64_t,uint64_t,PmFpsTrampoline*);
typedef struct {
 uint64_t arena,control,target,cookie;uint8_t before[14],after[14];
 int installed,uncertain,resumed;char message[192];
} PmFpsInjection;
// Caller owns the native-operation guard and durable intent, saved BEFORE this
// operation. No filesystem activity while the target is stopped.
int pm_fps_inject(int pid,uint64_t cookie,int(*identity)(void*),void*,PmFpsInjection*);
#ifdef __cplusplus
}
#endif
