/* net.c : the guest net client.
 *
 * Each operation fills the shared net channel, rings the doorbell (a
 * synchronous MMIO exit the host services inline, always non-blocking), and
 * reads back the result. When the host reports NET_EAGAIN the calling task
 * parks until the host raises a net event; the scheduler event hook
 * (net_wake_waiters) then re-readies every parked net task, which retry.
 *
 * The no-lost-wakeup ordering is: a task marks itself a waiter before the RPC
 * that arms the host-side poll. If readiness then races in before the task
 * finishes parking, the hook still sees the waiter and task_wake records it as
 * pending, which task_block honours instead of sleeping through it. */
#include "net.h"

#include <stdint.h>

#include "arch.h"
#include "string.h"
#include "task.h"

/* One waiter flag per task id. Set before an RPC that may arm host readiness,
 * so the event hook can never miss this task. */
static volatile int net_waiting[TASK_MAX];

/* Serialises access to the single-request channel across tasks and cpus, with
 * interrupts masked so a preempted holder cannot deadlock a spinning waiter. */
static volatile int net_lock;

static struct jotun_net *
chan(void)
{
    return (struct jotun_net *)(uintptr_t)JOTUN_NET_BASE;
}

static unsigned long
lock_chan(void)
{
    unsigned long f = arch_irq_save();
    while (__atomic_exchange_n(&net_lock, 1, __ATOMIC_ACQUIRE))
        arch_cpu_relax();
    return f;
}

static void
unlock_chan(unsigned long f)
{
    __atomic_store_n(&net_lock, 0, __ATOMIC_RELEASE);
    arch_irq_restore(f);
}

static void
doorbell(void)
{
    *(volatile uint32_t *)(uintptr_t)JOTUN_NET_DOORBELL = 1;
}

/* Re-ready every parked net task. Registered as the scheduler event hook, so
 * it runs on the event interrupt and when an idle cpu is woken. It returns
 * fast unless the host actually flagged a net event. */
static void
net_wake_waiters(void)
{
    if (!__atomic_exchange_n(&chan()->evt, 0, __ATOMIC_SEQ_CST))
        return;
    for (int i = 0; i < TASK_MAX; i++)
        if (__atomic_load_n(&net_waiting[i], __ATOMIC_SEQ_CST))
            task_wake(i);
}

void
net_init(void)
{
    task_add_event_hook(net_wake_waiters);
}

/* One non-blocking dial RPC. Returns a connection handle, NET_EAGAIN, or an
 * error. */
static int
dial_once(const char *target, unsigned long len)
{
    unsigned long f = lock_chan();
    struct jotun_net *c = chan();
    c->op = NET_OP_DIAL;
    c->len = (uint32_t)len;
    memcpy(c->data, target, len);
    doorbell();
    int st = c->status;
    unlock_chan(f);
    return st;
}

/* Dial a service and return a connection handle, or a negative error. A dial to
 * a co-running guest that has not listened yet reports NET_EAGAIN; the task
 * parks and retries so a client that starts before its server still connects
 * once the server listens (the startup race). An external dial never parks. */
int
net_dial(const char *target)
{
    unsigned long len = 0;
    while (target[len] != '\0')
        len++;
    if (len > JOTUN_NET_MSIZE)
        return NET_EINVAL;

    int me = task_self();
    __atomic_store_n(&net_waiting[me], 1, __ATOMIC_SEQ_CST);
    for (;;) {
        int st = dial_once(target, len);
        if (st == NET_EAGAIN) {
            task_block();
            continue;
        }
        __atomic_store_n(&net_waiting[me], 0, __ATOMIC_SEQ_CST);
        return st;
    }
}

/* Bind/register a dial string and return a listening handle, or a negative
 * error. In-host listening ("tcp!*!port") offers the service under this guest's
 * network name. */
int
net_listen(const char *target)
{
    unsigned long len = 0;
    while (target[len] != '\0')
        len++;
    if (len > JOTUN_NET_MSIZE)
        return NET_EINVAL;

    unsigned long f = lock_chan();
    struct jotun_net *c = chan();
    c->op = NET_OP_LISTEN;
    c->len = (uint32_t)len;
    memcpy(c->data, target, len);
    doorbell();
    int st = c->status;
    unlock_chan(f);
    return st;
}

/* One non-blocking accept RPC. Returns a new handle, NET_EAGAIN, or an error;
 * on success peer[] (capacity from addrlen) is filled with the dialer's name. */
static int
accept_once(int handle, char *peer, unsigned long peercap)
{
    unsigned long f = lock_chan();
    struct jotun_net *c = chan();
    c->op = NET_OP_ACCEPT;
    c->handle = handle;
    doorbell();
    int st = c->status;
    if (st >= 0 && peer && peercap) {
        unsigned long n = c->addrlen;
        if (n >= peercap)
            n = peercap - 1;
        memcpy(peer, c->addr, n);
        peer[n] = '\0';
    }
    unlock_chan(f);
    return st;
}

/* Accept one connection on a listening handle, blocking the calling task until
 * a connection arrives. Returns the new connection's handle, or a negative
 * error. The dialer's name is written to peer (truncated to peercap). */
int
net_accept(int handle, char *peer, unsigned long peercap)
{
    int me = task_self();

    __atomic_store_n(&net_waiting[me], 1, __ATOMIC_SEQ_CST);
    for (;;) {
        int st = accept_once(handle, peer, peercap);
        if (st == NET_EAGAIN) {
            task_block();
            continue;
        }
        __atomic_store_n(&net_waiting[me], 0, __ATOMIC_SEQ_CST);
        return st;
    }
}

