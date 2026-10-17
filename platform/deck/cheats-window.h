// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "mono-api.h"
#include "deck-bootstrap.h"
#define PM_CHEATS_PATCHES 4
int pm_cheats_prepare(PmMono*,void*,void*[PM_CHEATS_PATCHES],void*[PM_CHEATS_PATCHES]);
void pm_cheats_originals(void*[PM_CHEATS_PATCHES]);
void pm_cheats_bind(volatile PmDeckBoot*);
void pm_cheats_poll(const char[33]); // worker only
void pm_cheats_tick(void); // UI thread only
int pm_cheats_active(void);
