/* linuxelf.c : run a static Linux x86-64 binary under a SYSCALL trap (M16.1).
 *
 * The spike: load a static, non-PIE musl ELF (ET_EXEC linked at 0x400000, the
 * same window the native programs use) with the existing elf_load_fid, build a
 * System V initial stack (argc/argv/envp/auxv), arm the SYSCALL machinery, and
 * jump in. A small dispatcher services the syscalls a musl "hello" issues:
 * arch_prctl(ARCH_SET_FS), set_tid_address, ioctl(TIOCGWINSZ), write/writev,
 * and exit/exit_group. Everything else returns -ENOSYS, which is enough to
 * learn where the real work is. The console is the guest's con_putc.
 *
 * x86-64 only. The SYSCALL trap and the kernel<->program handoff live in
 * arch/x86_64_linuxsys.S. On other targets linux_elf_run is a stub. */
#include "linuxelf.h"

#include "print.h"

#if defined(__x86_64__)

#include "elfload.h"
#include "ns.h"
#include "p9.h"
#include "jotun_svc.h"       /* JOTUN_PROG_BASE / JOTUN_PROG_SIZE */

#include <stdint.h>

/* A distinct 9P fid, so loading a Linux program never collides with the native
 * program loader's fid (prog.c uses 10). */
#define LINUX_FID 12u

/* Linux syscall numbers (x86-64) we recognise. */
#define SYS_write          1
#define SYS_ioctl          16
#define SYS_writev         20
#define SYS_exit           60
#define SYS_arch_prctl     158
#define SYS_set_tid_address 218
#define SYS_exit_group     231

#define ARCH_SET_FS 0x1002

/* A few errnos, returned as negatives in rax the way the kernel does. */
#define LERR_BADF   9
#define LERR_NOTTY  25
#define LERR_NOSYS  38

/* x86-64 MSRs for the SYSCALL path. */
#define MSR_EFER     0xC0000080u
#define MSR_STAR     0xC0000081u
#define MSR_LSTAR    0xC0000082u
#define MSR_SFMASK   0xC0000084u
#define MSR_FS_BASE  0xC0000100u

/* The handoff and trap, from x86_64_linuxsys.S. */
extern void linux_syscall_entry(void);
extern long linux_enter(uint64_t entry, uint64_t user_rsp);
extern void linux_exit(long code) __attribute__((noreturn));

/* ELF auxv entry tags. */
#define AT_NULL   0
#define AT_PAGESZ 6
#define AT_RANDOM 25

/* The program's stack. 128 KiB is ample for a hello and keeps us well clear of
 * the loaded image at 0x400000 and the vcpu stacks at the top of RAM. */
static uint8_t linux_ustack[128 * 1024] __attribute__((aligned(16)));

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

static inline void
sti(void)
{
    __asm__ volatile ("sti" ::: "memory");
}

/* Print a signed decimal, since con_printf's specifier set is not relied on. */
static void
put_dec(long v)
{
    char b[24];
    int n = 0;
    unsigned long u;

    if (v < 0) {
        con_putc('-');
        u = (unsigned long)(-v);
    } else {
        u = (unsigned long)v;
    }
    do {
        b[n++] = (char)('0' + (int)(u % 10));
        u /= 10;
    } while (u);
    while (n)
        con_putc(b[--n]);
}

/****************************************************************
 * Syscall servicing.
 ****************************************************************/

struct iovec {
    const void *base;
    unsigned long len;
};

static long
do_write(long fd, const char *buf, unsigned long len)
{
    if (fd != 1 && fd != 2)
        return -LERR_BADF;
    for (unsigned long i = 0; i < len; i++)
        con_putc(buf[i]);
    return (long)len;
}

static long
do_writev(long fd, const struct iovec *iov, long cnt)
{
    long total = 0;

    if (fd != 1 && fd != 2)
        return -LERR_BADF;
    for (long i = 0; i < cnt; i++) {
        long n = do_write(fd, iov[i].base, iov[i].len);
        if (n < 0)
            return n;
        total += n;
    }
    return total;
}

