// SPDX-License-Identifier: GPL-3.0-or-later
// Bounded read-only interpretation of SDK v0.43 kernel.c/kernel.h layouts,
// copyright John Tornblom, GPL-3.0-or-later. No cached process/module pointers.
#include "memory-map.h"
#include <string.h>
static int kpointer(uint64_t p){return (p>>48)==0xffff&&!(p&7)&&p<=UINT64_MAX-0x1000;}
static int urange(uint64_t base,uint64_t size){return base>=0x10000&&base<UINT64_C(0x800000000000)&&size&&size<=UINT64_C(0x800000000000)-base;}
static uint64_t u64(const unsigned char *p){uint64_t n;memcpy(&n,p,8);return n;}
static int read(const PmMemoryReader *r,uint64_t at,void *out,size_t n){return r->read(r->context,at,out,n)?PM_MEMORY_READ:PM_MEMORY_OK;}
static int name_valid(const char *name){
  if(!name||!name[0])return 0;
  for(size_t i=0;;i++){unsigned char c=(unsigned char)name[i];if(i>=128)return 0;if(!c)return 1;
    if(c<0x21||c>0x7e||c=='/'||c=='\\')return 0;}
}
static int path_name(const PmMemoryReader *r,uint64_t address,char path[1024],const char **name){
  if(!kpointer(address))return PM_MEMORY_INVALID;
  // Small page-bounded chunks avoid one kernel-copy operation per character.
  // A boundary read failure falls back to individual bytes through the NUL.
  for(size_t i=0;i<1024;){
    size_t n=64;if(n>1024-i)n=1024-i;
    size_t page=0x1000-((address+i)&0xfff);if(n>page)n=page;
    if(read(r,address+i,path+i,n)){n=1;if(read(r,address+i,path+i,1))return PM_MEMORY_READ;}
    for(size_t j=0;j<n;j++)if(!path[i+j]){
      if(!i&&!j)return PM_MEMORY_INVALID;
      if(path[0]!='/')return PM_MEMORY_INVALID;
      const char *slash=strrchr(path,'/');*name=slash+1;return name_valid(*name)?PM_MEMORY_OK:PM_MEMORY_INVALID;
    }
    i+=n;
  }
  return PM_MEMORY_LIMIT;
}
int pm_memory_module(const PmMemoryReader *r,uint64_t proc,const char *basename,PmMemoryModule *out){
  if(out)memset(out,0,sizeof *out);
  if(!r||!r->read||!out||!kpointer(proc)||!name_valid(basename))return PM_MEMORY_INVALID;
  uint64_t list=0,node=0;int status=read(r,proc+0x3e8,&list,8);if(status)return status;
  if(!kpointer(list))return PM_MEMORY_INVALID;
  if((status=read(r,list,&node,8)))return status;
  PmMemoryModule found={0};unsigned matches=0;
  for(unsigned i=0;node&&i<512;i++){
    if(!kpointer(node))return PM_MEMORY_INVALID;
    unsigned char object[0x58];if((status=read(r,node,object,sizeof object)))return status;
    uint64_t next=u64(object),path=u64(object+8),handle=u64(object+0x28);
    char text[1024];const char *name=NULL;if((status=path_name(r,path,text,&name)))return status;
    if(!strcmp(basename,name)){
      if(++matches>1)return PM_MEMORY_AMBIGUOUS;
      uint64_t base=u64(object+0x30),size=u64(object+0x38);
      if(!urange(base,size)||handle>UINT32_MAX)return PM_MEMORY_INVALID;
      found=(PmMemoryModule){node,base,size,(uint32_t)handle};
    }
    if(next==node)return PM_MEMORY_LIMIT;node=next;
  }
  if(node)return PM_MEMORY_LIMIT;
  if(!matches)return PM_MEMORY_MISSING;*out=found;return PM_MEMORY_OK;
}
static int mapping(const PmMemoryReader *r,uint64_t node,PmMemoryMapping *out,uint64_t *left,uint64_t *right,uint64_t *next){
  if(!kpointer(node))return PM_MEMORY_INVALID;
  unsigned char bytes[0x65];int status=read(r,node,bytes,sizeof bytes);if(status)return status;
  *out=(PmMemoryMapping){node,u64(bytes+0x20),u64(bytes+0x28),bytes[0x64]};
  // Keep the exact protection byte (including non-rwx bits) for restoration.
  if(out->end<=out->start||out->end>UINT64_C(0x800000000000))return PM_MEMORY_INVALID;
  *next=u64(bytes+8);*left=u64(bytes+0x10);*right=u64(bytes+0x18);return PM_MEMORY_OK;
}
int pm_memory_span(const PmMemoryReader *r,uint64_t proc,uint64_t rootOffset,uint64_t address,size_t length,
  PmMemoryMapping *out,size_t capacity,size_t *count){
  if(count)*count=0;
  if(!r||!r->read||!count||!out||!capacity||capacity>4096||!kpointer(proc)||
     !rootOffset||rootOffset>0x1000||rootOffset%8||!urange(address,length))return PM_MEMORY_INVALID;
  uint64_t vm=0,node=0;int status=read(r,proc+0x200,&vm,8);if(status)return status;
  if(!kpointer(vm))return PM_MEMORY_INVALID;
  if((status=read(r,vm+rootOffset,&node,8)))return status;
  PmMemoryMapping m={0};uint64_t left=0,right=0,next=0;unsigned steps=0;
  for(;node&&steps<4096;steps++){
    if((status=mapping(r,node,&m,&left,&right,&next)))return status;
    if(address>=m.start&&address<m.end)break;
    uint64_t child=address<m.start?left:right;
    if(child==node)return PM_MEMORY_LIMIT;node=child;
  }
  if(!node)return PM_MEMORY_MISSING;if(steps==4096)return PM_MEMORY_LIMIT;
  uint64_t cursor=address,end=address+length;size_t used=0;
  while(cursor<end){
    if(used==capacity||used==4096)return PM_MEMORY_LIMIT;
    if(m.start>cursor||m.end<=cursor||(used&&m.start!=cursor))return PM_MEMORY_GAP;
    out[used++]=m;cursor=m.end<end?m.end:end;
    if(cursor==end){*count=used;return PM_MEMORY_OK;}
    if(!next)return PM_MEMORY_GAP;if(next==m.entry)return PM_MEMORY_LIMIT;
    if((status=mapping(r,next,&m,&left,&right,&next)))return status;
  }
  return PM_MEMORY_INVALID;
}
