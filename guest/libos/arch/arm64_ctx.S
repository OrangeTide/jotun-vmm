/* arm64_ctx.S : cooperative context switch for the AArch64 guest.
 *
 * void ctx_switch(void **oldsp, void *newsp)
 *   x0 = oldsp, x1 = newsp. Save the AAPCS64 callee-saved registers (x19-x28,
 *   x29, x30) on the current stack, store the resulting sp through oldsp
 *   (unless oldsp is NULL), load newsp, and return into the context stored
 *   there. The frame is 96 bytes so sp keeps its 16-byte alignment.
 *
 * A stack from stack_init resumes at its entry: it lays down the same twelve
 * slots this pops with the entry in the x30 slot, so the first switch into a
 * new task rets straight into it.
 */

    .text
    .global ctx_switch
ctx_switch:
    sub sp, sp, #96
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    cbz x0, 1f
    mov x2, sp
    str x2, [x0]              /* *oldsp = sp */
1:
    mov sp, x1               /* switch to the new stack */
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    add sp, sp, #96
    ret

    .section .note.GNU-stack, "", %progbits
