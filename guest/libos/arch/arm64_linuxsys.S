/* arm64_linuxsys.S : the SVC trap, abort handling, and user entry for the Linux
 * guest on AArch64. The x86-64 counterpart is arch/x86_64_linuxsys.S.
 *
 * The Linux process runs at EL1t (EL1 with SPSel=0, so it uses SP_EL0 as its
 * stack), the same exception level as the libOS; there is no privilege boundary.
 * Because the app runs with SPSel=0, any exception switches the CPU to SP_EL1,
 * which linux_jump/linux_resume point at the process's private handler stack.
 * So a fault taken on a bad user stack still has a good stack to run on, the way
 * the x86 side uses an IST.
 *
 * A synchronous exception from the app (vector offset 0x000) is either an SVC
 * (ESR_EL1.EC == 0x15), dispatched as a syscall, or a translation fault, demand
 * -mapped by pf_handle. A synchronous exception from the kernel (offset 0x200)
 * is a fault the kernel took reaching user memory, also sent to pf_handle.
 *
 * Several processes run as cooperatively scheduled libOS tasks, so the SVC path
 * saves the full user register set into the running process's trapframe (so a
 * blocking syscall can yield and resume, and fork/clone can copy the frame),
 * runs the C dispatcher on SP_EL1, then restores the frame and returns with
 * eret. The running process is lproc_cur (linuxelf.c), whose first field is the
 * trapframe at the TF_* offsets below, then kstack_top.
 *
 * The C side (linuxelf.c) provides:
 *   long linux_sys_dispatch(long nr, long a1, long a2, long a3, long a4, long a5)
 *   int  pf_handle(uint64_t far, uint64_t esr)   (arm64_vmspace.c)
 *   void arm64_fault_report(uint64_t far, uint64_t esr, uint64_t elr)  (no return)
 */

/* Trapframe field offsets within struct lproc (checked in arm64_linuxabi.h). */
#define TF_X0      0
#define TF_X8      64        /* syscall number */
#define TF_X30     240
#define TF_SP      248       /* user stack (SP_EL0) */
#define TF_PC      256       /* ELR_EL1 */
#define TF_PSTATE  264       /* SPSR_EL1 */
#define TF_TPIDR   272       /* TPIDR_EL0 */
#define TF_KSTACK  280       /* top of the handler stack */

    .text

/****************************************************************
 * Exception vector table (VBAR_EL1). Sixteen entries of 0x80 bytes:
 * four groups of four (sync, IRQ, FIQ, SError). The app runs at EL1t,
 * so its exceptions use the "current EL with SP_EL0" group (0x000);
 * kernel faults use "current EL with SP_ELx" (0x200).
 ****************************************************************/
    .balign 0x800
    .global arm64_vectors
arm64_vectors:
    /* Current EL with SP_EL0 (the app, running EL1t). */
    .balign 0x80
    b   el1t_sync               /* 0x000 synchronous: SVC or fault */
    .balign 0x80
    b   hang                    /* 0x080 IRQ (masked) */
    .balign 0x80
    b   hang                    /* 0x100 FIQ (masked) */
    .balign 0x80
    b   serror_entry            /* 0x180 SError */

    /* Current EL with SP_ELx (the kernel, running EL1h). */
    .balign 0x80
    b   abort_entry             /* 0x200 synchronous: kernel fault */
    .balign 0x80
    b   hang                    /* 0x280 IRQ */
    .balign 0x80
    b   hang                    /* 0x300 FIQ */
    .balign 0x80
    b   serror_entry            /* 0x380 SError */

    /* Lower EL, AArch64 (no EL0 in use). */
    .balign 0x80
    b   hang                    /* 0x400 */
    .balign 0x80
    b   hang                    /* 0x480 */
    .balign 0x80
    b   hang                    /* 0x500 */
    .balign 0x80
    b   hang                    /* 0x580 */

    /* Lower EL, AArch32 (unused). */
    .balign 0x80
    b   hang                    /* 0x600 */
    .balign 0x80
    b   hang                    /* 0x680 */
    .balign 0x80
    b   hang                    /* 0x700 */
    .balign 0x80
    b   hang                    /* 0x780 */

