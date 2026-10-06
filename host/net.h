/* net.h : the host net provider.
 *
 * Opens real host sockets on the guest's behalf (that is the NAT) and keeps
 * them non-blocking. A background poll thread blocks in epoll and, when a
 * socket a guest task is waiting on becomes ready, signals the guest through a
 * caller-supplied wake callback. Policy (which targets a guest may dial) is a
 * stub here and becomes the manifest network rules in a later step. */
#ifndef NET_H
#define NET_H

#include "jotun_net.h"

struct netdev;

/* Create the provider. chan is the guest net channel in host memory (where the
 * poll thread sets the readiness flag). wake is called from the poll thread
 * when a registered socket becomes ready; it must rouse the guest (inject the
 * event interrupt into running vcpus and wake idle ones). Returns NULL on
 * failure. */
struct netdev *net_init(struct jotun_net *chan, void (*wake)(void));

/* Handle one request already written into the channel, writing the result back
 * into it. Called from the net doorbell. */
void net_handle(struct netdev *nd, struct jotun_net *chan);

/* Stop the poll thread and close every connection. */
void net_shutdown(struct netdev *nd);

#endif /* NET_H */
