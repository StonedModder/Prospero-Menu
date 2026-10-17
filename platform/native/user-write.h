// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct {
 void *context;
 int (*current)(void*);
 int (*translate)(void*,uint64_t,uint64_t*,uint64_t*);
 int (*physical)(void*,uint64_t,const void*,size_t);
} PmUserWriter;
// Only within an explicitly owned range. Translate each 4 KiB page freshly.
// A failed write may be partial; caller uses a protocol tolerant of interruption.
int pm_user_write(const PmUserWriter*,uint64_t base,size_t span,uint64_t address,const void*,size_t);
