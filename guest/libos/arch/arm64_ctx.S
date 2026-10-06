/* arm64_ctx.S : cooperative context switch for the AArch64 guest.
 *
 * void ctx_switch(void **oldsp, void *newsp)
 *   x0 = oldsp, x1 = newsp. Save the AAPCS64 callee-saved registers (x19-x28,
 *   x29, x30) and the callee-saved vector registers (d8-d15) on the current
 *   stack, store the resulting sp through oldsp (unless oldsp is NULL), load
 *   newsp, and return into the context stored there. The frame is 160 bytes so
 *   sp keeps its 16-byte alignment.
 *
 * The d8-d15 slots let a floating-point task (the rasterizer) survive a
 * cooperative switch: AAPCS64 makes the low 64 bits of v8-v15 callee-saved, so
 * a caller with live values there expects ctx_switch to preserve them. The
 * other vector registers are caller-saved and already spilled around the call.
 *
 * A stack from stack_init resumes at its entry: it lays down the same twenty
 * slots this pops with the entry in the x30 slot, so the first switch into a
 * new task rets straight into it.
 */

    /* Enable FP instructions for the assembler: the frame saves d8-d15, but the
     * guest is built -mgeneral-regs-only, which otherwise rejects them here. */
    .arch armv8-a+fp

    .text
    .global ctx_switch
ctx_switch:
    sub sp, sp, #160
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8,  d9,  [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
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
    ldp d8,  d9,  [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    add sp, sp, #160
    ret

    .section .note.GNU-stack, "", %progbits
