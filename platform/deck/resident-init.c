// SPDX-License-Identifier: GPL-3.0-or-later
#include <ps5/payload.h>
#include <stddef.h>
#include "deck-bootstrap.h"
#ifdef PROSPEROMENU_BOOTSTRAP_SCAN
#include <ps5/kernel.h>
// SDK v0.43 exports these CRT globals but omits them from its public header.
extern uintptr_t KERNEL_ADDRESS_DMAP_BASE,KERNEL_ADDRESS_IOMMU_SOFTC;
static void kernel_snapshot(volatile PmDeckBoot* t){
 t->kernelDataBase=KERNEL_ADDRESS_DATA_BASE;t->kernelAllproc=KERNEL_ADDRESS_ALLPROC;
 t->kernelDmap=KERNEL_ADDRESS_DMAP_BASE;t->kernelPrison=KERNEL_ADDRESS_PRISON0;
 t->kernelIommu=KERNEL_ADDRESS_IOMMU_SOFTC;
}
#endif
// This code runs before libc imports resolve: no logging, allocation, errno,
// or imported calls. Only annotate the injector-owned shared record.
static volatile PmDeckBoot* trace;
static void phase(unsigned value){if(trace)trace->phase=value;}
extern int __real___crt_syscall_init(payload_args_t*);
extern int __real___kernel_init(payload_args_t*);
extern int __real___rtld_init(void);
int __wrap___crt_syscall_init(payload_args_t* args){
 trace=(volatile PmDeckBoot*)((unsigned char*)args+PM_DECK_BOOT_OFFSET);
 if(trace->magic!=PM_DECK_BOOT_MAGIC||trace->version!=PM_DECK_BOOT_VERSION)trace=0;
 if(trace){trace->kernelReadAddress=(uintptr_t)&trace;trace->kernelReadLength=(uintptr_t)trace;}
 phase(201);int result=__real___crt_syscall_init(args);phase(202);return result;
}
int __wrap___kernel_init(payload_args_t* args){
#ifdef PROSPEROMENU_BOOTSTRAP_SCAN
 if(trace){trace->bootstrapFirmware=kernel_get_fw_version();trace->kernelInitResult=INT32_MIN;}
#endif
#ifdef PROSPEROMENU_BOOTSTRAP_TRACE
 // Private diagnostic build only. Wait for the parent stager to finish its
 // breakpoint/register restoration before the resident enters kernel init.
 extern long __crt_syscall(long,...);
 struct {long sec,nsec;} delay={0,1000000};
 unsigned wait=0;
 while(trace&&trace->count!=0x5443&&wait++<5000)__crt_syscall(240,&delay,0);
 if(!trace||trace->count!=0x5443)return -1;
 trace->count=0;
#endif
 phase(203);int result=__real___kernel_init(args);phase(204);
#ifdef PROSPEROMENU_BOOTSTRAP_SCAN
 if(trace){trace->kernelInitResult=result;kernel_snapshot(trace);}
#endif
#ifdef PROSPEROMENU_BOOTSTRAP_TRACE
 if(trace){trace->reservedTrace=3;
  __asm__ volatile("lea 1f(%%rip),%%rax; mov %%rax,%0; int3; 1:" : "=m"(trace->kernelReadAddress) : : "rax","memory");
 }
#endif
 return result;
}
// A resident thread shares ShellUI credentials and syscall policy. Preserve
// them, matching the supported etaHEN resident initialization path.
int __wrap___patch_init(void){phase(205);return 0;}
int __wrap___rtld_init(void){
 phase(206);int result=__real___rtld_init();phase(207);return result;
}

// The SDK uses this primitive to discover its kernel mappings. Record progress
// in shared memory only, without adding syscalls or touching process policy.
extern int __real_kernel_copyout(uintptr_t,void*,size_t);
int __wrap_kernel_copyout(uintptr_t address,void* output,size_t length){
 int tracked=trace&&trace->phase==203;
 if(tracked){trace->kernelReadAddress=address;trace->kernelReadLength=length;
  trace->kernelReadCount++;trace->reservedTrace=1;}
#ifdef PROSPEROMENU_BOOTSTRAP_SCAN
 volatile PmDeckKernelRead* read=0;
 if(tracked){
  kernel_snapshot(trace);read=&trace->kernelReads[(trace->kernelReadCount-1)%PM_DECK_READ_HISTORY];
  read->sequence=0;read->complete=0;read->address=address;read->length=length;read->result=INT32_MIN;
  read->sequence=trace->kernelReadCount;
 }
#endif
 int result=__real_kernel_copyout(address,output,length);
#ifdef PROSPEROMENU_BOOTSTRAP_SCAN
 if(read){read->result=result;read->complete=1;}
#endif
 if(tracked){trace->kernelReadResult=result;trace->reservedTrace=2;}
 return result;
}