/* The C half of the SYSCALL handler. Return value goes back in rax. */
long
linux_sys_dispatch(long nr, long a1, long a2, long a3, long a4, long a5)
{
    (void)a4;
    (void)a5;
    switch (nr) {
    case SYS_arch_prctl:
        if (a1 == ARCH_SET_FS) {
            wrmsr(MSR_FS_BASE, (uint64_t)a2);
            return 0;
        }
        return -LERR_NOSYS;
    case SYS_set_tid_address:
        return 1;                       /* a plausible tid */
    case SYS_ioctl:
        return -LERR_NOTTY;             /* no tty: musl falls back */
    case SYS_write:
        return do_write(a1, (const char *)a2, (unsigned long)a3);
    case SYS_writev:
        return do_writev(a1, (const struct iovec *)a2, a3);
    case SYS_exit:
    case SYS_exit_group:
        linux_exit(a1);                 /* does not return */
    default:
        con_puts("[bin] unhandled syscall ");
        put_dec(nr);
        con_putc('\n');
        return -LERR_NOSYS;
    }
}

/****************************************************************
 * Stack and SYSCALL setup.
 ****************************************************************/

/* Build a System V initial stack in linux_ustack and return the rsp to enter
 * with: argc, argv[], NULL, envp[]=NULL, auxv, with the argv strings and 16
 * random bytes placed above it. argc is 1 (the program name). */
static uint64_t
build_stack(const char *argv0)
{
    uint8_t *top = linux_ustack + sizeof linux_ustack;

    /* Copy the program name string near the top. */
    char *name = (char *)(top - 64);
    int i = 0;
    while (argv0[i] && i < 48) {
        name[i] = argv0[i];
        i++;
    }
    name[i] = 0;

    /* 16 bytes for AT_RANDOM, seeded from the cycle counter. */
    uint8_t *rnd = (uint8_t *)(top - 96);
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t seed = ((uint64_t)hi << 32) | lo;
    for (int k = 0; k < 16; k++) {
        rnd[k] = (uint8_t)seed;
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    }

    /* The control block, 16-byte aligned, below the strings. Layout:
     *   argc, argv[0], NULL(argv end), NULL(envp end), auxv..., AT_NULL */
    uint64_t *blk = (uint64_t *)((uintptr_t)(top - 256) & ~(uintptr_t)0xF);
    int w = 0;
    blk[w++] = 1;                       /* argc */
    blk[w++] = (uintptr_t)name;         /* argv[0] */
    blk[w++] = 0;                       /* argv terminator */
    blk[w++] = 0;                       /* envp terminator (no env) */
    blk[w++] = AT_PAGESZ; blk[w++] = 4096;
    blk[w++] = AT_RANDOM; blk[w++] = (uintptr_t)rnd;
    blk[w++] = AT_NULL;   blk[w++] = 0;

    return (uint64_t)(uintptr_t)blk;
}

/* Turn on SYSCALL and point LSTAR at our handler. Our ring-0 CS is 0x08 and SS
 * is 0x10, so STAR's SYSCALL selector field is 0x08 (SYSCALL loads CS=0x08,
 * SS=0x10). SYSRET is unused (we return by jmp), so its field stays 0. */
static void
syscall_arm(void)
{
    wrmsr(MSR_EFER, read_efer() | 1u);          /* EFER.SCE */
    wrmsr(MSR_STAR, (uint64_t)0x08u << 32);
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)linux_syscall_entry);
    wrmsr(MSR_SFMASK, 0x600u);                  /* mask IF and DF on entry */
}

void
linux_elf_run(const char *file)
{
    con_puts("[bin] loading ");
    con_puts(file);
    con_putc('\n');

    if (ns_walk(file, LINUX_FID) != 0) {
        con_puts("[bin] not found in bundle\n");
        return;
    }
    if (p9_open(LINUX_FID) != 0) {
        p9_clunk(LINUX_FID);
        con_puts("[bin] open failed\n");
        return;
    }
    uint64_t entry = elf_load_fid(LINUX_FID, JOTUN_PROG_BASE,
                                  JOTUN_PROG_BASE + JOTUN_PROG_SIZE);
    p9_clunk(LINUX_FID);
    if (entry == 0) {
        con_puts("[bin] load failed (not ET_EXEC at 0x400000?)\n");
        return;
    }
    con_puts("[bin] loaded, entry=");
    con_puthex(entry);
    con_putc('\n');

    syscall_arm();
    con_puts("[bin] syscall armed\n");
    uint64_t rsp = build_stack(file);
    con_puts("[bin] stack built\n");

    con_puts("[bin] entering guest\n");
    long code = linux_enter(entry, rsp);
    sti();                              /* linux_enter ran with interrupts off */

    con_puts("[bin] exited, status=");
    put_dec(code);
    con_putc('\n');
}

#else  /* !__x86_64__ */

void
linux_elf_run(const char *file)
{
    (void)file;
    con_puts("[bin] the Linux binary-compat spike is x86-64 only\n");
}

#endif
