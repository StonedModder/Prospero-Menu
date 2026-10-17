// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "fps-inject.h"
// RAM-only lookup, caller subsequently verifies process/control/entry via fresh
// reads. An installed receipt is not evidence that frames are still arriving.
int pm_bc_fps_lookup(int pid,const char *generation,const char *title,PmFpsInjection*);
