/* net.c : the host net provider. Readiness via epoll on Linux, kqueue on macOS.
 *
 * The protocol logic is platform-neutral; the small poller_* layer below hides
 * the readiness mechanism. One poll thread blocks until a socket is ready (or a
 * shutdown wakeup), then flags the guest channel and rouses the guest, which
 * re-readies and retries its parked net tasks. */

#define _GNU_SOURCE     /* getaddrinfo, MSG_NOSIGNAL on Linux */

#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#else
#include <sys/epoll.h>
#include <sys/eventfd.h>
#endif

/* macOS has no MSG_NOSIGNAL; it uses the SO_NOSIGPIPE socket option instead
 * (set on each socket by set_nosigpipe), so the flag can fall back to zero. */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define NET_MAXCONN 16

/* What a socket is being armed to wait for. */
#define NET_WANT_READ  1
#define NET_WANT_WRITE 2

struct net_conn {
    int fd;                 /* host socket, -1 if the slot is free */
    int proto;
    int in_poll;            /* fd is registered with the poller */
    char peer[64];          /* "host!port" for a dial, "-" for a UDP socket */
};

struct netdev {
    struct net_conn conns[NET_MAXCONN];
#if defined(__APPLE__)
    int kqfd;
    int wake_pipe[2];       /* self-pipe: write to break the poll thread out */
#else
    int epfd;
    int evtfd;              /* ticked to break the poll thread out at shutdown */
#endif
    pthread_t poll_thread;
    pthread_mutex_t lock;   /* guards conns and the poller set */
    volatile int stop;
    struct jotun_net *chan; /* guest channel, for the readiness flag */
    void (*wake)(void);
    const struct net_policy *policy;
};

/* Suppress SIGPIPE on a socket where the platform needs a per-socket option
 * (macOS). On Linux MSG_NOSIGNAL on the send covers it, so this is a no-op. */
static void
set_nosigpipe(int fd)
{
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#else
    (void)fd;
#endif
}

/****************************************************************
 * Poller: epoll (Linux) or kqueue (macOS)
 ****************************************************************/

#if defined(__APPLE__)

static int
poller_create(struct netdev *nd)
{
    nd->kqfd = kqueue();
    if (nd->kqfd < 0)
        return -1;
    if (pipe(nd->wake_pipe) != 0) {
        close(nd->kqfd);
        return -1;
    }
    fcntl(nd->wake_pipe[0], F_SETFL,
          fcntl(nd->wake_pipe[0], F_GETFL, 0) | O_NONBLOCK);
    struct kevent kev;
    EV_SET(&kev, nd->wake_pipe[0], EVFILT_READ, EV_ADD, 0, 0, NULL);
    kevent(nd->kqfd, &kev, 1, NULL, 0, NULL);
    return 0;
}

/* Arm one direction, one-shot: it fires once and stays quiet until re-armed. */
static void
poller_arm(struct netdev *nd, struct net_conn *c, int want)
{
    int16_t filt = (want & NET_WANT_WRITE) ? EVFILT_WRITE : EVFILT_READ;
    struct kevent kev;
    EV_SET(&kev, c->fd, filt, EV_ADD | EV_ONESHOT, 0, 0, NULL);
    kevent(nd->kqfd, &kev, 1, NULL, 0, NULL);
    c->in_poll = 1;
}

