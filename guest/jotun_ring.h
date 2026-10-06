/* jotun_ring.h : guest-side ring helpers over the Jotun transport ABI.
 *
 * Header-only inline functions for a freestanding guest. The ring lives at a
 * fixed guest-physical address; a store to the doorbell is a synchronous MMIO
 * exit, so the host has drained the request and filled the response by the
 * time the store returns. */

#ifndef JOTUN_RING_H
#define JOTUN_RING_H

#include <stdint.h>
#include "jotun_abi.h"

static inline struct jotun_ring *
jotun_ring(void)
{
    return (struct jotun_ring *)(uintptr_t)JOTUN_RING_BASE;
}

static inline void
jotun_copy(uint8_t *dst, const uint8_t *src, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++)
        dst[i] = src[i];
}

/* Place a request in the ring and ring the doorbell. Returns on completion,
 * with the response already produced by the host. */
static inline void
jotun_send(const void *buf, uint16_t len)
{
    struct jotun_ring *r = jotun_ring();
    if (len > JOTUN_MSG_MAX)
        len = JOTUN_MSG_MAX;

    struct jotun_msg *m = &r->req[r->req_head % JOTUN_RING_SLOTS];
    m->len = len;
    jotun_copy(m->data, (const uint8_t *)buf, len);
    r->req_head++;

    *(volatile uint32_t *)(uintptr_t)JOTUN_DOORBELL = 1;
}

/* Take the next response, if any. Returns its length, or 0 when none. */
static inline uint16_t
jotun_recv(void *buf, uint16_t cap)
{
    struct jotun_ring *r = jotun_ring();
    if (r->rsp_tail == r->rsp_head)
        return 0;

    struct jotun_msg *m = &r->rsp[r->rsp_tail % JOTUN_RING_SLOTS];
    uint16_t len = m->len;
    if (len > cap)
        len = cap;
    jotun_copy((uint8_t *)buf, m->data, len);
    r->rsp_tail++;

    r->evt_pending &= ~JOTUN_EVT_RSP;   /* acknowledge the event */
    return len;
}

#endif /* JOTUN_RING_H */
