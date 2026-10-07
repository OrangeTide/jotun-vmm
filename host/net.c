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

struct vlistener;

/* One datagram queued for in-host delivery to a UDP endpoint. The payload is
 * heap-allocated to its exact length, so an idle endpoint costs nothing. */
#define UDP_BACKLOG 8
struct udp_dgram {
    char src[64];           /* sender's "name!port" */
    uint32_t len;
    uint8_t *data;          /* len bytes, or NULL for an empty datagram */
};

struct net_conn {
    int fd;                 /* host socket, -1 if the slot is free */
    int proto;
    int in_poll;            /* fd is registered with the poller */
    int is_listener;        /* an in-host listener: fd is its wake pipe */
    struct vlistener *vl;   /* the in-host listener registration, if any */
    int lfd;                /* a listener's real listening socket, or -1 */
    int lfd_in_poll;        /* lfd is registered with the poller */
    uint16_t uport;         /* in-host UDP port bound under the guest name, 0 none */
    struct udp_dgram uq[UDP_BACKLOG];   /* in-host datagrams waiting to be read */
    int uq_head, uq_count;
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
    void (*wake)(void *ctx);
    void *wake_ctx;
    const struct net_policy *policy;
    struct vswitch *vsw;    /* process-wide in-host name service, or NULL */
    char name[64];          /* this guest's network name, or "" */
};

/* The in-host name service and switch. Shared by every guest's provider in the
 * process. It holds the set of guest names (so the host field of a dial string
 * resolves to a co-running guest or, failing that, to an external name) and the
 * live listeners each guest has registered. A dial to a guest is spliced with a
 * socketpair, so the data path is an ordinary fd and never touches the network;
 * the host is the only bridge between guests. */
#define VSW_MAXNAME    16
#define VSW_MAXLISTEN  32
#define VSW_BACKLOG    8
#define VSW_MAXUDP     32

/* A UDP port bound under a guest's name, so a datagram addressed to
 * "name!port" can be routed to the owning endpoint. */
struct vudp {
    int used;
    char name[64];
    uint16_t port;
    struct netdev *owner;
    int32_t handle;
};

struct vlistener {
    int used;
    char name[64];                      /* owning guest's name */
    uint16_t port;
    struct netdev *owner;               /* for the /net view; wake is via fd */
    int wake_wr;                        /* write end of the owner's listen pipe */
    int fds[VSW_BACKLOG];               /* pending far socketpair ends */
    char peers[VSW_BACKLOG][64];        /* dialer name per pending connection */
    int head, count;
};

struct vswitch {
    pthread_mutex_t lock;
    char names[VSW_MAXNAME][64];
    int nnames;
    struct netdev *members[VSW_MAXNAME];    /* every provider, for wake-all */
    int nmembers;
    struct vlistener listeners[VSW_MAXLISTEN];
    struct vudp udps[VSW_MAXUDP];
    uint16_t next_ephem;                    /* next in-host UDP ephemeral port */
};

struct vswitch *
vswitch_new(void)
{
    struct vswitch *v = calloc(1, sizeof *v);
    if (v)
        pthread_mutex_init(&v->lock, NULL);
    return v;
}

/* Register a guest's name so the name service can resolve it. Idempotent. */
void
vsw_register_name(struct vswitch *v, const char *name)
{
    if (!v || !name || !name[0])
        return;
    pthread_mutex_lock(&v->lock);
    for (int i = 0; i < v->nnames; i++)
        if (strcmp(v->names[i], name) == 0) {
            pthread_mutex_unlock(&v->lock);
            return;
        }
    if (v->nnames < VSW_MAXNAME)
        snprintf(v->names[v->nnames++], sizeof v->names[0], "%s", name);
    pthread_mutex_unlock(&v->lock);
}

/* Record a guest's provider so a listen can wake every dialer parked on the
 * startup race. Called once per guest from net_init. */
static void
vsw_add_member(struct vswitch *v, struct netdev *nd)
{
    if (!v || !nd)
        return;
    pthread_mutex_lock(&v->lock);
    if (v->nmembers < VSW_MAXNAME)
        v->members[v->nmembers++] = nd;
    pthread_mutex_unlock(&v->lock);
}

/* Poke every guest so any task parked on a dial retries. A dial to a guest
 * that has not listened yet returns NET_EAGAIN and parks; a later listen does
 * not know which guests are waiting for it, so this broadcasts. Listens are
 * rare, and a guest with nothing parked just clears its flag and moves on. */
