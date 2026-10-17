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

#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>

#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/mman.h>

#include <ps5/kernel.h>
#include <ps5/mdbg.h>
#include <ps5/klog.h>

#include "pt.h"
#include "trace-wait.h"
#include <time.h>
#include "port_timing.h"
#include "port_diagnostic.h"


static int
sys_ptrace_checked(int request, pid_t pid, caddr_t addr, int data, int *accepted) {
  if(accepted)*accepted=0;
  pid_t mypid = getpid();
  uint8_t caps[16], fullcaps[16];
  memset(fullcaps,0xff,sizeof fullcaps);
  uint64_t authid;
  int ret;

  if(!(authid=kernel_get_ucred_authid(mypid))) {
    return -1;
  }
  if(kernel_get_ucred_caps(mypid,caps)) return -1;
  if(kernel_set_ucred_authid(mypid, 0x4800000000010003l)) {
    return -1;
  }
  if(kernel_set_ucred_caps(mypid,fullcaps)) {
    kernel_set_ucred_authid(mypid,authid);
    return -1;
  }

  ret = (int)syscall(SYS_ptrace, request, pid, addr, data);
  if(accepted&&ret>=0)*accepted=1;
  if(ret<0)port_diag("ptrace-error",(unsigned)request,ret,errno);

  int restore_caps=kernel_set_ucred_caps(mypid,caps);
  int restore_auth=kernel_set_ucred_authid(mypid,authid);
  if(restore_caps||restore_auth)return -1;

  return ret;
}
static int sys_ptrace(int request,pid_t pid,caddr_t addr,int data){
  return sys_ptrace_checked(request,pid,addr,data,NULL);
}


intptr_t
pt_resolve(pid_t pid, const char* nid) {
  intptr_t addr;

  if((addr=kernel_dynlib_resolve(pid, 0x1, nid))) {
    return addr;
  }

  return kernel_dynlib_resolve(pid, 0x2001, nid);
}


static int trace_now(void* unused,uint64_t* out){
  (void)unused;struct timespec ts;
  if(clock_gettime(CLOCK_MONOTONIC,&ts))return -1;
  *out=(uint64_t)ts.tv_sec*1000+ts.tv_nsec/1000000;return 0;
}
static PmTraceEvent trace_poll(void* context){
  pid_t pid=*(pid_t*)context;int status=0;pid_t got=waitpid(pid,&status,WNOHANG|WUNTRACED);
  if(!got||(got<0&&errno==EINTR))return (PmTraceEvent){PM_TRACE_PENDING,0,0};
  if(got<0)return (PmTraceEvent){PM_TRACE_POLL_ERROR,0,errno};
  if(WIFSTOPPED(status))return (PmTraceEvent){PM_TRACE_STOPPED,WSTOPSIG(status),0};
  if(WIFEXITED(status)||WIFSIGNALED(status))return (PmTraceEvent){PM_TRACE_EXITED,0,0};
  return (PmTraceEvent){PM_TRACE_PENDING,0,0};
}
static void trace_sleep(void* unused,unsigned ms){(void)unused;usleep(ms*1000);}
static int trace_wait_event(pid_t pid,int expected,unsigned timeout,PmTraceEvent* last){
  PmTraceWaitOps ops={&pid,trace_now,trace_poll,trace_sleep};
  return pm_trace_wait(&ops,expected,timeout,last);
}
int pt_wait_stop(pid_t pid,int expected,unsigned timeout){
  PmTraceEvent last;int result=trace_wait_event(pid,expected,timeout,&last);
  if(result==PM_TRACE_OK)return 0;
  errno=result==PM_TRACE_TIMEOUT?ETIMEDOUT:result==PM_TRACE_GONE?ESRCH:
    result==PM_TRACE_WRONG_STOP?EINTR:last.error?last.error:EIO;return -1;
}
int pt_wait_signal(pid_t pid,unsigned timeout,int* signal){
 PmTraceEvent last;int result=trace_wait_event(pid,0,timeout,&last);
 *signal=last.kind==PM_TRACE_STOPPED?last.signal:0;
 if(result==PM_TRACE_OK)return 0;
 errno=result==PM_TRACE_TIMEOUT?ETIMEDOUT:result==PM_TRACE_GONE?ESRCH:last.error?last.error:EIO;return -1;
}
int
pt_attach(pid_t pid) {
  if(sys_ptrace(PT_ATTACH,pid,0,0)==-1)return -1;
  if(!pt_wait_stop(pid,0,1000))return 0;
  int saved=errno;pt_detach(pid,0);errno=saved;return -1;
}
int pt_attach_owned(pid_t pid,int *owned){
  if(!owned){errno=EINVAL;return -1;}
  // Preserve ownership even if credential restoration or the stop wait fails.
  // The transactional caller remains responsible for the final detach.
  if(sys_ptrace_checked(PT_ATTACH,pid,0,0,owned)==-1)return -1;
  return pt_wait_stop(pid,0,1000);
}


