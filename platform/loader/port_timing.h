// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#ifdef ETAHEN_STARTUP_PROFILE
#include <time.h>
#include <stdio.h>
#include <unistd.h>
static inline uint64_t port_timing_now(void) {
 struct timespec ts;
 if(clock_gettime(CLOCK_MONOTONIC,&ts))return 0;
 return (uint64_t)ts.tv_sec*1000000u+(uint64_t)ts.tv_nsec/1000u;
}
static inline void port_timing_end(const char* operation,int target,uint64_t start,uint64_t detail) {
 const uint64_t now=port_timing_now();
 if(start&&now>=start)
  printf("[eta timing] pid=%d target=%d op=%s us=%llu detail=%llu\n",getpid(),target,operation,(unsigned long long)(now-start),(unsigned long long)detail);
}
#else
static inline uint64_t port_timing_now(void){return 0;}
static inline void port_timing_end(const char* op,int target,uint64_t start,uint64_t detail){(void)op;(void)target;(void)start;(void)detail;}
#endif
