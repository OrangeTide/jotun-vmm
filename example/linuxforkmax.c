/* linuxforkmax.c : measure how many processes can be live at once.
 *
 * The autotools run surfaced "fork: Cannot allocate memory" under load. fork
 * fails when the libOS scheduler task table is full (TASK_MAX), not when the
 * 48 process slots or RAM run out: the task table is the tighter ceiling. This
 * test pins the limit down. It forks children that each block reading a pipe,
 * so every child stays live and holds a task slot, and counts how many forks
 * succeed before one returns ENOMEM. Then it closes the pipe so the children
 * see EOF and exit, and reaps them, so the count of forks and the count of
 * reaps agree and the run ends cleanly.
 *
 * The number printed is the live-process ceiling the guest currently allows
 * (TASK_MAX minus the init task and this parent). Raising TASK_MAX raises it,
 * which is the scaling fix. A workload can also stay under the ceiling by not
 * holding many children live at once (fork, wait, repeat). */
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>

static void
say(const char *s)
{
    write(1, s, strlen(s));
}

static void
say_num(const char *label, int n)
{
    char b[80];
    int l = 0;
    while (*label)
        b[l++] = *label++;
    char t[12];
    int ti = 0;
    if (n == 0)
        t[ti++] = '0';
    for (int m = n; m > 0; m /= 10)
        t[ti++] = (char)('0' + m % 10);
    while (ti > 0)
        b[l++] = t[--ti];
    b[l++] = '\n';
    write(1, b, (size_t)l);
}

int
main(void)
{
    int p[2];
    if (pipe(p) < 0) {
        say("forkmax: pipe failed\n");
        return 1;
    }

    int live = 0;
    for (;;) {
        pid_t pid = fork();
        if (pid < 0)
            break;                      /* ENOMEM: the task table is full */
        if (pid == 0) {
            close(p[1]);                /* child: drop the write end */
            char c;
            read(p[0], &c, 1);          /* block until the parent closes the pipe */
            _exit(0);
        }
        live++;
        if (live >= 200)
            break;                      /* safety cap; we never expect to reach it */
    }
    say_num("forkmax: live children = ", live);

    close(p[1]);                        /* every writer gone: children get EOF */
    close(p[0]);

    int reaped = 0;
    while (waitpid(-1, 0, 0) > 0)
        reaped++;
    say_num("forkmax: reaped = ", reaped);

    return (reaped == live) ? 0 : 1;
}
