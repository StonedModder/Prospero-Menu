// SPDX-License-Identifier: GPL-3.0-or-later
// The pad callback schedules a game-owned pthread without waiting for it.
// Module initialization and optional activation run only on that worker.
// The coordinator fills disabled slots while the target is stopped; only an
// enabled slot can be consumed. A slot is never recycled in the same process.
#include "game-gate.h"
_Static_assert(sizeof(PmGameModule)==300,"Unexpected native module slot ABI");
_Static_assert(offsetof(PmGameGate,modules)==80,"Unexpected native game gate ABI");
_Static_assert(sizeof(PmGameGate)<=0x1000,"Game module control overlaps pad bridge data");
static void* PM_GAME_SYSV module_worker(void *arg);
__attribute__((section(".text.entry"),noinline))
void PM_GAME_SYSV pm_game_gate(PmGameGate *control,uint64_t cookie,unsigned lane){
  if(!control||lane>1||control->magic!=PM_GAME_GATE_MAGIC||control->version!=PM_GAME_GATE_VERSION||
     control->bytes!=sizeof *control||!cookie||control->cookie!=cookie)return;
  __atomic_fetch_add(&control->callbacks[lane],1,__ATOMIC_RELAXED);
  if(!__atomic_load_n(&control->enabled,__ATOMIC_ACQUIRE)||!control->loadStartModule||
     !control->threadCreate||!control->threadDetach||!control->dlsym)return;
  unsigned free=0;
  if(!__atomic_compare_exchange_n(&control->busy,&free,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return;
  for(unsigned i=0;i<PM_GAME_MODULE_SLOTS;i++){
    PmGameModule *slot=&control->modules[i];unsigned pending=PM_MODULE_PENDING;
    if(!__atomic_load_n(&slot->enabled,__ATOMIC_ACQUIRE)||
       !__atomic_compare_exchange_n(&slot->state,&pending,PM_MODULE_STARTING,0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED))continue;
    // Only content-addressed, sandbox-staged module paths are accepted. This
    // guards a partially written slot without doing filesystem I/O on input.
    const char prefix[]=PM_GAME_MODULE_PREFIX;unsigned at=0;int valid=1;
    for(;at<sizeof prefix-1;at++)if(slot->path[at]!=prefix[at])valid=0;
    for(unsigned n=0;n<64;n++,at++){unsigned c=(unsigned char)slot->path[at];if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))valid=0;}
    if(slot->path[at++]!='.')valid=0;
    if(slot->path[at]=='s')at++;
    if(slot->path[at]!='p'||slot->path[at+1]!='r'||slot->path[at+2]!='x'||slot->path[at+3])valid=0;
    if(!valid){slot->handle=-1;slot->startResult=-1;__atomic_store_n(&slot->state,PM_MODULE_REJECTED,__ATOMIC_RELEASE);break;}
    slot->handle=-1;
    __atomic_store_n(&slot->phase,PM_MODULE_PHASE_THREAD,__ATOMIC_RELEASE);
    uintptr_t thread=0;
    int result=((PmGameThreadCreate)(uintptr_t)control->threadCreate)(&thread,0,module_worker,control,"ProsperoMenu module");
    __atomic_store_n(&slot->threadResult,result,__ATOMIC_RELEASE);
    if(!result){
      // The worker may already have completed. Do not overwrite its state or
      // join it: a slow plugin must not block controller input.
      int detached=((PmGameThreadDetach)(uintptr_t)control->threadDetach)(thread);
      __atomic_store_n(&slot->detachResult,detached,__ATOMIC_RELEASE);
      return; // Worker owns busy until completion, including reentrant pad calls.
    }
    __atomic_store_n(&slot->state,PM_MODULE_REJECTED,__ATOMIC_RELEASE);break;
  }
  __atomic_store_n(&control->busy,0,__ATOMIC_RELEASE);
}

static void* PM_GAME_SYSV module_worker(void *arg){
  PmGameGate *control=arg;
  for(unsigned i=0;i<PM_GAME_MODULE_SLOTS;i++){
    PmGameModule *slot=&control->modules[i];
    if(__atomic_load_n(&slot->state,__ATOMIC_ACQUIRE)!=PM_MODULE_STARTING)continue;
    __atomic_store_n(&slot->phase,PM_MODULE_PHASE_LOAD,__ATOMIC_RELEASE);
    int result=0;
    int handle=((PmGameLoadModule)(uintptr_t)control->loadStartModule)(slot->path,0,0,0,0,&result);
    slot->handle=handle;slot->startResult=result;
    unsigned state=PM_MODULE_FAILED;
    if(handle>=0&&result>=0){
      __atomic_store_n(&slot->phase,PM_MODULE_PHASE_EXPORTS,__ATOMIC_RELEASE);
      void *load=0,*unload=0;
      PmGameDlsym dlsym=(PmGameDlsym)(uintptr_t)control->dlsym;
      slot->loadSymbolResult=dlsym(handle,"plugin_load",&load);
      slot->unloadSymbolResult=dlsym(handle,"plugin_unload",&unload);
      // Paired exports identify the GoldHEN-style activation contract. Ordinary
      // module_start-only PRXs must not be initialized a second time.
      if(!slot->loadSymbolResult&&load&&!slot->unloadSymbolResult&&unload){
        slot->activation=1;
        __atomic_store_n(&slot->phase,PM_MODULE_PHASE_ACTIVATE,__ATOMIC_RELEASE);
        slot->activationResult=((PmGamePluginLoad)load)(0,0);
        if(!slot->activationResult)state=PM_MODULE_STARTED;
      }else state=PM_MODULE_STARTED;
    }
    __atomic_store_n(&slot->phase,PM_MODULE_PHASE_COMPLETE,__ATOMIC_RELEASE);
    __atomic_store_n(&slot->state,state,__ATOMIC_RELEASE);break;
  }
  __atomic_store_n(&control->busy,0,__ATOMIC_RELEASE);return 0;
}
