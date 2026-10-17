// SPDX-License-Identifier: GPL-3.0-or-later
#include "relocate.h"
#include <string.h>
// Returns the number of original bytes the trampoline replaces (whole
// instructions, at least 14), or 0 when the entry cannot be relocated.
extern "C" int pm_deck_trampoline(const unsigned char* original,uint64_t address,void* memory,size_t capacity){
 RelocatedCode code;if(!BuildTrampoline(original,64,address,(uintptr_t)memory,&code)||code.size>capacity)return 0;
 memcpy(memory,code.bytes,code.size);return (int)code.stolen;
}
