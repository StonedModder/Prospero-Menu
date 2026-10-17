// SPDX-License-Identifier: GPL-3.0-or-later
// Replaces the SDK v0.43 PID-only cache at link time. Must also work during CRT
// startup, before libc/libkernel symbols have been resolved: no allocation,
// errno writes or imported functions on the console path.
#include <stdint.h>
#include <ps5/kernel.h>
#ifdef __PS5__
extern long __crt_syscall(long,...);
#else
#include <unistd.h>
#endif
static int pointer(uint64_t p){return (p>>48)==0xffff&&!(p&7);}
intptr_t __wrap_kernel_get_proc(int pid){
  if(pid<=0){
#ifdef __PS5__
    pid=(int)__crt_syscall(20); // FreeBSD SYS_getpid, same CRT ABI as the SDK.
#else
    pid=(int)getpid();
#endif
  }
  if(pid<=0||!pointer(KERNEL_ADDRESS_ALLPROC))return 0;
  uint64_t node=0;if(kernel_copyout(KERNEL_ADDRESS_ALLPROC,&node,sizeof node))return 0;
  for(unsigned n=0;node&&n<4096;n++){
    if(!pointer(node))return 0;int actual=0;
    if(kernel_copyout(node+KERNEL_OFFSET_PROC_P_PID,&actual,sizeof actual))return 0;
    if(actual==pid)return (intptr_t)node;
    uint64_t next=0;if(kernel_copyout(node,&next,sizeof next)||next==node)return 0;node=next;
  }
  return 0;
}
