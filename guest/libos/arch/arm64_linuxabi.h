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
#define SYS_dup             23
#define SYS_dup3            24
#define SYS_fcntl           25
#define SYS_ioctl           29
#define SYS_openat          56
#define SYS_close           57
#define SYS_pipe2           59
#define SYS_getdents64      61
#define SYS_lseek           62
#define SYS_read            63
#define SYS_write           64
#define SYS_writev          66
#define SYS_readlinkat      78
#define SYS_newfstatat      79
#define SYS_fstat           80
#define SYS_exit            93
#define SYS_exit_group      94
#define SYS_set_tid_address 96
#define SYS_futex           98
#define SYS_set_robust_list 99
#define SYS_clock_gettime   113
#define SYS_sched_yield     124
#define SYS_tgkill          131
#define SYS_rt_sigaction    134
#define SYS_rt_sigprocmask  135
#define SYS_uname           160
#define SYS_prctl           167
#define SYS_getpid          172
#define SYS_getppid         173
#define SYS_getuid          174
#define SYS_geteuid         175
#define SYS_getgid          176
#define SYS_getegid         177
#define SYS_gettid          178
#define SYS_brk             214
#define SYS_munmap          215
#define SYS_clone           220
#define SYS_execve          221
#define SYS_mmap            222
#define SYS_mprotect        226
#define SYS_madvise         233
#define SYS_wait4           260
#define SYS_prlimit64       261
#define SYS_getrandom       278
#define SYS_statx           291
#define SYS_rseq            293

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

/* A new thread's thread pointer. On AArch64 it rides in the trapframe (the SVC
 * restore sets TPIDR_EL0); on x86 the TLS lives elsewhere and this is a no-op. */
static inline void
tf_set_tls(struct trapframe *tf, uint64_t tls)
{
    tf->tpidr = tls;
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

#endif /* ARM64_LINUXABI_H */
