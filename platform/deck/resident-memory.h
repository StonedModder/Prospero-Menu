// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <sys/types.h>
void* pm_deck_mmap(void*,size_t,int,int,int,off_t);
int pm_deck_munmap(void*,size_t);
int pm_deck_mprotect(void*,size_t,int);
