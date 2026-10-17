// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from the credited etaHEN fps_counter_stub.h. Adds an aligned enable
// flag without calls, stack-argument movement or game-thread I/O.
#include "fps-inject.h"
#include "relocate.h"
#include <string.h>
#include <limits.h>
extern "C" int pm_fps_trampoline(const uint8_t *original,size_t available,uint64_t source,
 uint64_t stub,uint64_t control,PmFpsTrampoline *out){
 if(!original||!out||!control||(control&7)||source<0x10000||stub<0x10000||
    source>UINT64_C(0x7fffffffffff)-64||stub>UINT64_C(0x7fffffffffff)-320||control>UINT64_C(0x7fffffffffff)-32)return -1;
 memset(out,0,sizeof *out);
 int64_t enable=(int64_t)(control+offsetof(PmFpsGate,enabled))-(int64_t)(stub+8);
 int64_t count=(int64_t)(control+offsetof(PmFpsGate,frames))-(int64_t)(stub+18);
 if(enable<INT32_MIN||enable>INT32_MAX||count<INT32_MIN||count>INT32_MAX)return -1;
 // pushfq; cmp dword [rip+enabled],1; jne +8; lock inc qword [rip+frames]; popfq
 const uint8_t prefix[]={0x9c,0x83,0x3d,0,0,0,0,1,0x75,8,0xf0,0x48,0xff,0x05,0,0,0,0,0x9d};
 memcpy(out->bytes,prefix,sizeof prefix);int32_t e=(int32_t)enable,c=(int32_t)count;
 memcpy(out->bytes+3,&e,4);memcpy(out->bytes+14,&c,4);
 RelocatedCode code{};if(!BuildTrampoline(original,available,source,stub+sizeof prefix,&code)||code.size+sizeof prefix>sizeof out->bytes)return -1;
 memcpy(out->bytes+sizeof prefix,code.bytes,code.size);out->size=sizeof prefix+code.size;out->stolen=code.stolen;return 0;
}
