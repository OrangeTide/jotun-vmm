/* jotun_broker.h : the control channel between a broker guest and the host.
 *
 * This is the M15.2 step-two design, defined ahead of its implementation so the
 * step is additive rather than a rewrite. Step one shipped as `brokerd`, a
 * host process that spawns one `jotun` child per browser session; this channel
 * is how a future sandboxed broker guest would drive the same mechanism from
 * inside a guest instead, moving policy out of host C. Nothing wires it into the
 * trusted core yet. host/brokerctl.c is a host-side reference (mock) of the
 * operations below, exercised by a unit test, so the protocol can be developed
 * and verified before the core surgery lands.
 *
 * Shared verbatim by both sides, so it uses only fixed-width integer types and
 * no libc. Like the net and 9P channels it is one outstanding request at a time:
 * the broker fills the fields, stores to the broker doorbell (a synchronous MMIO
 * exit), and the host writes the result back before the store returns. The
 * channel is granted only to a guest marked `broker` in its manifest.
 *
 * Session lifecycle events (a browser connected, disconnected, or a session
 * guest exited) are delivered out of band: the host enqueues them and sets
 * `evt`, raising the event interrupt. The broker clears `evt` and drains the
 * queue with BROKER_OP_EVENT until it reports BROKER_EV_NONE. The host enforces
 * quotas on every operation (max sessions, total RAM, spawn rate), so a buggy or
 * hostile broker is at worst bounded denial of service, never a sandbox escape
 * or a read of another guest's memory.
 *
 * The base and doorbell addresses are provisional until the channel is wired;
 * they are placed above the net channel (jotun_net.h) and below the guest image
 * at 1 MiB, with the doorbell following the net doorbell. */

#ifndef JOTUN_BROKER_H
#define JOTUN_BROKER_H

#include <stdint.h>

#define JOTUN_BROKER_BASE       0x14000u
#define JOTUN_BROKER_DOORBELL   0x0a000028ull
#define JOTUN_BROKER_BUNDLEMAX  256u        /* bundle ref / name */
#define JOTUN_BROKER_PARAMMAX   512u        /* init params passed to the guest */
#define JOTUN_BROKER_TOKENMAX   64u         /* session routing token */

/* Operations (broker guest to host). */
enum {
    BROKER_OP_SPAWN   = 1,  /* create a session guest from bundle[] with params[]
                               and the limit_* fields; status = handle or error */
    BROKER_OP_DESTROY = 2,  /* destroy the session named by handle */
    BROKER_OP_BIND    = 3,  /* bind token[] to handle, so the host routes the
                               browser carrying that token to this session */
    BROKER_OP_EVENT   = 4,  /* dequeue one lifecycle event into evcode/evhandle;
                               BROKER_EV_NONE when the queue is empty */
};

/* Lifecycle event codes (host to broker), reported by BROKER_OP_EVENT in
 * evcode, with the session in evhandle. */
enum {
    BROKER_EV_NONE         = 0,
    BROKER_EV_CONNECTED    = 1,     /* a browser attached to the session */
    BROKER_EV_DISCONNECTED = 2,     /* the browser's draw connection closed */
    BROKER_EV_EXITED       = 3,     /* the session guest exited or was reaped */
};

/* status (host to broker): a positive value is a session handle (SPAWN). Zero is
 * success (DESTROY, BIND) or, for EVENT, BROKER_EV_NONE. A negative value is one
 * of these codes. */
#define BROKER_EPERM    (-1)    /* not a broker guest, or operation not allowed */
#define BROKER_EINVAL   (-2)    /* malformed request */
#define BROKER_ENOENT   (-3)    /* no session with that handle */
#define BROKER_EQUOTA   (-4)    /* a quota (sessions, RAM, rate) would be exceeded */
#define BROKER_ENORES   (-5)    /* spawn failed for want of a host resource */

struct jotun_broker {
    uint32_t op;                /* BROKER_OP_* (broker to host) */
    int32_t  handle;            /* session handle: in for DESTROY/BIND */
    int32_t  status;            /* result: handle, 0, event code, or error (out) */
    volatile uint32_t evt;      /* host sets when events are pending; guest clears */

    uint64_t limit_mem;         /* SPAWN: address-space cap in bytes (0 = default) */
    uint32_t limit_cpu;         /* SPAWN: CPU-time cap in seconds (0 = default) */

    int32_t  evcode;            /* EVENT: BROKER_EV_* (out) */
    int32_t  evhandle;          /* EVENT: the session the event is about (out) */

    uint32_t bundlelen;         /* SPAWN: length of bundle[] */
    uint32_t paramlen;          /* SPAWN: length of params[] */
    uint32_t tokenlen;          /* BIND: length of token[] */

    uint8_t  bundle[JOTUN_BROKER_BUNDLEMAX];
    uint8_t  params[JOTUN_BROKER_PARAMMAX];
    uint8_t  token[JOTUN_BROKER_TOKENMAX];
};

#endif /* JOTUN_BROKER_H */
