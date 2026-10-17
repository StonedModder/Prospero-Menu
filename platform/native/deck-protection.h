// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "deck-bootstrap.h"
#include <stddef.h>
#include <string.h>
// FreeBSD protection bits: read=1, execute=4. FF 25 reads an inline pointer,
// so its installed mapping cannot be execute-only. Rollback restores the exact
// original protection. Reject bytes outside the recorded transaction.
static inline int pm_deck_final_protection(int original,const PmDeckPatch* patch,
 uint64_t address,const void* bytes,size_t length){
 if(original<0||!(original&4)||!patch||!bytes||length!=pm_deck_patch_length(patch)||address!=patch->address)return -1;
 if(!memcmp(bytes,patch->before,length))return original;
 if(!memcmp(bytes,patch->after,length))return original|1;
 return -1;
}
