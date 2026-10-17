// SPDX-License-Identifier: GPL-3.0-or-later
#include "dispatch-receipt.h"
#include "sha256.h"
#include <string.h>
static void digest(const void *data,size_t length,uint8_t out[32]){
  SHA256_CTX hash;sha256_init(&hash);sha256_update(&hash,data,length);sha256_final(&hash,out);
}
static int hex(const char *s,size_t length){
  if(strlen(s)!=length)return 0;for(size_t i=0;i<length;i++)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return 0;return 1;
}
void pm_dispatch_seal(PmDispatchReceipt *r){digest(r,offsetof(PmDispatchReceipt,checksum),r->checksum);}
int pm_dispatch_decode(const void *data,size_t size,const char *request,PmDispatchReceipt *out){
  if(!data||!request||!out)return -1;PmDispatchReceipt r={0};uint8_t checksum[32];
  if(size==sizeof(PmDispatchReceiptV1)){
    PmDispatchReceiptV1 old;memcpy(&old,data,sizeof old);digest(&old,offsetof(PmDispatchReceiptV1,checksum),checksum);
    if(old.version!=1||memcmp(checksum,old.checksum,32))return -1;
    r.magic=old.magic;r.version=2;r.stage=old.stage;r.no_side_effects=old.no_side_effects;
    r.boot_sec=old.boot_sec;r.boot_usec=old.boot_usec;r.pid=old.pid;r.reserved=old.reserved;
    memcpy(r.request,old.request,sizeof r.request);memcpy(r.upload,old.upload,sizeof r.upload);
    memcpy(r.sha256,old.sha256,sizeof r.sha256);memcpy(r.message,old.message,sizeof r.message);
  }else if(size==sizeof r){
    memcpy(&r,data,sizeof r);digest(&r,offsetof(PmDispatchReceipt,checksum),checksum);
    if(r.version!=2||memcmp(checksum,r.checksum,32))return -1;
  }else return -1;
  if(r.magic!=PM_DISPATCH_MAGIC||r.stage<PM_DISPATCH_QUEUED||r.stage>PM_DISPATCH_UNKNOWN||r.plugin>1||r.no_side_effects>1||
     r.request[32]||r.upload[32]||r.sha256[64]||r.message[191]||r.plugin_id[9]||r.plugin_version[4]||r.generation[127]||
     !hex(r.request,32)||!hex(r.upload,32)||!hex(r.sha256,64)||strcmp(r.request,request))return -1;
  if(r.stage==PM_DISPATCH_SPAWNED&&(r.pid<=0||(r.plugin&&(!r.plugin_id[0]||!r.plugin_version[0]||!r.generation[0]))))return -1;
  *out=r;return 0;
}
int pm_dispatch_conflicts(const PmDispatchReceipt *r,const PmDispatchReceipt *prior){
  return r->boot_sec==prior->boot_sec&&r->boot_usec==prior->boot_usec&&
    (!strcmp(r->sha256,prior->sha256)||(r->plugin&&prior->plugin&&r->plugin_id[0]&&!strcmp(r->plugin_id,prior->plugin_id)))&&
    !(prior->stage==PM_DISPATCH_FAILED&&prior->no_side_effects);
}
