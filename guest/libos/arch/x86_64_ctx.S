/* x86_64_ctx.S : cooperative context switch for the 64-bit x86 guest.
 *
 * void ctx_switch(void **oldsp, void *newsp)
 *   rdi = oldsp, rsi = newsp. Save the SysV callee-saved registers and the
 *   return address on the current stack, store the resulting rsp through oldsp
 *   (unless oldsp is NULL), load newsp, and return into the context stored
 *   there. The volatile registers are the caller's responsibility per the ABI,
 *   so only rbx, rbp, and r12-r15 need saving.
 *
 * A stack from stack_init resumes at its entry: stack_init lays down the same
 * six registers this pops followed by the entry as the return address, so the
 * first switch into a new task "returns" straight into it.
 */

    .text
    .global ctx_switch
ctx_switch:
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    test %rdi, %rdi
    jz 1f
    mov %rsp, (%rdi)            /* *oldsp = rsp */
1:
    mov %rsi, %rsp             /* switch to the new stack */
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    ret

    .section .note.GNU-stack, "", @progbits
