// SPDX-License-Identifier: GPL-3.0-or-later
#include "relocate.h"
#include "hde64.h"
#include <string.h>
#include <limits.h>
namespace {
struct Instruction {hde64s h;size_t in,out,size;bool farRip;};
size_t immediates(const hde64s& h){return (h.flags&F_IMM8?1:0)+(h.flags&F_IMM16?2:0)+(h.flags&F_IMM32?4:0)+(h.flags&F_IMM64?8:0);}
bool rip(const hde64s& h){return (h.flags&F_MODRM)&&h.modrm_mod==0&&h.modrm_rm==5&&!h.p_67;}
bool generalRegister(const hde64s& h){return h.opcode==0x8b||h.opcode==0x89||h.opcode==0x8d||h.opcode==0x3b||h.opcode==0x39||h.opcode==0x85;}
bool canExpand(const hde64s& h){return (generalRegister(h)&&!(h.modrm_reg==4&&!h.rex_r))||h.opcode==0x80||h.opcode==0x81||h.opcode==0x83||h.opcode==0xc6||h.opcode==0xc7;}
void jump(unsigned char* p,uint64_t target){const unsigned char op[]={0xff,0x25,0,0,0,0};memcpy(p,op,6);memcpy(p+6,&target,8);}
bool branchTarget(const Instruction* list,size_t count,uint64_t source,size_t stolen,uint64_t dest,uint64_t* target){
 if(*target<source||*target>=source+stolen)return true;
 for(size_t i=0;i<count;++i)if(*target==source+list[i].in){*target=dest+list[i].out;return true;}
 return false;
}
}
bool BuildTrampoline(const unsigned char* input,size_t available,uint64_t source,uint64_t destination,RelocatedCode* out){
 if(!input||!out||available<15)return false;
 memset(out,0,sizeof(*out));Instruction list[32];size_t count=0,stolen=0,used=0;
 while(stolen<14){
  if(count==32||stolen+15>available)return false;
  Instruction item{};item.in=stolen;item.out=used;
  hde64_disasm(input+stolen,&item.h);
  if(!item.h.len||(item.h.flags&F_ERROR))return false;
  item.size=item.h.len;
  if(item.h.flags&F_RELATIVE){
   if(item.h.opcode==0xe8)item.size=16;
   else if(item.h.opcode==0xe9||item.h.opcode==0xeb)item.size=14;
   else if((item.h.opcode>=0x70&&item.h.opcode<=0x7f)||(item.h.opcode==0x0f&&item.h.opcode2>=0x80&&item.h.opcode2<=0x8f))item.size=16;
   else return false;
  }
  if(rip(item.h)){
   const auto& h=item.h;size_t imm=immediates(h);if(h.len<imm+4)return false;int32_t old;memcpy(&old,input+stolen+h.len-imm-4,4);
   int64_t delta=(int64_t)(source+stolen+h.len+old)-(int64_t)(destination+used+h.len);
   if(delta<INT32_MIN||delta>INT32_MAX){if(!canExpand(h))return false;item.farRip=true;item.size=h.len-4+((h.flags&F_PREFIX_REX)?0:1)+30;}
  }
  if(used+item.size+14>sizeof(out->bytes))return false;
  list[count++]=item;stolen+=item.h.len;used+=item.size;
 }
 for(size_t i=0;i<count;++i){const auto& item=list[i];const auto& h=item.h;auto p=out->bytes+item.out;
  if(h.flags&F_RELATIVE){
   int64_t delta;
   if(h.flags&F_IMM8)delta=(int8_t)input[item.in+h.len-1];
   else if(h.flags&F_IMM32){int32_t d;memcpy(&d,input+item.in+h.len-4,4);delta=d;}
   else return false;
   uint64_t target=source+item.in+h.len+delta;
   if(!branchTarget(list,count,source,stolen,destination,&target))return false;
   if(h.opcode==0xe8){const unsigned char call[]={0xff,0x15,2,0,0,0,0xeb,8};memcpy(p,call,8);memcpy(p+8,&target,8);}
   else if(h.opcode==0xe9||h.opcode==0xeb)jump(p,target);
   else{p[0]=0x70|(((h.opcode==0x0f?h.opcode2:h.opcode)&15)^1);p[1]=14;jump(p+2,target);}
  }else{
   memcpy(p,input+item.in,h.len);
   if(rip(h)){
    size_t imm=immediates(h);
    if(h.len<4+imm)return false;size_t offset=h.len-imm-4;
    int32_t old;memcpy(&old,p+offset,4);uint64_t target=source+item.in+h.len+old;
    if(target>=source&&target<source+stolen)return false;
    int64_t delta=(int64_t)target-(int64_t)(destination+item.out+h.len);
    if(item.farRip){
     // Save a scratch register below the SysV red zone. LEA, PUSH, MOV and
     // POP preserve flags; instructions involving RSP are rejected above.
     unsigned reg=generalRegister(h)&&((h.rex_r?8:0)+h.modrm_reg)==11?10:11;
     const unsigned char down[]={0x48,0x8d,0xa4,0x24,0x80,0xff,0xff,0xff};
     const unsigned char up[]={0x48,0x8d,0xa4,0x24,0x80,0,0,0};
     size_t at=0;memcpy(p+at,down,8);at+=8;p[at++]=0x41;p[at++]=0x50+(reg&7);
     p[at++]=0x49;p[at++]=0xb8+(reg&7);memcpy(p+at,&target,8);at+=8;
     size_t opcode=0;
     while(opcode<h.len){unsigned b=input[item.in+opcode];if(b==0xf0||b==0xf2||b==0xf3||b==0x66||b==0x67||b==0x2e||b==0x36||b==0x3e||b==0x26||b==0x64||b==0x65){p[at++]=b;++opcode;}else break;}
     if(h.flags&F_PREFIX_REX){p[at++]=input[item.in+opcode++]|1;}else p[at++]=0x41;
     // Supported forms have a one-byte opcode and a ModRM byte.
     p[at++]=input[item.in+opcode++];p[at++]=(input[item.in+opcode++]&0xf8)|(reg&7);
     if(opcode!=offset)return false;
     opcode+=4;while(opcode<h.len)p[at++]=input[item.in+opcode++];
     p[at++]=0x41;p[at++]=0x58+(reg&7);memcpy(p+at,up,8);at+=8;
     if(at!=item.size)return false;
    }else{if(delta<INT32_MIN||delta>INT32_MAX)return false;int32_t replacement=(int32_t)delta;memcpy(p+offset,&replacement,4);}
   }
  }
 }
 jump(out->bytes+used,source+stolen);out->size=used+14;out->stolen=stolen;return true;
}
