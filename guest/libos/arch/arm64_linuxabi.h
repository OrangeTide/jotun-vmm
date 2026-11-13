/* arm64_linuxabi.h : the AArch64 backend for the Linux-compat guest.
 *
 * The AArch64 counterpart of x86_64_linuxabi.h. The syscall numbers are the
 * generic (asm-generic) ABI Linux uses on arm64, which has no open/stat/pipe/
 * dup2/fork (userspace uses openat/fstatat/pipe2/dup3/clone). The dispatcher is
 * shared, so those x86-only names still appear as case labels; here they are
 * distinct negative sentinels that never match a real call number. */
#ifndef ARM64_LINUXABI_H
#define ARM64_LINUXABI_H

#include <stdint.h>

/* AArch64 (asm-generic) syscall numbers the dispatcher handles. */
#define SYS_getcwd          17
#define SYS_chdir           49
#define SYS_fchdir          50
#define SYS_dup             23
#define SYS_dup3            24
#define SYS_fcntl           25
#define SYS_mkdirat         34
#define SYS_unlinkat        35
#define SYS_symlinkat       36
#define SYS_linkat          37
#define SYS_renameat        38
#define SYS_renameat2       276
#define SYS_copy_file_range 285
#define SYS_fchmod          52
#define SYS_fchmodat        53
#define SYS_ioctl           29
#define SYS_statfs          43
#define SYS_fstatfs         44
#define SYS_sigaltstack     132
#define SYS_setregid        143
#define SYS_setgid          144
#define SYS_setreuid        145
#define SYS_setuid          146
#define SYS_setresuid       147
#define SYS_setresgid       149
#define SYS_setpgid         154
#define SYS_setsid          157
#define SYS_getgroups       158
#define SYS_setgroups       159
#define SYS_mincore         232
#define SYS_faccessat       48
#define SYS_openat          56
#define SYS_close           57
#define SYS_pipe2           59
#define SYS_getdents64      61
#define SYS_lseek           62
#define SYS_read            63
#define SYS_pread64         67
#define SYS_pwrite64        68
#define SYS_pkey_mprotect   288
#define SYS_pkey_alloc      289
#define SYS_pkey_free       290
#define SYS_eventfd2        19
#define SYS_capget          90
#define SYS_clock_getres    114
#define SYS_io_uring_setup  425
#define SYS_write           64
#define SYS_writev          66
#define SYS_readlinkat      78
#define SYS_newfstatat      79
#define SYS_fstat           80
#define SYS_utimensat       88
#define SYS_exit            93
#define SYS_exit_group      94
#define SYS_set_tid_address 96
#define SYS_futex           98
#define SYS_set_robust_list 99
#define SYS_nanosleep       101
#define SYS_getitimer       102
#define SYS_setitimer       103
#define SYS_clock_nanosleep 115
#define SYS_sched_getaffinity 123
#define SYS_clock_gettime   113
#define SYS_sched_yield     124
#define SYS_kill            129
#define SYS_tkill           130
#define SYS_tgkill          131
#define SYS_rt_sigaction    134
#define SYS_rt_sigprocmask  135
#define SYS_rt_sigsuspend   133
#define SYS_rt_sigreturn    139
#define SYS_uname           160
#define SYS_getrusage       165
#define SYS_umask           166
#define SYS_prctl           167
#define SYS_gettimeofday    169
#define SYS_getpid          172
#define SYS_getppid         173
#define SYS_getuid          174
#define SYS_geteuid         175
#define SYS_getgid          176
#define SYS_getegid         177
#define SYS_gettid          178
#define SYS_sysinfo         179
#define SYS_socket          198
#define SYS_bind            200
#define SYS_listen          201
#define SYS_accept          202
#define SYS_connect         203
#define SYS_getsockname     204
#define SYS_getpeername     205
#define SYS_sendto          206
#define SYS_recvfrom        207
#define SYS_setsockopt      208
#define SYS_getsockopt      209
#define SYS_shutdown        210
#define SYS_accept4         242
#define SYS_setxattr        5
#define SYS_lsetxattr       6
#define SYS_fsetxattr       7
#define SYS_getxattr        8
#define SYS_lgetxattr       9
#define SYS_fgetxattr       10
#define SYS_brk             214
#define SYS_munmap          215
#define SYS_mremap          216
#define SYS_clone           220
#define SYS_fadvise64       223
#define SYS_execve          221
#define SYS_mmap            222
#define SYS_mprotect        226
#define SYS_madvise         233
#define SYS_wait4           260
#define SYS_prlimit64       261
#define SYS_getrandom       278
#define SYS_statx           291
#define SYS_rseq            293
#define SYS_clone3          435
#define SYS_close_range     436
#define SYS_faccessat2      439

