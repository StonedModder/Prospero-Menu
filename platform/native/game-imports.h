// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "memory-map.h"
// Caller owns a stopped process lease. Resolves only undefined pad imports in
// the executable's runtime metadata; does not read/write game code or GOT slots.
// 0 = verified table (zero addresses means imports absent), -1 = invalid/read failure.
int pm_game_pad_imports(const PmMemoryReader*,const PmMemoryModule*,uint64_t slots[2]);