static void
poller_del(struct netdev *nd, struct net_conn *c)
{
    struct kevent kev[2];
    EV_SET(&kev[0], c->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&kev[1], c->fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(nd->kqfd, kev, 2, NULL, 0, NULL);        /* ENOENT is harmless */
    c->in_poll = 0;
}

/* Block until a socket is ready or the wakeup fires. Returns 1 if a real socket
 * is ready, 0 for only a wakeup, -1 on a fatal error. */
static int
poller_wait(struct netdev *nd)
{
    struct kevent evs[NET_MAXCONN + 1];
    int n = kevent(nd->kqfd, NULL, 0, evs, NET_MAXCONN + 1, NULL);
    if (n < 0)
        return errno == EINTR ? 0 : -1;
    int woke = 0;
    for (int i = 0; i < n; i++) {
        if ((int)evs[i].ident == nd->wake_pipe[0]) {
            char buf[64];
            while (read(nd->wake_pipe[0], buf, sizeof buf) > 0)
                ;
            continue;                               /* shutdown tick */
        }
        woke = 1;
    }
    return woke;
}

static void
poller_wake(struct netdev *nd)
{
    char one = 1;
    if (write(nd->wake_pipe[1], &one, 1) < 0) {
        /* pipe full: a wakeup is already pending, nothing to do */
    }
}

static void
poller_destroy(struct netdev *nd)
{
    close(nd->kqfd);
    close(nd->wake_pipe[0]);
    close(nd->wake_pipe[1]);
}

#else /* Linux: epoll + eventfd */

static int
poller_create(struct netdev *nd)
{
    nd->epfd = epoll_create1(0);
    nd->evtfd = eventfd(0, EFD_NONBLOCK);
    if (nd->epfd < 0 || nd->evtfd < 0) {
        if (nd->epfd >= 0)
            close(nd->epfd);
        if (nd->evtfd >= 0)
            close(nd->evtfd);
        return -1;
    }
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = nd->evtfd };
    epoll_ctl(nd->epfd, EPOLL_CTL_ADD, nd->evtfd, &ev);
    return 0;
}

/* Arm (or re-arm) the socket one-shot, so it fires once and then stays quiet
 * until the next would-block re-arms it. */
static void
poller_arm(struct netdev *nd, struct net_conn *c, int want)
{
    uint32_t events = ((want & NET_WANT_READ) ? EPOLLIN : 0)
                    | ((want & NET_WANT_WRITE) ? EPOLLOUT : 0);
    struct epoll_event ev = { .events = events | EPOLLONESHOT, .data.fd = c->fd };
    if (c->in_poll) {
        epoll_ctl(nd->epfd, EPOLL_CTL_MOD, c->fd, &ev);
    } else if (epoll_ctl(nd->epfd, EPOLL_CTL_ADD, c->fd, &ev) == 0) {
        c->in_poll = 1;
    }
}

static void
poller_del(struct netdev *nd, struct net_conn *c)
{
    if (c->in_poll)
        epoll_ctl(nd->epfd, EPOLL_CTL_DEL, c->fd, NULL);
    c->in_poll = 0;
}

static int
poller_wait(struct netdev *nd)
{
    struct epoll_event evs[NET_MAXCONN + 1];
    int n = epoll_wait(nd->epfd, evs, NET_MAXCONN + 1, -1);
    if (n < 0)
        return errno == EINTR ? 0 : -1;
    int woke = 0;
    for (int i = 0; i < n; i++) {
        if (evs[i].data.fd == nd->evtfd) {
            uint64_t v;
            if (read(nd->evtfd, &v, sizeof v) < 0) {
                /* spurious; the stop flag is checked by the caller */
            }
            continue;                               /* shutdown tick */
        }
        woke = 1;
    }
    return woke;
}

static void
poller_wake(struct netdev *nd)
{
    uint64_t one = 1;
    if (write(nd->evtfd, &one, sizeof one) < 0) {
        /* counter at max: a wakeup is already pending */
    }
}

static void
poller_destroy(struct netdev *nd)
{
    close(nd->epfd);
    close(nd->evtfd);
}

#endif

/****************************************************************
 * Protocol logic (platform-neutral)
 ****************************************************************/

/* Set a result code and clear the payload length. */
static void
reply(struct jotun_net *chan, int32_t status, uint32_t len)
{
    chan->status = status;
    chan->len = len;
}

/* Extract the IPv4 address (host byte order) and port from a sockaddr. Returns
 * 0 for anything that is not AF_INET, since the policy rules are IPv4. */
static int
sa_v4(const struct sockaddr *sa, uint32_t *ip, uint16_t *port)
{
    if (sa->sa_family != AF_INET)
        return 0;
    const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
    *ip = ntohl(in->sin_addr.s_addr);
    *port = ntohs(in->sin_port);
    return 1;
}

/* Decide whether a resolved address is allowed. With no rules every target is
 * allowed; otherwise a target must match at least one rule. A non-IPv4 target
 * can only pass the no-rules case, since the rules are IPv4. */
static int
policy_allows(const struct net_policy *p, const struct sockaddr *sa, int proto)
{
    if (p->n == 0)
        return 1;

    uint32_t ip;
    uint16_t port;
    if (!sa_v4(sa, &ip, &port))
        return 0;

    for (int i = 0; i < p->n; i++) {
        const struct net_rule *r = &p->rules[i];
        if ((ip & r->mask) != (r->addr & r->mask))
            continue;
        if (port < r->port_lo || port > r->port_hi)
            continue;
        if (r->proto != 0 && r->proto != proto)
            continue;
        return 1;
    }
    return 0;
}

