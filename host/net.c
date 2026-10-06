/* net.c : the host net provider (Linux epoll). */

#define _GNU_SOURCE     /* getaddrinfo, epoll, eventfd, MSG_NOSIGNAL */

#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#define NET_MAXCONN 16

struct net_conn {
    int fd;                 /* host socket, -1 if the slot is free */
    int proto;
    int in_epoll;           /* fd has been added to the epoll set */
};

struct netdev {
    struct net_conn conns[NET_MAXCONN];
    int epfd;
    int evtfd;              /* ticked to break the poll thread out at shutdown */
    pthread_t poll_thread;
    pthread_mutex_t lock;   /* guards conns and the epoll set */
    volatile int stop;
    struct jotun_net *chan; /* guest channel, for the readiness flag */
    void (*wake)(void);
    const struct net_policy *policy;
};

/* Set a result code and clear the payload length. */
static void
reply(struct jotun_net *chan, int32_t status, uint32_t len)
{
    chan->status = status;
    chan->len = len;
}

/* Arm (or re-arm) a connection's socket for the given epoll events, one-shot so
 * it fires once and then stays quiet until the next would-block re-arms it. */
static void
arm(struct netdev *nd, struct net_conn *c, uint32_t events)
{
    struct epoll_event ev = { .events = events | EPOLLONESHOT, .data.fd = c->fd };
    if (c->in_epoll) {
        epoll_ctl(nd->epfd, EPOLL_CTL_MOD, c->fd, &ev);
    } else if (epoll_ctl(nd->epfd, EPOLL_CTL_ADD, c->fd, &ev) == 0) {
        c->in_epoll = 1;
    }
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
    nd->conns[handle].in_epoll = 0;
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
        arm(nd, c, EPOLLOUT);        /* writable also signals connect done */
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
        arm(nd, c, EPOLLIN);
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
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    nd->conns[handle].fd = fd;
    nd->conns[handle].proto = NET_PROTO_UDP;
    nd->conns[handle].in_epoll = 0;
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
        arm(nd, c, EPOLLOUT);
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
        arm(nd, c, EPOLLIN);
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
        if (c->in_epoll)
            epoll_ctl(nd->epfd, EPOLL_CTL_DEL, c->fd, NULL);
        close(c->fd);
        c->fd = -1;
        c->in_epoll = 0;
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
    struct epoll_event evs[NET_MAXCONN + 1];

    while (!__atomic_load_n(&nd->stop, __ATOMIC_SEQ_CST)) {
        int n = epoll_wait(nd->epfd, evs, NET_MAXCONN + 1, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        int woke = 0;
        for (int i = 0; i < n; i++) {
            if (evs[i].data.fd == nd->evtfd) {
                uint64_t v;
                (void)read(nd->evtfd, &v, sizeof v);
                continue;       /* shutdown tick, loop checks the stop flag */
            }
            woke = 1;           /* a real socket is ready */
        }
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

    nd->epfd = epoll_create1(0);
    nd->evtfd = eventfd(0, EFD_NONBLOCK);
    if (nd->epfd < 0 || nd->evtfd < 0) {
        if (nd->epfd >= 0) close(nd->epfd);
        if (nd->evtfd >= 0) close(nd->evtfd);
        free(nd);
        return NULL;
    }

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = nd->evtfd };
    epoll_ctl(nd->epfd, EPOLL_CTL_ADD, nd->evtfd, &ev);

    if (pthread_create(&nd->poll_thread, NULL, poll_loop, nd) != 0) {
        close(nd->epfd);
        close(nd->evtfd);
        free(nd);
        return NULL;
    }
    return nd;
}

void
net_shutdown(struct netdev *nd)
{
    if (!nd)
        return;

    __atomic_store_n(&nd->stop, 1, __ATOMIC_SEQ_CST);
    uint64_t one = 1;
    (void)write(nd->evtfd, &one, sizeof one);
    pthread_join(nd->poll_thread, NULL);

    for (int i = 0; i < NET_MAXCONN; i++)
        if (nd->conns[i].fd >= 0)
            close(nd->conns[i].fd);
    close(nd->epfd);
    close(nd->evtfd);
    free(nd);
}
