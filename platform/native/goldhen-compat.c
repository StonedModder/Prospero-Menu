// SPDX-License-Identifier: GPL-3.0-or-later
#include "goldhen-compat.h"
#include <stdlib.h>
static int range(uint64_t p,size_t n){return p>=0x10000&&p<UINT64_C(0x800000000000)&&n&&n<=65536&&n<=UINT64_C(0x800000000000)-p;}
int pm_goldhen_patch(void *data,size_t size,uint64_t entry,uint64_t control){
  static const unsigned char expected[]={0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x10,0x48,0x89,0x7d,0xf8,0x48,0x89,0x75,0xf0,0x48,0x8b,0x75,0xf8,0x48,0x8b,0x55,0xf0,0xbf,0xf4,0x01,0x00,0x00,0xb0,0x00,0xe8,0xec,0x01,0x00,0x00,0x48,0x83,0xc4,0x10,0x5d,0xc3};
  const size_t offset=0x850+0x16840;
  static const unsigned char hookBefore[]={0x48,0x8b,0x7d,0xe0,0x48,0x8d,0x75,0xea,0xba,0x0e,0x00,0x00,0x00,0xe8,0xd7,0x59,0xff,0xff,0x48,0x8b,0x7d,0xe0,0x48,0x83,0xc7,0x06,0x48,0x8d,0x75,0xd8,0xba,0x08,0x00,0x00,0x00,0xe8,0xc1,0x59,0xff,0xff,0xe9,0x13,0x00,0x00,0x00};
  const size_t hookOffset=0x850+0xe607;
  if(!data||size!=202688||!range(entry,4096)||!range(control,sizeof(PmGoldhenBridge))||
    memcmp((unsigned char*)data+offset,expected,sizeof expected)||memcmp((unsigned char*)data+hookOffset,hookBefore,sizeof hookBefore))return -1;
  // cmd(rdi,data=rsi) -> bridge(control=rdi,cmd=rsi,data=rdx).
  unsigned char code[42]={0x48,0x89,0xf2,0x48,0x89,0xfe,0x48,0xbf};
  memcpy(code+8,&control,8);code[16]=0x48;code[17]=0xb8;memcpy(code+18,&entry,8);code[26]=0xff;code[27]=0xe0;
  // Assemble the full 14-byte absolute jump in Havana's existing stack buffer
  // before its single write. The original writes FF25 + a null pointer, then
  // fills the pointer in a second syscall. Resuming between those calls is unsafe.
  unsigned char hook[45]={0x48,0x8b,0x45,0xd8,0x48,0x89,0x45,0xf0,0x48,0x8b,0x7d,0xe0,0x48,0x8d,0x75,0xea,0xba,0x0e,0,0,0,0xe8};
  int32_t call=(int32_t)(0x3ff0-(0xe607+26));memcpy(hook+22,&call,4);
  hook[26]=0xe9;int32_t next=(int32_t)(0xe647-(0xe607+31));memcpy(hook+27,&next,4);memset(hook+31,0x90,sizeof hook-31);
  memset(code+28,0x90,sizeof code-28);memcpy((unsigned char*)data+offset,code,sizeof code);
  memcpy((unsigned char*)data+hookOffset,hook,sizeof hook);return 0;
}
static uint32_t u32(const unsigned char *p){uint32_t v;memcpy(&v,p,4);return v;}
static uint16_t u16(const unsigned char *p){uint16_t v;memcpy(&v,p,2);return v;}
int pm_goldhen_sfo(const void *data,size_t size,const char *key,char *out,size_t capacity){
  if(!out||!capacity)return -1;out[0]=0;
  const unsigned char *b=data;if(!b||size<20||u32(b)!=0x46535000||!key)return -1;
  uint32_t keys=u32(b+8),values=u32(b+12),count=u32(b+16);
  if(count>(size-20)/16||keys<20+(uint64_t)count*16||keys>=size||values<keys||values>=size)return -1;
  for(unsigned i=0;i<count;i++){
    const unsigned char *e=b+20+i*16;uint64_t k=(uint64_t)keys+u16(e),v=(uint64_t)values+u32(e+12);uint32_t n=u32(e+4);
    if(k>=values||!memchr(b+k,0,values-k)||v>size||n>size-v)return -1;
    if(strcmp((const char*)b+k,key))continue;
    if(u16(e+2)!=0x204||!n||n>capacity||!memchr(b+v,0,n))return -1;
    memcpy(out,b+v,n);out[capacity-1]=0;return 0;
  }
  return -1;
}
int pm_goldhen_request(const PmGoldhenOps *ops,const PmGoldhenInfo *info,uint64_t command,uint64_t argument){
  if(!ops||!ops->read||!ops->write||!info)return -1;
  if(command==4)return range(argument,sizeof *info)?ops->write(ops->context,argument,info,sizeof *info):-1;
  if(command!=5||!range(argument,sizeof(PmGoldhenRw)))return -1;
  PmGoldhenRw rw;if(ops->read(ops->context,argument,&rw,sizeof rw)||rw.length>65536||rw.write>1||!range(rw.address,(size_t)rw.length)||!range(rw.data,(size_t)rw.length))return -1;
  unsigned char *buffer=malloc((size_t)rw.length);if(!buffer)return -1;
  int result=ops->read(ops->context,rw.write?rw.data:rw.address,buffer,(size_t)rw.length);
  if(!result)result=ops->write(ops->context,rw.write?rw.address:rw.data,buffer,(size_t)rw.length);
  free(buffer);return result?-1:0;
}
