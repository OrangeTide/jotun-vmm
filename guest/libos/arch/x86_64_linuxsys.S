/* x86_64_linuxsys.S : the SYSCALL trap and user entry for the Linux spike.
 *
 * The guest runs the loaded Linux binary in the same ring as the libOS (there
 * is no privilege boundary), so a SYSCALL does not change ring and we must not
 * return with SYSRET. The CPU still vectors SYSCALL to LSTAR with the caller's
 * return address in rcx and its RFLAGS in r11, masking the bits in SFMASK. We
 * save those, switch to a private kernel stack, call the C dispatcher with the
 * Linux argument registers, and resume the caller by restoring rcx/r11 and
 * jumping back (a plain `jmp *%rcx` with RFLAGS reloaded), not SYSRET.
 *
 *   void   linux_syscall_entry(void)   -- the LSTAR target, never called by C.
 *   long   linux_enter(uint64_t entry, uint64_t user_rsp)
 *       Save a resume context, set rsp to the SysV stack we built, and jump to
 *       the program entry. It "returns" only when the program exits, via
 *       linux_exit, yielding the exit code. Callee-saved state is preserved
 *       across the whole run because linux_exit restores it.
 *   void   linux_exit(long code)   -- restore the linux_enter context, so that
 *       linux_enter returns `code` to its caller. Does not return.
 *
 * The C side (linuxelf.c) provides:
 *   long   linux_sys_dispatch(long nr, long a1, long a2, long a3,
 *                             long a4, long a5)
 */

    .text

/****************************************************************
 * SYSCALL entry: the LSTAR target.
 ****************************************************************/
    .global linux_syscall_entry
linux_syscall_entry:
    /* On entry rcx = user return RIP, r11 = user RFLAGS, and the user rsp is
     * still live. Park the user rsp and switch to our own stack so a nested
     * fault or a deep C call never runs on the program's stack. */
    movq %rsp, le_user_rsp(%rip)
    movq le_sc_kstack(%rip), %rsp

    /* Preserve the SYSCALL-clobbered return state across the C call. rcx and
     * r11 are now free to use as scratch for the argument marshal. */
    pushq %rcx                 /* user RIP */
    pushq %r11                 /* user RFLAGS */

    /* Linux puts the call number in rax and args in rdi, rsi, rdx, r10, r8, r9.
     * The C dispatcher wants (nr, a1, a2, a3, a4, a5) in the SysV registers
     * rdi, rsi, rdx, rcx, r8, r9. Shuffle in an order that reads each source
     * before it is overwritten:
     *   a5 = arg5 (r8->r9), a4 = arg4 (r10->r8), a3 = arg3 (rdx->rcx),
     *   a2 = arg2 (rsi->rdx), a1 = arg1 (rdi->rsi), nr = rax->rdi. */
    movq %r8,  %r9
    movq %r10, %r8
    movq %rdx, %rcx
    movq %rsi, %rdx
    movq %rdi, %rsi
    movq %rax, %rdi
    call linux_sys_dispatch

    /* rax holds the return value. Restore the user return state and resume. */
    popq %r11                  /* user RFLAGS */
    popq %rcx                  /* user RIP */
    movq le_user_rsp(%rip), %rsp
    pushq %r11
    popfq                      /* reload RFLAGS (re-enables IF if it was set) */
    jmp *%rcx                  /* return to the instruction after SYSCALL */

/****************************************************************
 * linux_enter / linux_exit: kernel<->program handoff.
 ****************************************************************/
    .global linux_enter
linux_enter:
    /* rdi = program entry, rsi = user rsp. Save a full resume context so
     * linux_exit can return to our caller with the exit code. */
    movq (%rsp), %rax          /* return address of linux_enter */
    movq %rax, le_rip(%rip)
    leaq 8(%rsp), %rax         /* caller rsp after a normal return */
    movq %rax, le_rsp(%rip)
    movq %rbx, le_rbx(%rip)
    movq %rbp, le_rbp(%rip)
    movq %r12, le_r12(%rip)
    movq %r13, le_r13(%rip)
    movq %r14, le_r14(%rip)
    movq %r15, le_r15(%rip)

    /* Run the program with interrupts off for the spike: it is short, output is
     * polled MMIO, and this keeps the cooperative scheduler's timer from firing
     * on the program's stack. linux_elf_run re-enables them when we return. */
    cli

    /* Hand control to the program on its own stack. The SysV entry contract:
     * rsp points at argc, and rdx (a function to register with atexit) is 0. */
    movq %rsi, %rsp
    xorq %rdx, %rdx
    xorq %rbp, %rbp
    jmp *%rdi

    .global linux_exit
linux_exit:
    /* rdi = exit code. Restore the linux_enter context and return the code as
     * if linux_enter had returned normally. */
    movq %rdi, %rax            /* return value = exit code */
    movq le_rbx(%rip), %rbx
    movq le_rbp(%rip), %rbp
    movq le_r12(%rip), %r12
    movq le_r13(%rip), %r13
    movq le_r14(%rip), %r14
    movq le_r15(%rip), %r15
    movq le_rsp(%rip), %rsp
    jmp *le_rip(%rip)

/****************************************************************
 * Private kernel stack for the SYSCALL handler, and saved state.
 ****************************************************************/
    .bss
    .align 16
    .space 16384
le_sc_kstack_top:

    .data
    .align 8
le_sc_kstack:  .quad le_sc_kstack_top   /* top of the handler stack */
le_user_rsp:   .quad 0
le_rip:        .quad 0
le_rsp:        .quad 0
le_rbx:        .quad 0
le_rbp:        .quad 0
le_r12:        .quad 0
le_r13:        .quad 0
le_r14:        .quad 0
le_r15:        .quad 0

    .section .note.GNU-stack, "", @progbits
