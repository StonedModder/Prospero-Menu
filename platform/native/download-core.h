// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct { int status;uint64_t length;int has_length,chunked;char location[8193]; } PmDownloadHeaders;
// 0 success, -1 malformed or contradictory headers. Does not trust body lengths.
int pm_download_headers(const char *,size_t,PmDownloadHeaders *);
typedef struct {
 void *context;
 int (*cancelled)(void *);
 int (*read)(void *,void *,size_t);
 int (*write)(void *,const void *,size_t);
 void (*progress)(void *,uint64_t,uint64_t);
} PmDownloadStream;
enum { PM_DL_OK=0,PM_DL_CANCELLED=1,PM_DL_LIMIT=2,PM_DL_TRUNCATED=3,PM_DL_READ=4,PM_DL_WRITE=5,PM_DL_EMPTY=6 };
int pm_download_stream(const PmDownloadStream *,const PmDownloadHeaders *,uint64_t);

int pm_download_chunk(const PmDownloadStream *,const PmDownloadHeaders *,uint64_t,uint64_t *,const void *,size_t);
int pm_download_complete(const PmDownloadHeaders *,uint64_t);
// Validate real available bytes, retain 16 MiB, and limit unchecked writes to
// 4 MiB. Unknown-length responses pass only their next write, not the file cap.
int pm_download_space(uint64_t,uint64_t,uint64_t,uint64_t *);
