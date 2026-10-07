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

/* Open a connection to a dial string "network!host!port" (network is tcp or
 * udp). The host resolves the host field: a co-running guest's name gives an
 * in-host link, any other name resolves externally. Returns a non-negative
 * handle, or a negative NET_* error. */
int net_dial(const char *target);

/* Offer a service named by a dial string and return a listening handle, or a
 * negative NET_* error. "tcp!*!port" listens in-host under this guest's name,
 * so a peer reaches it by dialing "tcp!<thisguest>!port". */
int net_listen(const char *target);

/* Accept one connection on a listening handle, blocking the calling task until
 * one arrives. Returns the new connection's handle (use net_read/net_write/
 * net_close on it), or a negative NET_* error. The dialer's name is written to
 * peer, truncated to peercap. */
int net_accept(int handle, char *peer, unsigned long peercap);

/* Write n bytes, blocking until all are sent. Returns n, or a negative NET_*
 * error. */
long net_write(int handle, const void *buf, unsigned long n);

/* Read up to max bytes, blocking until at least one arrives. Returns the byte
 * count (0 means the peer closed), or a negative NET_* error. */
long net_read(int handle, void *buf, unsigned long max);

/* Open an unconnected UDP socket. Returns a handle, or a negative NET_* error.
 * Use net_sendto/net_recvfrom with it; close it with net_close. */
int net_udp(void);

/* Send one datagram of n bytes to dst ("host!port"), blocking if the socket is
 * momentarily full. Returns the byte count, or a negative NET_* error. */
long net_sendto(int handle, const char *dst, const void *buf, unsigned long n);

/* Receive one datagram, up to max bytes, blocking until one arrives. The
 * source is written to src as "host!port" (truncated to srccap). Returns the
 * datagram's byte count, or a negative NET_* error. */
long net_recvfrom(int handle, char *src, unsigned long srccap,
                  void *buf, unsigned long max);

/* Close a connection or UDP socket. Returns 0, or a negative NET_* error. */
int net_close(int handle);

#endif /* LIBOS_NET_H */