static void
vsw_wake_all(struct vswitch *v)
{
    struct netdev *m[VSW_MAXNAME];
    int n = 0;
    pthread_mutex_lock(&v->lock);
    for (int i = 0; i < v->nmembers; i++)
        m[n++] = v->members[i];
    pthread_mutex_unlock(&v->lock);
    for (int i = 0; i < n; i++) {
        __atomic_store_n(&m[i]->chan->evt, 1, __ATOMIC_SEQ_CST);
        m[i]->wake(m[i]->wake_ctx);
    }
}

/* Whether a name belongs to a co-running guest (caller holds no lock). */
static int
vsw_is_guest(struct vswitch *v, const char *name)
{
    if (!v)
        return 0;
    int found = 0;
    pthread_mutex_lock(&v->lock);
    for (int i = 0; i < v->nnames; i++)
        if (strcmp(v->names[i], name) == 0) {
            found = 1;
            break;
        }
    pthread_mutex_unlock(&v->lock);
    return found;
}

/* Find the UDP binding for "name!port" (caller holds v->lock). */
static struct vudp *
vudp_find(struct vswitch *v, const char *name, uint16_t port)
{
    for (int i = 0; i < VSW_MAXUDP; i++)
        if (v->udps[i].used && v->udps[i].port == port
            && strcmp(v->udps[i].name, name) == 0)
            return &v->udps[i];
    return NULL;
}

/* Register a UDP port under a guest's name (caller holds v->lock). Returns 0,
 * -1 if the port is already bound for that name, or -2 if the table is full. */
static int
vudp_register(struct vswitch *v, const char *name, uint16_t port,
              struct netdev *owner, int32_t handle)
{
    if (vudp_find(v, name, port))
        return -1;
    for (int i = 0; i < VSW_MAXUDP; i++)
        if (!v->udps[i].used) {
            v->udps[i].used = 1;
            snprintf(v->udps[i].name, sizeof v->udps[i].name, "%s", name);
            v->udps[i].port = port;
            v->udps[i].owner = owner;
            v->udps[i].handle = handle;
            return 0;
        }
    return -2;
}

/* Assign and register an unused in-host ephemeral port for a guest's UDP
 * endpoint (caller holds v->lock), so a datagram it sends has a source address
 * peers can reply to. Returns the port, or 0 if none could be assigned. */
static uint16_t
vudp_autoport(struct vswitch *v, const char *name, struct netdev *owner,
              int32_t handle)
{
    for (int tries = 0; tries < 32768; tries++) {
        if (v->next_ephem < 32768)
            v->next_ephem = 32768;
        uint16_t port = v->next_ephem++;
        if (v->next_ephem == 0)
            v->next_ephem = 32768;
        if (!vudp_find(v, name, port)
            && vudp_register(v, name, port, owner, handle) == 0)
            return port;
    }
    return 0;
}

/* Drop a UDP binding owned by a given endpoint (takes v->lock). */
static void
vudp_unbind(struct vswitch *v, struct netdev *owner, int32_t handle)
{
    if (!v)
        return;
    pthread_mutex_lock(&v->lock);
    for (int i = 0; i < VSW_MAXUDP; i++)
        if (v->udps[i].used && v->udps[i].owner == owner
            && v->udps[i].handle == handle)
            v->udps[i].used = 0;
    pthread_mutex_unlock(&v->lock);
}

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

/* Arm one direction of one fd, one-shot: it fires once and stays quiet until
 * re-armed. */
static void
poller_arm_fd(struct netdev *nd, int fd, int *in_poll, int want)
{
    int16_t filt = (want & NET_WANT_WRITE) ? EVFILT_WRITE : EVFILT_READ;
    struct kevent kev;
    EV_SET(&kev, fd, filt, EV_ADD | EV_ONESHOT, 0, 0, NULL);
    kevent(nd->kqfd, &kev, 1, NULL, 0, NULL);
    *in_poll = 1;
}

