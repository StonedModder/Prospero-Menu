/* Copyright (C) 2024 John Törnblom

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING. If not, see
<http://www.gnu.org/licenses/>.  */

#include <elf.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>

#include <sys/un.h>
#include <sys/user.h>
#include <sys/wait.h>

#include <ps5/kernel.h>

#include "elfldr.h"
#include "port_timing.h"
#include "log.h"
#include "pt.h"


#ifndef IPV6_2292PKTOPTIONS
#define IPV6_2292PKTOPTIONS 25
#endif


/**
 * Convenient macros.
 **/
#define ROUND_PG(x) (((x) + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1))
#define TRUNC_PG(x) ((x) & ~(PAGE_SIZE - 1))
#define PFLAGS(x)   ((((x) & PF_R) ? PROT_READ  : 0) | \
		     (((x) & PF_W) ? PROT_WRITE : 0) | \
		     (((x) & PF_X) ? PROT_EXEC  : 0))


/**
 * Context structure for the ELF loader.
 **/
typedef struct elfldr_ctx {
  uint8_t* elf;
  pid_t    pid;

  intptr_t base_addr;
  size_t   base_size;
  void*    base_mirror;
} elfldr_ctx_t;


/**
 * Absolute path to the SceSpZeroConf eboot.
 **/
static const char* SceSpZeroConf = "/system/vsh/app/NPXS40112/eboot.bin";


extern intptr_t pm_elf_map(pid_t, const uint8_t*, size_t);

/**
 * Create payload args in the address space of the process with the given pid.
 **/
