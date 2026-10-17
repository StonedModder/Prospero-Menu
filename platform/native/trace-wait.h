// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
enum { PM_TRACE_PENDING=0,PM_TRACE_STOPPED=1,PM_TRACE_EXITED=2,PM_TRACE_POLL_ERROR=3 };
enum { PM_TRACE_OK=0,PM_TRACE_TIMEOUT=1,PM_TRACE_GONE=2,PM_TRACE_WRONG_STOP=3,PM_TRACE_ERROR=4 };
typedef struct { int kind,signal,error; } PmTraceEvent;
typedef struct {
 void* context;
 int (*now)(void*,uint64_t*);
 PmTraceEvent (*poll)(void*);
 void (*sleep)(void*,unsigned);
} PmTraceWaitOps;
// Polls only; it never resumes, kills or sends a signal to a target. Callers
// retain ownership of restoration/detach and treat uncertain outcomes as such.
int pm_trace_wait(const PmTraceWaitOps*,int expectedSignal,unsigned timeoutMs,PmTraceEvent* last);
