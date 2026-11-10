/* x86_64_linuxabi.h : the x86-64 backend for the Linux-compat guest.
 *
 * Provides what linuxelf.c's arch-neutral core needs from the machine: the
 * Linux x86-64 syscall numbers, the user-register trapframe the SYSCALL trap
 * saves and restores, and small helpers to enter user code, set the thread
 * pointer, and arm the trap. Mirrored by arch/arm64_linuxabi.h. */
#ifndef X86_64_LINUXABI_H
#define X86_64_LINUXABI_H

#include <stdint.h>

/* Linux x86-64 syscall numbers the dispatcher handles. */
#define SYS_read            0
#define SYS_write           1
#define SYS_open            2
#define SYS_close           3
#define SYS_stat            4
#define SYS_fstat           5
#define SYS_lstat           6
#define SYS_poll            7
#define SYS_ppoll           271
#define SYS_lseek           8
#define SYS_mmap            9
#define SYS_mprotect        10
#define SYS_munmap          11
#define SYS_mremap          25
#define SYS_brk             12
#define SYS_rt_sigaction    13
#define SYS_rt_sigprocmask  14
#define SYS_rt_sigreturn    15
#define SYS_pause           34
#define SYS_ioctl           16
#define SYS_pread64         17
#define SYS_writev          20
#define SYS_access          21
#define SYS_pipe            22
#define SYS_sched_yield     24
#define SYS_mincore         27
#define SYS_madvise         28
#define SYS_dup             32
#define SYS_dup2            33
#define SYS_getpid          39
#define SYS_socket          41
#define SYS_getsockname     51
#define SYS_getpeername     52
#define SYS_setxattr        188
#define SYS_lsetxattr       189
#define SYS_fsetxattr       190
#define SYS_getxattr        191
#define SYS_lgetxattr       192
#define SYS_fgetxattr       193
#define SYS_clone           56
#define SYS_fork            57
#define SYS_vfork           58
#define SYS_kill            62
#define SYS_tkill           200
#define SYS_execve          59
#define SYS_exit            60
#define SYS_wait4           61
#define SYS_uname           63
#define SYS_fcntl           72
#define SYS_getcwd          79
#define SYS_chdir           80
#define SYS_fchdir          81
#define SYS_rename          82
#define SYS_mkdir           83
#define SYS_rmdir           84
#define SYS_link            86
#define SYS_unlink          87
#define SYS_symlink         88
#define SYS_readlink        89
#define SYS_chmod           90
#define SYS_fchmod          91
#define SYS_umask           95
#define SYS_gettimeofday    96
#define SYS_getrusage       98
#define SYS_sysinfo         99
#define SYS_getuid          102
#define SYS_setuid          105
#define SYS_setgid          106
#define SYS_getgid          104
#define SYS_geteuid         107
#define SYS_getegid         108
#define SYS_setpgid         109
#define SYS_getppid         110
#define SYS_getpgrp         111
#define SYS_setsid          112
#define SYS_setreuid        113
#define SYS_setregid        114
#define SYS_getgroups       115
#define SYS_setgroups       116
#define SYS_setresuid       117
#define SYS_setresgid       119
#define SYS_sigaltstack     131
#define SYS_statfs          137
#define SYS_fstatfs         138
#define SYS_prctl           157
#define SYS_arch_prctl      158
#define SYS_gettid          186
#define SYS_time            201
#define SYS_futex           202
#define SYS_getdents64      217
#define SYS_set_tid_address 218
#define SYS_fadvise64       221
#define SYS_sched_getaffinity 204
#define SYS_nanosleep       35
#define SYS_clock_gettime   228
#define SYS_clock_nanosleep 230
#define SYS_exit_group      231
#define SYS_tgkill          234
#define SYS_openat          257
#define SYS_mkdirat         258
#define SYS_newfstatat      262
#define SYS_unlinkat        263
#define SYS_renameat        264
#define SYS_linkat          265
#define SYS_symlinkat       266
#define SYS_readlinkat      267
#define SYS_fchmodat        268
#define SYS_faccessat       269
#define SYS_set_robust_list 273
#define SYS_dup3            292
#define SYS_pipe2           293
#define SYS_prlimit64       302
#define SYS_renameat2       316
#define SYS_getrandom       318
#define SYS_copy_file_range 326
#define SYS_statx           332
#define SYS_rseq            334
#define SYS_clone3          435
#define SYS_close_range     436
#define SYS_faccessat2      439

