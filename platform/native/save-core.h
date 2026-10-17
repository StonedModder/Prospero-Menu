// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stddef.h>
#define PM_SAVE_ROOT "/data/ProsperoMenu/save-export"
#define PM_SAVE_MAX_BYTES (UINT64_C(32)*1024*1024*1024)
#define PM_SAVE_MAX_ARCHIVE (PM_SAVE_MAX_BYTES+32*1024*1024)
#define PM_SAVE_MAX_FILES 10000u
#define PM_SAVE_MAX_CATALOG 2048u
#define PM_SAVE_MAGIC UINT32_C(0x54415356)
enum { PM_SAVE_CATALOG=1, PM_SAVE_EXPORT=2 };
enum { PM_SAVE_QUEUED=1,PM_SAVE_PREPARING,PM_SAVE_COPYING,PM_SAVE_MOUNTING,
       PM_SAVE_EXTRACTING,PM_SAVE_UNMOUNTING,PM_SAVE_PUBLISHING,PM_SAVE_COMPLETE,PM_SAVE_REJECTED,PM_SAVE_UNKNOWN };
typedef struct {
 uint32_t magic,version,mode,destination; // destination 0: FTP share, 1..8: usb0..7
 int64_t bootSec,bootUsec;
 char request[33],user[9],title[10],slot[128],stamp[65],volume[65];
 uint32_t platform,reserved; // 4 or 5 for export; no caller-controlled paths
 unsigned char checksum[32];
} PmSaveRequest;
typedef struct {
 PmSaveRequest input;
 uint32_t stage,clean,mounted,files;int32_t error,pid;
 uint64_t updated,completed,total,archiveBytes;
 char output[256],sha256[65],message[192];unsigned char checksum[32];
} PmSaveReceipt;
int pm_save_request_valid(const PmSaveRequest* input);
int pm_save_hex(const char* text,size_t length);
int pm_save_user(const char* text);
int pm_save_slot(const char* text);
int pm_save_relative(const char* text);
int pm_save_platform(const char* title);
void pm_save_digest(const void* data,size_t bytes,unsigned char output[32]);
void pm_save_hash_text(const unsigned char digest[32],char text[65]);
typedef int (*PmSaveWrite)(void* context,const void* data,size_t bytes);
// POSIX pax archive headers; paths never become links or absolute entries.
int pm_save_tar_header(PmSaveWrite write,void* context,const char* path,uint64_t bytes,int directory);
int pm_save_tar_padding(PmSaveWrite write,void* context,uint64_t bytes);
int pm_save_tar_end(PmSaveWrite write,void* context);
int pm_save_json_string(PmSaveWrite write,void* context,const char* text);
