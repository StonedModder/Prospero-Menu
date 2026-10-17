// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ps5/kernel.h>
#include "resident-memory.h"
// The SDK mmap/mprotect overrides contain inline syscall instructions. A
// resident ShellUI module must use the initialized libkernel gateway instead.
// These explicit helpers are called only after CRT initialization; they must
// not globally replace the SDK memory functions during its own bootstrap.
extern intptr_t __crt_syscall(intptr_t,...);
int pm_deck_munmap(void* address,size_t length){
 return (int)__crt_syscall(SYS_munmap,(intptr_t)address,(intptr_t)length);
}
int pm_deck_mprotect(void* address,size_t length,int protection){
 return kernel_mprotect((pid_t)__crt_syscall(SYS_getpid),(intptr_t)address,length,protection);
}
void* pm_deck_mmap(void* address,size_t length,int protection,int flags,int fd,off_t offset){
 intptr_t mapped=__crt_syscall(SYS_mmap,(intptr_t)address,(intptr_t)length,
  (intptr_t)(protection&~PROT_EXEC),(intptr_t)flags,(intptr_t)fd,(intptr_t)offset);
 if(mapped==(intptr_t)MAP_FAILED)return MAP_FAILED;
 if((protection&PROT_EXEC)&&pm_deck_mprotect((void*)mapped,length,protection)){
  pm_deck_munmap((void*)mapped,length);errno=EPERM;return MAP_FAILED;
 }
 return (void*)mapped;
}
