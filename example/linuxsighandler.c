/* linuxsighandler.c : exercise caught-signal delivery (handler invocation).
 *
 * The companion to linuxsignal.c, which covers the dispositions that need no
 * handler (terminate, ignore, mask). This one checks that an installed handler
 * actually runs, with its several modes: a plain handler, a SA_SIGINFO handler
 * that reads the siginfo, the alternate signal stack (SA_ONSTACK), the signal
 * masked inside its own handler, SA_RESETHAND one-shot disposition, and a
 * handler interrupting a blocked syscall as EINTR or restarting it (SA_RESTART).
 *
 * The scheduler is cooperative, so the EINTR/restart/pause cases put the handler
 * in a child that blocks while the parent sends the signal, the same shape
 * linuxsignal.c uses. An ordinary Linux binary. */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
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

/* --- shared handler observations --- */
static volatile sig_atomic_t ran;
static volatile int          seen_signo;
static volatile int          masked_in_handler;
static volatile uintptr_t    handler_sp;
static char                  altstack[16 * 1024];

static void
plain_handler(int sig)
{
    ran = sig;
    sigset_t cur;
    sigprocmask(SIG_BLOCK, NULL, &cur);     /* the signal is masked in its handler */
    masked_in_handler = sigismember(&cur, sig);
}

static void
info_handler(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    ran = sig;
    seen_signo = si->si_signo;
}

static void
onstack_handler(int sig)
{
    volatile char here;
    ran = sig;
    handler_sp = (uintptr_t)&here;          /* must land inside altstack[] */
}

int
main(void)
{
    struct sigaction sa;
    int st, sync[2], hold[2];
    char c;

    /* 1. A plain handler runs on raise(), and the signal is masked while it runs. */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = plain_handler;
    ran = 0;
    masked_in_handler = -1;
    check(sigaction(SIGUSR1, &sa, NULL) == 0, "install plain handler");
    raise(SIGUSR1);
    check(ran == SIGUSR1, "plain handler ran for SIGUSR1");
    check(masked_in_handler == 1, "signal is masked inside its own handler");

    /* 2. A SA_SIGINFO handler gets the siginfo (si_signo). */
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = info_handler;
    sa.sa_flags = SA_SIGINFO;
    ran = 0;
    seen_signo = 0;
    check(sigaction(SIGUSR2, &sa, NULL) == 0, "install SA_SIGINFO handler");
    raise(SIGUSR2);
    check(ran == SIGUSR2, "SA_SIGINFO handler ran");
    check(seen_signo == SIGUSR2, "siginfo si_signo is correct");

    /* 3. SA_ONSTACK runs the handler on the alternate stack. */
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack, .ss_flags = 0 };
    check(sigaltstack(&ss, NULL) == 0, "install sigaltstack");
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = onstack_handler;
    sa.sa_flags = SA_ONSTACK;
    ran = 0;
    handler_sp = 0;
    sigaction(SIGUSR1, &sa, NULL);
    raise(SIGUSR1);
    check(ran == SIGUSR1, "SA_ONSTACK handler ran");
    check(handler_sp >= (uintptr_t)altstack &&
          handler_sp < (uintptr_t)altstack + sizeof altstack,
          "handler ran on the alternate stack");

    /* 4. SA_RESETHAND resets the disposition to default after one delivery. */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = plain_handler;
    sa.sa_flags = SA_RESETHAND;
    ran = 0;
    sigaction(SIGUSR2, &sa, NULL);
    raise(SIGUSR2);
    check(ran == SIGUSR2, "SA_RESETHAND handler ran once");
    struct sigaction after;
    sigaction(SIGUSR2, NULL, &after);
    check(after.sa_handler == SIG_DFL, "disposition reset to SIG_DFL after delivery");

    /* 5. A handler interrupts a blocked read as EINTR (no SA_RESTART). The child
     * blocks in read(); the parent signals; read returns -1/EINTR after the
     * handler. The child encodes the outcome in its exit status. */
    if (pipe(sync) || pipe(hold))
        return 2;
    pid_t p = fork();
    if (p == 0) {
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = plain_handler;      /* no SA_RESTART */
        sigaction(SIGUSR1, &sa, NULL);
        ran = 0;
        write(sync[1], "r", 1);
        errno = 0;
        ssize_t n = read(hold[0], &c, 1);   /* only the signal ends this */
        _exit((n == -1 && errno == EINTR && ran == SIGUSR1) ? 11 : 1);
    }
    read(sync[0], &c, 1);                    /* the child is blocked in read */
    kill(p, SIGUSR1);
    check(waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 11,
          "handler interrupts a blocked read with EINTR");
    close(sync[0]); close(sync[1]); close(hold[0]); close(hold[1]);

    /* 6. SA_RESTART restarts the interrupted read instead of failing. The parent
     * signals, then writes the byte; the restarted read returns it. */
    if (pipe(sync) || pipe(hold))
        return 2;
    p = fork();
    if (p == 0) {
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = plain_handler;
        sa.sa_flags = SA_RESTART;
        sigaction(SIGUSR1, &sa, NULL);
        ran = 0;
        write(sync[1], "r", 1);
        ssize_t n = read(hold[0], &c, 1);   /* restarts after the handler */
        _exit((n == 1 && c == 'X' && ran == SIGUSR1) ? 12 : 1);
    }
    read(sync[0], &c, 1);
    kill(p, SIGUSR1);                        /* interrupts the read */
    write(hold[1], "X", 1);                  /* the restarted read finds this */
    check(waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 12,
          "SA_RESTART restarts the interrupted read");
    close(sync[0]); close(sync[1]); close(hold[0]); close(hold[1]);

    /* 7. pause() returns EINTR once a handler has run. */
    if (pipe(sync))
        return 2;
    p = fork();
    if (p == 0) {
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = plain_handler;
        sigaction(SIGUSR1, &sa, NULL);
        ran = 0;
        write(sync[1], "r", 1);
        errno = 0;
        int r = pause();                     /* returns only via a caught signal */
        _exit((r == -1 && errno == EINTR && ran == SIGUSR1) ? 13 : 1);
    }
    read(sync[0], &c, 1);                    /* the child is parked in pause */
    kill(p, SIGUSR1);
    check(waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 13,
          "pause returns EINTR after the handler");
    close(sync[0]); close(sync[1]);

    puts2(fails ? "linuxsighandler: FAILED\n" : "linuxsighandler: all ok\n");
    return fails ? 1 : 0;
}
