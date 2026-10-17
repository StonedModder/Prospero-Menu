// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from GronedWaffel/ps-neighborhood receiver-ps5/install-abi.h.
// Common input/progress contract from historical etaHEN; only 13.60's
// additional error-tail offset is evidenced. Do not read that tail elsewhere.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "package-core.h"
typedef struct {const char *uri,*extra,*scenario,*content,*name,*icon;uint32_t option;uint8_t flag;} PmInstallMeta;
typedef struct {char content[48];int type,platform;} PmInstallPackage;
typedef struct {char languages[30][8],scenarios[64][3],contents[64][48];uint8_t reserved[6480];} PmInstallPlaygo;
typedef struct {int32_t code,version;char description[512],type[9];} PmInstallError;
typedef struct {char state[16],source[8];uint32_t seconds;uint64_t downloaded,initial,total;uint32_t promote;uint8_t reserved[100];PmInstallError error;int32_t copyPercent;_Bool copyOnly;uint8_t trailing[15];} PmInstallStatus;
_Static_assert(sizeof(PmInstallMeta)==56&&offsetof(PmInstallMeta,flag)==0x34,"InstallByPackage metadata ABI");
_Static_assert(sizeof(PmInstallPackage)==56&&sizeof(PmInstallPlaygo)==9984,"InstallByPackage output ABI");
_Static_assert(sizeof(PmInstallStatus)==712&&offsetof(PmInstallStatus,error)==0xa0&&offsetof(PmInstallStatus,total)==0x30,"13.60 installation status ABI");

// The historical 600-byte and captured 712-byte outputs share these first
// 56 bytes. Pass overallocated aligned storage, never a legacy short struct.
// Reserved output capacity is not an assertion about another firmware's tail.
typedef struct {char state[16],source[8];uint32_t seconds;uint64_t downloaded,initial,total;} PmInstallProgressPrefix;
typedef union {uint64_t alignment;unsigned char bytes[4096];} PmInstallStatusStorage;
_Static_assert(sizeof(PmInstallProgressPrefix)==56&&offsetof(PmInstallProgressPrefix,downloaded)==0x20&&offsetof(PmInstallProgressPrefix,total)==0x30,"Common etaHEN progress prefix");
static inline int pm_install_progress(const void* raw,size_t bytes,uint32_t firmware,PmPackageProgress* out){
 if(!raw||!out||bytes<sizeof(PmInstallProgressPrefix))return -1;
 PmInstallProgressPrefix p;memcpy(&p,raw,sizeof p);
 if(!memchr(p.state,0,sizeof p.state)||!p.state[0]||p.total>PM_PACKAGE_MAX||p.downloaded>p.total)return -1;
 memset(out,0,sizeof *out);memcpy(out->state,p.state,sizeof out->state);out->downloaded=p.downloaded;out->total=p.total;
 if((firmware&0xffff0000u)==0x13600000u){
  if(bytes<sizeof(PmInstallStatus))return -1;
  memcpy(&out->error,(const unsigned char*)raw+offsetof(PmInstallStatus,error),sizeof out->error);
 }
 // Else the documented textual error state is authoritative. Its variable
 // numeric-detail tail is deliberately not interpreted as a 13.60 structure.
 return 0;
}
