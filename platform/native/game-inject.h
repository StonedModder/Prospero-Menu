// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
// Caller owns the native operation guard and durable intent. A successful
// install publishes disabled module slots only; it does not load any PRX.
typedef struct { intptr_t arena,control;uint64_t cookie;int installed,uncertain,resumed;char message[192]; } PmGameInjection;
int pm_game_inject(pid_t pid,uint64_t cookie,int inputBridge,int(*identity)(void*),void* context,PmGameInjection *out);
