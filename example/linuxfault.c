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

    /* 2. A SIGFPE handler catches an integer divide by zero. */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = simple_handler;
    sigaction(SIGFPE, &sa, NULL);
    caught = 0;
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
