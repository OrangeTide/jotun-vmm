/* x86_64_linuxsys.S : the SYSCALL trap and user entry for the Linux guest.
 *
 * Each Linux process runs in the same ring as the libOS (no privilege
 * boundary), so a SYSCALL does not change ring and we must not return with
 * SYSRET. The CPU vectors SYSCALL to LSTAR with the caller's return address in
 * rcx and its RFLAGS in r11, masking the bits in SFMASK.
 *
 * Several processes run as libOS tasks that switch cooperatively, so the entry
 * saves the full user register set into the running process's trapframe (so a
 * blocking syscall can yield and later resume, and fork can copy the frame),
 * runs the C dispatcher on that process's private handler stack, and returns by
 * restoring the frame and jumping back (jmp *rcx), not SYSRET.
 *
 * The running process is lproc_cur (linuxelf.c), a struct whose first fields are
 * the trapframe at the TF_* offsets below, then kstack_top. Those offsets are
 * mirrored and checked with _Static_assert on the C side.
 *
 *   void linux_syscall_entry(void)         -- the LSTAR target, never called by C.
 *   void linux_jump(uint64_t entry, uint64_t user_rsp)
 *       Enter a freshly loaded image: interrupts off, rsp = the SysV stack,
 *       jump to the entry. Does not return.
 *   void linux_resume(struct lproc *p)
 *       Resume a process from its trapframe (used for the first run of a forked
 *       child, whose frame has rax = 0). Does not return.
 *
 * The C side (linuxelf.c) provides:
 *   long linux_sys_dispatch(long nr, long a1, long a2, long a3, long a4, long a5)
 */

/* Trapframe field offsets within struct lproc (checked in linuxelf.c). */
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
#define TF_KSTACK 128       /* top of the handler stack */

    .text

/****************************************************************
 * SYSCALL entry: the LSTAR target.
 ****************************************************************/
    .global linux_syscall_entry
linux_syscall_entry:
    /* rcx = user rip, r11 = user rflags, user rsp still live, rax = call number,
     * args in rdi, rsi, rdx, r10, r8, r9. Free r15 as a scratch (saved to a
     * global slot) so we can reach the trapframe through lproc_cur. */
    movq %r15, le_scratch(%rip)
    movq lproc_cur(%rip), %r15

    movq %rax, TF_RAX(%r15)
    movq %rbx, TF_RBX(%r15)
    movq %rcx, TF_RCX(%r15)         /* user rip */
    movq %rdx, TF_RDX(%r15)
    movq %rsi, TF_RSI(%r15)
    movq %rdi, TF_RDI(%r15)
    movq %rbp, TF_RBP(%r15)
    movq %r8,  TF_R8(%r15)
    movq %r9,  TF_R9(%r15)
    movq %r10, TF_R10(%r15)
    movq %r11, TF_R11(%r15)         /* user rflags */
    movq %r12, TF_R12(%r15)
    movq %r13, TF_R13(%r15)
    movq %r14, TF_R14(%r15)
    movq le_scratch(%rip), %rax     /* the original r15 */
    movq %rax, TF_R15(%r15)
    movq %rsp, TF_RSP(%r15)         /* user rsp */

    /* Switch to this process's private handler stack. */
    movq TF_KSTACK(%r15), %rsp

    /* Dispatch args: nr in rdi, a1..a5 in rsi, rdx, rcx, r8, r9. */
    movq TF_RAX(%r15), %rdi         /* nr */
    movq TF_RDI(%r15), %rsi         /* a1 */
    movq TF_RSI(%r15), %rdx         /* a2 */
    movq TF_RDX(%r15), %rcx         /* a3 */
    movq TF_R10(%r15), %r8          /* a4 */
    movq TF_R8(%r15),  %r9          /* a5 */
    call linux_sys_dispatch

    /* A blocking syscall may have switched tasks and back, so reload lproc_cur.
     * Store the return value, then restore the frame and resume the user. */
    movq lproc_cur(%rip), %r15
    movq %rax, TF_RAX(%r15)
    jmp restore_tf

/* Restore the user register set from the trapframe at %r15 and resume at the
 * saved user rip. Shared by the syscall return and linux_resume. */
restore_tf:
    movq TF_RAX(%r15), %rax
    movq TF_RBX(%r15), %rbx
    movq TF_RDX(%r15), %rdx
    movq TF_RSI(%r15), %rsi
    movq TF_RBP(%r15), %rbp
    movq TF_R8(%r15),  %r8
    movq TF_R9(%r15),  %r9
    movq TF_R10(%r15), %r10
    movq TF_R12(%r15), %r12
    movq TF_R13(%r15), %r13
    movq TF_R14(%r15), %r14
    movq TF_RCX(%r15), %rcx         /* user rip (jmp target) */
    movq TF_R11(%r15), %r11         /* user rflags */
    pushq %r11
    popfq                           /* restore rflags; IF stays off for the user */
    movq TF_RDI(%r15), %rdi
    movq TF_RSP(%r15), %rsp         /* back to the user stack */
    movq TF_R15(%r15), %r15         /* finally user r15 (drops the frame pointer) */
    jmp *%rcx

/****************************************************************
 * linux_jump / linux_resume: enter user code.
 ****************************************************************/
    .global linux_jump
linux_jump:
    /* rdi = entry, rsi = user rsp. The SysV entry contract: rsp points at argc,
     * rdx (an atexit function to register) is 0, rbp is 0. Interrupts off so the
     * cooperative scheduler never preempts user code. */
    cli
    movq %rsi, %rsp
    xorq %rdx, %rdx
    xorq %rbp, %rbp
    jmp *%rdi

    .global linux_resume
linux_resume:
    /* rdi = struct lproc *. Resume from its trapframe. */
    cli
    movq %rdi, %r15
    jmp restore_tf

/****************************************************************
 * Scratch slot for the entry path.
 ****************************************************************/
    .data
    .align 8
le_scratch:    .quad 0

    .section .note.GNU-stack, "", @progbits