int
pt_detach(pid_t pid, int sig) {
  port_diag("detach-begin",pid,sig,0);
  if(sys_ptrace(PT_DETACH, pid, 0, sig) == -1) {
    port_diag("detach-failed",pid,-1,errno);
    return -1;
  }
  port_diag("detach-ok",pid,0,0);port_diag_flush();

  return 0;
}


int
pt_step(int pid) {
  if(sys_ptrace(PT_STEP,pid,(caddr_t)1,0))return -1;
  return pt_wait_stop(pid,SIGTRAP,1000);
}


int
pt_continue(pid_t pid, int sig) {
  if(sys_ptrace(PT_CONTINUE, pid, (caddr_t)1, sig) == -1) {
    return -1;
  }

  return 0;
}


int
pt_getint(pid_t pid, intptr_t addr) {
  return sys_ptrace(PT_READ_D, pid, (caddr_t)addr, 0);
}


int
pt_setint(pid_t pid, intptr_t addr, int val) {
  return sys_ptrace(PT_WRITE_D, pid, (caddr_t)addr, val);
}


int
pt_getregs(pid_t pid, struct reg *r) {
  return sys_ptrace(PT_GETREGS, pid, (caddr_t)r, 0);
}
int pt_getlwps(pid_t pid,int* tids,size_t capacity){
 int count=sys_ptrace(PT_GETNUMLWPS,pid,0,0);
 if(count<=0||(size_t)count>capacity)return -1;
 int got=sys_ptrace(PT_GETLWPLIST,pid,(caddr_t)tids,count);
 return got==count?got:-1;
}
int pt_stop_info(pid_t pid,int* thread,int* signal,int* code,uintptr_t* address){
 struct ptrace_lwpinfo info={0};
 if(sys_ptrace(PT_LWPINFO,pid,(caddr_t)&info,sizeof info))return -1;
 *thread=info.pl_lwpid;*signal=0;*code=0;*address=0;
 if(info.pl_flags&PL_FLAG_SI){*signal=info.pl_siginfo.si_signo;*code=info.pl_siginfo.si_code;*address=(uintptr_t)info.pl_siginfo.si_addr;}
 return 0;
}


int
pt_setregs(pid_t pid, const struct reg *r) {
  return sys_ptrace(PT_SETREGS, pid, (caddr_t)r, 0);
}


int
pt_copyin(pid_t pid, const void* buf, intptr_t addr, size_t len) {
#ifdef ETAHEN_PORT_1360
  const uint64_t write_started=port_timing_now();
  struct ptrace_io_desc iod = {
    .piod_op = PIOD_WRITE_D, .piod_offs = (void*)addr,
    .piod_addr = (void*)buf, .piod_len = len};
  int result=sys_ptrace(PT_IO,pid,(caddr_t)&iod,0);
  port_diag("copyin",addr,result,result?errno:0);
  port_diag("copyin-length",len,iod.piod_len,0);
  port_timing_end("write",pid,write_started,len);
  #ifdef PROSPEROMENU_NATIVE_TRACE
  printf("ptrace memory write: %d, errno %d, length %zu/%zu\n",result,errno,iod.piod_len,len);
  #endif
  if(result || iod.piod_len!=len) return -1;
  // Some SDK mdbg paths return success after a short transfer. Verify every
  // byte before allowing the injector to execute the remote image.
  const uint64_t verify_started=port_timing_now();
  unsigned char check[4096];
  for(size_t offset=0;offset<len;) {
    size_t count=0x1000-((addr+offset)&0xfff);if(count>len-offset)count=len-offset;
    if(kernel_proc_copyout(pid,addr+offset,check,count) ||
       memcmp(check,(const unsigned char*)buf+offset,count)) {
      port_diag("copyin-verify-failed",addr+offset,count,errno);
      errno=EIO;
      return -1;
    }
    offset+=count;
  }
  port_timing_end("verify",pid,verify_started,len);
  return 0;
#else
  struct ptrace_io_desc iod = {
    .piod_op = PIOD_WRITE_D,
    .piod_offs = (void*)addr,
    .piod_addr = (void*)buf,
    .piod_len = len};
  return sys_ptrace(PT_IO, pid, (caddr_t)&iod, 0);
#endif
}


int
pt_setchar(pid_t pid, intptr_t addr, char val) {
  return pt_copyin(pid, &val, addr, sizeof(val));
}


int
pt_setshort(pid_t pid, intptr_t addr, short val) {
  return pt_copyin(pid, &val, addr, sizeof(val));
}


