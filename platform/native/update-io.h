// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "update-core.h"
#define PM_UPDATE_ROOT "/data/ProsperoMenu/updates-compatibility"
int pm_update_lock(void);
int pm_update_read(PmUpdateState*,int*);
int pm_update_write(PmUpdateState*);
int pm_update_boot(int64_t*,int64_t*);
int pm_update_read_image(const PmUpdateHeader*,uint32_t,uint8_t**,size_t*);
int pm_update_save_image(const void*,size_t,uint32_t,PmUpdateHeader*);
// Caller must hold selection.lock and supply a freshly read, unrecovered state.
int pm_update_retire_image(const PmUpdateState*,const char*,uint64_t);