/* arch_prctl subfunction: set the FS base (TLS). */
#define ARCH_SET_FS 0x1002

/* The user register set the SYSCALL trap saves on entry and restores on return.
 * Its layout is mirrored by the TF_* offsets in arch/x86_64_linuxsys.S, so a
 * process resumes exactly where it trapped; fork copies it to the child. */
struct trapframe {
    uint64_t rax;   /* 0 */
    uint64_t rbx;   /* 8 */
    uint64_t rcx;   /* 16  user rip (SYSCALL return address) */
    uint64_t rdx;   /* 24 */
    uint64_t rsi;   /* 32 */
    uint64_t rdi;   /* 40 */
    uint64_t rbp;   /* 48 */
    uint64_t r8;    /* 56 */
    uint64_t r9;    /* 64 */
    uint64_t r10;   /* 72 */
    uint64_t r11;   /* 80  user rflags */
    uint64_t r12;   /* 88 */
    uint64_t r13;   /* 96 */
    uint64_t r14;   /* 104 */
    uint64_t r15;   /* 112 */
    uint64_t rsp;   /* 120 user rsp */
};
_Static_assert(sizeof(struct trapframe) == 128, "trapframe size feeds TF_KSTACK");

/* After fork or a thread clone the child resumes as if the syscall returned 0. */
static inline void
tf_set_ret0(struct trapframe *tf)
{
    tf->rax = 0;
}

/* A thread's entry stack pointer. */
static inline void
tf_set_sp(struct trapframe *tf, uint64_t sp)
{
    tf->rsp = sp;
}

/* The interrupted user stack pointer (where a signal frame is placed). */
static inline uint64_t
tf_user_sp(const struct trapframe *tf)
{
    return tf->rsp;
}

/* The syscall/return value register (rax), preserved when a fault is caught. */
static inline uint64_t
tf_retval(const struct trapframe *tf)
{
    return tf->rax;
}

/* On x86-64 the TLS base lives in lproc->tls and is loaded by the resume hook
 * (arch_load_tls), not in the trapframe, so recording it here is a no-op. */
static inline void
tf_set_tls(struct trapframe *tf, uint64_t tls)
{
    (void)tf;
    (void)tls;
}

/* The 6th syscall argument (mmap's offset), which the dispatcher's register
 * set stops short of. On x86-64 syscall arg 6 is r9. */
static inline uint64_t
tf_arg6(struct trapframe *tf)
{
    return tf->r9;
}

/* clone() passes the new TLS and the child-tid pointer in different argument
 * slots on different architectures. x86-64 uses the order
 * clone(flags, stack, ptid, ctid, tls), so the fourth argument is the child-tid
 * pointer and the fifth is the TLS. (aarch64 is CLONE_BACKWARDS and swaps the
 * two.) This normalizes the fourth and fifth syscall arguments for the
 * dispatcher. */
static inline void
clone_split_tls_ctid(uint64_t a4, uint64_t a5, uint64_t *tls, uint64_t *ctid)
{
    *ctid = a4;
    *tls = a5;
}

/* x86-64 MSRs for the SYSCALL path. */
#define MSR_EFER     0xC0000080u
#define MSR_STAR     0xC0000081u
#define MSR_LSTAR    0xC0000082u
#define MSR_SFMASK   0xC0000084u
#define MSR_FS_BASE  0xC0000100u

static inline void
wrmsr(uint32_t msr, uint64_t val)
{
    __asm__ volatile ("wrmsr"
                      : : "c"(msr), "a"((uint32_t)val),
                          "d"((uint32_t)(val >> 32)));
}

static inline uint64_t
read_efer(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_EFER));
    return ((uint64_t)hi << 32) | lo;
}

/* Load the thread pointer (TLS base) into the hardware register. */
static inline void
arch_load_tls(uint64_t v)
{
    wrmsr(MSR_FS_BASE, v);
}

/* A free-running cycle counter, the source for the coarse time and random
 * seeds. Not a real clock. */
