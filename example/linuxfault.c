/* linuxfault.c : a synchronous CPU fault reaches an installed handler.
 *
 * The companion to linuxsighandler.c (which covers sent signals). Here the guest
 * saves the faulting user context in the CPU fault stub and, when a handler is
 * installed for the fault's signal and it is not blocked, delivers to it: a
 * SIGSEGV handler sees the fault address in the siginfo and recovers with
 * siglongjmp, a SIGFPE handler catches a divide by zero, a SIGILL handler
 * catches an illegal instruction, and an uncaught fault still terminates the
 * process (reported WIFSIGNALED). An ordinary Linux binary. */
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void
puts2(const char *s)
{
    write(1, s, strlen(s));
}

static int fails;

static void
check(int ok, const char *what)
{
    puts2(ok ? "[ok]   " : "[FAIL] ");
    puts2(what);
    puts2("\n");
    if (!ok)
        fails++;
}

static sigjmp_buf     recover;
static volatile int   caught;
static volatile void *fault_addr;

/* An address outside the user window: the demand-pager refuses it, so touching
 * it faults instead of being mapped. */
#define BADADDR ((volatile long *)0x40000000000ULL)

static void
segv_handler(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    caught = sig;
    fault_addr = si->si_addr;
    siglongjmp(recover, 1);
}

static void
simple_handler(int sig)
{
    caught = sig;
    siglongjmp(recover, 1);
}

#if defined(__aarch64__)
/* Whether this core implements trapped floating-point exceptions. Integer divide
 * never traps on AArch64, so a divide can raise SIGFPE only through a trapping FP
 * divide, and the trap-enable bits are an optional feature (RES0 where absent, as
 * on Cortex-A72 and Apple cores). Set the divide-by-zero enable (FPCR.DZE) and see
 * if it reads back. */
static int
fp_divzero_traps(void)
{
    uint64_t v, back;
    __asm__ volatile ("mrs %0, fpcr" : "=r"(v));
    __asm__ volatile ("msr fpcr, %0; isb" : : "r"(v | (1u << 9)));
    __asm__ volatile ("mrs %0, fpcr" : "=r"(back));
    return (int)((back >> 9) & 1);
}
#endif

int
main(void)
{
    struct sigaction sa;

    /* 1. A SIGSEGV handler runs on a bad access and sees the fault address. */
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = segv_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    caught = 0;
    fault_addr = (void *)1;
    if (sigsetjmp(recover, 1) == 0) {
        volatile long v = *BADADDR;         /* faults: out-of-window read */
        (void)v;
        check(0, "SIGSEGV handler ran (no fault taken)");
    } else {
        check(caught == SIGSEGV, "SIGSEGV handler ran on an invalid access");
        check(fault_addr == (void *)BADADDR, "siginfo si_addr is the fault address");
    }

    /* 2. A SIGFPE handler catches a divide by zero. On x86 an integer divide by
     * zero traps (#DE). AArch64's integer divide never traps (sdiv returns 0), so
     * there a divide can raise SIGFPE only through a trapping floating-point
     * divide, and only where the core implements trapped FP exceptions. Where it
     * does not (Cortex-A72, Apple cores), confirm the architectural behavior
     * instead: the divide completes and no signal is raised. */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = simple_handler;
    sigaction(SIGFPE, &sa, NULL);
    caught = 0;
#if defined(__aarch64__)
    if (!fp_divzero_traps()) {
        volatile int a = 7, b = 0;
        volatile int q = a / b;             /* sdiv: returns 0, no trap */
        check(caught == 0 && q == 0, "divide by zero does not trap on this core");
    } else if (sigsetjmp(recover, 1) == 0) {
        volatile double a = 1.0, b = 0.0;
        volatile double q = a / b;          /* trapped FP divide by zero -> SIGFPE */
        (void)q;
        check(0, "SIGFPE handler ran (no fault taken)");
    } else {
        check(caught == SIGFPE, "SIGFPE handler ran on divide by zero");
    }
#else
    if (sigsetjmp(recover, 1) == 0) {
        /* Both operands are volatile so the compiler emits a real idiv rather
         * than folding a constant dividend into comparisons. */
        volatile int a = 7, b = 0;
        volatile int q = a / b;             /* #DE -> SIGFPE */
        (void)q;
        check(0, "SIGFPE handler ran (no fault taken)");
    } else {
        check(caught == SIGFPE, "SIGFPE handler ran on divide by zero");
    }
#endif

    /* 3. A SIGILL handler catches an illegal instruction. */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = simple_handler;
    sigaction(SIGILL, &sa, NULL);
    caught = 0;
    if (sigsetjmp(recover, 1) == 0) {
        __builtin_trap();                   /* UD2 -> SIGILL */
        check(0, "SIGILL handler ran (no fault taken)");
    } else {
        check(caught == SIGILL, "SIGILL handler ran on an illegal instruction");
    }

    /* 4. An uncaught fault still terminates the process (WIFSIGNALED). Reset the
     * disposition to default first (fork inherits it) so the child's fault is not
     * caught by the handler installed above. */
    signal(SIGSEGV, SIG_DFL);
    pid_t p = fork();
    if (p == 0) {
        volatile long v = *BADADDR;         /* default SIGSEGV: terminates */
        (void)v;
        _exit(0);                           /* not reached */
    }
    int st;
    check(waitpid(p, &st, 0) == p && WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV,
          "an uncaught fault terminates with SIGSEGV");

    puts2(fails ? "linuxfault: FAILED\n" : "linuxfault: all ok\n");
    return fails ? 1 : 0;
}
