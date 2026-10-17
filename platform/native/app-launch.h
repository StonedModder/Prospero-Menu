// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PM_APP_LAUNCH_H
#define PM_APP_LAUNCH_H
#include <stdint.h>
typedef struct {uint32_t magic,stage;char request[33],title[10];int32_t result,appId,pid;int64_t bootSec,bootUsec;char message[192];unsigned char checksum[32];} PmAppLaunch;
int pm_app_id_valid(const char*);
int pm_app_title_valid(const char*);
int pm_app_load(const char*,PmAppLaunch*);
int pm_app_save(PmAppLaunch*,int);
int pm_app_boot(PmAppLaunch*,int);
#endif