/* One non-blocking send RPC. Returns the byte count, NET_EAGAIN, or an error. */
static long
send_once(int handle, const void *buf, unsigned long n)
{
    if (n > JOTUN_NET_MSIZE)
        n = JOTUN_NET_MSIZE;

    unsigned long f = lock_chan();
    struct jotun_net *c = chan();
    c->op = NET_OP_SEND;
    c->handle = handle;
    c->len = (uint32_t)n;
    memcpy(c->data, buf, n);
    doorbell();
    long st = c->status;
    unlock_chan(f);
    return st;
}

long
net_write(int handle, const void *buf, unsigned long n)
{
    const unsigned char *p = buf;
    unsigned long total = 0;
    int me = task_self();

    __atomic_store_n(&net_waiting[me], 1, __ATOMIC_SEQ_CST);
    while (total < n) {
        long st = send_once(handle, p + total, n - total);
        if (st >= 0) {
            total += (unsigned long)st;
            continue;
        }
        if (st == NET_EAGAIN) {
            task_block();
            continue;
        }
        __atomic_store_n(&net_waiting[me], 0, __ATOMIC_SEQ_CST);
        return st;
    }
    __atomic_store_n(&net_waiting[me], 0, __ATOMIC_SEQ_CST);
    return (long)total;
}

/* One non-blocking recv RPC. On success copies the bytes into buf. */
static long
recv_once(int handle, void *buf, unsigned long max)
{
    if (max > JOTUN_NET_MSIZE)
        max = JOTUN_NET_MSIZE;

    unsigned long f = lock_chan();
    struct jotun_net *c = chan();
    c->op = NET_OP_RECV;
    c->handle = handle;
    c->maxlen = (uint32_t)max;
    doorbell();
    long st = c->status;
    if (st > 0)
        memcpy(buf, c->data, c->len);
    unlock_chan(f);
    return st;
}

long
net_read(int handle, void *buf, unsigned long max)
{
    int me = task_self();

    __atomic_store_n(&net_waiting[me], 1, __ATOMIC_SEQ_CST);
    for (;;) {
        long st = recv_once(handle, buf, max);
        if (st >= 0) {                  /* count, or 0 for peer closed */
            __atomic_store_n(&net_waiting[me], 0, __ATOMIC_SEQ_CST);
            return st;
        }
        if (st == NET_EAGAIN) {
            task_block();
            continue;
        }
        __atomic_store_n(&net_waiting[me], 0, __ATOMIC_SEQ_CST);
        return st;
    }
}

int
net_udp(void)
{
    unsigned long f = lock_chan();
    struct jotun_net *c = chan();
    c->op = NET_OP_UDP;
    doorbell();
    int st = c->status;
    unlock_chan(f);
    return st;
}

/* One non-blocking sendto RPC. */
static long
sendto_once(int handle, const char *dst, const void *buf, unsigned long n)
{
    unsigned long alen = 0;
    while (dst[alen] != '\0')
        alen++;
    if (alen >= JOTUN_NET_ADDRMAX)
        return NET_EINVAL;
    if (n > JOTUN_NET_MSIZE)
        n = JOTUN_NET_MSIZE;

    unsigned long f = lock_chan();
    struct jotun_net *c = chan();
    c->op = NET_OP_SENDTO;
    c->handle = handle;
    c->addrlen = (uint32_t)alen;
    memcpy(c->addr, dst, alen);
    c->len = (uint32_t)n;
    memcpy(c->data, buf, n);
    doorbell();
    long st = c->status;
    unlock_chan(f);
    return st;
}

long
net_sendto(int handle, const char *dst, const void *buf, unsigned long n)
{
    int me = task_self();

    __atomic_store_n(&net_waiting[me], 1, __ATOMIC_SEQ_CST);
    for (;;) {
        long st = sendto_once(handle, dst, buf, n);
        if (st == NET_EAGAIN) {
            task_block();
            continue;
        }
        __atomic_store_n(&net_waiting[me], 0, __ATOMIC_SEQ_CST);
        return st;              /* datagram count, or an error */
    }
}

/* One non-blocking recvfrom RPC. On success copies the payload and the source
 * address string out. */
static long
recvfrom_once(int handle, char *src, unsigned long srccap,
              void *buf, unsigned long max)
{
    if (max > JOTUN_NET_MSIZE)
        max = JOTUN_NET_MSIZE;

    unsigned long f = lock_chan();
    struct jotun_net *c = chan();
    c->op = NET_OP_RECVFROM;
    c->handle = handle;
    c->maxlen = (uint32_t)max;
    doorbell();
    long st = c->status;
    if (st >= 0) {
        memcpy(buf, c->data, c->len);
        unsigned long a = c->addrlen;
        if (src && srccap > 0) {
            if (a > srccap - 1)
                a = srccap - 1;
            memcpy(src, c->addr, a);
            src[a] = '\0';
        }
    }
    unlock_chan(f);
    return st;
}

long
net_recvfrom(int handle, char *src, unsigned long srccap,
             void *buf, unsigned long max)
{
    int me = task_self();

    __atomic_store_n(&net_waiting[me], 1, __ATOMIC_SEQ_CST);
    for (;;) {
        long st = recvfrom_once(handle, src, srccap, buf, max);
        if (st == NET_EAGAIN) {
            task_block();
            continue;
        }
        __atomic_store_n(&net_waiting[me], 0, __ATOMIC_SEQ_CST);
        return st;
    }
}

int
net_close(int handle)
{
    unsigned long f = lock_chan();
    struct jotun_net *c = chan();
    c->op = NET_OP_CLOSE;
    c->handle = handle;
    doorbell();
    int st = c->status;
    unlock_chan(f);
    return st;
}
