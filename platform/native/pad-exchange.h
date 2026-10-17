// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pad-bridge.h"
// Offset-only access to the receipt-owned pad block. No game pointer is supplied
// by the caller/UI. The adapter checks process identity around every copy.
typedef struct {
 void *context;
 int (*read)(void*,size_t,void*,size_t);
 int (*write)(void*,size_t,const void*,size_t);
 uint64_t (*now)(void*);
} PmPadExchangeOps;
typedef struct {uint32_t epoch,mode;int32_t owner;uint32_t connection;} PmPadRequest;
typedef struct {
 uint32_t count,faults,epoch,reset;uint64_t deadline;
 PmPadEvent events[PM_PAD_EVENTS];
} PmPadBatch;
// 0 = exchange confirmed, 1 = rejected before writes, -1 = write outcome
// uncertain. Every failure clears the batch. No automatic retry or rollback of
// a live lease: an interrupted odd revision is unreadable, and an even lease
// can last at most 400 ms from the supplied monotonic timestamp.
int pm_pad_exchange(const PmPadExchangeOps*,uint64_t arena,uint64_t cookie,
 const PmPadRequest*,uint64_t now,PmPadBatch*);
