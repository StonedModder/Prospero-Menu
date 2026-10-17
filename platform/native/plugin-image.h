// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>
#define PM_PLUGIN_HEADER 29
typedef struct { char id[10],version[5]; size_t offset; } PmPluginImage;
int pm_plugin_inspect(const uint8_t *data,size_t size,PmPluginImage *plugin,char *reason,size_t capacity);