int
pt_setlong(pid_t pid, intptr_t addr, long val) {
  return pt_copyin(pid, &val, addr, sizeof(val));
}


int
pt_copyout(pid_t pid, intptr_t addr, void* buf, size_t len) {
#ifdef ETAHEN_PORT_1360
  for(size_t off=0;off<len;){size_t count=0x1000-((addr+off)&0xfff);if(count>len-off)count=len-off;
    if(kernel_proc_copyout(pid,addr+off,(unsigned char*)buf+off,count))return -1;off+=count;}
  return 0;
#else
  struct ptrace_io_desc iod = {
    .piod_op = PIOD_READ_D,
    .piod_offs = (void*)addr,
    .piod_addr = buf,
    .piod_len = len};
  return sys_ptrace(PT_IO, pid, (caddr_t)&iod, 0);
#endif
}


char
pt_getchar(pid_t pid, intptr_t addr) {
  char val = 0;

  pt_copyout(pid, addr, &val, sizeof(val));

  return val;
}


short
pt_getshort(pid_t pid, intptr_t addr) {
  short val = 0;

  pt_copyout(pid, addr, &val, sizeof(val));

  return val;
}


long
pt_getlong(pid_t pid, intptr_t addr) {
  long val = 0;

  pt_copyout(pid, addr, &val, sizeof(val));

  return val;
}


long
pm_pt_call(pid_t pid, intptr_t addr, const uint64_t args[6]) {
  struct reg jmp_reg;
  struct reg bak_reg;

  if(pt_getregs(pid, &bak_reg)) {
    return -1;
  }

  memcpy(&jmp_reg, &bak_reg, sizeof(jmp_reg));
  jmp_reg.r_rip = addr;

  jmp_reg.r_rdi = args[0];
  jmp_reg.r_rsi = args[1];
  jmp_reg.r_rdx = args[2];
  jmp_reg.r_rcx = args[3];
  jmp_reg.r_r8  = args[4];
  jmp_reg.r_r9  = args[5];

  if(pt_setregs(pid, &jmp_reg)) {
    return -1;
  }

  // Bound the complete call as well as each wait. Restore saved registers
  // on every failure; if the tracee is not stopped, restoration fails closed.
  uint64_t started=0,now=0;unsigned steps=0;int failed=trace_now(0,&started);
  while(!failed && jmp_reg.r_rsp<=bak_reg.r_rsp){
    if(++steps>4096||trace_now(0,&now)||now<started||now-started>=2000){failed=1;break;}
    failed=pt_step(pid)||pt_getregs(pid,&jmp_reg);
  }
  long result=failed?-1:jmp_reg.r_rax;
  if(pt_setregs(pid,&bak_reg))return -1;
  return result;
}

long
pm_pt_call2(pid_t pid, intptr_t addr, const uint64_t args[6])
{
  struct reg jmp_reg;
  struct reg bak_reg;

  if(pt_getregs(pid, &bak_reg)) {
    return -1;
  }

  memcpy(&jmp_reg, &bak_reg, sizeof(jmp_reg));
  jmp_reg.r_rip = addr;
  // Preserve the interrupted thread's red zone and satisfy the SysV ABI.
  jmp_reg.r_rsp = ((bak_reg.r_rsp - 0x200) & ~15ULL) - 8;

  jmp_reg.r_rdi = args[0];
  jmp_reg.r_rsi = args[1];
  jmp_reg.r_rdx = args[2];
  jmp_reg.r_rcx = args[3];
  jmp_reg.r_r8  = args[4];
  jmp_reg.r_r9  = args[5];

  if(pt_setregs(pid, &jmp_reg)) {
    return -1;
  }

  //
  // Continue until hit a breakpoint, that must be generated by the called function
  //
  if (pt_continue(pid, 0)) {
    pt_setregs(pid, &bak_reg);
    return -1;
  }
  PmTraceEvent last;
  int outcome=trace_wait_event(pid,SIGTRAP,2000,&last);
  int reached=outcome==PM_TRACE_OK;
  if(outcome==PM_TRACE_TIMEOUT){
    // Recovery has its own finite deadline. Never report a timed-out call as
    // successful even if its trap races the stop request.
    if(kill(pid,SIGSTOP))return -1;
    trace_wait_event(pid,0,1000,&last);
  }
  long result=-1;
  if(last.kind==PM_TRACE_STOPPED){
    if(reached&&!pt_getregs(pid,&jmp_reg))result=jmp_reg.r_rax;
    if(pt_setregs(pid,&bak_reg))result=-1;
  }
  return result;
}


