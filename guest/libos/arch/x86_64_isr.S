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
 * It also saves and restores the full FPU/vector state around the handler. The
 * handler may switch to another task that uses the FPU, so without this an
 * interrupt would not be transparent to a task that was mid-computation in
 * floating point or SIMD. When XSAVE is enabled (fpu_use_xsave, set by
 * arch_fpu_init once AVX is on) the wider state is saved with XSAVE/XRSTOR;
 * otherwise FXSAVE/FXRSTOR covers x87 and SSE. The area is carved from the
 * stack, 64-byte aligned (XSAVE's requirement, which also satisfies FXSAVE's
 * 16), and sized for x87 + SSE + AVX; arch_fpu_init keeps the state within it. */

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
    sub $1024, %rsp            /* carve the FPU save area */
    and $-64, %rsp             /* XSAVE needs 64-byte alignment (FXSAVE wants 16) */
    mov %rsp, %r12             /* r12: save area, call-preserved */

    cmpb $0, fpu_use_xsave(%rip)
    je 1f
    /* Zero the 64-byte XSAVE header (at offset 512) before saving. The
     * non-compacted XSAVE writes only XSTATE_BV; it leaves XCOMP_BV and the
     * reserved header bytes untouched, and XRSTOR raises #GP if either is
     * nonzero. The area is carved fresh from the stack each time, so without
     * this it restores over stale stack bytes and faults. */
    xorq %rax, %rax
    movq %rax, 512(%r12)
    movq %rax, 520(%r12)
    movq %rax, 528(%r12)
    movq %rax, 536(%r12)
    movq %rax, 544(%r12)
    movq %rax, 552(%r12)
    movq %rax, 560(%r12)
    movq %rax, 568(%r12)
    mov $0xffffffff, %eax      /* XSAVE/XRSTOR component mask: all enabled state */
    mov $0xffffffff, %edx
    xsave (%r12)
    jmp 2f
1:  fxsave (%r12)
2:
    cld
    movq 128(%rbp), %rdi       /* saved CS (15 GP regs then rip sit above rbp) */
    andl $3, %edi              /* arg: the interrupted privilege level */
    call irq_event

    cmpb $0, fpu_use_xsave(%rip)
    je 3f
    mov $0xffffffff, %eax
    mov $0xffffffff, %edx
    xrstor (%r12)
    jmp 4f
3:  fxrstor (%r12)
4:
    mov %rbp, %rsp             /* drop the save area back to the GP frame */

    /* Signal end-of-interrupt to the local APIC (x2APIC EOI MSR) so it will
     * deliver the next one. Done before the GP registers are popped, since this
     * clobbers rax/rcx/rdx, which the pops then restore to the interrupted
     * values. Skipped when no APIC was enabled (then nothing uses this vector). */
    cmpb $0, apic_enabled(%rip)
    je 5f
    xor %eax, %eax
    xor %edx, %edx
    mov $0x80b, %ecx
    wrmsr
5:
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
