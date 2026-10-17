// SPDX-License-Identifier: GPL-3.0-or-later
#include "native-operation.h"
static unsigned active;
int pm_native_claim(void){unsigned expected=0;return __atomic_compare_exchange_n(&active,&expected,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);}
void pm_native_release(void){__atomic_store_n(&active,0,__ATOMIC_RELEASE);}
int pm_native_busy(void){return __atomic_load_n(&active,__ATOMIC_ACQUIRE)!=0;}