long
pm_pt_syscall(pid_t pid, int sysno, const uint64_t args[6]) {
  const uint64_t started=port_timing_now();
  #ifdef PROSPEROMENU_NATIVE_TRACE
  printf("Remote syscall %d: resolve\n",sysno);
  #endif
  intptr_t addr = pt_resolve(pid, "HoLVWNanBBc");
  struct reg jmp_reg;
  struct reg bak_reg;

  if(!addr) {
    return -1;
  } else {
    addr += 0xa;
  }
  uint8_t opcode[2];
  int read_result=pt_copyout(pid,addr,opcode,sizeof(opcode));
  #ifdef PROSPEROMENU_NATIVE_TRACE
  printf("Syscall gate read %d, bytes %02x %02x\n",read_result,opcode[0],opcode[1]);
  #endif
  if (read_result || opcode[0]!=0x0f || opcode[1]!=0x05) return -1;
  #ifdef PROSPEROMENU_NATIVE_TRACE
  puts("Remote syscall instruction verified");
  #endif

  if(pt_getregs(pid, &bak_reg)) {
    return -1;
  }

  memcpy(&jmp_reg, &bak_reg, sizeof(jmp_reg));
  jmp_reg.r_rip = addr;
  jmp_reg.r_rax = sysno;

  jmp_reg.r_rdi = args[0];
  jmp_reg.r_rsi = args[1];
  jmp_reg.r_rdx = args[2];
  jmp_reg.r_r10 = args[3];
  jmp_reg.r_r8  = args[4];
  jmp_reg.r_r9  = args[5];

  if(pt_setregs(pid, &jmp_reg)) {
    return -1;
  }

  // Execute only the validated syscall instruction. Following a foreign
  // function's RET on the interrupted thread's stack is unnecessary.
  int failed=pt_step(pid) || pt_getregs(pid,&jmp_reg);
  #ifdef PROSPEROMENU_NATIVE_TRACE
  printf("Remote syscall step result %d\n",failed);
  #endif
  long result=failed || (jmp_reg.r_rflags & 1) ? -1 : jmp_reg.r_rax;
  if(pt_setregs(pid,&bak_reg)) return -1;
  port_timing_end("remote-syscall",pid,started,(uint64_t)sysno);
  return result;
}


intptr_t
pt_mmap(pid_t pid, intptr_t addr, size_t len, int prot, int flags,
	int fd, off_t off) {
  return pt_syscall(pid, SYS_mmap, addr, len, prot, flags, fd, off);
}


int
pt_msync(pid_t pid, intptr_t addr, size_t len, int flags) {
  return pt_syscall(pid, SYS_msync, addr, len, flags);
}


int
pt_munmap(pid_t pid, intptr_t addr, size_t len) {
  return pt_syscall(pid, SYS_munmap, addr, len);
}


int
pt_mprotect(pid_t pid, intptr_t addr, size_t len, int prot) {
  return pt_syscall(pid, SYS_mprotect, addr, len, prot);
}


int
pt_socket(pid_t pid, int domain, int type, int protocol) {
  return (int)pt_syscall(pid, SYS_socket, domain, type, protocol);
}


int
pt_setsockopt(pid_t pid, int fd, int level, int optname, intptr_t optval,
	      socklen_t optlen) {
  return (int)pt_syscall(pid, SYS_setsockopt, fd, level, optname, optval,
			 optlen, 0);
}


int
pt_close(pid_t pid, int fd) {
  return (int)pt_syscall(pid, SYS_close, fd);
}


int
pt_bind(pid_t pid, int sockfd, intptr_t addr, uint32_t addrlen) {
  return (int)pt_syscall(pid, SYS_bind, sockfd, addr, addrlen);
}


ssize_t
pt_recvmsg(pid_t pid, int fd, intptr_t msg, int flags) {
  return (int)pt_syscall(pid, SYS_recvmsg, fd, msg, flags);
}


int
pt_dup2(pid_t pid, int oldfd, int newfd) {
  return (int)pt_syscall(pid, SYS_dup2, oldfd, newfd);
}


int
pt_rdup(pid_t pid, pid_t other_pid, int fd) {
  return (int)pt_syscall(pid, 0x25b, other_pid, fd);
}


int
pt_pipe(pid_t pid, intptr_t pipefd) {
  intptr_t faddr = pt_resolve(pid, "-Jp7F+pXxNg");
  return (int)pt_call(pid, faddr, pipefd);
}


int
pt_errno(pid_t pid) {
  intptr_t faddr = pt_resolve(pid, "9BcDykPmo1I");
  intptr_t addr = pt_call(pid, faddr);
  return pt_getint(pid, addr);
}


intptr_t
pt_sceKernelGetProcParam(pid_t pid) {
  intptr_t faddr = pt_resolve(pid, "959qrazPIrg");

  return pt_call(pid, faddr);
}
