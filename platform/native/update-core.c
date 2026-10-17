// SPDX-License-Identifier: GPL-3.0-or-later
#include "update-core.h"
#include "update-profiles.h"
#include "sha256.h"
#include "elf-image.h"
#include <string.h>
#define STATE_MAGIC UINT32_C(0x55544354)
static int zero(const void *p,size_t n){const uint8_t *b=p;for(size_t i=0;i<n;i++)if(b[i])return 0;return 1;}
static uint32_t firmware_word(const PmUpdateHeader *h,unsigned word){
  if(!word)return h->firmware_mask;
  if(h->version!=2||word>2)return 0;
  uint32_t value;memcpy(&value,h->reserved+(word-1)*4,sizeof value);return value;
}
static int firmware_mask_valid(const PmUpdateHeader *h){
  if(h->version==1)return h->firmware_mask&&!(h->firmware_mask&~PM_UPDATE_MASK)&&zero(h->reserved,sizeof h->reserved);
  if(h->version!=2||!zero(h->reserved+8,sizeof h->reserved-8))return 0;
  uint32_t any=0;for(unsigned i=0;i<3;i++){uint32_t word=firmware_word(h,i);if(word&~pm_update_masks[i])return 0;any|=word;}
  return any!=0;
}
void pm_update_digest(const void *p,size_t n,uint8_t out[32]){SHA256_CTX h;sha256_init(&h);sha256_update(&h,p,n);sha256_final(&h,out);}
void pm_update_hex(const uint8_t hash[32],char out[65]){const char *digits="0123456789abcdef";for(unsigned i=0;i<32;i++){out[i*2]=digits[hash[i]>>4];out[i*2+1]=digits[hash[i]&15];}out[64]=0;}
int pm_update_unhex(const char *s,uint8_t out[32]){if(!s||strlen(s)!=64)return -1;for(unsigned i=0;i<32;i++){unsigned v=0;for(unsigned j=0;j<2;j++){char c=s[i*2+j];if(c>='0'&&c<='9')v=v*16+c-'0';else if(c>='a'&&c<='f')v=v*16+c-'a'+10;else return -1;}out[i]=(uint8_t)v;}return 0;}
int pm_update_header(const void *raw,size_t size,PmUpdateHeader *out){
  if(!raw||size<sizeof *out)return -1;PmUpdateHeader h;memcpy(&h,raw,sizeof h);uint8_t digest[32];pm_update_digest(&h,offsetof(PmUpdateHeader,checksum),digest);
  if(memcmp(h.magic,"TACUPD01",8)||h.header_size!=256||h.state_schema!=1||h.bytes<64||h.bytes>PM_UPDATE_LIMIT||
     !firmware_mask_valid(&h)||zero(h.payload_hash,32)||zero(h.source_hash,32)||memcmp(h.checksum,digest,32))return -1;
  size_t n=0;for(;n<sizeof h.label&&h.label[n];n++)if(h.label[n]<32||h.label[n]>126)return -1;
  if(!n||n==sizeof h.label||!zero(h.label+n,sizeof h.label-n))return -1;
  *out=h;return 0;
}
int pm_update_supported(const PmUpdateHeader *h,uint32_t fw){
  if(!h||(h->version!=1&&h->version!=2))return 0;
  fw&=0xffff0000;for(unsigned i=0;i<sizeof pm_update_profiles/sizeof pm_update_profiles[0];i++)
    if(pm_update_profiles[i]==fw)return !!(firmware_word(h,i/32)&(1u<<(i%32)));
  return 0;
}
int pm_update_image(const void *raw,size_t size,uint32_t fw,PmUpdateHeader *out){
  PmUpdateHeader h;if(pm_update_header(raw,size,&h)||size!=sizeof h+h.bytes||!pm_update_supported(&h,fw))return -1;
  const uint8_t *elf=(const uint8_t*)raw+sizeof h;uint8_t digest[32];pm_update_digest(elf,(size_t)h.bytes,digest);
  PmElfImage image;char error[192];if(memcmp(h.payload_hash,digest,32)||pm_elf_inspect(elf,(size_t)h.bytes,&image,error,sizeof error))return -1;
  *out=h;return 0;
}
void pm_update_init(PmUpdateState *s){memset(s,0,sizeof *s);s->magic=STATE_MAGIC;s->version=1;}
void pm_update_seal(PmUpdateState *s){pm_update_digest(s,offsetof(PmUpdateState,checksum),s->checksum);}
static int optional_header(const PmUpdateHeader *h){PmUpdateHeader out;return zero(h,sizeof *h)||!pm_update_header(h,sizeof *h,&out);}
int pm_update_decode(const void *raw,size_t size,PmUpdateState *out){
  if(size!=sizeof *out)return -1;PmUpdateState s;memcpy(&s,raw,sizeof s);uint8_t sum[32];pm_update_digest(&s,offsetof(PmUpdateState,checksum),sum);
  if(s.magic!=STATE_MAGIC||s.version!=1||s.phase>PM_UPDATE_TRIAL||s.reserved||s.revision>INT64_MAX||memcmp(sum,s.checksum,32)||
     !optional_header(&s.good)||!optional_header(&s.previous)||!optional_header(&s.candidate)||!optional_header(&s.attempt)||
     s.boot_sec<0||s.boot_usec<0||s.boot_usec>=1000000||s.pid<0||s.attempt_kind>PM_BOOT_ROLLBACK)return -1;
  if((s.phase==PM_UPDATE_IDLE)!=zero(&s.candidate,sizeof s.candidate))return -1;
  if(s.attempt_kind==PM_BOOT_NONE){if(s.boot_sec||s.boot_usec||s.pid||!zero(&s.attempt,sizeof s.attempt))return -1;}
  else if(!s.boot_sec||zero(&s.attempt,sizeof s.attempt))return -1;
  if(s.phase==PM_UPDATE_TRIAL&&(s.attempt_kind!=PM_BOOT_TRIAL||memcmp(&s.attempt,&s.candidate,sizeof s.attempt)))return -1;
  *out=s;return 0;
}
int pm_update_stage(PmUpdateState *s,const PmUpdateHeader *header,uint64_t revision){
  PmUpdateHeader h;if(s->revision!=revision||s->phase!=PM_UPDATE_IDLE||pm_update_header(header,sizeof *header,&h)||!memcmp(h.payload_hash,s->good.payload_hash,32))return -1;
  s->candidate=h;s->phase=PM_UPDATE_STAGED;return 0;
}
int pm_update_arm(PmUpdateState *s,uint64_t revision){if(s->revision!=revision||s->phase!=PM_UPDATE_STAGED||!s->good.bytes)return -1;s->phase=PM_UPDATE_ARMED;return 0;}
int pm_update_discard(PmUpdateState *s,uint64_t revision){if(s->revision!=revision||s->phase==PM_UPDATE_TRIAL)return -1;s->phase=PM_UPDATE_IDLE;memset(&s->candidate,0,sizeof s->candidate);return 0;}
int pm_update_retirable(const PmUpdateState *s,const uint8_t hash[32],uint64_t revision){
  if(s->revision!=revision||zero(hash,32))return -1;
  const PmUpdateHeader *keep[]={&s->good,&s->previous,&s->candidate,&s->attempt};
  for(unsigned i=0;i<sizeof keep/sizeof keep[0];i++)if(keep[i]->bytes&&!memcmp(hash,keep[i]->payload_hash,32))return -1;
  return 0;
}
int pm_update_revert(PmUpdateState *s,uint64_t revision){
  if(s->revision!=revision)return -1;
  // A failed trial returns to the last confirmed build; a confirmed update can
  // request its previous build. Both changes are deferred to another boot.
  const PmUpdateHeader *target=s->phase==PM_UPDATE_TRIAL?&s->good:&s->previous;
  if(zero(target,sizeof *target))return -1;s->candidate=*target;s->phase=PM_UPDATE_ARMED;return 0;
}
int pm_update_plan(PmUpdateState *s,int64_t sec,int64_t usec,const PmUpdateHeader *fallback){
  PmUpdateHeader h;if(sec<=0||usec<0||usec>=1000000||pm_update_header(fallback,sizeof *fallback,&h)||
     (s->attempt_kind&&s->boot_sec==sec&&s->boot_usec==usec))return -1;
  unsigned kind=PM_BOOT_BASELINE;
  if(s->phase==PM_UPDATE_TRIAL){s->phase=PM_UPDATE_IDLE;memset(&s->candidate,0,sizeof s->candidate);kind=PM_BOOT_ROLLBACK;}
  if(s->phase==PM_UPDATE_ARMED){s->attempt=s->candidate;s->phase=PM_UPDATE_TRIAL;kind=PM_BOOT_TRIAL;}
  else s->attempt=zero(&s->good,sizeof s->good)?h:s->good;
  s->boot_sec=sec;s->boot_usec=usec;s->pid=0;s->attempt_kind=kind;return 0;
}
int pm_update_plan_bundled(PmUpdateState *s,int64_t sec,int64_t usec,const PmUpdateHeader *bundled){
  PmUpdateHeader h;if(sec<=0||usec<0||usec>=1000000||pm_update_header(bundled,sizeof *bundled,&h)||
     (s->attempt_kind&&s->boot_sec==sec&&s->boot_usec==usec))return -1;
  // Do not replace an unrelated update the user has already staged or armed.
  if((s->phase==PM_UPDATE_STAGED||s->phase==PM_UPDATE_ARMED)&&memcmp(&s->candidate,&h,sizeof h))return -2;
  // An explicit new-boot test may retry/replace an unacknowledged trial, but
  // good and previous remain intact. The normal launcher still rolls it back.
  s->attempt=h;s->boot_sec=sec;s->boot_usec=usec;s->pid=0;
  if(memcmp(&s->good,&h,sizeof h)){
    s->candidate=h;s->phase=PM_UPDATE_TRIAL;s->attempt_kind=PM_BOOT_TRIAL;
  }else{
    memset(&s->candidate,0,sizeof s->candidate);s->phase=PM_UPDATE_IDLE;s->attempt_kind=PM_BOOT_BASELINE;
  }
  return 0;
}
int pm_update_fallback(PmUpdateState *s,const PmUpdateHeader *fallback){
  PmUpdateHeader h;if(pm_update_header(fallback,sizeof *fallback,&h)||s->pid)return -1;
  s->attempt=h;s->attempt_kind=PM_BOOT_ROLLBACK;s->phase=PM_UPDATE_IDLE;memset(&s->candidate,0,sizeof s->candidate);return 0;
}
int pm_update_ack(PmUpdateState *s,int64_t sec,int64_t usec,int32_t pid,const uint8_t source[32]){
  if(!s->attempt_kind||s->boot_sec!=sec||s->boot_usec!=usec||pid<=0||s->pid!=pid||memcmp(source,s->attempt.source_hash,32))return -1;
  if(memcmp(s->good.payload_hash,s->attempt.payload_hash,32)){s->previous=s->good;s->good=s->attempt;}
  if(s->phase==PM_UPDATE_TRIAL){s->phase=PM_UPDATE_IDLE;memset(&s->candidate,0,sizeof s->candidate);}return 0;
}
