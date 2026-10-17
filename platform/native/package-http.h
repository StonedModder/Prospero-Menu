// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct {unsigned status,head;uint64_t start,length;} PmPackageHttp;
// Complete, bounded HTTP/1.x headers only; one range and no request body.
int pm_package_http_parse(const char* header,size_t bytes,const char* target,uint64_t size,PmPackageHttp* out);
int pm_package_http_response(char* buffer,size_t capacity,const PmPackageHttp* request,uint64_t size);
typedef struct PmPackageSource PmPackageSource;
// The source owns a duplicated descriptor; only the exact request URL is served.
int pm_package_source_start(PmPackageSource** source,int fd,const char* request,uint64_t bytes,char* url,size_t capacity);
int pm_package_source_healthy(PmPackageSource* source);
void pm_package_source_stop(PmPackageSource* source);
