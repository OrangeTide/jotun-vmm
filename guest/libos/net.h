/* net.h : the guest net client.
 *
 * A socket-like API over the net channel. Each call is one synchronous RPC to
 * the host; a call that would block parks the task (not the vcpu) until a net
 * event wakes it, so other tasks keep running. The channel carries one request
 * at a time, so a lock serialises concurrent callers. */
#ifndef LIBOS_NET_H
#define LIBOS_NET_H

#include "jotun_net.h"

/* Register the net event hook with the scheduler. Call once at boot. */
void net_init(void);

/* Open a connection to target ("host!port") over the given NET_PROTO_*.
 * Returns a non-negative handle, or a negative NET_* error. */
int net_dial(int proto, const char *target);

/* Write n bytes, blocking until all are sent. Returns n, or a negative NET_*
 * error. */
long net_write(int handle, const void *buf, unsigned long n);

/* Read up to max bytes, blocking until at least one arrives. Returns the byte
 * count (0 means the peer closed), or a negative NET_* error. */
long net_read(int handle, void *buf, unsigned long max);

/* Close a connection. Returns 0, or a negative NET_* error. */
int net_close(int handle);

#endif /* LIBOS_NET_H */
