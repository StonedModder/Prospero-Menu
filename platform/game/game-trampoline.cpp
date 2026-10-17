// SPDX-License-Identifier: GPL-3.0-or-later
// Calling convention derived from the GPL etaHEN port's fps_limiter_stub.h;
// instruction relocation is the credited BuildTrampoline implementation.
#include "game-gate.h"
#include "relocate.h"
#include <string.h>
extern "C" int pm_game_import_trampoline(uint64_t original,uint64_t control,uint64_t cookie,uint64_t gate,unsigned lane,PmGameTrampoline *out){
  if(!original||!control||!cookie||!gate||lane>1||!out)return -1;
  // Both pad APIs return int and take at most three integer/pointer arguments.
  // Call the existing import first with its untouched arguments, then schedule
  // the worker. Preserve the return value and callee-saved RBX, with SysV alignment.
  size_t n=0;out->bytes[n++]=0x53; // push rbx (rsp now 16-byte aligned)
  out->bytes[n++]=0x49;out->bytes[n++]=0xbb;memcpy(out->bytes+n,&original,8);n+=8;
  const uint8_t call[]={0x41,0xff,0xd3,0x89,0xc3};memcpy(out->bytes+n,call,sizeof call);n+=sizeof call;
  out->bytes[n++]=0x48;out->bytes[n++]=0xbf;memcpy(out->bytes+n,&control,8);n+=8;
  out->bytes[n++]=0x48;out->bytes[n++]=0xbe;memcpy(out->bytes+n,&cookie,8);n+=8;
  out->bytes[n++]=0xba;memcpy(out->bytes+n,&lane,4);n+=4;
  out->bytes[n++]=0x48;out->bytes[n++]=0xb8;memcpy(out->bytes+n,&gate,8);n+=8;
  const uint8_t tail[]={0xff,0xd0,0x89,0xd8,0x5b,0xc3};memcpy(out->bytes+n,tail,sizeof tail);n+=sizeof tail;
  out->size=n;out->stolen=0;return 0;
}
extern "C" int pm_game_trampoline(const uint8_t *before,size_t available,uint64_t target,uint64_t stub,
  uint64_t control,uint64_t cookie,uint64_t gate,unsigned lane,PmGameTrampoline *out){
  if(!before||!out||lane>1||!cookie||!control||!gate)return -1;
  // Preserve original argument registers, flags and floating-point state,
  // including the original stack arguments. Align the private FXSAVE area.
  const uint8_t save[]={0x9c,0x50,0x51,0x52,0x56,0x57,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53,0x55,
    0x48,0x89,0xe5,0x48,0x83,0xe4,0xf0,0x48,0x81,0xec,0,2,0,0,0x48,0x0f,0xae,0x04,0x24};
  size_t n=0;memcpy(out->bytes,save,sizeof save);n+=sizeof save;
  out->bytes[n++]=0x48;out->bytes[n++]=0xbf;memcpy(out->bytes+n,&control,8);n+=8;
  out->bytes[n++]=0x48;out->bytes[n++]=0xbe;memcpy(out->bytes+n,&cookie,8);n+=8;
  out->bytes[n++]=0xba;memcpy(out->bytes+n,&lane,4);n+=4;
  out->bytes[n++]=0x48;out->bytes[n++]=0xb8;memcpy(out->bytes+n,&gate,8);n+=8;
  const uint8_t restore[]={0xff,0xd0,0x48,0x0f,0xae,0x0c,0x24,0x48,0x89,0xec,0x5d,
    0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,0x5f,0x5e,0x5a,0x59,0x58,0x9d};
  memcpy(out->bytes+n,restore,sizeof restore);n+=sizeof restore;
  RelocatedCode relocated{};
  if(!BuildTrampoline(before,available,target,stub+n,&relocated)||n+relocated.size>sizeof out->bytes)return -1;
  memcpy(out->bytes+n,relocated.bytes,relocated.size);out->size=n+relocated.size;out->stolen=relocated.stolen;return 0;
}
