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

/* Trapframe field offsets within struct lproc (mirrors x86_64_linuxsys.S). A
 * CPL-3 fault saves the faulting user context here before its IRETQ redirect
 * reuses the fault stack, so a signal handler or teardown can see it.
 * kstack_top (the handler stack the redirect lands on) follows the trapframe. */
#define TF_RAX    0
#define TF_RBX    8
#define TF_RCX    16        /* user rip */
#define TF_RDX    24
#define TF_RSI    32
#define TF_RDI    40
#define TF_RBP    48
#define TF_R8     56
#define TF_R9     64
#define TF_R10    72
#define TF_R11    80        /* user rflags */
#define TF_R12    88
#define TF_R13    96
#define TF_R14    104
#define TF_R15    112
#define TF_RSP    120       /* user rsp */
#define TF_KSTACK 128

/* Kernel and user segment selectors (see the GDT in x86_64.c). */
#define KERN_CS   0x08
#define KERN_SS   0x10

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
 * Save the GP registers below that. A fault taken in CPL 3 (a Linux process)
 * terminates just that process; a fault in ring 0 (the kernel itself, or a
 * double fault) is reported and halts. */
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
    cmpq $8, 120(%rsp)        /* #DF: the fault machinery is itself suspect */
    je 1f
    testb $3, 144(%rsp)       /* saved CS RPL: 3 means the fault was in CPL 3 */
    jnz kill_user_fault
1:  mov %rsp, %rdi            /* struct faultframe * */
    call fault_report
    /* fault_report halts and does not return, but guard anyway. */
2:  cli
    hlt
    jmp 2b

/* Shared tail for a CPU fault taken in CPL 3: redirect the CPU's return frame
 * so the IRETQ below re-enters the kernel at linux_fault_entry, running in ring 0
 * on the faulting process's handler stack. That C routine runs a handler if one
 * is installed for the fault's signal, else terminates the process. On entry the
 * 15 saved GP registers sit at 0(%rsp) (r15) up to 112(%rsp) (rax), then the
 * uniform trap frame: vec(120) err(128) rip(136) cs(144) rflags(152) rsp(160)
 * ss(168). Reached from exc_common and pf_isr, which lay down the same frame.
 *
 * The faulting user context must survive the redirect (the handler stack the
 * IRETQ lands on is the same stack the fault frame sits atop, so running C there
 * overwrites it). Copy the whole register set into lproc_cur->tf first; the
 * trapframe is the first member of struct lproc, so lproc_cur is its address. */
kill_user_fault:
    movq lproc_cur(%rip), %rbx /* rbx = &tf; rax is the copy scratch */
    movq 112(%rsp), %rax
    movq %rax, TF_RAX(%rbx)
    movq 104(%rsp), %rax
    movq %rax, TF_RBX(%rbx)
    movq 136(%rsp), %rax       /* user rip */
    movq %rax, TF_RCX(%rbx)
    movq 88(%rsp), %rax
    movq %rax, TF_RDX(%rbx)
    movq 80(%rsp), %rax
    movq %rax, TF_RSI(%rbx)
    movq 72(%rsp), %rax
    movq %rax, TF_RDI(%rbx)
    movq 64(%rsp), %rax
    movq %rax, TF_RBP(%rbx)
    movq 56(%rsp), %rax
    movq %rax, TF_R8(%rbx)
    movq 48(%rsp), %rax
    movq %rax, TF_R9(%rbx)
    movq 40(%rsp), %rax
    movq %rax, TF_R10(%rbx)
    movq 152(%rsp), %rax       /* user rflags */
    movq %rax, TF_R11(%rbx)
    movq 24(%rsp), %rax
    movq %rax, TF_R12(%rbx)
    movq 16(%rsp), %rax
    movq %rax, TF_R13(%rbx)
    movq 8(%rsp), %rax
    movq %rax, TF_R14(%rbx)
    movq 0(%rsp), %rax
    movq %rax, TF_R15(%rbx)
    movq 160(%rsp), %rax       /* user rsp */
    movq %rax, TF_RSP(%rbx)

    /* Arguments for linux_fault_entry(vec, cr2): overwrite the saved rdi and rsi
     * slots, which the pops below load into rdi and rsi. */
    movq 120(%rsp), %rax       /* vec -> first argument */
    movq %rax, 72(%rsp)        /* saved rdi */
    movq %cr2, %rax            /* faulting address -> second argument */
    movq %rax, 80(%rsp)        /* saved rsi */

    movq TF_KSTACK(%rbx), %rax
    movq %rax, 160(%rsp)      /* return rsp = the process handler stack */
    movq $KERN_SS, 168(%rsp)  /* return ss */
    movq $KERN_CS, 144(%rsp)  /* return cs */
    movq $0x2, 152(%rsp)      /* return rflags: reserved bit only, IF off */
    leaq linux_fault_entry(%rip), %rax
    movq %rax, 136(%rsp)      /* return rip */
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi                  /* = vec */
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
    add $16, %rsp             /* discard vec and err */
    iretq

/* Page-fault handler (vector 14). Tries to demand-map the faulting page via
 * pf_handle; on success it retries the instruction, otherwise it falls through
 * to the same report-and-halt path as the other exceptions. The CPU has pushed
 * an error code; we push the vector so the fatal path matches struct faultframe
 * exactly (vec, err, rip, ...). */
    .global pf_isr
pf_isr:
    pushq $14                  /* vec, below the CPU's error code */
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
    mov %cr2, %rdi             /* arg1: faulting address */
    mov 128(%rsp), %rsi        /* arg2: error code (15 regs + vec above it) */
    call pf_handle
    test %rax, %rax
    jz 4f                      /* 0: not handled; kill the process or report */

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
    add $16, %rsp              /* discard vec and the error code */
    iretq

    /* Not demand-paged: a fault in CPL 3 terminates the process, one in ring 0
     * (a kernel bug, or OOM on a demand page) is reported and halts. */
4:  testb $3, 144(%rsp)        /* saved CS RPL */
    jnz kill_user_fault
    mov %rsp, %rdi             /* struct faultframe * */
    call fault_report
3:  cli
    hlt
    jmp 3b

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
