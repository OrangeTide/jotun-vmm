/* x86_64_fault.S : CPU exception stubs for diagnostics.
 *
 * One stub per CPU exception vector (0..31). Each normalises the stack to a
 * uniform frame and jumps to a common path that saves the general-purpose
 * registers and calls fault_report (x86_64.c), which prints the fault and
 * halts. In 64-bit mode an exception always pushes SS:RSP:RFLAGS:CS:RIP even
 * without a privilege change, so the only irregularity is that some vectors
 * also push an error code. The no-error-code stubs push a zero so every frame
 * looks the same to the C side.
 *
 * exc_stub_table is an array of the 32 stub entry points, read by the C side to
 * fill the IDT. These are a safety net: a healthy guest never takes any of
 * them. They matter for the Linux binary-compat path, where a foreign binary
 * can fault in ways the libOS never would, and a silent triple fault is the
 * alternative. */

    .text

/* A stub for a vector the CPU does not give an error code: push a zero in the
 * error-code slot, then the vector, then go to the common path. */
    .macro EXC_NOERR vec
exc_stub_\vec:
    pushq $0
    pushq $\vec
    jmp exc_common
    .endm

/* A stub for a vector that already carries an error code on the stack. */
    .macro EXC_ERR vec
exc_stub_\vec:
    pushq $\vec
    jmp exc_common
    .endm

    EXC_NOERR 0
    EXC_NOERR 1
    EXC_NOERR 2
    EXC_NOERR 3
    EXC_NOERR 4
    EXC_NOERR 5
    EXC_NOERR 6
    EXC_NOERR 7
    EXC_ERR   8
    EXC_NOERR 9
    EXC_ERR   10
    EXC_ERR   11
    EXC_ERR   12
    EXC_ERR   13
    EXC_ERR   14
    EXC_NOERR 15
    EXC_NOERR 16
    EXC_ERR   17
    EXC_NOERR 18
    EXC_NOERR 19
    EXC_NOERR 20
    EXC_ERR   21
    EXC_NOERR 22
    EXC_NOERR 23
    EXC_NOERR 24
    EXC_NOERR 25
    EXC_NOERR 26
    EXC_NOERR 27
    EXC_NOERR 28
    EXC_ERR   29
    EXC_ERR   30
    EXC_NOERR 31

/* Common path: on entry the stack holds (low to high)
 *   vec, err, RIP, CS, RFLAGS, RSP, SS
 * Save the GP registers below that and hand their address to fault_report. */
exc_common:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    cld
    mov %rsp, %rdi             /* struct faultframe * */
    call fault_report
    /* fault_report halts and does not return, but guard anyway. */
1:  cli
    hlt
    jmp 1b

    .data
    .align 8
    .global exc_stub_table
exc_stub_table:
    .quad exc_stub_0,  exc_stub_1,  exc_stub_2,  exc_stub_3
    .quad exc_stub_4,  exc_stub_5,  exc_stub_6,  exc_stub_7
    .quad exc_stub_8,  exc_stub_9,  exc_stub_10, exc_stub_11
    .quad exc_stub_12, exc_stub_13, exc_stub_14, exc_stub_15
    .quad exc_stub_16, exc_stub_17, exc_stub_18, exc_stub_19
    .quad exc_stub_20, exc_stub_21, exc_stub_22, exc_stub_23
    .quad exc_stub_24, exc_stub_25, exc_stub_26, exc_stub_27
    .quad exc_stub_28, exc_stub_29, exc_stub_30, exc_stub_31

    .section .note.GNU-stack, "", @progbits