/* Not present on AArch64: distinct sentinels so the shared cases never match a
 * real (non-negative) call number. */
#define SYS_open       (-1)
#define SYS_stat       (-2)
#define SYS_lstat      (-3)
#define SYS_pipe       (-4)
#define SYS_dup2       (-5)
#define SYS_fork       (-6)
#define SYS_vfork      (-7)
#define SYS_arch_prctl (-8)
#define SYS_time       (-9)
#define SYS_access     (-10)
#define SYS_getpgrp    (-11)
#define SYS_readlink   (-12)
#define SYS_unlink     (-13)
#define SYS_chmod      (-14)
#define SYS_mkdir      (-15)
#define SYS_rmdir      (-16)
#define SYS_rename     (-17)
#define SYS_link       (-18)
#define SYS_symlink    (-19)
#define SYS_poll       (-20)       /* AArch64 has ppoll (73), not poll */
#define SYS_pause      (-21)       /* AArch64 has ppoll, not pause */
#define SYS_epoll_create (-22)     /* AArch64 has epoll_create1 (20), not this */
#define SYS_epoll_wait (-23)       /* AArch64 has epoll_pwait (22), not this */
#define SYS_eventfd    (-24)       /* AArch64 has eventfd2 (19), not legacy eventfd */
#define SYS_ppoll           73     /* musl's poll() and pause() both use this */
#define SYS_splice          76
#define SYS_sendfile        71
#define SYS_epoll_create1   20
#define SYS_epoll_ctl       21
#define SYS_epoll_pwait     22

/* arch_prctl is x86-only; defined so the shared case body compiles. */
#define ARCH_SET_FS 0x1002

/* The user register set the SVC trap saves and restores. x[0] is the syscall
 * return value and x[8] the call number; sp/pc/pstate/tpidr are the banked
 * user state (SP_EL0, ELR_EL1, SPSR_EL1, TPIDR_EL0). The offsets are mirrored
 * in arch/arm64_linuxsys.S. */
struct trapframe {
    uint64_t x[31];     /* x0..x30; x[0]=0, x[8]=64, x[30]=240 */
    uint64_t sp;        /* 248: user stack (SP_EL0) */
    uint64_t pc;        /* 256: ELR_EL1 */
    uint64_t pstate;    /* 264: SPSR_EL1 */
    uint64_t tpidr;     /* 272: TPIDR_EL0 (thread pointer) */
};
_Static_assert(sizeof(struct trapframe) == 280, "trapframe size feeds TF_KSTACK");

/* After fork or a thread clone the child resumes as if the syscall returned 0. */
static inline void
tf_set_ret0(struct trapframe *tf)
{
    tf->x[0] = 0;
}

/* A thread's entry stack pointer (SP_EL0). */
static inline void
tf_set_sp(struct trapframe *tf, uint64_t sp)
{
    tf->sp = sp;
}

/* The interrupted user stack pointer (where a signal frame is placed). */
static inline uint64_t
tf_user_sp(const struct trapframe *tf)
{
    return tf->sp;
}

