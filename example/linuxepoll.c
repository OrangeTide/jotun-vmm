/* linuxepoll.c : exercise the epoll syscalls against pipe endpoints, the one
 * descriptor kind in the guest that actually blocks. Covers create/ctl/wait,
 * level-triggered readiness in both directions, EPOLL_CTL_MOD and _DEL, a
 * blocking wait woken by a peer-less self-write, a timeout with nothing ready,
 * and EPOLLHUP when the write end closes. Prints "epoll ok" on success. */
#include <sys/epoll.h>
#include <unistd.h>
#include <string.h>

static int fails;

static void
check(int cond, const char *msg)
{
    if (!cond) {
        write(2, "epoll FAIL: ", 12);
        write(2, msg, strlen(msg));
        write(2, "\n", 1);
        fails++;
    }
}

int
main(void)
{
    int ep = epoll_create1(0);
    check(ep >= 0, "epoll_create1");

    int p[2];
    check(pipe(p) == 0, "pipe");

    struct epoll_event ev;
    struct epoll_event out[4];

    /* Watch the read end for readable, with a recognizable cookie. */
    ev.events = EPOLLIN;
    ev.data.u64 = 0xABCDu;
    check(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0, "ctl add rd");

    /* Nothing written yet: a zero-timeout wait reports nothing. */
    check(epoll_wait(ep, out, 4, 0) == 0, "empty wait");

    /* After a write, the read end is readable and carries the cookie. */
    check(write(p[1], "x", 1) == 1, "write");
    int n = epoll_wait(ep, out, 4, 100);
    check(n == 1, "wait ready count");
    check((out[0].events & EPOLLIN) != 0, "wait EPOLLIN");
    check(out[0].data.u64 == 0xABCDu, "wait data cookie");

    /* Drain it; readiness goes away (level-triggered). */
    char c;
    check(read(p[0], &c, 1) == 1, "read");
    check(epoll_wait(ep, out, 4, 0) == 0, "drained wait");

    /* The write end is writable while the pipe has room. */
    ev.events = EPOLLOUT;
    ev.data.u64 = 0x1111u;
    check(epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &ev) == 0, "ctl add wr");
    n = epoll_wait(ep, out, 4, 0);
    check(n == 1, "wr-only ready");
    check(out[0].data.u64 == 0x1111u && (out[0].events & EPOLLOUT), "wr event");

    /* MOD the read end to a new cookie, then make it ready. */
    ev.events = EPOLLIN;
    ev.data.u64 = 0x2222u;
    check(epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ev) == 0, "ctl mod");
    check(write(p[1], "y", 1) == 1, "write2");
    /* Both ends are now ready (read has data, write has room): expect two. */
    n = epoll_wait(ep, out, 4, 100);
    check(n == 2, "two ready");
    int saw_new = 0;
    for (int i = 0; i < n; i++)
        if (out[i].data.u64 == 0x2222u && (out[i].events & EPOLLIN))
            saw_new = 1;
    check(saw_new, "mod cookie seen");

    /* DEL the read end; only the write end stays ready even with data pending. */
    check(epoll_ctl(ep, EPOLL_CTL_DEL, p[0], &ev) == 0, "ctl del");
    n = epoll_wait(ep, out, 4, 0);
    check(n == 1 && out[0].data.u64 == 0x1111u, "after del only wr");
    check(read(p[0], &c, 1) == 1, "drain2");

    /* Re-add the read end, close the write end: a read-end watch reports HUP. */
    ev.events = EPOLLIN;
    ev.data.u64 = 0x3333u;
    check(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0, "re-add rd");
    check(epoll_ctl(ep, EPOLL_CTL_DEL, p[1], &ev) == 0, "del wr");
    close(p[1]);
    n = epoll_wait(ep, out, 4, 100);
    check(n == 1, "hup count");
    check((out[0].events & EPOLLHUP) != 0, "EPOLLHUP");

    close(p[0]);
    close(ep);

    if (fails == 0)
        write(1, "epoll ok\n", 9);
    return fails;
}