intptr_t
pm_payload_args(pid_t pid) {
  int victim_sock;
  int master_sock;
  intptr_t buf;
  int pipe0;
  int pipe1;

  if((buf=pt_mmap(pid, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
		  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0)) == -1) {
    LOG_PT_PERROR(pid, "pt_mmap");
    return 0;
  }

  if((master_sock=pt_socket(pid, AF_INET6, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
    LOG_PT_PERROR(pid, "pt_socket");
    return 0;
  }

  if(pt_setint(pid, buf+0x00, 20))return 0;
  if(pt_setint(pid, buf+0x04, IPPROTO_IPV6))return 0;
  if(pt_setint(pid, buf+0x08, IPV6_TCLASS))return 0;
  if(pt_setint(pid, buf+0x0c, 0))return 0;
  if(pt_setint(pid, buf+0x10, 0))return 0;
  if(pt_setint(pid, buf+0x14, 0))return 0;
  if(pt_setsockopt(pid, master_sock, IPPROTO_IPV6, IPV6_2292PKTOPTIONS, buf, 24)) {
    LOG_PT_PERROR(pid, "pt_setsockopt");
    return 0;
  }

  if((victim_sock=pt_socket(pid, AF_INET6, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
    LOG_PT_PERROR(pid, "pt_socket");
    return 0;
  }

  if(pt_setint(pid, buf+0x00, 0))return 0;
  if(pt_setint(pid, buf+0x04, 0))return 0;
  if(pt_setint(pid, buf+0x08, 0))return 0;
  if(pt_setint(pid, buf+0x0c, 0))return 0;
  if(pt_setint(pid, buf+0x10, 0))return 0;
  if(pt_setsockopt(pid, victim_sock, IPPROTO_IPV6, IPV6_PKTINFO, buf, 20)) {
    LOG_PT_PERROR(pid, "pt_setsockopt");
    return 0;
  }

  if(kernel_overlap_sockets(pid, master_sock, victim_sock)) {
    LOG_PUTS("kernel_overlap_sockets failed");
    return 0;
  }

  if(pt_pipe(pid, buf)) {
    LOG_PT_PERROR(pid, "pt_pipe");
    return 0;
  }
  pipe0 = pt_getint(pid, buf);
  pipe1 = pt_getint(pid, buf+4);
  if(pipe0<0||pipe1<0)return 0;

  intptr_t args       = buf;
  intptr_t rwpipe     = buf + 0x100;
  intptr_t rwpair     = buf + 0x200;
  intptr_t kpipe_addr = kernel_get_proc_file(pid, pipe0);
  intptr_t payloadout = buf + 0x300;
  if(!kpipe_addr)return 0;
  // SDK v0.43 expects sys_dynlib_dlsym, not the getpid function.
  intptr_t dlsym = pt_resolve(pid, "LwG8g3niqwA");
  uint8_t call[5]; int32_t rel32;
  if (!dlsym || pt_copyout(pid, dlsym + 4, call, sizeof(call)) || call[0] != 0xe8) return 0;
  memcpy(&rel32, call + 1, sizeof(rel32));
  dlsym += 9 + rel32;
  if(pt_setlong(pid, args + 0x00, dlsym))return 0;
  if(pt_setlong(pid, args + 0x08, rwpipe))return 0;
  if(pt_setlong(pid, args + 0x10, rwpair))return 0;
  if(pt_setlong(pid, args + 0x18, kpipe_addr))return 0;
  if(pt_setlong(pid, args + 0x20, KERNEL_ADDRESS_DATA_BASE))return 0;
  if(pt_setlong(pid, args + 0x28, payloadout))return 0;
  if(pt_setint(pid, rwpipe + 0, pipe0))return 0;
  if(pt_setint(pid, rwpipe + 4, pipe1))return 0;
  if(pt_setint(pid, rwpair + 0, master_sock))return 0;
  if(pt_setint(pid, rwpair + 4, victim_sock))return 0;
  if(pt_setint(pid, payloadout, 0))return 0;

  return args;
}


/**
 * Prepare registers of a process for execution of an ELF.
 **/
static int
elfldr_prepare_exec(pid_t pid, uint8_t *elf, size_t size) {
  intptr_t entry;
  intptr_t args;
  struct reg r;

  if(pt_getregs(pid, &r)) {
    LOG_PERROR("pt_getregs");
    return -1;
  }

  if(!(entry=pm_elf_map(pid, elf, size))) {
    LOG_PUTS("elfldr_load failed");
    return -1;
  }

  if(!(args=pm_payload_args(pid))) {
    LOG_PUTS("elfldr_payload_args failed");
    return -1;
  }

  r.r_rsp &= ~0xfl;
  pt_setlong(pid, r.r_rsp-8, r.r_rip);
  r.r_rsp -= 8;
  r.r_rip = entry;
  r.r_rdi = args;

  if(pt_setregs(pid, &r)) {
    LOG_PERROR("pt_setregs");
    pt_detach(pid, SIGKILL);
    return -1;
  }

  return 0;
}


/**
 * Set the name of a process.
 **/
static int
elfldr_set_procname(pid_t pid, const char* name) {
  intptr_t buf;

  if((buf=pt_mmap(pid, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
		  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0)) == -1) {
    LOG_PT_PERROR(pid, "pt_mmap");
    return -1;
  }

  pt_copyin(pid, name, buf, strlen(name)+1);
  pt_syscall(pid, SYS_thr_set_name, -1, buf);
  pt_msync(pid, buf, PAGE_SIZE, MS_SYNC);
  pt_munmap(pid, buf, PAGE_SIZE);

  return 0;
}


/**
 * Escape jail and raise privileges.
 **/
int
elfldr_raise_privileges(pid_t pid) {
  static const uint8_t caps[16] = {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
				   0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
  intptr_t vnode;

  if(!(vnode=kernel_get_root_vnode())) {
    return -1;
  }
  if(kernel_set_proc_rootdir(pid, vnode)) {
    return -1;
  }
  if(kernel_set_proc_jaildir(pid, 0)) {
    return -1;
  }
  if(kernel_set_ucred_uid(pid, 0)) {
    return -1;
  }
  if(kernel_set_ucred_caps(pid, caps)) {
    return -1;
  }

  return 0;
}


/**
 * Execute an ELF inside the process with the given pid.
 **/
int
elfldr_exec(pid_t pid, int stdio, uint8_t* elf, size_t size) {
  uint8_t caps[16];
  intptr_t jaildir;
  intptr_t rootdir;
  uint64_t authid;
  int error = 0;

  // backup privileges
  jaildir = kernel_get_proc_jaildir(pid);
  if(!(rootdir=kernel_get_proc_rootdir(pid))) {
    LOG_PUTS("kernel_get_proc_rootdir failed");
    pt_detach(pid, 0);
    return -1;
  }
  if(kernel_get_ucred_caps(pid, caps)) {
    LOG_PUTS("kernel_get_ucred_caps failed");
    pt_detach(pid, 0);
    return -1;
  }
  if(!(authid=kernel_get_ucred_authid(pid))) {
    LOG_PUTS("kernel_get_ucred_authid failed");
    pt_detach(pid, 0);
    return -1;
  }

  if(elfldr_raise_privileges(pid)) {
    LOG_PUTS("Unable to raise privileges");
    pt_detach(pid, 0);
    return -1;
  }

  if(stdio > 0) {
    stdio = pt_rdup(pid, getpid(), stdio);

    pt_close(pid, STDERR_FILENO);
    pt_close(pid, STDOUT_FILENO);
    pt_close(pid, STDIN_FILENO);

    pt_dup2(pid, stdio, STDIN_FILENO);
    pt_dup2(pid, stdio, STDOUT_FILENO);
    pt_dup2(pid, stdio, STDERR_FILENO);

    pt_close(pid, stdio);
  }

  if(elfldr_prepare_exec(pid, elf, size)) {
    error = -1;
  }

  // restore privileges
  if(kernel_set_proc_jaildir(pid, jaildir)) {
    LOG_PUTS("kernel_set_proc_jaildir failed");
    error = -1;
  }
  if(kernel_set_proc_rootdir(pid, rootdir)) {
    LOG_PUTS("kernel_set_proc_rootdir failed");
    error = -1;
  }

  if(kernel_set_ucred_caps(pid, caps)) {
    LOG_PUTS("kernel_set_ucred_caps failed");
    error = -1;
  }
  if(kernel_set_ucred_authid(pid, authid)) {
    LOG_PUTS("kernel_set_ucred_authid failed");
    error = -1;
  }

  if(pt_detach(pid, error ? SIGKILL : 0)) {
    LOG_PERROR("pt_detach");
    error = -1;
  }

  return error;
}


/**
 * Set the heap size for libc.
 **/
static int
elfldr_set_heap_size(pid_t pid, ssize_t size) {
  intptr_t sceLibcHeapSize;
  intptr_t sceLibcParam;
  intptr_t sceProcParam;
  intptr_t Need_sceLibc;

  if(!(sceProcParam=pt_sceKernelGetProcParam(pid))) {
    LOG_PT_PERROR(pid, "pt_sceKernelGetProcParam");
    return -1;
  }

  if(pt_copyout(pid, sceProcParam+56, &sceLibcParam,
		sizeof(sceLibcParam))) {
    LOG_PERROR("pt_copyout");
    return -1;
  }

  if(pt_copyout(pid, sceLibcParam+16, &sceLibcHeapSize,
		sizeof(sceLibcHeapSize))) {
    LOG_PERROR("pt_copyout");
    return -1;
  }

  if(pt_setlong(pid, sceLibcHeapSize, size)) {
    LOG_PERROR("pt_setlong");
    return -1;
  }

  if(size != -1) {
    return 0;
  }

  if(pt_copyout(pid, sceLibcParam+72, &Need_sceLibc,
		sizeof(Need_sceLibc))) {
    LOG_PERROR("pt_copyout");
    return -1;
  }

  return pt_setlong(pid, sceLibcParam+32, Need_sceLibc);
}


static int
sys_budget_set(long budget) {
  return __syscall(0x23b, budget);
}


static int
elfldr_rfork_entry(void* progname) {
  char* const argv[] = {(char*)progname, 0};

  if(sys_budget_set(0)) {
    klog_perror("sys_budget_set");
    return -1;
  }
  if(open("/dev/deci_stdin", O_RDONLY) < 0) {
    klog_perror("open");
    return -1;
  }
  if(open("/dev/deci_stdout", O_WRONLY) < 0) {
    klog_perror("open");
    return -1;
  }
  if(open("/dev/deci_stderr", O_WRONLY) < 0) {
    klog_perror("open");
    return -1;
  }

  if(ptrace(PT_TRACE_ME, 0, 0, 0)) {
    klog_perror("ptrace");
    return -1;
  }

  execve(SceSpZeroConf, argv, 0);
  klog_perror("execve");
  return -1;
}


/**
 * Execute an ELF inside a new process.
 **/

pid_t
elfldr_spawn(const char* cwd, int stdio, uint8_t* elf, const char* name, size_t size) {
  const uint64_t spawn_started=port_timing_now();
  uint8_t int3instr = 0xcc;
  intptr_t brkpoint;
  uint8_t orginstr;
  void *stack;
  pid_t pid;

  if(!(stack=malloc(PAGE_SIZE)))return -1;
  if((pid=rfork_thread(RFPROC|RFCFDG|RFMEM,stack+PAGE_SIZE-8,elfldr_rfork_entry,(void*)name))<0){free(stack);return -1;}
  int initial_status=0;pid_t waited=0;
  for(unsigned attempt=0;attempt<1000;attempt++){
    waited=waitpid(pid,&initial_status,WNOHANG|WUNTRACED);
    if(waited==pid || (waited<0&&errno!=EINTR))break;
    usleep(10000);
  }
  if(waited!=pid || !WIFSTOPPED(initial_status) || WSTOPSIG(initial_status)!=SIGTRAP){
    if(waited==pid&&(WIFEXITED(initial_status)||WIFSIGNALED(initial_status)))free(stack);
    else if(waited==0 || (waited==pid&&WIFSTOPPED(initial_status)) || (waited<0&&errno!=ECHILD)){
      kill(pid,SIGKILL);
      for(unsigned reap=0;reap<1000;reap++){
        int status=0;pid_t ended=waitpid(pid,&status,WNOHANG|WUNTRACED);
        if(ended==pid&&(WIFEXITED(status)||WIFSIGNALED(status))){free(stack);break;}
        if(ended<0&&errno!=EINTR)break;
        usleep(1000);
      }
    }
    // Unknown child ownership keeps its shared stack allocated.
    return -1;
  }
  free(stack);

  port_timing_end("spawn-process",pid,spawn_started,0);
  const uint64_t prepare_started=port_timing_now();

  // The proc is now in the STOP state, with the instruction pointer pointing
  // at the libkernel entry. Let the kernel assign process parameters accessed
  // via sceKernelGetProcParam()
  if(pt_syscall(pid, 599)) {
    LOG_PT_PERROR(pid, "sys_dynlib_process_needed_and_relocate");
    pt_detach(pid, SIGKILL);
    return -1;
  }

  // Allow libc to allocate arbitrary amount of memory.
  elfldr_set_heap_size(pid, -1);

  //Insert a breakpoint at the eboot entry.
  if(!(brkpoint=kernel_dynlib_entry_addr(pid, 0))) {
    LOG_PUTS("kernel_dynlib_entry_addr failed");
    pt_detach(pid, SIGKILL);
    return -1;
  }
  brkpoint += 58;// offset to invocation of main()

  if(kernel_mprotect(pid, brkpoint, PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC)) {
    LOG_PUTS("kernel_mprotect failed");
    pt_detach(pid, SIGKILL);
    return -1;
  }

  if(pt_copyout(pid, brkpoint, &orginstr, sizeof(orginstr))) {
    LOG_PERROR("pt_copyout");
    pt_detach(pid, SIGKILL);
    return -1;
  }
  if(pt_copyin(pid, &int3instr, brkpoint, sizeof(int3instr))) {
    LOG_PERROR("pt_copyin");
    pt_detach(pid, SIGKILL);
    return -1;
  }

  // Continue execution until we hit the breakpoint, then remove it.
  if(pt_continue(pid, SIGCONT)) {
    LOG_PERROR("pt_continue");
    pt_detach(pid, SIGKILL);
    return -1;
  }
  if(pt_wait_stop(pid, SIGTRAP, 2000)) {
    LOG_PERROR("waitpid");
    pt_detach(pid, SIGKILL);
    return -1;
  }
  struct reg stopped_regs;
  if(pt_getregs(pid,&stopped_regs)||stopped_regs.r_rip!=(uint64_t)brkpoint+1){
    pt_detach(pid,SIGKILL);kill(pid,SIGKILL);return -1;
  }
  if(pt_copyin(pid, &orginstr, brkpoint, sizeof(orginstr))) {
    LOG_PERROR("pt_copyin");
    pt_detach(pid, SIGKILL);
    return -1;
  }

  // Execute the ELF
  port_timing_end("spawn-prepare",pid,prepare_started,0);
  const uint64_t image_started=port_timing_now();
  elfldr_set_procname(pid, name);
  if(elfldr_exec(pid, stdio, elf, size)) {
    kill(pid, SIGKILL);
    return -1;
  }

  port_timing_end("spawn-image",pid,image_started,0);
  port_timing_end(name?name:"spawn-total",pid,spawn_started,0);
  return pid;
}
