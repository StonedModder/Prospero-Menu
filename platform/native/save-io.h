// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "save-core.h"
#include <sys/stat.h>
typedef struct { int directory,image,key;struct stat imageStat,keyStat;char stamp[65]; } PmSaveSource;
int pm_save_directory(const char* path,int create);
int pm_save_child_directory(int parent,const char* name,int create);
int pm_save_source_open(const PmSaveRequest* input,PmSaveSource* source);
void pm_save_source_close(PmSaveSource* source);
int pm_save_source_current(const PmSaveRequest* input,const PmSaveSource* source);
int pm_save_volume(unsigned destination,char path[64],char stamp[65]);
int pm_save_volume_open(unsigned destination,char path[64],char stamp[65]);
int pm_save_read_all(int fd,uint64_t offset,void* data,size_t bytes);
int pm_save_write_all(void* fd,const void* data,size_t bytes);
int pm_save_request_load(PmSaveRequest* input);
int pm_save_request_save(PmSaveRequest* input);
int pm_save_receipt_load(const char* id,PmSaveReceipt* receipt);
int pm_save_receipt_save(PmSaveReceipt* receipt);
int pm_save_boot(PmSaveRequest* input,int compare);
uint64_t pm_save_millis(void);
typedef int (*PmSaveProgress)(void* context,uint64_t bytes);
int pm_save_copy(int source,int target,uint64_t bytes,char sha256[65],PmSaveProgress progress,void* context);
int pm_save_hash_file(int file,uint64_t bytes,char sha256[65],PmSaveProgress progress,void* context);