/****************************************************************
 * App synchronous exception: SVC -> syscall, else -> abort.
 ****************************************************************/
el1t_sync:
    /* On SP_EL1 (the process handler stack). Peek at ESR without disturbing the
     * app registers: stash x0, read the exception class, branch. */
    str     x0, [sp, #-16]!
    mrs     x0, esr_el1
    lsr     x0, x0, #26             /* EC in bits [31:26] */
    cmp     x0, #0x15               /* SVC from AArch64 */
    b.ne    1f
    ldr     x0, [sp], #16           /* restore x0 */
    b       svc_entry
1:
    ldr     x0, [sp], #16           /* restore x0 */
    b       abort_entry

/****************************************************************
 * SVC: save the user frame, dispatch, restore, eret.
 ****************************************************************/
svc_entry:
    /* Free x0,x1 by parking them on SP_EL1, then reach the trapframe via
     * lproc_cur (its first field is the trapframe). */
    stp     x0, x1, [sp, #-16]!
    adrp    x0, lproc_cur
    ldr     x0, [x0, #:lo12:lproc_cur]

    stp     x2,  x3,  [x0, #16]
    stp     x4,  x5,  [x0, #32]
    stp     x6,  x7,  [x0, #48]
    stp     x8,  x9,  [x0, #64]
    stp     x10, x11, [x0, #80]
    stp     x12, x13, [x0, #96]
    stp     x14, x15, [x0, #112]
    stp     x16, x17, [x0, #128]
    stp     x18, x19, [x0, #144]
    stp     x20, x21, [x0, #160]
    stp     x22, x23, [x0, #176]
    stp     x24, x25, [x0, #192]
    stp     x26, x27, [x0, #208]
    stp     x28, x29, [x0, #224]
    str     x30,      [x0, #240]

    ldp     x2, x3, [sp], #16       /* the parked x0, x1 */
    stp     x2, x3, [x0, #0]

    mrs     x2, sp_el0
    str     x2, [x0, #TF_SP]
    mrs     x2, elr_el1
    str     x2, [x0, #TF_PC]
    mrs     x2, spsr_el1
    str     x2, [x0, #TF_PSTATE]
    mrs     x2, tpidr_el0
    str     x2, [x0, #TF_TPIDR]

    /* linux_sys_dispatch(nr, a1..a5): nr is x8, args are x0..x4. x19 is
     * callee-saved, so the trapframe pointer survives the call. */
    mov     x19, x0
    ldr     x0, [x19, #TF_X8]       /* nr */
    ldr     x1, [x19, #0]           /* a1 (user x0) */
    ldr     x2, [x19, #8]           /* a2 (user x1) */
    ldr     x3, [x19, #16]          /* a3 (user x2) */
    ldr     x4, [x19, #24]          /* a4 (user x3) */
    ldr     x5, [x19, #32]          /* a5 (user x4) */
    bl      linux_sys_dispatch

    /* A blocking syscall may have switched tasks and back, so reload lproc_cur,
     * store the return value, and resume. */
    adrp    x19, lproc_cur
    ldr     x19, [x19, #:lo12:lproc_cur]
    str     x0, [x19, #TF_X0]
    b       restore_tf

/* Restore the user register set from the trapframe at x19 and eret. Shared by
 * the syscall return and linux_resume. SP_EL1 is already the handler stack. */
restore_tf:
    ldr     x0, [x19, #TF_SP]
    msr     sp_el0, x0
    ldr     x0, [x19, #TF_PC]
    msr     elr_el1, x0
    ldr     x0, [x19, #TF_PSTATE]
    msr     spsr_el1, x0
    ldr     x0, [x19, #TF_TPIDR]
    msr     tpidr_el0, x0

    ldp     x0,  x1,  [x19, #0]
    ldp     x2,  x3,  [x19, #16]
    ldp     x4,  x5,  [x19, #32]
    ldp     x6,  x7,  [x19, #48]
    ldp     x8,  x9,  [x19, #64]
    ldp     x10, x11, [x19, #80]
    ldp     x12, x13, [x19, #96]
    ldp     x14, x15, [x19, #112]
    ldp     x16, x17, [x19, #128]
    ldr     x18,      [x19, #144]
    ldp     x20, x21, [x19, #160]
    ldp     x22, x23, [x19, #176]
    ldp     x24, x25, [x19, #192]
    ldp     x26, x27, [x19, #208]
    ldp     x28, x29, [x19, #224]
    ldr     x30,      [x19, #240]
    ldr     x19,      [x19, #152]   /* finally the user x19 */
    eret

/****************************************************************
 * Abort: demand-map the faulting page, then retry. The faulting PC is left in
 * ELR_EL1 unchanged, so eret re-executes the access. Runs on SP_EL1 (the app's
 * handler stack for an app fault, the kernel's own stack for a kernel fault).
 * Save every register so the retry sees exactly the pre-fault state.
 ****************************************************************/
abort_entry:
    sub     sp, sp, #256
    stp     x0,  x1,  [sp, #0]
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30,      [sp, #240]

    mrs     x0, far_el1
    mrs     x1, esr_el1
    bl      pf_handle
    cbz     w0, 9f                  /* 0: not a page we can fill -> fatal */

    ldp     x0,  x1,  [sp, #0]
    ldp     x2,  x3,  [sp, #16]
    ldp     x4,  x5,  [sp, #32]
    ldp     x6,  x7,  [sp, #48]
    ldp     x8,  x9,  [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30,      [sp, #240]
    add     sp, sp, #256
    eret

9:
    /* Unrecoverable: report and halt. arm64_fault_report does not return. */
    mrs     x0, far_el1
    mrs     x1, esr_el1
    mrs     x2, elr_el1
    bl      arm64_fault_report
    b       .

serror_entry:
    mrs     x0, far_el1
    mrs     x1, esr_el1
    mrs     x2, elr_el1
    bl      arm64_fault_report
    b       .

hang:
    b       .

/****************************************************************
 * linux_jump / linux_resume: enter user code at EL1t.
 ****************************************************************/
    .global linux_jump
linux_jump:
    /* x0 = entry, x1 = user sp. Enter EL1t (SPSel=0 -> SP_EL0) with all
     * interrupts masked, so the cooperative scheduler never preempts the app.
     * SP_EL1 is set to the handler stack for the first exception. */
    msr     daifset, #0xf
    msr     sp_el0, x1
    msr     elr_el1, x0
    mov     x2, #0x3c4              /* SPSR: EL1t (M=0b0100), DAIF masked */
    msr     spsr_el1, x2

    adrp    x2, lproc_cur
    ldr     x2, [x2, #:lo12:lproc_cur]
    ldr     x2, [x2, #TF_KSTACK]
    mov     sp, x2                  /* SP_EL1 = handler stack */

    mov     x0, #0
    mov     x1, #0
    mov     x2, #0
    mov     x3, #0                  /* SysV: no atexit function to register */
    mov     x29, #0
    mov     x30, #0
    eret

    .global linux_resume
linux_resume:
    /* x0 = struct lproc * (its trapframe is the first field). Resume from the
     * frame: a forked child (x0 = 0), or a freshly spawned thread. */
    msr     daifset, #0xf
    mov     x19, x0
    ldr     x2, [x19, #TF_KSTACK]
    mov     sp, x2                  /* SP_EL1 = handler stack */
    b       restore_tf

    .section .note.GNU-stack, "", %progbits
