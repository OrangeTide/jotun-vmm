/* brokerctl.c : a host-side reference (mock) of the broker control channel.
 *
 * See brokerctl.h. This implements the jotun_broker.h operations against an
 * in-memory session table and event queue, without spawning any VM, so the
 * broker-guest protocol has something to run against before the real host
 * mechanism is wired in (M15.2 step two). The operation semantics, status
 * codes, and event delivery are meant to match what the real host will do. */
#include "brokerctl.h"

#include <string.h>

void
brokerctl_init(struct brokerctl *b, int max_sessions)
{
    memset(b, 0, sizeof *b);
    b->next_handle = 1;                 /* 0 is never a valid handle */
    b->max_sessions = max_sessions;
}

static int
count_sessions(const struct brokerctl *b)
{
    int n = 0;
    for (int i = 0; i < BROKERCTL_MAX; i++)
        if (b->s[i].inuse)
            n++;
    return n;
}

static struct brokerctl_session *
find(struct brokerctl *b, int32_t handle)
{
    if (handle <= 0)
        return NULL;
    for (int i = 0; i < BROKERCTL_MAX; i++)
        if (b->s[i].inuse && b->s[i].handle == handle)
            return &b->s[i];
    return NULL;
}

void
brokerctl_post_event(struct brokerctl *b, int32_t code, int32_t handle)
{
    int next = (b->evtail + 1) % BROKERCTL_EVQ;
    if (next == b->evhead)
        return;                         /* queue full: drop, as a bounded host would */
    b->evq[b->evtail].code = code;
    b->evq[b->evtail].handle = handle;
    b->evtail = next;
}

static int
events_pending(const struct brokerctl *b)
{
    return b->evhead != b->evtail;
}

static void
op_spawn(struct brokerctl *b, struct jotun_broker *m)
{
    if (count_sessions(b) >= b->max_sessions) {
        m->status = BROKER_EQUOTA;
        return;
    }
    for (int i = 0; i < BROKERCTL_MAX; i++)
        if (!b->s[i].inuse) {
            b->s[i].inuse = 1;
            b->s[i].handle = b->next_handle++;
            b->s[i].tokenlen = 0;
            m->status = b->s[i].handle;
            return;
        }
    m->status = BROKER_ENORES;          /* table full below the quota */
}

static void
op_destroy(struct brokerctl *b, struct jotun_broker *m)
{
    struct brokerctl_session *s = find(b, m->handle);
    if (!s) {
        m->status = BROKER_ENOENT;
        return;
    }
    s->inuse = 0;
    m->status = 0;
}

static void
op_bind(struct brokerctl *b, struct jotun_broker *m)
{
    struct brokerctl_session *s = find(b, m->handle);
    if (!s) {
        m->status = BROKER_ENOENT;
        return;
    }
    if (m->tokenlen > JOTUN_BROKER_TOKENMAX) {
        m->status = BROKER_EINVAL;
        return;
    }
    memcpy(s->token, m->token, m->tokenlen);
    s->tokenlen = m->tokenlen;
    m->status = 0;
}

static void
op_event(struct brokerctl *b, struct jotun_broker *m)
{
    if (!events_pending(b)) {
        m->evcode = BROKER_EV_NONE;
        m->evhandle = 0;
        m->status = BROKER_EV_NONE;
        return;
    }
    m->evcode = b->evq[b->evhead].code;
    m->evhandle = b->evq[b->evhead].handle;
    m->status = m->evcode;
    b->evhead = (b->evhead + 1) % BROKERCTL_EVQ;
}

void
brokerctl_handle(struct brokerctl *b, struct jotun_broker *m)
{
    switch (m->op) {
    case BROKER_OP_SPAWN:   op_spawn(b, m);   break;
    case BROKER_OP_DESTROY: op_destroy(b, m); break;
    case BROKER_OP_BIND:    op_bind(b, m);    break;
    case BROKER_OP_EVENT:   op_event(b, m);   break;
    default:                m->status = BROKER_EINVAL; break;
    }
    m->evt = events_pending(b) ? 1u : 0u;
}
