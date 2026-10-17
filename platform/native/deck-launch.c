// SPDX-License-Identifier: GPL-3.0-or-later
#include "deck-launch.h"
#include "pt.h"
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

int pm_deck_launch(pid_t pid,intptr_t entry,intptr_t afterTrap,uintptr_t argument,PmDeckLaunch* report){
 struct reg original={0},call={0},stopped={0};int signal=0,code=0;uintptr_t address=0;
 memset(report,0,sizeof *report);
 unsigned char opcode=0;
 if(afterTrap<=entry||pt_copyout(pid,afterTrap-1,&opcode,1)||opcode!=0xcc)return -1;
 if(pt_stop_info(pid,&report->caller,&signal,&code,&address)||report->caller<=0||
    pt_getregs(report->caller,&original))return -1;
 call=original;call.r_rip=entry;call.r_rsp=((original.r_rsp-0x200)&~UINT64_C(15))-8;
 call.r_rdi=argument;call.r_rsi=call.r_rdx=call.r_rcx=call.r_r8=call.r_r9=0;
 if(pt_setregs(report->caller,&call))return -1;
 int ready=0,ownStop=0,timedOut=0;
 if(!pt_continue(pid,0)){
  if(!pt_wait_signal(pid,2000,&signal))ready=1;
  else if(errno==ETIMEDOUT){
   timedOut=1;
   if(!kill(pid,SIGSTOP)&&!pt_wait_signal(pid,1000,&signal)){ready=1;ownStop=signal==SIGSTOP;}
  }
 }else{
  report->restored=!pt_setregs(report->caller,&original);return -1;
 }
 if(!ready)return -1;
 report->signal=signal;report->deliver=ownStop?0:signal;
 int infoSignal=0;
 int haveInfo=!pt_stop_info(pid,&report->stoppedThread,&infoSignal,&code,&address);
 int haveRegs=haveInfo&&report->stoppedThread>0&&!pt_getregs(report->stoppedThread,&stopped);
 if(haveRegs)report->stoppedRip=stopped.r_rip;
 int exact=!ownStop&&signal==SIGTRAP&&haveRegs&&
   report->stoppedThread==report->caller&&stopped.r_rip==(uint64_t)afterTrap;
 if(exact)report->deliver=0;
 // Never restore the caller's registers through a process ID: a newly spawned
 // sibling can be the reporting thread by the time waitpid returns.
 report->restored=!pt_setregs(report->caller,&original);
 return exact&&!timedOut&&report->restored&&stopped.r_rax==0?0:-1;
}
