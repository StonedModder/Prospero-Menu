// SPDX-License-Identifier: GPL-3.0-or-later
#include "process-current.h"
#include <ps5/kernel.h>
#include <stdint.h>
static int pointer(uint64_t value){return (value>>48)==0xffff&&!(value&7);}
int pm_process_current(int pid){
  if(pid<=1||!pointer(KERNEL_ADDRESS_ALLPROC))return 0;
  uint64_t node=0;if(kernel_copyout(KERNEL_ADDRESS_ALLPROC,&node,sizeof node))return 0;
  for(unsigned i=0;node&&i<4096;i++){
    if(!pointer(node))return 0;
    int actual=0;if(kernel_copyout(node+KERNEL_OFFSET_PROC_P_PID,&actual,sizeof actual))return 0;
    if(actual==pid)return (uint64_t)kernel_get_proc(pid)==node;
    uint64_t next=0;if(kernel_copyout(node,&next,sizeof next)||next==node)return 0;node=next;
  }
  return 0;
}
