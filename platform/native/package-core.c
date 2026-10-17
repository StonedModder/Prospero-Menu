// SPDX-License-Identifier: GPL-3.0-or-later
#include "package-core.h"
#include <string.h>
static uint32_t be32(const unsigned char* p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static uint64_t be64(const unsigned char* p){return (uint64_t)be32(p)<<32|be32(p+4);}
static uint64_t le64(const unsigned char* p){uint64_t n=0;for(unsigned i=0;i<8;i++)n|=(uint64_t)p[i]<<(8*i);return n;}
int pm_package_source_path_valid(const char* p){
 if(!p)return 0;size_t n=strlen(p);if(n<7||n>=1024)return 0;
 if(strncmp(p,"/data/",6) && !(n>10&&!strncmp(p,"/mnt/usb",8)&&p[8]>='0'&&p[8]<='7'&&p[9]=='/'))return 0;
 const char* start=p+1;
 for(const char* at=start;;at++){
  if(*at && ((unsigned char)*at<32||*at=='\\'))return 0;
  if(!*at||*at=='/'){
   size_t size=(size_t)(at-start);if(!size||(size==1&&start[0]=='.')||(size==2&&start[0]=='.'&&start[1]=='.'))return 0;
   if(!*at)break;start=at+1;
  }
 }
 return 1;
}
int pm_package_hex(const char* p,size_t n){if(!p||strlen(p)!=n)return 0;for(size_t i=0;i<n;i++)if(!((p[i]>='0'&&p[i]<='9')||(p[i]>='a'&&p[i]<='f')))return 0;return 1;}
static int content_valid(const char* p){
 if(strlen(p)!=36||p[6]!='-'||p[16]!='_'||p[17]!='0'||p[18]!='0'||p[19]!='-'||!memcmp(p+7,"NPXS",4))return 0;
 for(unsigned i=0;i<36;i++){
  if(i==6||i==16||i==19)continue;
  if(i>=7&&i<11){if(p[i]<'A'||p[i]>'Z')return 0;}
  else if(i>=11&&i<16){if(p[i]<'0'||p[i]>'9')return 0;}
  else if(!((p[i]>='A'&&p[i]<='Z')||(p[i]>='0'&&p[i]<='9')))return 0;
 }return 1;
}
static int type_valid(uint32_t type,uint32_t platform){
 if(platform==5)return type==0x20||type==0x21||type==0x22||type==0x26;
 return platform==4&&(type==0x1a||type==0x1b||type==0x1c||type==0x1e);
}
static unsigned platform_for(const char* content){return (!memcmp(content+7,"PPSA",4)||!memcmp(content+7,"MOUU",4))?5:4;}
int pm_package_request_valid(const PmPackageRequest* r){
 if(r->magic!=PM_PACKAGE_MAGIC||!((r->version==1&&!r->reserved)||(r->version==2&&r->reserved==1))||r->bytes<8192||r->bytes>PM_PACKAGE_MAX||
   r->bootSec<=0||r->bootUsec<0||r->bootUsec>=1000000||r->patch>1||
   r->request[32]||r->upload[32]||r->sha256[64]||r->content[36]||r->name[127])return 0;
 if(!pm_package_hex(r->request,32)||!pm_package_hex(r->upload,32)||!pm_package_hex(r->sha256,64)||!content_valid(r->content)||
   r->platform!=platform_for(r->content)||!type_valid(r->type,r->platform))return 0;
 for(size_t i=0;r->name[i];i++)if((unsigned char)r->name[i]<32)return 0;
 return r->name[0]!=0;
}
int pm_package_inspect(PmPackageRead read,void* context,uint64_t size,PmPackageInfo* out){
 unsigned char header[0x438];uint64_t base=0;unsigned fih=0;
 if(!read||!out||size<8192||size>PM_PACKAGE_MAX||read(context,0,header,sizeof header))return -1;
 if(be32(header)==0x7f464948){
  if(header[6]!=3||header[7]||(header[5]!=0&&header[5]!=128))return -1;
  fih=1;base=le64(header+0x58);uint64_t start=le64(header+0x10),length=le64(header+0x18);
  if(base>size-sizeof header||start<0x100||start>base||!length||length>base-start||read(context,base,header,sizeof header))return -1;
 }
 if(be32(header)!=0x7f434e54)return -1;
 PmPackageInfo info={.bytes=size};memcpy(info.content,header+0x40,36);
 if(!content_valid(info.content))return -1;
 info.platform=platform_for(info.content);info.type=be32(header+0x74);uint32_t flags=be32(header+0x78);
 if(!type_valid(info.type,info.platform))return -1;
 if(info.platform==5){
  uint32_t count=be32(header+0x10);uint64_t table=be32(header+0x18),body=be64(header+0x20),length=be64(header+0x28);
  if(!count||count>4096||table<0x80||body<0x80||body>size-base||!length||length>size-base-body||table+(uint64_t)count*32>body+length)return -1;
 }else if(fih||be64(header+0x430)!=size)return -1;
 info.patch=info.platform==5?!!(flags&0x40100000):(info.type==0x1e||!!(flags&0x60100000));*out=info;return 0;
}
int pm_package_track(PmPackageTracker* t,const PmPackageProgress* p,uint64_t bytes,uint32_t platform){
 if(!memchr(p->state,0,sizeof p->state)||!p->state[0]||p->total>PM_PACKAGE_MAX||p->downloaded>p->total||p->error){t->terminal=0;return -1;}
 // The textual terminal failure remains authoritative even when its optional
 // numeric error is zero. A vanished task after observed transfer is unresolved,
 // not a reason to wait for an unrelated later playable state.
 if(!strcmp(p->state,"error")||(!strcmp(p->state,"none")&&t->observed)){t->terminal=0;return -1;}
 int terminal=!strcmp(p->state,"completed")||!strcmp(p->state,"playable");
 int changed=!t->baselineValid||strcmp(p->state,t->baseline.state)||p->total!=t->baseline.total||p->downloaded!=t->baseline.downloaded;
 // A partial playable chunk is active transfer, not a full installation.
 if(strcmp(p->state,"none")&&changed&&p->total&&p->downloaded<p->total)t->observed=1;
 if(!t->observed&&(!strcmp(p->state,"none")||(p->total&&p->downloaded==p->total))){
  if(t->unproven<120)t->unproven++;
  if(t->unproven>=120){t->terminal=0;return -2;}
 }else t->unproven=0;
 int complete=terminal&&p->total&&p->downloaded==p->total&&(platform!=4||p->total==bytes);
 if(!complete||!t->observed)t->terminal=0;else if(t->terminal<2)t->terminal++;
 return t->terminal==2?1:0;
}