static void
poller_del_fd(struct netdev *nd, int fd, int *in_poll)
{
    struct kevent kev[2];
    EV_SET(&kev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&kev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(nd->kqfd, kev, 2, NULL, 0, NULL);        /* ENOENT is harmless */
    *in_poll = 0;
}

static void
poller_arm(struct netdev *nd, struct net_conn *c, int want)
{
    poller_arm_fd(nd, c->fd, &c->in_poll, want);
}

static void
poller_del(struct netdev *nd, struct net_conn *c)
{
    poller_del_fd(nd, c->fd, &c->in_poll);
    if (c->lfd >= 0)
        poller_del_fd(nd, c->lfd, &c->lfd_in_poll);
}

/* Block until a socket is ready or the wakeup fires. Returns 1 if a real socket
 * is ready, 0 for only a wakeup, -1 on a fatal error. */
static int
poller_wait(struct netdev *nd)
{
    struct kevent evs[2 * NET_MAXCONN + 1];
    int n = kevent(nd->kqfd, NULL, 0, evs, 2 * NET_MAXCONN + 1, NULL);
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
poller_arm_fd(struct netdev *nd, int fd, int *in_poll, int want)
{
    uint32_t events = ((want & NET_WANT_READ) ? EPOLLIN : 0)
                    | ((want & NET_WANT_WRITE) ? EPOLLOUT : 0);
    struct epoll_event ev = { .events = events | EPOLLONESHOT, .data.fd = fd };
    if (*in_poll) {
        epoll_ctl(nd->epfd, EPOLL_CTL_MOD, fd, &ev);
    } else if (epoll_ctl(nd->epfd, EPOLL_CTL_ADD, fd, &ev) == 0) {
        *in_poll = 1;
    }
}

static void
poller_del_fd(struct netdev *nd, int fd, int *in_poll)
{
    if (*in_poll)
        epoll_ctl(nd->epfd, EPOLL_CTL_DEL, fd, NULL);
    *in_poll = 0;
}

static void
poller_arm(struct netdev *nd, struct net_conn *c, int want)
{
    poller_arm_fd(nd, c->fd, &c->in_poll, want);
}

static void
poller_del(struct netdev *nd, struct net_conn *c)
{
    poller_del_fd(nd, c->fd, &c->in_poll);
    if (c->lfd >= 0)
        poller_del_fd(nd, c->lfd, &c->lfd_in_poll);
}

static int
poller_wait(struct netdev *nd)
{
    struct epoll_event evs[2 * NET_MAXCONN + 1];
    int n = epoll_wait(nd->epfd, evs, 2 * NET_MAXCONN + 1, -1);
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
        if (r->kind == NET_RULE_NAME)   /* a name rule never allows an IP */
            continue;
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

/* Decide whether a guest may dial a co-running guest by name. Like external
 * dialing this is default-allow: with no rules any peer is reachable; with
 * rules the name must match a NAME rule or the match-any "*". A CIDR rule is
 * about external addresses and never authorises a by-name link. */
static int
policy_allows_name(const struct net_policy *p, const char *name, uint16_t port,
                   int proto)
{
    if (p->n == 0)
        return 1;
    for (int i = 0; i < p->n; i++) {
        const struct net_rule *r = &p->rules[i];
        if (r->kind == NET_RULE_CIDR)
            continue;
        if (r->kind == NET_RULE_NAME && strcmp(r->name, name) != 0)
            continue;
        if (port < r->port_lo || port > r->port_hi)
            continue;
        if (r->proto != 0 && r->proto != proto)
            continue;
        return 1;
    }
    return 0;
}

/* Decide whether a guest may bind a real local listening port. Unlike dialing,
 * binding an external port is opt-in: with no rules a listen is in-host only,
 * and a real bind needs an explicit rule naming the port. The rule is matched
 * by port and protocol; its address range does not apply to a local bind. A
 * name rule is about in-host reach, not external exposure, so it never opens a
 * local port. */
static int
policy_allows_bind(const struct net_policy *p, uint16_t port, int proto)
{
    if (p->n == 0)
        return 0;
    for (int i = 0; i < p->n; i++) {
        const struct net_rule *r = &p->rules[i];
        if (r->kind == NET_RULE_NAME)
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

/* Find a free connection slot and reset its bookkeeping (caller holds
 * nd->lock). The caller then sets fd, proto, and peer. Returns an index, or -1
 * when the table is full. */
static int
alloc_conn(struct netdev *nd)
{
    for (int i = 0; i < NET_MAXCONN; i++)
        if (nd->conns[i].fd < 0) {
            struct net_conn *c = &nd->conns[i];
            c->in_poll = 0;
            c->is_listener = 0;
            c->vl = NULL;
            c->lfd = -1;
            c->lfd_in_poll = 0;
            c->uport = 0;
            c->uq_head = 0;
            c->uq_count = 0;
            return i;
        }
    return -1;
}

/* Parse a decimal port. Returns 0 on error (0 is not a valid service port). */
static uint16_t
parse_port(const char *s)
{
    long v = strtol(s, NULL, 10);
    if (v <= 0 || v > 65535)
        return 0;
    return (uint16_t)v;
}

/* Dial a co-running guest's service by name. The two guests are spliced with a
 * socketpair, so the data path is an ordinary fd and no traffic touches the
 * real network. The dialer keeps one end; the other is queued on the target's
 * accept backlog and its listen pipe is poked, so the target guest wakes and
 * accepts. */
static void
do_dial_inhost(struct netdev *nd, struct jotun_net *chan, const char *host,
               const char *portstr)
{
    struct vswitch *v = nd->vsw;
    uint16_t port = parse_port(portstr);
    if (!v || port == 0) {
        reply(chan, NET_EINVAL, 0);
        return;
    }

    /* Resolve the listener first, before committing any resources. The guest
     * may exist but not have called net_listen yet (the startup race): that is
     * not a refusal, it is "not ready", so the dial parks and retries until a
     * listen wakes it. A backlog that is genuinely full is a hard refusal. */
    pthread_mutex_lock(&v->lock);
    struct vlistener *vl = NULL;
    for (int i = 0; i < VSW_MAXLISTEN; i++)
        if (v->listeners[i].used && v->listeners[i].port == port
            && strcmp(v->listeners[i].name, host) == 0) {
            vl = &v->listeners[i];
            break;
        }
    if (!vl) {
        pthread_mutex_unlock(&v->lock);
        reply(chan, NET_EAGAIN, 0);
        return;
    }
    if (vl->count >= VSW_BACKLOG) {
        pthread_mutex_unlock(&v->lock);
        reply(chan, NET_ECONN, 0);
        return;
    }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        pthread_mutex_unlock(&v->lock);
        reply(chan, NET_ECONN, 0);
        return;
    }
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL, 0) | O_NONBLOCK);

    pthread_mutex_lock(&nd->lock);
    int handle = alloc_conn(nd);
    if (handle >= 0) {
        nd->conns[handle].fd = sv[0];
        nd->conns[handle].proto = NET_PROTO_TCP;
        nd->conns[handle].in_poll = 0;
        nd->conns[handle].is_listener = 0;
        nd->conns[handle].vl = NULL;
        snprintf(nd->conns[handle].peer, sizeof nd->conns[handle].peer,
                 "%.40s!%.16s", host, portstr);
    }
    pthread_mutex_unlock(&nd->lock);
    if (handle < 0) {
        pthread_mutex_unlock(&v->lock);
        close(sv[0]);
        close(sv[1]);
        reply(chan, NET_ENORES, 0);
        return;
    }

    int slot = (vl->head + vl->count) % VSW_BACKLOG;
    vl->fds[slot] = sv[1];
    snprintf(vl->peers[slot], sizeof vl->peers[0], "%s", nd->name);
    vl->count++;
    char b = 'x';
    ssize_t wr = write(vl->wake_wr, &b, 1);
    (void)wr;
    pthread_mutex_unlock(&v->lock);

    reply(chan, handle, 0);
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

    /* "network!host!port": split the network token, then host and port. */
    char *net_bang = strchr(target, '!');
    if (!net_bang) {
        reply(chan, NET_EINVAL, 0);
        return;
    }
    *net_bang = '\0';
    const char *network = target;
    char *rest = net_bang + 1;

    char *bang = strchr(rest, '!');
    if (!bang) {
        reply(chan, NET_EINVAL, 0);
        return;
    }
    *bang = '\0';
    const char *host = rest;
    const char *port = bang + 1;

    int proto;
    if (strcmp(network, "tcp") == 0)
        proto = NET_PROTO_TCP;
    else if (strcmp(network, "udp") == 0)
        proto = NET_PROTO_UDP;
    else {
        reply(chan, NET_EINVAL, 0);
        return;
    }

    /* The name service resolves the host field: a co-running guest's name is an
     * in-host link, anything else resolves externally below. */
    if (proto == NET_PROTO_TCP && vsw_is_guest(nd->vsw, host)) {
        if (!policy_allows_name(nd->policy, host, parse_port(port), proto)) {
            reply(chan, NET_EPERM, 0);
            return;
        }
        do_dial_inhost(nd, chan, host, port);
        return;
    }

    struct addrinfo hints = { 0 };
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = proto == NET_PROTO_UDP ? SOCK_DGRAM : SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        reply(chan, NET_ENORES, 0);
        return;
    }

    pthread_mutex_lock(&nd->lock);
    int handle = alloc_conn(nd);
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
        if (!policy_allows(nd->policy, ai->ai_addr, proto))
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
    nd->conns[handle].proto = proto;
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
    int handle = alloc_conn(nd);
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

/* Queue a datagram for an in-host UDP endpoint and wake its owner. The caller
 * holds the switch lock so the binding stays valid; this takes the target's nd
 * lock to enqueue. Returns 1 if queued, 0 if dropped (endpoint gone or its
 * queue full), matching UDP's lossy delivery. */
static int
udp_deliver(struct netdev *dst, int32_t handle, const char *src,
            const uint8_t *data, uint32_t len)
{
    int queued = 0;
    pthread_mutex_lock(&dst->lock);
    struct net_conn *c = conn_of(dst, handle);
    if (c && c->proto == NET_PROTO_UDP && c->uq_count < UDP_BACKLOG) {
        uint8_t *copy = NULL;
        if (len > 0 && (copy = malloc(len)) == NULL)
            goto out;               /* out of memory: drop the datagram */
        if (len > 0)
            memcpy(copy, data, len);
        struct udp_dgram *d = &c->uq[(c->uq_head + c->uq_count) % UDP_BACKLOG];
        d->data = copy;
        d->len = len;
        snprintf(d->src, sizeof d->src, "%s", src);
        c->uq_count++;
        queued = 1;
    }
out:
    pthread_mutex_unlock(&dst->lock);
    if (queued) {
        __atomic_store_n(&dst->chan->evt, 1, __ATOMIC_SEQ_CST);
        dst->wake(dst->wake_ctx);
    }
    return queued;
}

/* Send a datagram to a co-running guest by name. The datagram is routed through
 * the switch and never touches the real network. If the sender has no in-host
 * source port yet, one is assigned so the receiver can reply. A datagram to a
 * name that is not bound is dropped, as UDP is lossy. */
static void
do_sendto_inhost(struct netdev *nd, struct jotun_net *chan, const char *host,
                 const char *portstr)
{
    struct vswitch *v = nd->vsw;
    uint16_t port = parse_port(portstr);
    if (!v || port == 0) {
        reply(chan, NET_EINVAL, 0);
        return;
    }

    uint32_t len = chan->len;
    if (len > JOTUN_NET_MSIZE)
        len = JOTUN_NET_MSIZE;

    pthread_mutex_lock(&v->lock);

    /* Give the sender a source identity (its own in-host port) if it has none,
     * so a reply can find it, the way a kernel socket auto-binds on first send. */
    uint16_t sport = 0;
    pthread_mutex_lock(&nd->lock);
    struct net_conn *sc = conn_of(nd, chan->handle);
    if (!sc || sc->proto != NET_PROTO_UDP) {
        pthread_mutex_unlock(&nd->lock);
        pthread_mutex_unlock(&v->lock);
        reply(chan, NET_EINVAL, 0);
        return;
    }
    if (sc->uport == 0 && nd->name[0])
        sc->uport = vudp_autoport(v, nd->name, nd, chan->handle);
    sport = sc->uport;
    pthread_mutex_unlock(&nd->lock);

    char src[96];
    snprintf(src, sizeof src, "%s!%u", nd->name[0] ? nd->name : "?",
             (unsigned)sport);

    struct vudp *tgt = vudp_find(v, host, port);
    if (tgt)
        udp_deliver(tgt->owner, tgt->handle, src, chan->data, len);
    pthread_mutex_unlock(&v->lock);

    /* Report the datagram as sent whether or not anyone was listening. */
    reply(chan, (int32_t)len, 0);
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

    /* A datagram to a co-running guest's name is routed in-host, like a dial. */
    if (vsw_is_guest(nd->vsw, host)) {
        if (!policy_allows_name(nd->policy, host, parse_port(port),
                                NET_PROTO_UDP)) {
            reply(chan, NET_EPERM, 0);
            return;
        }
        do_sendto_inhost(nd, chan, host, port);
        return;
    }

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

    /* An in-host datagram waiting in the queue is delivered before anything the
     * real socket may hold, with its source reported as the sender's name. */
    if (c->uq_count > 0) {
        struct udp_dgram *d = &c->uq[c->uq_head];
        uint32_t n = d->len < max ? d->len : max;
        if (n > 0)
            memcpy(chan->data, d->data, n);
        uint32_t sn = (uint32_t)strlen(d->src);
        if (sn > JOTUN_NET_ADDRMAX - 1)
            sn = JOTUN_NET_ADDRMAX - 1;
        memcpy(chan->addr, d->src, sn);
        chan->addrlen = sn;
        free(d->data);
        d->data = NULL;
        c->uq_head = (c->uq_head + 1) % UDP_BACKLOG;
        c->uq_count--;
        pthread_mutex_unlock(&nd->lock);
        reply(chan, (int32_t)n, n);
        return;
    }

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

/* Bind a UDP handle to a port under this guest's name ("udp!*!port"), so peers
 * can reach it by name. Parallels net_listen for TCP, but a UDP socket needs no
 * accept: the bound port just routes datagrams to this endpoint. */
static void
do_udpbind(struct netdev *nd, struct jotun_net *chan)
{
    char target[256];
    uint32_t n = chan->len;
    if (n >= sizeof target) {
        reply(chan, NET_EINVAL, 0);
        return;
    }
    memcpy(target, chan->data, n);
    target[n] = '\0';

    char *net_bang = strchr(target, '!');
    if (!net_bang) {
        reply(chan, NET_EINVAL, 0);
        return;
    }
    *net_bang = '\0';
    char *rest = net_bang + 1;
    char *bang = strchr(rest, '!');
    if (!bang) {
        reply(chan, NET_EINVAL, 0);
        return;
    }
    *bang = '\0';
    const char *network = target;
    const char *host = rest;
    const char *portstr = bang + 1;

    if (strcmp(network, "udp") != 0 || strcmp(host, "*") != 0) {
        reply(chan, NET_EINVAL, 0);
        return;
    }
    uint16_t port = parse_port(portstr);
    if (port == 0) {
        reply(chan, NET_EINVAL, 0);
        return;
    }

    struct vswitch *v = nd->vsw;
    if (!v || !nd->name[0]) {
        reply(chan, NET_EPERM, 0);      /* an unnamed guest cannot be addressed */
        return;
    }

    pthread_mutex_lock(&v->lock);
    pthread_mutex_lock(&nd->lock);
    struct net_conn *c = conn_of(nd, chan->handle);
    if (!c || c->proto != NET_PROTO_UDP || c->uport != 0) {
        pthread_mutex_unlock(&nd->lock);
        pthread_mutex_unlock(&v->lock);
        reply(chan, NET_EINVAL, 0);     /* not a UDP socket, or already bound */
        return;
    }
    int rc = vudp_register(v, nd->name, port, nd, chan->handle);
    if (rc == 0) {
        c->uport = port;
        snprintf(c->peer, sizeof c->peer, "udp %.32s!%u", nd->name,
                 (unsigned)port);
    }
    pthread_mutex_unlock(&nd->lock);
    pthread_mutex_unlock(&v->lock);

    reply(chan, rc == 0 ? 0 : (rc == -1 ? NET_ECONN : NET_ENORES), 0);
}

/* Free a listener's switch registration: close its wake pipe and any unaccepted
 * backlog ends, and release the slot. */
static void
free_listener(struct vswitch *v, struct vlistener *vl)
{
    pthread_mutex_lock(&v->lock);
    if (vl->used) {
        close(vl->wake_wr);
        for (int i = 0; i < vl->count; i++)
            close(vl->fds[(vl->head + i) % VSW_BACKLOG]);
        vl->used = 0;
    }
    pthread_mutex_unlock(&v->lock);
}

static void
do_close(struct netdev *nd, struct jotun_net *chan)
{
    pthread_mutex_lock(&nd->lock);
    struct net_conn *c = conn_of(nd, chan->handle);
    struct vlistener *vl = NULL;
    int was_udp = 0;
    if (c) {
        poller_del(nd, c);
        if (c->is_listener)
            vl = c->vl;
        was_udp = c->proto == NET_PROTO_UDP;
        while (c->uq_count > 0) {        /* drop any undelivered datagrams */
            free(c->uq[c->uq_head].data);
            c->uq[c->uq_head].data = NULL;
            c->uq_head = (c->uq_head + 1) % UDP_BACKLOG;
            c->uq_count--;
        }
        close(c->fd);
        c->fd = -1;
        if (c->lfd >= 0) {
            close(c->lfd);
            c->lfd = -1;
        }
        c->is_listener = 0;
        c->vl = NULL;
        c->uport = 0;
    }
    pthread_mutex_unlock(&nd->lock);
    if (vl)
        free_listener(nd->vsw, vl);
    if (was_udp)
        vudp_unbind(nd->vsw, nd, chan->handle);
    reply(chan, 0, 0);
}

/* Bind a non-blocking TCP listening socket on all interfaces at port. Returns
 * the fd, or -1 if the socket, bind, or listen fails. */
static int
bind_listen(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    set_nosigpipe(fd);

    struct sockaddr_in sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}

/* Register a service under this guest's name ("tcp!*!port"). The service is
 * in-host by default; it also binds a real external port when the manifest
 * explicitly allows that port. The listener's wake pipe (in-host) or its real
 * socket (real-only) is its readiness fd, so a waiting accept parks like any
 * socket; a listener with both watches both. */
static void
do_listen(struct netdev *nd, struct jotun_net *chan)
{
    char target[256];
    uint32_t n = chan->len;
    if (n >= sizeof target) {
        reply(chan, NET_EINVAL, 0);
        return;
    }
    memcpy(target, chan->data, n);
    target[n] = '\0';

    char *net_bang = strchr(target, '!');
    if (!net_bang) {
        reply(chan, NET_EINVAL, 0);
        return;
    }
    *net_bang = '\0';
    const char *network = target;
    char *rest = net_bang + 1;
    char *bang = strchr(rest, '!');
    if (!bang) {
        reply(chan, NET_EINVAL, 0);
        return;
    }
    *bang = '\0';
    const char *host = rest;
    const char *portstr = bang + 1;

    if (strcmp(network, "tcp") != 0 || strcmp(host, "*") != 0) {
        reply(chan, NET_EINVAL, 0);     /* only "tcp!*!port" for now */
        return;
    }
    uint16_t port = parse_port(portstr);
    if (port == 0) {
        reply(chan, NET_EINVAL, 0);
        return;
    }

    struct vswitch *v = nd->vsw;
    int want_inhost = v && nd->name[0];

    /* Bind a real external port when the manifest explicitly allows it; with no
     * such rule the service is in-host only. */
    int rfd = -1;
    if (policy_allows_bind(nd->policy, port, NET_PROTO_TCP)) {
        rfd = bind_listen(port);
        if (rfd < 0) {
            reply(chan, NET_ECONN, 0);      /* port in use or bind refused */
            return;
        }
    }
    if (!want_inhost && rfd < 0) {
        reply(chan, NET_EPERM, 0);  /* no name and no permitted external port */
        return;
    }

    /* Register the in-host service (with its wake pipe) when the guest is
     * named. A real-only listener skips this and uses rfd as its fd. */
    struct vlistener *vl = NULL;
    int pipe_rd = -1;
    if (want_inhost) {
        int p[2];
        if (pipe(p) != 0) {
            if (rfd >= 0)
                close(rfd);
            reply(chan, NET_ECONN, 0);
            return;
        }
        fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL, 0) | O_NONBLOCK);
        fcntl(p[1], F_SETFL, fcntl(p[1], F_GETFL, 0) | O_NONBLOCK);

        pthread_mutex_lock(&v->lock);
        for (int i = 0; i < VSW_MAXLISTEN; i++)
            if (v->listeners[i].used && v->listeners[i].port == port
                && strcmp(v->listeners[i].name, nd->name) == 0) {
                pthread_mutex_unlock(&v->lock);
                close(p[0]);
                close(p[1]);
                if (rfd >= 0)
                    close(rfd);
                reply(chan, NET_EINVAL, 0);     /* already listening */
                return;
            }
        for (int i = 0; i < VSW_MAXLISTEN; i++)
            if (!v->listeners[i].used) {
                vl = &v->listeners[i];
                break;
            }
        if (!vl) {
            pthread_mutex_unlock(&v->lock);
            close(p[0]);
            close(p[1]);
            if (rfd >= 0)
                close(rfd);
            reply(chan, NET_ENORES, 0);
            return;
        }
        memset(vl, 0, sizeof *vl);
        vl->used = 1;
        snprintf(vl->name, sizeof vl->name, "%s", nd->name);
        vl->port = port;
        vl->owner = nd;
        vl->wake_wr = p[1];
        pthread_mutex_unlock(&v->lock);
        pipe_rd = p[0];
    }

    /* The connection's primary fd is the wake pipe for an in-host listener, or
     * the real socket for a real-only one; the real socket (if any) also lives
     * in lfd so accept can watch both. */
    pthread_mutex_lock(&nd->lock);
    int handle = alloc_conn(nd);
    if (handle >= 0) {
        struct net_conn *c = &nd->conns[handle];
        c->fd = want_inhost ? pipe_rd : rfd;
        c->proto = NET_PROTO_TCP;
        c->is_listener = 1;
        c->vl = vl;
        c->lfd = want_inhost ? rfd : -1;
        snprintf(c->peer, sizeof c->peer, "listen %.32s!%u",
                 want_inhost ? nd->name : "*", (unsigned)port);
    }
    pthread_mutex_unlock(&nd->lock);
    if (handle < 0) {
        if (vl)
            free_listener(v, vl);
        if (pipe_rd >= 0)
            close(pipe_rd);
        if (rfd >= 0)
            close(rfd);
        reply(chan, NET_ENORES, 0);
        return;
    }

    /* A new in-host service may have been racing ahead of a dialer that parked
     * on NET_EAGAIN; wake every guest so those dials retry and find it now. */
    if (want_inhost)
        vsw_wake_all(v);

    reply(chan, handle, 0);
}

/* Accept one connection on a listening handle: a pending in-host peer first,
 * then a real inbound connection, else park on both sources. */
static void
do_accept(struct netdev *nd, struct jotun_net *chan)
{
    pthread_mutex_lock(&nd->lock);
    struct net_conn *c = conn_of(nd, chan->handle);
    if (!c || !c->is_listener) {
        pthread_mutex_unlock(&nd->lock);
        reply(chan, NET_EINVAL, 0);
        return;
    }
    struct vlistener *vl = c->vl;
    int wake_fd = vl ? c->fd : -1;          /* in-host wake pipe, if any */
    int real_fd = vl ? c->lfd : c->fd;      /* real listening socket, or -1 */
    pthread_mutex_unlock(&nd->lock);

    int newfd = -1;
    char peer[64] = "";

    if (vl) {
        struct vswitch *v = nd->vsw;
        pthread_mutex_lock(&v->lock);
        if (vl->count > 0) {
            newfd = vl->fds[vl->head];
            snprintf(peer, sizeof peer, "%s", vl->peers[vl->head]);
            vl->head = (vl->head + 1) % VSW_BACKLOG;
            vl->count--;
        }
        pthread_mutex_unlock(&v->lock);
        if (newfd >= 0) {
            char one;
            ssize_t rd = read(wake_fd, &one, 1);        /* consume one byte */
            (void)rd;
        }
    }

    if (newfd < 0 && real_fd >= 0) {
        struct sockaddr_in sa;
        socklen_t sl = sizeof sa;
        int a = accept(real_fd, (struct sockaddr *)&sa, &sl);
        if (a >= 0) {
            set_nosigpipe(a);
            fcntl(a, F_SETFL, fcntl(a, F_GETFL, 0) | O_NONBLOCK);
            newfd = a;
            uint32_t ip = ntohl(sa.sin_addr.s_addr);
            snprintf(peer, sizeof peer, "%u.%u.%u.%u!%u",
                     (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff,
                     ip & 0xff, (unsigned)ntohs(sa.sin_port));
        }
    }

    if (newfd < 0) {
        /* Nothing pending: drain stale in-host wake bytes and park on whichever
         * sources this listener has, so a later dial or inbound wakes it. */
        if (wake_fd >= 0) {
            char drain[64];
            while (read(wake_fd, drain, sizeof drain) > 0)
                ;
        }
        pthread_mutex_lock(&nd->lock);
        c = conn_of(nd, chan->handle);
        if (c) {
            poller_arm_fd(nd, c->fd, &c->in_poll, NET_WANT_READ);
            if (c->lfd >= 0)
                poller_arm_fd(nd, c->lfd, &c->lfd_in_poll, NET_WANT_READ);
        }
        pthread_mutex_unlock(&nd->lock);
        reply(chan, NET_EAGAIN, 0);
        return;
    }

    pthread_mutex_lock(&nd->lock);
    int handle = alloc_conn(nd);
    if (handle >= 0) {
        nd->conns[handle].fd = newfd;
        nd->conns[handle].proto = NET_PROTO_TCP;
        snprintf(nd->conns[handle].peer, sizeof nd->conns[handle].peer,
                 "%s", peer[0] ? peer : "-");
    }
    pthread_mutex_unlock(&nd->lock);
    if (handle < 0) {
        close(newfd);
        reply(chan, NET_ENORES, 0);
        return;
    }

    uint32_t pn = (uint32_t)strlen(peer);
    if (pn > JOTUN_NET_ADDRMAX)
        pn = JOTUN_NET_ADDRMAX;
    memcpy(chan->addr, peer, pn);
    chan->addrlen = pn;
    reply(chan, handle, 0);
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
    case NET_OP_LISTEN:   do_listen(nd, chan);   break;
    case NET_OP_ACCEPT:   do_accept(nd, chan);   break;
    case NET_OP_UDPBIND:  do_udpbind(nd, chan);  break;
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
            nd->wake(nd->wake_ctx);
        }
    }
    return NULL;
}

