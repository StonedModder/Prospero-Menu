// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
enum { PM_DISPATCH_QUEUED=1,PM_DISPATCH_SPAWNING,PM_DISPATCH_SPAWNED,PM_DISPATCH_FAILED,PM_DISPATCH_UNKNOWN };
typedef struct {
  uint32_t magic,version,stage,no_side_effects;
  int64_t boot_sec,boot_usec;
  int32_t pid,reserved;
  char request[33],upload[33],sha256[65],message[192];
  uint32_t plugin;
  char plugin_id[10],plugin_version[5],generation[128];
  uint8_t checksum[32];
} PmDispatchReceipt;
int pm_loader_start(const char *request,const char *upload,const char *sha256);
int pm_plugin_start(const char *request,const char *upload,const char *sha256);
int pm_loader_poll(const char *request,PmDispatchReceipt *receipt);
int pm_loader_busy(void);
