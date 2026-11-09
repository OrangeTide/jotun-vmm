/* linuxsignal.c : a static Linux test for signal generation and the default
 * terminate action (stages A to C: disposition and mask state, kill/tkill, and
 * killable processes). Handler invocation is a later stage, so this checks the
 * behaviors that do not need a handler: a kill terminates a process blocked in a
 * syscall (reported as WIFSIGNALED), abort self-terminates, SIGKILL ignores the
 * mask, a default-ignore signal does not kill, and a blocked signal is held off.
 *
 * The scheduler is cooperative, so a child must block (not spin) for the parent
 * to run and send the signal. Each child parks in read() on a "hold" pipe and
 * announces readiness through a "sync" pipe. Run with
 * `make linux-spike SPIKE_PROG=linuxsignal`. An ordinary Linux binary. */
#include <errno.h>
#include <signal.h>
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

int
main(void)
{
    int st, sync[2], hold[2];
    char c;

    /* SIGTERM terminates a child blocked in a syscall; wait reports the signal. */
    if (pipe(sync) || pipe(hold))
        return 2;
    pid_t p = fork();
    if (p == 0) {
        write(sync[1], "r", 1);
        read(hold[0], &c, 1);           /* blocks: only the signal ends this */
        _exit(0);
    }
    read(sync[0], &c, 1);               /* the child is now blocked in read */
    check(kill(p, SIGTERM) == 0, "kill(SIGTERM)");
    check(waitpid(p, &st, 0) == p && WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM,
          "blocked child terminated by SIGTERM");
    close(sync[0]); close(sync[1]); close(hold[0]); close(hold[1]);

    /* abort() raises SIGABRT at itself and terminates. */
    p = fork();
    if (p == 0)
        abort();
    check(waitpid(p, &st, 0) == p && WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT,
          "abort terminates with SIGABRT");

    /* SIGKILL kills even a child that has blocked every signal. */
    if (pipe(sync) || pipe(hold))
        return 2;
    p = fork();
    if (p == 0) {
        sigset_t all;
        sigfillset(&all);
        sigprocmask(SIG_BLOCK, &all, NULL);
        write(sync[1], "r", 1);
        read(hold[0], &c, 1);
        _exit(0);
    }
    read(sync[0], &c, 1);               /* the child has blocked all signals */
    check(kill(p, SIGKILL) == 0, "kill(SIGKILL) against a full block");
    check(waitpid(p, &st, 0) == p && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL,
          "SIGKILL ignores the signal mask");
    close(sync[0]); close(sync[1]); close(hold[0]); close(hold[1]);

    /* A default-ignore signal (SIGURG) does not kill; the child exits normally. */
    if (pipe(sync) || pipe(hold))
        return 2;
    p = fork();
    if (p == 0) {
        write(sync[1], "r", 1);
        read(hold[0], &c, 1);           /* woken by SIGURG, then by the "go" byte */
        _exit(5);
    }
    read(sync[0], &c, 1);
    check(kill(p, SIGURG) == 0, "kill(SIGURG)");
    write(hold[1], "g", 1);             /* let the child's read complete */
    check(waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 5,
          "SIGURG is ignored, child exits normally");
    close(sync[0]); close(sync[1]); close(hold[0]); close(hold[1]);

    /* A blocked SIGTERM is held off: the child survives it and exits normally. */
    if (pipe(sync) || pipe(hold))
        return 2;
    p = fork();
    if (p == 0) {
        sigset_t s;
        sigemptyset(&s);
        sigaddset(&s, SIGTERM);
        sigprocmask(SIG_BLOCK, &s, NULL);
        write(sync[1], "r", 1);
        read(hold[0], &c, 1);
        _exit(9);
    }
    read(sync[0], &c, 1);               /* the child has blocked SIGTERM */
    check(kill(p, SIGTERM) == 0, "kill(SIGTERM) while blocked");
    write(hold[1], "g", 1);
    check(waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 9,
          "blocked SIGTERM held off, child exits normally");
    close(sync[0]); close(sync[1]); close(hold[0]); close(hold[1]);

    /* The existence probe (signal 0). */
    check(kill(getpid(), 0) == 0, "kill(self, 0) succeeds");
    errno = 0;
    check(kill(999999, 0) == -1 && errno == ESRCH, "kill(absent, 0) is ESRCH");

    puts2(fails ? "linuxsignal: FAILED\n" : "linuxsignal: all ok\n");
    return fails ? 1 : 0;
}