static struct net_conn *
conn_of(struct netdev *nd, int32_t handle)
{
    if (handle < 0 || handle >= NET_MAXCONN)
        return NULL;
    if (nd->conns[handle].fd < 0)
        return NULL;
    return &nd->conns[handle];
}

static void
do_dial(struct netdev *nd, struct jotun_net *chan)
{
    /* The target arrives as "host!port", not NUL-terminated on the wire. */
    char target[256];
    uint32_t n = chan->len;
    if (n >= sizeof target) {
        reply(chan, NET_EINVAL, 0);
        return;
    }
    memcpy(target, chan->data, n);
    target[n] = '\0';

    char *bang = strchr(target, '!');
    if (!bang) {
        reply(chan, NET_EINVAL, 0);
        return;
    }
    *bang = '\0';
    const char *host = target;
    const char *port = bang + 1;

    struct addrinfo hints = { 0 };
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = chan->proto == NET_PROTO_UDP ? SOCK_DGRAM : SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        reply(chan, NET_ENORES, 0);
        return;
    }

    pthread_mutex_lock(&nd->lock);
    int handle = -1;
    for (int i = 0; i < NET_MAXCONN; i++) {
        if (nd->conns[i].fd < 0) {
            handle = i;
            break;
        }
    }
    if (handle < 0) {
        pthread_mutex_unlock(&nd->lock);
        freeaddrinfo(res);
        reply(chan, NET_ENORES, 0);
        return;
    }

    /* Try each resolved address that policy allows, connecting to the first
     * that works. Policy is checked against the resolved address, so a name
     * cannot smuggle a connection to a blocked address. */
    int fd = -1;
    int any_allowed = 0;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        if (!policy_allows(nd->policy, ai->ai_addr, NET_PROTO_TCP))
            continue;
        any_allowed = 1;

        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        set_nosigpipe(fd);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0 || errno == EINPROGRESS)
            break;              /* connected, or in progress */
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        pthread_mutex_unlock(&nd->lock);
        reply(chan, any_allowed ? NET_ECONN : NET_EPERM, 0);
        return;
    }

    nd->conns[handle].fd = fd;
    nd->conns[handle].proto = NET_PROTO_TCP;
    nd->conns[handle].in_poll = 0;
    snprintf(nd->conns[handle].peer, sizeof nd->conns[handle].peer,
             "%.40s!%.16s", host, port);
    pthread_mutex_unlock(&nd->lock);

    reply(chan, handle, 0);
}

static void
do_send(struct netdev *nd, struct jotun_net *chan)
{
    pthread_mutex_lock(&nd->lock);
    struct net_conn *c = conn_of(nd, chan->handle);
    if (!c) {
        pthread_mutex_unlock(&nd->lock);
        reply(chan, NET_EINVAL, 0);
        return;
    }

    uint32_t len = chan->len;
    if (len > JOTUN_NET_MSIZE)
        len = JOTUN_NET_MSIZE;
    ssize_t s = send(c->fd, chan->data, len, MSG_NOSIGNAL);
    if (s >= 0) {
        pthread_mutex_unlock(&nd->lock);
        reply(chan, (int32_t)s, 0);
        return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOTCONN) {
        poller_arm(nd, c, NET_WANT_WRITE);  /* writable also signals connect */
        pthread_mutex_unlock(&nd->lock);
        reply(chan, NET_EAGAIN, 0);
        return;
    }
    pthread_mutex_unlock(&nd->lock);
    reply(chan, NET_ECONN, 0);
}