struct netdev *
net_init(struct jotun_net *chan, void (*wake)(void *ctx), void *wake_ctx,
         const struct net_policy *policy, struct vswitch *vsw,
         const char *name)
{
    struct netdev *nd = calloc(1, sizeof(*nd));
    if (!nd)
        return NULL;

    for (int i = 0; i < NET_MAXCONN; i++) {
        nd->conns[i].fd = -1;
        nd->conns[i].lfd = -1;
    }
    nd->chan = chan;
    nd->wake = wake;
    nd->wake_ctx = wake_ctx;
    nd->policy = policy;
    nd->vsw = vsw;
    if (name)
        snprintf(nd->name, sizeof nd->name, "%s", name);
    vsw_register_name(vsw, nd->name);
    vsw_add_member(vsw, nd);
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

    for (int i = 0; i < NET_MAXCONN; i++) {
        struct net_conn *c = &nd->conns[i];
        if (c->is_listener && c->vl)
            free_listener(nd->vsw, c->vl);
        if (c->proto == NET_PROTO_UDP && c->fd >= 0)
            vudp_unbind(nd->vsw, nd, i);
        while (c->uq_count > 0) {
            free(c->uq[c->uq_head].data);
            c->uq[c->uq_head].data = NULL;
            c->uq_head = (c->uq_head + 1) % UDP_BACKLOG;
            c->uq_count--;
        }
        if (c->fd >= 0)
            close(c->fd);
        if (c->lfd >= 0)
            close(c->lfd);
    }
    poller_destroy(nd);
    free(nd);
}
