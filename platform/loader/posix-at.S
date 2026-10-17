/* SPDX-License-Identifier: GPL-3.0-or-later
 * SDK fallback *at stubs return raw FreeBSD errno in RAX with carry set.
 * Use the initialized libkernel gate and implement POSIX -1/errno semantics.
 * Without this, ENOENT is mistaken for descriptor 2 and EEXIST rejects every
 * save-directory traversal. All wrapped calls have at most four arguments.
 */
.text
.macro AT_WRAPPER name, number
.global __wrap_\name
.type __wrap_\name,@function
__wrap_\name:
    mov %rcx,%r8
    mov %rdx,%rcx
    mov %rsi,%rdx
    mov %rdi,%rsi
    mov $\number,%edi
    sub $8,%rsp
    call __crt_syscall
    jc .Lat_error
    add $8,%rsp
    ret
.endm
AT_WRAPPER mkdirat,496
AT_WRAPPER openat,499
AT_WRAPPER renameat,501
AT_WRAPPER unlinkat,503
AT_WRAPPER fstatat,493
.Lat_error:
    mov %eax,(%rsp)
    call __error
    mov (%rsp),%edx
    mov %edx,(%rax)
    mov $-1,%rax
    add $8,%rsp
    ret
