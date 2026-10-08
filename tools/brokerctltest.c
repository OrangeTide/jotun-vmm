/* brokerctltest.c : a round-trip test of the broker control channel ABI.
 *
 * Drives the host-side mock (host/brokerctl.c) the way a broker guest would
 * drive the channel over the shared struct (abi/jotun_broker.h): spawn sessions
 * up to the quota, bind tokens, drain lifecycle events, and destroy. It checks
 * the status codes, the quota, the unknown-handle errors, and that a freed slot
 * can be spawned into again. This exercises the ABI and the protocol ahead of
 * any trusted-core wiring. Exits 0 on success, 1 on failure. */
#include "brokerctl.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void
expect(int cond, const char *what)
{
    if (!cond) {
        fails++;
        printf("  FAIL %s\n", what);
    }
}

/* One request/response round trip over a fresh message struct. */
static void
msg_reset(struct jotun_broker *m, int op)
{
    memset(m, 0, sizeof *m);
    m->op = (uint32_t)op;
}

int
main(void)
{
    struct brokerctl b;
    struct jotun_broker m;
    brokerctl_init(&b, 2);              /* quota: two sessions */

    /* Spawn up to the quota, then be refused. */
    msg_reset(&m, BROKER_OP_SPAWN);
    brokerctl_handle(&b, &m);
    int h1 = m.status;
    expect(h1 == 1, "spawn 1 -> handle 1");

    msg_reset(&m, BROKER_OP_SPAWN);
    brokerctl_handle(&b, &m);
    int h2 = m.status;
    expect(h2 == 2, "spawn 2 -> handle 2");

    msg_reset(&m, BROKER_OP_SPAWN);
    brokerctl_handle(&b, &m);
    expect(m.status == BROKER_EQUOTA, "spawn 3 -> EQUOTA");

    /* Bind a token to a live session, and reject an unknown handle. */
    msg_reset(&m, BROKER_OP_BIND);
    m.handle = h1;
    memcpy(m.token, "tok-abc", 7);
    m.tokenlen = 7;
    brokerctl_handle(&b, &m);
    expect(m.status == 0, "bind h1 -> 0");

    msg_reset(&m, BROKER_OP_BIND);
    m.handle = 99;
    m.tokenlen = 3;
    memcpy(m.token, "xyz", 3);
    brokerctl_handle(&b, &m);
    expect(m.status == BROKER_ENOENT, "bind unknown -> ENOENT");

    /* Lifecycle events: the host posts, the broker drains in order. */
    brokerctl_post_event(&b, BROKER_EV_CONNECTED, h1);
    brokerctl_post_event(&b, BROKER_EV_EXITED, h2);

    msg_reset(&m, BROKER_OP_EVENT);
    brokerctl_handle(&b, &m);
    expect(m.evcode == BROKER_EV_CONNECTED && m.evhandle == h1,
           "event 1 -> CONNECTED h1");
    expect(m.evt == 1, "evt set while one event remains");

    msg_reset(&m, BROKER_OP_EVENT);
    brokerctl_handle(&b, &m);
    expect(m.evcode == BROKER_EV_EXITED && m.evhandle == h2,
           "event 2 -> EXITED h2");

    msg_reset(&m, BROKER_OP_EVENT);
    brokerctl_handle(&b, &m);
    expect(m.evcode == BROKER_EV_NONE, "event 3 -> NONE");
    expect(m.evt == 0, "evt clear when queue drained");

    /* Destroy frees the slot and the handle check is exact. */
    msg_reset(&m, BROKER_OP_DESTROY);
    m.handle = h1;
    brokerctl_handle(&b, &m);
    expect(m.status == 0, "destroy h1 -> 0");

    msg_reset(&m, BROKER_OP_DESTROY);
    m.handle = h1;
    brokerctl_handle(&b, &m);
    expect(m.status == BROKER_ENOENT, "destroy h1 again -> ENOENT");

    /* A freed slot admits a new spawn, with a fresh (non-reused) handle. */
    msg_reset(&m, BROKER_OP_SPAWN);
    brokerctl_handle(&b, &m);
    expect(m.status == 3, "spawn after destroy -> handle 3");

    /* An unknown op is rejected. */
    msg_reset(&m, 999);
    brokerctl_handle(&b, &m);
    expect(m.status == BROKER_EINVAL, "bad op -> EINVAL");

    if (fails == 0)
        printf("brokerctltest: ok\n");
    else
        printf("brokerctltest: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
