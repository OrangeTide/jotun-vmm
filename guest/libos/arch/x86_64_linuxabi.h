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
#define SYS_lseek           8
#define SYS_mmap            9
#define SYS_mprotect        10
#define SYS_munmap          11
#define SYS_brk             12
#define SYS_rt_sigaction    13
#define SYS_rt_sigprocmask  14
#define SYS_ioctl           16
#define SYS_writev          20
#define SYS_pipe            22
#define SYS_sched_yield     24
#define SYS_madvise         28
#define SYS_dup             32
#define SYS_dup2            33
#define SYS_getpid          39
#define SYS_clone           56
#define SYS_fork            57
#define SYS_vfork           58
#define SYS_execve          59
#define SYS_exit            60
#define SYS_wait4           61
#define SYS_uname           63
#define SYS_fcntl           72
#define SYS_getcwd          79
#define SYS_getuid          102
#define SYS_getgid          104
#define SYS_geteuid         107
#define SYS_getegid         108
#define SYS_getppid         110
#define SYS_prctl           157
#define SYS_arch_prctl      158
#define SYS_gettid          186
#define SYS_time            201
#define SYS_futex           202
#define SYS_getdents64      217
#define SYS_set_tid_address 218
#define SYS_clock_gettime   228
#define SYS_exit_group      231
#define SYS_tgkill          234
#define SYS_openat          257
#define SYS_newfstatat      262
#define SYS_readlinkat      267
#define SYS_set_robust_list 273
#define SYS_dup3            292
#define SYS_pipe2           293
#define SYS_prlimit64       302
#define SYS_getrandom       318
#define SYS_statx           332
#define SYS_rseq            334

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
 * set the kernel selector in STAR, and mask IF and DF on entry. */
static inline void
linux_trap_init(void)
{
    wrmsr(MSR_EFER, read_efer() | 1u);          /* EFER.SCE */
    wrmsr(MSR_STAR, (uint64_t)0x08u << 32);
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)linux_syscall_entry);
    wrmsr(MSR_SFMASK, 0x600u);                  /* mask IF and DF on entry */
}

#endif /* X86_64_LINUXABI_H */