static void
do_recv(struct netdev *nd, struct jotun_net *chan)
{
    pthread_mutex_lock(&nd->lock);
    struct net_conn *c = conn_of(nd, chan->handle);
    if (!c) {
        pthread_mutex_unlock(&nd->lock);
        reply(chan, NET_EINVAL, 0);
        return;
    }

    uint32_t max = chan->maxlen;
    if (max > JOTUN_NET_MSIZE)
        max = JOTUN_NET_MSIZE;
    ssize_t r = recv(c->fd, chan->data, max, 0);
    if (r > 0) {
        pthread_mutex_unlock(&nd->lock);
        reply(chan, (int32_t)r, (uint32_t)r);
        return;
    }
    if (r == 0) {
        pthread_mutex_unlock(&nd->lock);
        reply(chan, 0, 0);           /* peer closed */
        return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOTCONN) {
        poller_arm(nd, c, NET_WANT_READ);
        pthread_mutex_unlock(&nd->lock);
        reply(chan, NET_EAGAIN, 0);
        return;
    }
    pthread_mutex_unlock(&nd->lock);
    reply(chan, NET_ECONN, 0);
}

static void
do_udp(struct netdev *nd, struct jotun_net *chan)
{
    pthread_mutex_lock(&nd->lock);
    int handle = -1;
    for (int i = 0; i < NET_MAXCONN; i++) {
        if (nd->conns[i].fd < 0) {
            handle = i;
            break;
        }
    }
    if (handle < 0) {
        pthread_mutex_unlock(&nd->lock);
        reply(chan, NET_ENORES, 0);
        return;
    }

    /* Unconnected datagram socket. IPv4 for this PoC, so sendto resolves its
     * targets as IPv4 too. */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        pthread_mutex_unlock(&nd->lock);
        reply(chan, NET_ECONN, 0);
        return;
    }
    set_nosigpipe(fd);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    nd->conns[handle].fd = fd;
    nd->conns[handle].proto = NET_PROTO_UDP;
    nd->conns[handle].in_poll = 0;
    snprintf(nd->conns[handle].peer, sizeof nd->conns[handle].peer, "-");
    pthread_mutex_unlock(&nd->lock);

    reply(chan, handle, 0);
}

static void
do_sendto(struct netdev *nd, struct jotun_net *chan)
{
    /* Resolve the per-datagram destination before taking the lock. */
    char target[JOTUN_NET_ADDRMAX];
    uint32_t an = chan->addrlen;
    if (an >= sizeof target) {
        reply(chan, NET_EINVAL, 0);
        return;
    }
    memcpy(target, chan->addr, an);
    target[an] = '\0';

    char *bang = strchr(target, '!');
    if (!bang) {
        reply(chan, NET_EINVAL, 0);
        return;
    }
    *bang = '\0';
    const char *host = target;
    const char *port = bang + 1;

    struct addrinfo hints = { 0 };
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        reply(chan, NET_ENORES, 0);
        return;
    }

    if (!policy_allows(nd->policy, res->ai_addr, NET_PROTO_UDP)) {
        freeaddrinfo(res);
        reply(chan, NET_EPERM, 0);
        return;
    }

    pthread_mutex_lock(&nd->lock);
    struct net_conn *c = conn_of(nd, chan->handle);
    if (!c) {
        pthread_mutex_unlock(&nd->lock);
        freeaddrinfo(res);
        reply(chan, NET_EINVAL, 0);
        return;
    }

    uint32_t len = chan->len;
    if (len > JOTUN_NET_MSIZE)
        len = JOTUN_NET_MSIZE;
    ssize_t s = sendto(c->fd, chan->data, len, 0, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (s >= 0) {
        pthread_mutex_unlock(&nd->lock);
        reply(chan, (int32_t)s, 0);
        return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        poller_arm(nd, c, NET_WANT_WRITE);
        pthread_mutex_unlock(&nd->lock);
        reply(chan, NET_EAGAIN, 0);
        return;
    }
    pthread_mutex_unlock(&nd->lock);
    reply(chan, NET_ECONN, 0);
}

