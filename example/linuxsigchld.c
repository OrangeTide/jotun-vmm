/* Regression probe for the background-job reap path a shell uses: block SIGCHLD,
 * poll with waitpid(WNOHANG) while the child still runs, then sigsuspend with
 * SIGCHLD unblocked until the child exits and the handler fires. Exercises
 * WNOHANG, SIGCHLD delivery on child exit, and rt_sigsuspend together. Prints
 * "sigchld ok" when the child is reaped after the suspend. */
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

static volatile sig_atomic_t got;

static void
on_chld(int s)
{
    (void)s;
    got = 1;
}

int
main(void)
{
    struct sigaction sa = { 0 };
    sa.sa_handler = on_chld;
    sigaction(SIGCHLD, &sa, 0);

    sigset_t chld, old;
    sigemptyset(&chld);
    sigaddset(&chld, SIGCHLD);
    sigprocmask(SIG_BLOCK, &chld, &old);   /* block SIGCHLD; old leaves it open */

    pid_t c = fork();
    if (c == 0) {
        struct timespec r = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&r, 0);                  /* child lives ~1s, then exits */
        _exit(0);
    }

    /* Poll first: the child is still running, so this must report "nothing yet"
     * (0) rather than block. Then suspend until SIGCHLD wakes the handler. */
    if (waitpid(-1, 0, WNOHANG) != 0) {
        printf("FAIL: WNOHANG did not return 0 while child ran\n");
        return 1;
    }
    while (!got)
        sigsuspend(&old);                  /* SIGCHLD unblocked here */

    int st = 0;
    pid_t r = waitpid(c, &st, 0);
    if (r == c && WIFEXITED(st) && WEXITSTATUS(st) == 0)
        printf("sigchld ok\n");
    else
        printf("FAIL: reap r=%d st=%d\n", (int)r, st);
    sigprocmask(SIG_SETMASK, &old, 0);
    fflush(stdout);
    return 0;
}
