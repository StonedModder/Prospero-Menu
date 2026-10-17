// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <sys/types.h>
int pm_deck_inject(pid_t pid,const uint8_t* image,size_t bytes,const char credential[33],intptr_t* control,int(*identity)(void*),void* cookie,int* resumed);
