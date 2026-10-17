// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "loader-worker.h"
#include <stddef.h>
#define PM_DISPATCH_MAGIC UINT32_C(0x54414344)
typedef struct {
  uint32_t magic,version,stage,no_side_effects;
  int64_t boot_sec,boot_usec;
  int32_t pid,reserved;
  char request[33],upload[33],sha256[65],message[192];
  uint8_t checksum[32];
} PmDispatchReceiptV1;
void pm_dispatch_seal(PmDispatchReceipt *receipt);
int pm_dispatch_decode(const void *data,size_t size,const char *request,PmDispatchReceipt *out);
int pm_dispatch_conflicts(const PmDispatchReceipt *requested,const PmDispatchReceipt *prior);
