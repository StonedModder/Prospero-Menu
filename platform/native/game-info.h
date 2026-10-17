// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct {uint32_t appId;unsigned titleOffset;char title[10];} PmAppInfo;
int pm_game_title(const char* title);
int pm_app_info_decode(const unsigned char* bytes,size_t length,PmAppInfo* out);
int pm_game_app_info(int pid,PmAppInfo* out);
int pm_game_info_available(void);
