/* SPDX-License-Identifier: GPL-3.0-or-later
 * Use SDK v0.43's initialized libkernel syscall gate. The SDK libc raw
 * syscall instruction in payload memory is not permitted without kstuff.
 * Tail calls preserve the six-argument SysV stack/register layout.
 */
.text
.global __wrap_syscall
.type __wrap_syscall,@function
__wrap_syscall:
    jmp __crt_syscall
.global __wrap___syscall
.type __wrap___syscall,@function
__wrap___syscall:
    jmp __crt_syscall
.global __wrap_ptrace
.type __wrap_ptrace,@function
__wrap_ptrace:
    mov %rcx,%r8
    mov %rdx,%rcx
    mov %rsi,%rdx
    mov %rdi,%rsi
    mov $26,%edi
    jmp __crt_syscall
