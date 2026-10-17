// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct {
 void *context;
 int (*current)(void*);
 int (*translate)(void*,uint64_t,uint64_t*,uint64_t*);
 int (*physical)(void*,uint64_t,void*,size_t);
} PmUserReader;
// Live, read-only observation, not an atomic snapshot. Translate every page.
// At most 64 KiB; clears partial output on read/identity/translation failure.
int pm_user_read(const PmUserReader*,uint64_t,void*,size_t);
