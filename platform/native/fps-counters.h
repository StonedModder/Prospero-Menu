// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct {void *context;int (*read)(void*,uint64_t,void*,size_t);} PmCounterReader;
// AGC ring layout documented by OnionHEN/LightningMods, following PHU by ArkSama.
// These are counter observations, not measured FPS until sampled over time.
int pm_fps_ring(const PmCounterReader*,uint64_t *count);
