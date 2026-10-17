// SPDX-License-Identifier: GPL-3.0-or-later
#include "pad-bridge.h"
#include "relocate.h"
#include <string.h>
extern "C" int pm_pad_trampoline(const uint8_t *before,size_t available,uint64_t target,uint64_t stub,
 uint64_t control,uint64_t cookie,uint64_t bridge,unsigned lane,PmGameTrampoline *out){
 if(!before||!out||lane>1||!cookie||!control||!bridge||target<0x10000||stub<0x10000||
    target>UINT64_C(0x7fffffffffff)-64||stub>UINT64_C(0x7fffffffffff)-512)return -1;
 memset(out,0,sizeof *out);uint64_t original=stub+256;
 // Typed SysV pad ABI: (handle, samples, capacity) becomes seven bridge args.
 // Argument seven is placed on the stack; rsp is aligned before the call.
 const uint8_t head[]={0x48,0x83,0xec,8,0x89,0x14,0x24,0x41,0x89,0xf8,0x49,0x89,0xf1};
 size_t n=sizeof head;memcpy(out->bytes,head,n);
 const uint8_t regs[]={0xbf,0xbe,0xb9,0xb8};const uint64_t values[]={control,cookie,original,bridge};
 for(unsigned i=0;i<4;i++){out->bytes[n++]=0x48;out->bytes[n++]=regs[i];memcpy(out->bytes+n,&values[i],8);n+=8;}
 out->bytes[n++]=0xba;memcpy(out->bytes+n,&lane,4);n+=4;
 const uint8_t tail[]={0xff,0xd0,0x48,0x83,0xc4,8,0xc3};memcpy(out->bytes+n,tail,sizeof tail);n+=sizeof tail;
 RelocatedCode relocated{};if(n>256||!BuildTrampoline(before,available,target,original,&relocated)||256+relocated.size>sizeof out->bytes)return -1;
 memcpy(out->bytes+256,relocated.bytes,relocated.size);out->size=256+relocated.size;out->stolen=relocated.stolen;return 0;
}
