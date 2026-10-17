// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "package-core.h"
#include "sha256.h"
typedef struct {PmPackageRead read;void* context;SHA256_CTX hash;} PmPackageHeaderReader;
static inline int pm_package_header_read(void* context,uint64_t at,void* bytes,size_t count){
 PmPackageHeaderReader* r=context;int result=r->read(r->context,at,bytes,count);
 if(!result)sha256_update(&r->hash,bytes,count);return result;
}
// Hash exactly the header reads used for bounds/content inspection, including
// both FIH and CNT headers. This is explicitly not a whole-package checksum.
static inline int pm_package_header_fingerprint(PmPackageRead read,void* context,uint64_t bytes,PmPackageInfo* info,unsigned char digest[32]){
 PmPackageHeaderReader r={.read=read,.context=context};sha256_init(&r.hash);
 int result=pm_package_inspect(pm_package_header_read,&r,bytes,info);
 if(!result)sha256_final(&r.hash,digest);return result;
}
