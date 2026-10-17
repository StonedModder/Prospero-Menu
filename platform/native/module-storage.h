// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
// Returns the writable backing directory. The game sees it through a read-only,
// executable nullfs mount, so large PRXs do not consume sandbox tmpfs space.
int pm_module_storage_open(int pid,const char *generation,const char *title,int platform,uint32_t sdk,char reason[192]);
