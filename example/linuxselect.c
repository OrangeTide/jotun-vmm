/* linuxselect.c : exercise select(2) and pselect(2) against pipe endpoints.
 * Covers an empty zero-timeout poll, readable after a write, writable while the
 * pipe has room, a drained read end, a short pselect timeout with nothing
 * ready, and the ready count across both sets at once. On glibc and musl,
 * select() is the x86-64 select syscall and the arm64 pselect6, and pselect()
 * is pselect6 on both arches, so this covers both syscalls. GNU make's -j
 * jobserver waits on its pipe through pselect6, so this is the path a parallel
 * build depends on. Prints "select ok" on success. */
#include <sys/select.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

static int fails;

static void
check(int cond, const char *msg)
{
    if (!cond) {
        write(2, "select FAIL: ", 13);
        write(2, msg, strlen(msg));
        write(2, "\n", 1);
        fails++;
    }
}

int
main(void)
{
    int p[2];
    check(pipe(p) == 0, "pipe");

    fd_set r, w;
    struct timeval z = { 0, 0 };

    /* Nothing written: a zero-timeout select on the read end reports nothing,
     * and clears the set. */
    FD_ZERO(&r);
    FD_SET(p[0], &r);
    int n = select(p[0] + 1, &r, NULL, NULL, &z);
    check(n == 0, "empty read select");
    check(!FD_ISSET(p[0], &r), "empty clears set");

    /* After a write, the read end is readable. */
    check(write(p[1], "x", 1) == 1, "write");
    FD_ZERO(&r);
    FD_SET(p[0], &r);
    n = select(p[0] + 1, &r, NULL, NULL, &z);
    check(n == 1, "read ready count");
    check(FD_ISSET(p[0], &r), "read set marked");

    /* The write end is writable while the pipe has room. */
    FD_ZERO(&w);
    FD_SET(p[1], &w);
    n = select(p[1] + 1, NULL, &w, NULL, &z);
    check(n == 1, "write ready count");
    check(FD_ISSET(p[1], &w), "write set marked");

    /* Drain; the read end is no longer ready. */
    char c;
    check(read(p[0], &c, 1) == 1, "read");
    FD_ZERO(&r);
    FD_SET(p[0], &r);
    n = select(p[0] + 1, &r, NULL, NULL, &z);
    check(n == 0, "drained read select");

    /* pselect with a short timeout and nothing ready returns 0. */
    FD_ZERO(&r);
    FD_SET(p[0], &r);
    struct timespec ts = { 0, 20 * 1000 * 1000 };       /* 20 ms */
    n = pselect(p[0] + 1, &r, NULL, NULL, &ts, NULL);
    check(n == 0, "pselect timeout");

    /* pselect sees a pending write as readable (zero timeout, after writing). */
    check(write(p[1], "y", 1) == 1, "write2");
    struct timespec zt = { 0, 0 };
    FD_ZERO(&r);
    FD_SET(p[0], &r);
    n = pselect(p[0] + 1, &r, NULL, NULL, &zt, NULL);
    check(n == 1 && FD_ISSET(p[0], &r), "pselect read ready");

    /* Read and write ready at once: the count is two, one per set. */
    FD_ZERO(&r);
    FD_ZERO(&w);
    FD_SET(p[0], &r);
    FD_SET(p[1], &w);
    int hi = p[0] > p[1] ? p[0] : p[1];
    n = pselect(hi + 1, &r, &w, NULL, &zt, NULL);
    check(n == 2, "both ready count");
    check(FD_ISSET(p[0], &r) && FD_ISSET(p[1], &w), "both sets marked");

    close(p[0]);
    close(p[1]);

    if (fails == 0)
        write(1, "select ok\n", 10);
    return fails;
}