/* The interrupted user instruction pointer (for fault diagnostics): ELR_EL1. */
static inline uint64_t
tf_pc(const struct trapframe *tf)
{
    return tf->pc;
}

/* The syscall/return value register (x0), preserved when a fault is caught. */
static inline uint64_t
tf_retval(const struct trapframe *tf)
{
    return tf->x[0];
}

/* A new thread's thread pointer. On AArch64 it rides in the trapframe (the SVC
 * restore sets TPIDR_EL0); on x86 the TLS lives elsewhere and this is a no-op. */
static inline void
tf_set_tls(struct trapframe *tf, uint64_t tls)
{
    tf->tpidr = tls;
}

/* The 6th syscall argument (mmap's offset), which the dispatcher's register
 * set stops short of. On AArch64 syscall arg 6 is x5. */
static inline uint64_t
tf_arg6(struct trapframe *tf)
{
    return tf->x[5];
}

/* clone() passes the new TLS and the child-tid pointer in different argument
 * slots on different architectures. AArch64 is a CLONE_BACKWARDS architecture
 * and uses the order clone(flags, stack, ptid, tls, ctid), so the fourth
 * argument is the TLS and the fifth is the child-tid pointer. (x86-64 swaps the
 * two.) This normalizes the fourth and fifth syscall arguments for the
 * dispatcher. */
static inline void
clone_split_tls_ctid(uint64_t a4, uint64_t a5, uint64_t *tls, uint64_t *ctid)
{
    *tls = a4;
    *ctid = a5;
}

/* Load the thread pointer into TPIDR_EL0. The SVC restore also sets it from the
 * trapframe, so this mainly keeps the register sane while kernel code runs. */
static inline void
arch_load_tls(uint64_t v)
{
    __asm__ volatile ("msr tpidr_el0, %0" : : "r"(v) : "memory");
}

