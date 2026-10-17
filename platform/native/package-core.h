// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stddef.h>
#define PM_PACKAGE_MAX UINT64_C(1099511627776)
#define PM_PACKAGE_ROOT "/data/ProsperoMenu/package-install"
#define PM_PACKAGE_MAGIC UINT32_C(0x5441504b)
enum { PM_PKG_QUEUED=1,PM_PKG_HASHING,PM_PKG_PREFLIGHT,PM_PKG_SUBMITTING,
       PM_PKG_INSTALLING,PM_PKG_COMPLETED,PM_PKG_REJECTED,PM_PKG_UNKNOWN };
typedef struct {
 uint32_t magic,version;uint64_t bytes;int64_t bootSec,bootUsec;
 uint32_t type,platform,patch,reserved; // v1: 0/full file digest; v2: 1/inspected header digest
 char request[33],upload[33],sha256[65],content[37],name[128];
 unsigned char checksum[32];
} PmPackageRequest;
typedef struct {
 PmPackageRequest input;uint32_t stage,clean,accepted,observed;
 int32_t error,pid;uint64_t updated,downloaded,total;char state[16],message[192];
 unsigned char checksum[32];
} PmPackageReceipt;
static inline int pm_package_ended_preflight(const PmPackageReceipt*r,const PmPackageRequest*current){
 return current->bootSec>0 && r->input.bootSec>0 && (r->input.bootSec!=current->bootSec||r->input.bootUsec!=current->bootUsec) &&
  r->stage>=PM_PKG_QUEUED && r->stage<=PM_PKG_PREFLIGHT && r->clean && !r->accepted && !r->observed;
}
typedef struct { uint64_t bytes;uint32_t type,platform,patch;char content[37]; } PmPackageInfo;
typedef int (*PmPackageRead)(void*,uint64_t,void*,size_t);
int pm_package_inspect(PmPackageRead read,void* context,uint64_t bytes,PmPackageInfo* out);
int pm_package_request_valid(const PmPackageRequest* request);
int pm_package_hex(const char* value,size_t length);
int pm_package_source_path_valid(const char* path);
typedef struct {char state[16];uint64_t downloaded,total;int32_t error;} PmPackageProgress;
// A successful API return alone is not completion. Require progress belonging
// to this submission and two terminal observations. Prior playable installs
// must never be mistaken for a newly submitted replacement/patch.
typedef struct {PmPackageProgress baseline;unsigned baselineValid,observed,terminal,unproven;} PmPackageTracker;
// Returns -2 after 120 consecutive unproven none/already-complete polls,
// -1 for invalid/error/lost-task status, 0 while observing, 1 for completion.
int pm_package_track(PmPackageTracker* tracker,const PmPackageProgress* progress,uint64_t bytes,uint32_t platform);
