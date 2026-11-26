/* rcl2 - process entry and signal return trampoline.
 *
 * The kernel starts us at _start with rsp 16-aligned, pointing at
 * argc/argv/envp. We hand the stack pointer straight to C so the argc
 * check (selftest / poweroff / console modes) stays L2-compatible, and
 * enter main without any crt. If main ever returns, PID 1 would do the
 * unthinkable - exit - so trap here instead and spin.
 */
.section .text
.global _start
.type _start, @function
_start:
    xor   %ebp, %ebp            # no frame marker
    mov   %rsp, %rdi            # true stack top -> argc/argv/envp before align
    and   $-16, %rsp            # SysV ABI 16-byte alignment before call
    call  rcl_enter             # never returns for PID 1
    mov   $1, %edi              # unreachable in practice: exit_group(1) if it did
    mov   $231, %eax            # SYS_exit_group
    syscall
1:  jmp   1b
.size _start, .-_start

/* SA_RESTORER target: the kernel pushes a sigframe and jumps here. */
.global rcl_sigreturn
.type rcl_sigreturn, @function
rcl_sigreturn:
    mov   $15, %eax             # SYS_rt_sigreturn
    syscall
    ud2                         # unreachable
.size rcl_sigreturn, .-rcl_sigreturn

/* no executable stack */
.section .note.GNU-stack,"",@progbits
