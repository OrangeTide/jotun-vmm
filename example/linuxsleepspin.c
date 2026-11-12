/* Regression probe: a parent timed-sleep must complete while a sibling busy
 * yields in ring 0. Mirrors a backgrounded server spinning in epoll_wait while
 * a foreground shell runs `sleep`. Prints "sleepspin ok" when the parent wakes. */
#include <sched.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

int
main(void)
{
    pid_t c = fork();
    if (c == 0) {
        for (;;)
            sched_yield();           /* busy sibling, stays runnable in ring 0 */
    }
    struct timespec req = { .tv_sec = 2, .tv_nsec = 0 };
    nanosleep(&req, NULL);           /* must return though the sibling spins */
    printf("sleepspin ok\n");
    fflush(stdout);
    kill(c, SIGKILL);
    waitpid(c, 0, 0);
    return 0;
}
