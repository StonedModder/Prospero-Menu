// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <sys/types.h>
typedef struct {
 int caller, stoppedThread, signal, deliver, restored;
 uint64_t stoppedRip;
} PmDeckLaunch;
int pm_deck_launch(pid_t pid,intptr_t entry,intptr_t afterTrap,uintptr_t argument,PmDeckLaunch* report);
