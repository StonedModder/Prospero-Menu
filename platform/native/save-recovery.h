// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "save-core.h"
typedef struct {
 void* context;
 int (*acquire)(void*,PmSaveReceipt*);
 int (*quiescent)(void*,const PmSaveReceipt*);
 int (*unmounted)(void*,const PmSaveReceipt*);
 // 1: exact published archive verified, 0: publication absent, -1: uncertain.
 int (*published)(void*,PmSaveReceipt*);
 int (*finish)(void*,PmSaveReceipt*,int complete);
 void (*release)(void*);
} PmSaveRecovery;
int pm_save_recovery_flow(const PmSaveRecovery*);
typedef void (*PmSaveRecoveryProgress)(void*,uint64_t,uint64_t);
int pm_save_recover(const char* request,const unsigned char expected[32],char message[192],
 PmSaveRecoveryProgress progress,void* context);
