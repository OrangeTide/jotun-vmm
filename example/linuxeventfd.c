/* linuxeventfd.c : exercise the eventfd counter semantics libuv's loop relies
 * on. Covers a non-blocking read on an empty counter (EAGAIN), accumulating
 * writes, a read that returns the sum and clears it, poll readiness tracking the
 * counter, and semaphore mode (a read returns 1 and decrements). An ordinary
 * Linux binary; prints "eventfd ok" on success. */
#include <sys/eventfd.h>
#include <poll.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

static int fails;

static void
check(int cond, const char *msg)
{
    if (!cond) {
        write(2, "eventfd FAIL: ", 14);
        write(2, msg, strlen(msg));
        write(2, "\n", 1);
        fails++;
    }
}

int
main(void)
{
    int fd = eventfd(0, EFD_NONBLOCK);
    check(fd >= 0, "eventfd");

    uint64_t v;

    /* Empty and non-blocking: a read reports EAGAIN. */
    check(read(fd, &v, sizeof v) < 0 && errno == EAGAIN, "empty read EAGAIN");

    /* poll reports writable but not readable while the counter is zero. */
    struct pollfd pf = { .fd = fd, .events = POLLIN | POLLOUT };
    check(poll(&pf, 1, 0) == 1, "poll zero count");
    check((pf.revents & POLLOUT) && !(pf.revents & POLLIN), "zero: writable not readable");

    /* Two writes accumulate; one read returns the sum and clears the counter. */
    v = 2;
    check(write(fd, &v, sizeof v) == sizeof v, "write 2");
    v = 3;
    check(write(fd, &v, sizeof v) == sizeof v, "write 3");
    pf.revents = 0;
    check(poll(&pf, 1, 0) == 1 && (pf.revents & POLLIN), "readable after write");
    check(read(fd, &v, sizeof v) == sizeof v, "read sum");
    check(v == 5, "sum is 5");
    check(read(fd, &v, sizeof v) < 0 && errno == EAGAIN, "drained EAGAIN");

    close(fd);

    /* Semaphore mode: a read returns 1 and decrements, so a count of 2 reads
     * back as two single reads then blocks. */
    int sfd = eventfd(2, EFD_NONBLOCK | EFD_SEMAPHORE);
    check(sfd >= 0, "eventfd sem");
    check(read(sfd, &v, sizeof v) == sizeof v && v == 1, "sem read 1");
    check(read(sfd, &v, sizeof v) == sizeof v && v == 1, "sem read 1 again");
    check(read(sfd, &v, sizeof v) < 0 && errno == EAGAIN, "sem drained");
    close(sfd);

    if (fails == 0)
        write(1, "eventfd ok\n", 11);
    return fails;
}
