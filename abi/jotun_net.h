/* jotun_net.h : the networking channel between the guest net client and the
 * host net provider.
 *
 * Shared verbatim by both sides, so it uses only fixed-width integer types and
 * no libc. Like the 9P channel it is one outstanding request at a time: the
 * guest fills the fields, stores to the net doorbell (a synchronous MMIO exit),
 * and the host writes the result back before the store returns. Every host
 * operation is non-blocking; an operation that would block returns NET_EAGAIN
 * and the guest parks the calling task until a net readiness event arrives.
 *
 * Readiness is signalled out of band: when a socket a parked task is waiting on
 * becomes ready, the host sets `evt` and raises the event interrupt (and wakes
 * any idle vcpu). The guest clears `evt` and re-readies its parked net tasks,
 * which then retry. This keeps the data path synchronous while reads and writes
 * still block at task granularity. */

#ifndef JOTUN_NET_H
#define JOTUN_NET_H

#include <stdint.h>

/* The channel sits well above the 9P channel (jotun_9p.h ends near 0xC010) and
 * below the guest image at 1 MiB. The doorbell follows the SMP doorbells
 * (jotun_abi.h). */
#define JOTUN_NET_BASE      0x10000u
#define JOTUN_NET_DOORBELL  0x0a000020ull
#define JOTUN_NET_MSIZE     8192u
#define JOTUN_NET_ADDRMAX   128u        /* a "host!port" address string */

/* Operations (guest to host). */
enum {
    NET_OP_DIAL     = 1,    /* open a connection to the dial string in data[] */
    NET_OP_SEND     = 2,    /* write len bytes of data[] to a connection */
    NET_OP_RECV     = 3,    /* read up to maxlen bytes from a connection */
    NET_OP_CLOSE    = 4,    /* close a connection */
    NET_OP_UDP      = 5,    /* open an unconnected UDP socket */
    NET_OP_SENDTO   = 6,    /* send a datagram (data[]) to addr[] "host!port" */
    NET_OP_RECVFROM = 7,    /* receive a datagram, addr[] filled with the source */
    NET_OP_LISTEN   = 8,    /* bind/register the dial string in data[], return a
                               listening handle */
    NET_OP_ACCEPT   = 9,    /* accept one connection on a listening handle, the
                               new connection's handle in status, the peer in
                               addr[] */
};

/* A dial string is a Plan 9 style "network!host!port":
 *   tcp!host!port      a TCP connection
 *   udp!host!port      a UDP connection
 * NET_OP_DIAL and NET_OP_LISTEN both take this form. The host runs one name
 * service over the host field: a name assigned to a co-running guest resolves
 * to an in-host link, and any other name resolves externally (DNS). An in-host
 * link is backed by a socketpair, so its data path (send/recv/readiness) is
 * identical to a real socket, and the external and inter-guest cases differ
 * only in how the name resolved. There is no path to the real network except
 * through the host.
 *
 * NET_PROTO_* classify a live connection for the /net information tree and the
 * policy rules. */
enum {
    NET_PROTO_TCP = 1,
    NET_PROTO_UDP = 2,
};

/* status (host to guest): a non-negative value is a connection handle (DIAL) or
 * a byte count (SEND, and RECV where 0 means the peer closed). A negative value
 * is one of these codes. */
#define NET_EAGAIN  (-1)    /* would block: park the task and retry */
#define NET_EPERM   (-2)    /* policy refused the target */
#define NET_EINVAL  (-3)    /* malformed request */
#define NET_ECONN   (-4)    /* connect failed, or the connection broke */
#define NET_ENORES  (-5)    /* no connection slot, or address did not resolve */

struct jotun_net {
    uint32_t op;                /* NET_OP_* (guest to host) */
    uint32_t reserved;          /* was proto; the dial string now carries it */
    int32_t  handle;            /* connection handle for SEND/RECV/CLOSE (in) */
    int32_t  status;            /* result code, handle, or byte count (out) */
    uint32_t len;               /* data length: SEND in, RECV out */
    uint32_t maxlen;            /* RECV: max bytes to read (guest to host) */
    volatile uint32_t evt;      /* host sets on a net event; guest clears it */
    uint32_t addrlen;           /* addr length: SENDTO in, RECVFROM out */
    uint8_t  addr[JOTUN_NET_ADDRMAX];   /* "host!port" for SENDTO/RECVFROM */
    uint8_t  data[JOTUN_NET_MSIZE];
};

#endif /* JOTUN_NET_H */