static inline uint64_t
arch_cycles(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* The SYSCALL trap entry and the two user-entry paths (arch/x86_64_linuxsys.S).
 * linux_jump enters a freshly loaded image; linux_resume continues a process
 * from its trapframe (a fork child, or a thread). */
extern void linux_syscall_entry(void);
extern void linux_jump(uint64_t entry, uint64_t user_rsp) __attribute__((noreturn));

/* Arm the SYSCALL trap on this cpu: enable EFER.SCE, point LSTAR at the handler,
 * set the selectors in STAR, and mask IF and DF on entry.
 *
 * STAR[47:32] is the kernel code selector SYSCALL loads (0x08; SS becomes
 * 0x10). STAR[63:48] is the SYSRET selector base: SYSRET would load SS from
 * base+8 and CS from base+16, both forced to RPL 3, so base 0x18 selects the
 * user data at 0x20 and the user code at 0x28 (see the GDT in x86_64.c). The
 * return path uses IRETQ for now, which ignores STAR[63:48]; the base is set so
 * a later SYSRET fast path is a drop-in. */
static inline void
linux_trap_init(void)
{
    wrmsr(MSR_EFER, read_efer() | 1u);          /* EFER.SCE */
    wrmsr(MSR_STAR, ((uint64_t)0x08u << 32) | ((uint64_t)0x18u << 48));
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)linux_syscall_entry);
    wrmsr(MSR_SFMASK, 0x600u);                  /* mask IF and DF on entry */
}

/****************************************************************
 * Signal-handler delivery (the x86-64 rt_sigframe ABI).
 *
 * When the kernel delivers a caught signal it saves the interrupted user
 * context into a frame on the user stack and re-points the trapframe at the
 * handler. The handler runs, then its restorer (glibc's __restore_rt, or a
 * trampoline) calls rt_sigreturn, which reads the frame back and restores the
 * context. The struct layout below is the exact x86-64 kernel ABI, so glibc's
 * restorer and any handler that inspects uc_mcontext see the fields where they
 * expect them. The common selection and masking logic is in linuxelf.c; these
 * helpers are just the machine-dependent frame assembly.
 *
 * The floating-point state is not saved (fpstate = 0). Delivery happens only at
 * a syscall-return boundary, where the SysV ABI already treats the vector
 * registers as call-clobbered (the libc syscall wrapper is a function call), so
 * the interrupted code holds no live vector state across it. A later move to
 * delivering at an arbitrary preemption point would need the FP save the
 * preemption ISR already performs.
 ****************************************************************/

/* The machine context, matching struct sigcontext_64. The register order is the
 * glibc gregs[] order, so uc_mcontext aliases it correctly. */
struct linux_sigcontext {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip;
    uint64_t eflags;
    uint16_t cs, gs, fs, ss;        /* one 8-byte word (gregs[REG_CSGSFS]) */
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate;               /* pointer to FP state, 0 = none */
    uint64_t reserved1[8];
};
_Static_assert(sizeof(struct linux_sigcontext) == 256, "sigcontext_64 ABI");

struct linux_ucontext {
    uint64_t uc_flags;
    uint64_t uc_link;
    struct { uint64_t ss_sp; uint32_t ss_flags; uint32_t __pad; uint64_t ss_size; } uc_stack;
    struct linux_sigcontext uc_mcontext;
    uint8_t  uc_sigmask[128];       /* sigset_t; only the low 8 bytes are used */
};

struct linux_siginfo {
    int32_t si_signo, si_errno, si_code;
    int32_t __pad0;
    int32_t si_pid, si_uid;         /* the _kill union, at offset 16 */
    uint8_t __pad[128 - 24];
};
_Static_assert(sizeof(struct linux_siginfo) == 128, "siginfo ABI");

/* The frame the kernel lays on the user stack: the restorer address, then the
 * ucontext, then the siginfo. The handler gets rsi = &info, rdx = &uc. */
struct linux_rt_sigframe {
    uint64_t pretcode;
    struct linux_ucontext uc;
    struct linux_siginfo info;
};

static inline uint64_t
arch_sigframe_size(void)
{
    return sizeof(struct linux_rt_sigframe);
}

/* Place the frame below the chosen stack top: skip the 128-byte red zone on the
 * normal stack, reserve the frame, and align so the handler entry sees
 * rsp % 16 == 8 (as if it had been reached by a CALL). */
static inline uint64_t
arch_sigframe_place(uint64_t sp, int onstack)
{
    if (!onstack)
        sp -= 128;                                      /* red zone */
    sp -= sizeof(struct linux_rt_sigframe);
    sp = (sp & ~(uint64_t)15) - 8;
    return sp;
}

/* Where the ucontext sits when rt_sigreturn is entered: the restorer has popped
 * pretcode, so the user rsp points at uc. */
