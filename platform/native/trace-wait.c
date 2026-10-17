// SPDX-License-Identifier: GPL-3.0-or-later
#include "trace-wait.h"
#include <stddef.h>
int pm_trace_wait(const PmTraceWaitOps* o,int expected,unsigned timeout,PmTraceEvent* last){
 if(!o||!o->now||!o->poll||!o->sleep||!last||!timeout||timeout>10000)return PM_TRACE_ERROR;
 *last=(PmTraceEvent){0};uint64_t start,previous;
 if(o->now(o->context,&start))return PM_TRACE_ERROR;previous=start;
 // An iteration bound also protects against a broken/frozen time source.
 for(unsigned i=0;i<=timeout+1;i++){
  uint64_t now;if(o->now(o->context,&now)||now<previous)return PM_TRACE_ERROR;previous=now;
  if(now-start>=timeout)return PM_TRACE_TIMEOUT;
  *last=o->poll(o->context);
  if(last->kind==PM_TRACE_STOPPED)return !expected||expected==last->signal?PM_TRACE_OK:PM_TRACE_WRONG_STOP;
  if(last->kind==PM_TRACE_EXITED)return PM_TRACE_GONE;
  if(last->kind!=PM_TRACE_PENDING)return PM_TRACE_ERROR;
  o->sleep(o->context,1);
 }
 return PM_TRACE_TIMEOUT;
}