/* A free-running counter for coarse time and random seeds. Not a real clock. */
static inline uint64_t
arch_cycles(void)
{
    uint64_t v;
    __asm__ volatile ("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

/* linux_jump enters a freshly loaded image; linux_resume continues a process
 * from its trapframe (arch/arm64_linuxsys.S). The exception vectors are
 * installed by arch_traps_init at boot, so the SVC trap needs no per-run arming. */
extern void linux_jump(uint64_t entry, uint64_t user_sp) __attribute__((noreturn));

static inline void
linux_trap_init(void)
{
}

/****************************************************************
 * Signal-handler delivery (the AArch64 rt_sigframe ABI).
 *
 * The AArch64 counterpart of the x86-64 frame in x86_64_linuxabi.h. The layout
 * follows the arm64 kernel: the frame is siginfo then ucontext, and the machine
 * state (uc_mcontext) is last in the ucontext. AArch64 has no SA_RESTORER; the
 * kernel points the return address (x30) at a sigreturn trampoline in the VDSO.
 * There is no VDSO here, so a two-instruction trampoline is written into the
 * frame itself (the user stack is executable) and x30 points at it. The FP state
 * is not saved, for the reason noted in the x86-64 header; a null terminator in
 * __reserved stops the kernel FP walk.
 *
 * This path is cross-compiled and structurally complete, but the arm64 guest is
 * not exercised at runtime yet.
 ****************************************************************/

struct linux_sigcontext {
    uint64_t fault_address;
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
    uint8_t  __reserved[16] __attribute__((aligned(16)));    /* null FP terminator */
};

struct linux_siginfo {
    int32_t si_signo, si_errno, si_code;
    int32_t __pad0;
    int32_t si_pid, si_uid;
    uint8_t __pad[128 - 24];
};
_Static_assert(sizeof(struct linux_siginfo) == 128, "siginfo ABI");

struct linux_ucontext {
    uint64_t uc_flags;
    uint64_t uc_link;
    struct { uint64_t ss_sp; uint32_t ss_flags; uint32_t __pad; uint64_t ss_size; } uc_stack;
    uint8_t  uc_sigmask[128];       /* sigset_t; low 8 bytes used, rest __unused */
    struct linux_sigcontext uc_mcontext;    /* must be last */
};

struct linux_rt_sigframe {
    struct linux_siginfo info;
    struct linux_ucontext uc;
    uint32_t tramp[2];              /* mov x8,#__NR_rt_sigreturn ; svc #0 */
};

static inline uint64_t
arch_sigframe_size(void)
{
    return sizeof(struct linux_rt_sigframe);
}

static inline uint64_t
arch_sigframe_place(uint64_t sp, int onstack)
{
    (void)onstack;                  /* no red zone on AArch64 */
    sp -= sizeof(struct linux_rt_sigframe);
    sp &= ~(uint64_t)15;            /* sp must be 16-aligned */
    return sp;
}

static inline uint64_t
arch_sigframe_ucaddr(const struct trapframe *tf)
{
    return tf->sp + __builtin_offsetof(struct linux_rt_sigframe, uc);
}

static inline void
arch_sigframe_build(uint64_t frame, struct trapframe *tf, int sig,
                    uint64_t handler, uint64_t flags, uint64_t restorer,
                    uint64_t result, int restart, uint64_t oldmask,
                    uint64_t altsp, uint64_t altsize,
                    int si_code, int si_pid, int si_uid, uint64_t si_addr)
{
    (void)flags;
    (void)restorer;                 /* AArch64 ignores sa_restorer */
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
    for (int i = 0; i < 31; i++)
        sc->regs[i] = tf->x[i];
    sc->regs[0] = restart ? tf->x[0] : result;  /* restart: x0 = original arg0 */
    sc->sp      = tf->sp;
    sc->pc      = restart ? (tf->pc - 4) : tf->pc;      /* back to the 4-byte SVC */
    sc->pstate  = tf->pstate;
    *(uint64_t *)f->uc.uc_sigmask = oldmask;

    /* The trampoline: MOVZ x8, #139 ; SVC #0. */
    f->tramp[0] = 0xd2801168u;      /* mov x8, #0x8b (139 = __NR_rt_sigreturn) */
    f->tramp[1] = 0xd4000001u;      /* svc #0 */
    /* These two words are code the handler returns through, just written as data.
     * On a core whose caches are not coherent the instruction cache would fetch
     * stale lines for this stack address, so clean the writes to the point of
     * unification and drop any stale instruction-cache lines for them. This runs
     * with the process's own address space active, so the trampoline VA resolves. */
    __asm__ volatile ("dc cvau, %0; dsb ish; ic ivau, %0; dsb ish; isb"
                      : : "r"(&f->tramp[0]) : "memory");

    tf->pc = handler;
    tf->sp = frame;
    tf->x[0] = (uint64_t)sig;
    tf->x[1] = frame + __builtin_offsetof(struct linux_rt_sigframe, info);
    tf->x[2] = frame + __builtin_offsetof(struct linux_rt_sigframe, uc);
    tf->x[30] = frame + __builtin_offsetof(struct linux_rt_sigframe, tramp);
    tf->pstate = 0x304;             /* EL1t, IRQ and FIQ unmasked: a handler is preemptible too */
}

static inline uint64_t
arch_sigframe_restore(struct trapframe *tf, const struct linux_ucontext *uc,
                      uint64_t *out_mask)
{
    const struct linux_sigcontext *sc = &uc->uc_mcontext;
    for (int i = 0; i < 31; i++)
        tf->x[i] = sc->regs[i];
    tf->sp = sc->sp;
    tf->pc = sc->pc;
    tf->pstate = (sc->pstate & ~(uint64_t)0xf) | 0x4;   /* force EL1t (the app's mode) */
    *out_mask = *(const uint64_t *)uc->uc_sigmask;
    return sc->regs[0];
}

#endif /* ARM64_LINUXABI_H */
