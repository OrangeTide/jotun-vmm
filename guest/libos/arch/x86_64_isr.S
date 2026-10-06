/* x86_64_isr.S : the event interrupt service routine.
 *
 * Entered through the IDT gate for JOTUN_EVENT_VECTOR with interrupts masked.
 * It saves the full general-purpose register set (the interrupted task's
 * caller-saved registers too, since the task was interrupted at an arbitrary
 * point), calls the C handler, restores them, and returns with iretq, which
 * restores the task's RFLAGS (re-enabling interrupts) and RIP. The C handler
 * may reschedule; when this task is resumed the stack is restored exactly, so
 * the iretq returns where the task left off.
 *
 * It also saves and restores the full FPU/SSE state with FXSAVE/FXRSTOR around
 * the handler. The handler may switch to another task that uses the FPU, so
 * without this an interrupt would not be transparent to an interrupted task
 * that was mid-computation in floating point. The 512-byte save area is carved
 * from the stack, 16-byte aligned as FXSAVE requires. */

    .text
    .global isr_event
isr_event:
    push %rax
    push %rcx
    push %rdx
    push %rbx
    push %rbp
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    mov %rsp, %rbp             /* rbp: top of the saved GP frame, call-preserved */
    sub $512, %rsp             /* carve the FXSAVE area */
    and $-16, %rsp             /* FXSAVE needs 16-byte alignment */
    fxsave (%rsp)
    mov %rsp, %r12             /* r12: FXSAVE area, call-preserved */

    cld
    call irq_event

    fxrstor (%r12)
    mov %rbp, %rsp             /* drop the FXSAVE area back to the GP frame */

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rbp
    pop %rbx
    pop %rdx
    pop %rcx
    pop %rax
    iretq

    .section .note.GNU-stack, "", @progbits
