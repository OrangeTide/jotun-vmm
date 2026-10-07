/* net.h : the host net provider.
 *
 * Opens real host sockets on the guest's behalf (that is the NAT) and keeps
 * them non-blocking. A background poll thread blocks in epoll and, when a
 * socket a guest task is waiting on becomes ready, signals the guest through a
 * caller-supplied wake callback. Policy (which targets a guest may dial) is a
 * stub here and becomes the manifest network rules in a later step. */
#ifndef NET_H
#define NET_H

#include <stdint.h>

#include "jotun_net.h"

struct netdev;

/* The process-wide in-host name service and switch, shared by every guest's
 * provider. Create one and pass it to each net_init. */
struct vswitch;
struct vswitch *vswitch_new(void);

#define NET_MAXRULES 16

/* One network-policy rule: an IPv4 CIDR, an inclusive port range, and a
 * protocol (0 means any). A target is allowed if it matches any rule. */
struct net_rule {
    uint32_t addr;          /* network address, host byte order */
    uint32_t mask;          /* netmask derived from the prefix length */
    uint16_t port_lo;
    uint16_t port_hi;
    int      proto;         /* 0 = any, else NET_PROTO_TCP / NET_PROTO_UDP */
};

/* The resolved policy. With no rules every target is allowed; with one or more
 * rules the default is to deny, and only matching targets pass. */
struct net_policy {
    struct net_rule rules[NET_MAXRULES];
    int n;
};

/* Create the provider. chan is the guest net channel in host memory (where the
 * poll thread sets the readiness flag). wake is called from the poll thread
 * when a registered socket becomes ready, with wake_ctx; it must rouse the
 * guest that owns this provider (inject the event interrupt into running vcpus
 * and wake idle ones). policy is borrowed and must outlive the provider.
 * Returns NULL on failure.
 *
 * vsw is the shared in-host switch (or NULL for no inter-guest networking), and
 * name is this guest's network name (or NULL/"" if it has none, in which case
 * the guest cannot be dialed by name or offer in-host services). */
struct netdev *net_init(struct jotun_net *chan, void (*wake)(void *ctx),
                        void *wake_ctx, const struct net_policy *policy,
                        struct vswitch *vsw, const char *name);

/* Handle one request already written into the channel, writing the result back
 * into it. Called from the net doorbell. */
void net_handle(struct netdev *nd, struct jotun_net *chan);

/* A snapshot of one live connection, for the /net information tree. */
struct net_conninfo {
    int  handle;
    int  proto;             /* NET_PROTO_* */
    char peer[64];          /* "host!port" for a TCP dial, "-" for UDP */
};

/* Copy up to max live connections into out. Returns the number copied. */
int net_conn_snapshot(struct netdev *nd, struct net_conninfo *out, int max);

/* Stop the poll thread and close every connection. */
void net_shutdown(struct netdev *nd);

#endif /* NET_H */
