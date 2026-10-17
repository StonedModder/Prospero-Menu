// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PM_MODULE_THREAD_H
#define PM_MODULE_THREAD_H
#include <pthread.h>
// Console pthread defaults are only 64 KiB. Module preparation nests file
// verification, metadata parsing, ptrace and SDK calls while owning a stopped
// game. Reserve explicit headroom before any of that work can begin.
#define PM_MODULE_WORKER_STACK_BYTES (1024u*1024u)
static int pm_module_thread_start(pthread_t *thread,void *(*entry)(void *),void *arg){
 pthread_attr_t attr;int err=pthread_attr_init(&attr);if(err)return err;
 err=pthread_attr_setstacksize(&attr,PM_MODULE_WORKER_STACK_BYTES);
 if(!err)err=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
 if(!err)err=pthread_create(thread,&attr,entry,arg);
 pthread_attr_destroy(&attr);return err;
}
#endif
