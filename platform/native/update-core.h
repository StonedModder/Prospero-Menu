// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>
#define PM_UPDATE_LIMIT (64u*1024u*1024u)
#define PM_UPDATE_MASK 0x3fffu
// Wire v1: firmware_mask is the original fourteen bits and reserved is zero.
// Wire v2: reserved[0..7] extends the mask by two little-endian uint32 words.
// Keep the 256-byte header/state layout and original bit identities unchanged.
typedef struct {
  char magic[8];uint32_t version,header_size;uint64_t bytes;
  uint32_t firmware_mask,state_schema;uint8_t payload_hash[32],source_hash[32];
  char label[64];uint8_t reserved[64],checksum[32];
} PmUpdateHeader;
enum { PM_UPDATE_IDLE=0,PM_UPDATE_STAGED=1,PM_UPDATE_ARMED=2,PM_UPDATE_TRIAL=3 };
enum { PM_BOOT_NONE=0,PM_BOOT_BASELINE=1,PM_BOOT_TRIAL=2,PM_BOOT_ROLLBACK=3 };
typedef struct {
  uint32_t magic,version,phase,reserved;uint64_t revision;
  PmUpdateHeader good,previous,candidate,attempt;
  int64_t boot_sec,boot_usec;int32_t pid;uint32_t attempt_kind;
  uint8_t checksum[32];
} PmUpdateState;
_Static_assert(sizeof(PmUpdateHeader)==256,"Update header is a fixed wire format");
void pm_update_digest(const void*,size_t,uint8_t[32]);
void pm_update_hex(const uint8_t[32],char[65]);
int pm_update_unhex(const char*,uint8_t[32]);
int pm_update_header(const void*,size_t,PmUpdateHeader*);
int pm_update_image(const void*,size_t,uint32_t,PmUpdateHeader*);
int pm_update_supported(const PmUpdateHeader*,uint32_t);
void pm_update_init(PmUpdateState*);
void pm_update_seal(PmUpdateState*);
int pm_update_decode(const void*,size_t,PmUpdateState*);
int pm_update_stage(PmUpdateState*,const PmUpdateHeader*,uint64_t);
int pm_update_arm(PmUpdateState*,uint64_t);
int pm_update_revert(PmUpdateState*,uint64_t);
int pm_update_discard(PmUpdateState*,uint64_t);
int pm_update_retirable(const PmUpdateState*,const uint8_t[32],uint64_t);
int pm_update_plan(PmUpdateState*,int64_t,int64_t,const PmUpdateHeader*);
// Explicit test launcher: use its embedded runtime, retaining the confirmed
// build until acknowledgement. -2 means a different update is pending.
int pm_update_plan_bundled(PmUpdateState*,int64_t,int64_t,const PmUpdateHeader*);
int pm_update_fallback(PmUpdateState*,const PmUpdateHeader*);
int pm_update_ack(PmUpdateState*,int64_t,int64_t,int32_t,const uint8_t[32]);
