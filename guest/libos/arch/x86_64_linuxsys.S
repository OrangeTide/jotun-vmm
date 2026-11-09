/* x86_64_linuxsys.S : the SYSCALL trap and user entry for the Linux guest.
 *
 * A Linux process runs in CPL 3; the libOS kernel runs in CPL 0. SYSCALL
 * crosses from user to kernel: the CPU loads the kernel code and stack
 * selectors from STAR, leaves the user rsp live, and puts the caller's return
 * address in rcx and its RFLAGS in r11 (masked by SFMASK). The return crosses
 * back to CPL 3 with an IRETQ frame (user SS, rsp, rflags, CS, rip); SYSRET
 * would also work, but IRETQ is used uniformly for the syscall return, a forked
 * child, and a fresh image, and it sidesteps the SYSRET corner cases.
 *
 * Several processes run as libOS tasks that switch cooperatively, so the entry
 * saves the full user register set into the running process's trapframe (so a
 * blocking syscall can yield and later resume, and fork can copy the frame),
 * runs the C dispatcher on that process's private handler stack, and returns by
 * restoring the frame and IRETQ-ing back down to CPL 3.
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

/* User selectors: the DPL-3 GDT entries (0x20 data, 0x28 code) with RPL 3. The
 * IRETQ frame returns to CPL 3 with these. See the GDT in x86_64.c. */
#define USER_CS   0x2b
#define USER_SS   0x23

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

/* Restore the user register set from the trapframe at %r15 and resume in CPL 3
 * at the saved user rip. Shared by the syscall return and linux_resume.
 *
 * Build the CPU privilege-return frame (SS, user rsp, rflags, CS, rip) on the
 * current kernel handler stack, then load the general registers by hand and
 * IRETQ. IRETQ reloads rip, rflags, rsp, cs, and ss from that frame and drops
 * to CPL 3; rax is the only scratch needed to lay the frame down, so it is
 * restored from the trapframe afterward. The user's saved rflags carries IF set
 * (user runs preemptibly, from linux_jump's 0x202, preserved through each
 * syscall in r11), so IRETQ resumes user code with interrupts enabled. */
restore_tf:
    movq TF_RSP(%r15), %rax
    pushq $USER_SS
    pushq %rax                      /* user rsp */
    movq TF_R11(%r15), %rax
    pushq %rax                      /* user rflags */
    pushq $USER_CS
    movq TF_RCX(%r15), %rax
    pushq %rax                      /* user rip */

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
    movq TF_RCX(%r15), %rcx         /* user rcx (SYSCALL clobbers it; forks keep it) */
    movq TF_R11(%r15), %r11         /* user r11 */
    movq TF_RDI(%r15), %rdi
    movq TF_R15(%r15), %r15         /* last: drops the trapframe pointer */
    iretq

/****************************************************************
 * linux_jump / linux_resume: enter user code.
 ****************************************************************/
    .global linux_jump
linux_jump:
    /* rdi = entry, rsi = user rsp. Enter the fresh image in CPL 3 through an
     * IRETQ frame. The SysV entry contract: rsp points at argc, rdx (an atexit
     * function to register) is 0, rbp is 0. rflags is 0x202 (reserved bit plus
     * IF) so the preemption timer can interrupt a user compute loop; the kernel
     * stays non-preemptible because SYSCALL masks IF via SFMASK. */
    cli
    xorq %rdx, %rdx
    xorq %rbp, %rbp
    pushq $USER_SS
    pushq %rsi                      /* user rsp */
    pushq $0x202                    /* rflags: IF on */
    pushq $USER_CS
    pushq %rdi                      /* entry */
    iretq

    .global linux_resume
linux_resume:
    /* rdi = struct lproc *. Resume from its trapframe into CPL 3. */
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
