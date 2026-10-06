/* x86_64_isr.S : the event interrupt service routine.
 *
 * Entered through the IDT gate for JOTUN_EVENT_VECTOR with interrupts masked.
 * It saves the full general-purpose register set (the interrupted task's
 * caller-saved registers too, since the task was interrupted at an arbitrary
 * point), calls the C handler, restores them, and returns with iretq, which
 * restores the task's RFLAGS (re-enabling interrupts) and RIP. The C handler
 * may reschedule; when this task is resumed the stack is restored exactly, so
 * the iretq returns where the task left off. */

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

    mov %rsp, %rbp             /* align the stack for the C call */
    and $-16, %rsp
    cld
    call irq_event
    mov %rbp, %rsp

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