static inline uint64_t
arch_sigframe_ucaddr(const struct trapframe *tf)
{
    return tf->rsp;
}

/* Build the frame at the identity-mapped user address `frame` (the caller has
 * range-checked it) and re-point tf at the handler. `result` is the value the
 * interrupted code should see in rax on return; `restart` re-runs the trapped
 * syscall instead (rip back to the SYSCALL, rax back to the call number, which
 * tf still holds here). altsp is reported in uc_stack for a handler that reads
 * it. */
static inline void
arch_sigframe_build(uint64_t frame, struct trapframe *tf, int sig,
                    uint64_t handler, uint64_t flags, uint64_t restorer,
                    uint64_t result, int restart, uint64_t oldmask,
                    uint64_t altsp, uint64_t altsize,
                    int si_code, int si_pid, int si_uid, uint64_t si_addr)
{
    (void)flags;
    struct linux_rt_sigframe *f = (struct linux_rt_sigframe *)(uintptr_t)frame;
    memset(f, 0, sizeof *f);

    f->info.si_signo = sig;
    f->info.si_code  = si_code;
    if (si_code != 0)               /* a fault: _sigfault.si_addr at offset 16 */
        *(uint64_t *)&f->info.si_pid = si_addr;
    else {                          /* a sent signal: _kill.si_pid / si_uid */
        f->info.si_pid = si_pid;
        f->info.si_uid = si_uid;
    }

    f->uc.uc_stack.ss_sp   = altsp;
    f->uc.uc_stack.ss_size = altsize;

    struct linux_sigcontext *sc = &f->uc.uc_mcontext;
    sc->r8 = tf->r8;  sc->r9 = tf->r9;  sc->r10 = tf->r10; sc->r11 = tf->r11;
    sc->r12 = tf->r12; sc->r13 = tf->r13; sc->r14 = tf->r14; sc->r15 = tf->r15;
    sc->rdi = tf->rdi; sc->rsi = tf->rsi; sc->rbp = tf->rbp; sc->rbx = tf->rbx;
    sc->rdx = tf->rdx; sc->rcx = tf->rcx; sc->rsp = tf->rsp;
    sc->rax    = restart ? tf->rax : result;    /* restart: rax = syscall number */
    sc->rip    = restart ? (tf->rcx - 2) : tf->rcx;   /* back to the 2-byte SYSCALL */
    sc->eflags = tf->r11;
    sc->cs = 0x2b; sc->ss = 0x23;
    sc->oldmask = oldmask;
    sc->fpstate = 0;

    *(uint64_t *)f->uc.uc_sigmask = oldmask;
    f->pretcode = restorer;

    /* Enter the handler: handler(sig, &info, &uc) with a fresh, clean rflags. */
    tf->rcx = handler;                          /* user rip */
    tf->rsp = frame;
    tf->rdi = (uint64_t)sig;
    tf->rsi = frame + __builtin_offsetof(struct linux_rt_sigframe, info);
    tf->rdx = frame + __builtin_offsetof(struct linux_rt_sigframe, uc);
    tf->rax = 0;
    tf->r11 = 0x202;                            /* IF set, DF clear */
}

/* Restore the interrupted context from a ucontext copied out of the frame. The
 * restored rax is returned (rt_sigreturn returns it, so the syscall return path
 * puts it back in tf). The blocked mask comes from uc_sigmask. */
static inline uint64_t
arch_sigframe_restore(struct trapframe *tf, const struct linux_ucontext *uc,
                      uint64_t *out_mask)
{
    const struct linux_sigcontext *sc = &uc->uc_mcontext;
    tf->r8 = sc->r8;  tf->r9 = sc->r9;  tf->r10 = sc->r10;
    tf->r12 = sc->r12; tf->r13 = sc->r13; tf->r14 = sc->r14; tf->r15 = sc->r15;
    tf->rdi = sc->rdi; tf->rsi = sc->rsi; tf->rbp = sc->rbp; tf->rbx = sc->rbx;
    tf->rdx = sc->rdx; tf->rsp = sc->rsp;
    tf->rcx = sc->rip;                          /* user rip */
    tf->rax = sc->rax;
    tf->r11 = (sc->eflags & 0x0cd5u) | 0x202u;  /* arithmetic/DF flags; force IF */
    *out_mask = *(const uint64_t *)uc->uc_sigmask;
    return sc->rax;
}

#endif /* X86_64_LINUXABI_H */