static void
do_recvfrom(struct netdev *nd, struct jotun_net *chan)
{
    pthread_mutex_lock(&nd->lock);
    struct net_conn *c = conn_of(nd, chan->handle);
    if (!c) {
        pthread_mutex_unlock(&nd->lock);
        reply(chan, NET_EINVAL, 0);
        return;
    }

    uint32_t max = chan->maxlen;
    if (max > JOTUN_NET_MSIZE)
        max = JOTUN_NET_MSIZE;
    struct sockaddr_storage src;
    socklen_t srclen = sizeof src;
    ssize_t r = recvfrom(c->fd, chan->data, max, 0, (struct sockaddr *)&src,
                         &srclen);
    if (r >= 0) {
        /* Report the source as a numeric "host!port" string. */
        char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
        chan->addrlen = 0;
        if (getnameinfo((struct sockaddr *)&src, srclen, hbuf, sizeof hbuf,
                        sbuf, sizeof sbuf,
                        NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
            int n = snprintf((char *)chan->addr, JOTUN_NET_ADDRMAX, "%s!%s",
                             hbuf, sbuf);
            if (n > 0)
                chan->addrlen = (uint32_t)(n < (int)JOTUN_NET_ADDRMAX
                                           ? n : (int)JOTUN_NET_ADDRMAX - 1);
        }
        pthread_mutex_unlock(&nd->lock);
        reply(chan, (int32_t)r, (uint32_t)r);
        return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        poller_arm(nd, c, NET_WANT_READ);
        pthread_mutex_unlock(&nd->lock);
        reply(chan, NET_EAGAIN, 0);
        return;
    }
    pthread_mutex_unlock(&nd->lock);
    reply(chan, NET_ECONN, 0);
}

static void
do_close(struct netdev *nd, struct jotun_net *chan)
{
    pthread_mutex_lock(&nd->lock);
    struct net_conn *c = conn_of(nd, chan->handle);
    if (c) {
        poller_del(nd, c);
        close(c->fd);
        c->fd = -1;
    }
    pthread_mutex_unlock(&nd->lock);
    reply(chan, 0, 0);
}

void
net_handle(struct netdev *nd, struct jotun_net *chan)
{
    switch (chan->op) {
    case NET_OP_DIAL:     do_dial(nd, chan);     break;
    case NET_OP_SEND:     do_send(nd, chan);     break;
    case NET_OP_RECV:     do_recv(nd, chan);     break;
    case NET_OP_CLOSE:    do_close(nd, chan);    break;
    case NET_OP_UDP:      do_udp(nd, chan);      break;
    case NET_OP_SENDTO:   do_sendto(nd, chan);   break;
    case NET_OP_RECVFROM: do_recvfrom(nd, chan); break;
    default:              reply(chan, NET_EINVAL, 0); break;
    }
}

static void *
poll_loop(void *arg)
{
    struct netdev *nd = arg;

    while (!__atomic_load_n(&nd->stop, __ATOMIC_SEQ_CST)) {
        int woke = poller_wait(nd);
        if (woke < 0)
            break;
        if (woke) {
            /* Readiness is delivered generically: flag the channel and rouse
             * the guest, which re-readies and retries its parked net tasks. */
            __atomic_store_n(&nd->chan->evt, 1, __ATOMIC_SEQ_CST);
            nd->wake();
        }
    }
    return NULL;
}

struct netdev *
net_init(struct jotun_net *chan, void (*wake)(void),
         const struct net_policy *policy)
{
    struct netdev *nd = calloc(1, sizeof(*nd));
    if (!nd)
        return NULL;

    for (int i = 0; i < NET_MAXCONN; i++)
        nd->conns[i].fd = -1;
    nd->chan = chan;
    nd->wake = wake;
    nd->policy = policy;
    pthread_mutex_init(&nd->lock, NULL);

    if (poller_create(nd) != 0) {
        free(nd);
        return NULL;
    }

    if (pthread_create(&nd->poll_thread, NULL, poll_loop, nd) != 0) {
        poller_destroy(nd);
        free(nd);
        return NULL;
    }
    return nd;
}

int
net_conn_snapshot(struct netdev *nd, struct net_conninfo *out, int max)
{
    int n = 0;
    pthread_mutex_lock(&nd->lock);
    for (int i = 0; i < NET_MAXCONN && n < max; i++) {
        if (nd->conns[i].fd < 0)
            continue;
        out[n].handle = i;
        out[n].proto = nd->conns[i].proto;
        snprintf(out[n].peer, sizeof out[n].peer, "%s", nd->conns[i].peer);
        n++;
    }
    pthread_mutex_unlock(&nd->lock);
    return n;
}

void
net_shutdown(struct netdev *nd)
{
    if (!nd)
        return;

    __atomic_store_n(&nd->stop, 1, __ATOMIC_SEQ_CST);
    poller_wake(nd);
    pthread_join(nd->poll_thread, NULL);

    for (int i = 0; i < NET_MAXCONN; i++)
        if (nd->conns[i].fd >= 0)
            close(nd->conns[i].fd);
    poller_destroy(nd);
    free(nd);
}
