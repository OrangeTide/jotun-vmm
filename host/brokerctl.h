/* brokerctl.h : the host side of the broker control channel (jotun_broker.h).
 *
 * A reference (mock) implementation, so the broker-guest protocol can be
 * developed and tested before the trusted-core wiring of M15.2 step two exists.
 * It models sessions and the event queue in memory and never spawns a VM. The
 * real host side will replace the mock bodies with runtime guest spawn, destroy,
 * token-to-session routing, and lifecycle events, keeping this same ABI. */
#ifndef BROKERCTL_H
#define BROKERCTL_H

#include "jotun_broker.h"

#define BROKERCTL_MAX   64      /* session slots in the mock */
#define BROKERCTL_EVQ   128     /* pending lifecycle events */

struct brokerctl_session {
    int      inuse;
    int32_t  handle;
    uint8_t  token[JOTUN_BROKER_TOKENMAX];
    uint32_t tokenlen;
};

struct brokerctl {
    struct brokerctl_session s[BROKERCTL_MAX];
    int32_t  next_handle;       /* monotonic, so handles are not reused */
    int      max_sessions;      /* the session quota */
    struct {
        int32_t code;
        int32_t handle;
    } evq[BROKERCTL_EVQ];
    int      evhead, evtail;
};

/* Reset the mock, with max_sessions as the spawn quota. */
void brokerctl_init(struct brokerctl *b, int max_sessions);

/* Perform msg->op against the mock, writing msg->status (and, for an event,
 * msg->evcode/evhandle). Sets msg->evt to whether events remain queued. */
void brokerctl_handle(struct brokerctl *b, struct jotun_broker *msg);

/* Enqueue a lifecycle event, as the host would when a browser connects or
 * disconnects or a session guest exits. */
void brokerctl_post_event(struct brokerctl *b, int32_t code, int32_t handle);

#endif /* BROKERCTL_H */
