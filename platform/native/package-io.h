// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "package-core.h"
int pm_package_directory(void);
int pm_package_open_source(const char* upload,char* path,size_t capacity);
int pm_package_request_load(PmPackageRequest* request);
int pm_package_request_save(PmPackageRequest* request);
int pm_package_receipt_load(const char* request,PmPackageReceipt* receipt);
int pm_package_receipt_save(PmPackageReceipt* receipt);
int pm_package_boot(PmPackageRequest* request,int compare);
uint64_t pm_package_millis(void);
