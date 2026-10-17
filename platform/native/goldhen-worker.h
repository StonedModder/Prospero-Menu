// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "module-session.h"
// Caller owns native-operation. -2 means resume was unconfirmed, not a clean rejection.
int pm_goldhen_prepare(const PmModuleSession*,void*,size_t,int*,char[192]);
