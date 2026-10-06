/* draw.c : the host draw sink.
 *
 * A SUBMIT carries a chunk of structured draw ops. The host walks the stream
 * op by op, checking every opcode and every field length against the buffer
 * bounds. A malformed stream is rejected whole with DRAW_EINVAL and nothing is
 * retained, so a buggy or hostile guest can never drive the host off the end of
 * the chunk. No SVG is authored here; the canonical emitter is M6.2.
 *
 * POLL returns one queued input event. The only event source so far is the
 * self-test one: when it is enabled, a submitted node marked clickable gets a
 * synthetic click queued and the guest is roused, which lets the full
 * submit/park/poll loop run with no front end present. */
#include "draw.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DRAW_MAXEVT  16         /* queued input events before the oldest drops */
#define DRAW_VALMAX  64         /* event value bytes retained (matches guest) */

struct draw_evt {
    uint32_t node;
    uint16_t type;
    uint16_t vlen;
    uint8_t  value[DRAW_VALMAX];
};

struct drawdev {
    struct jotun_draw *chan;
    void (*wake)(void);
    int test;

    struct draw_evt q[DRAW_MAXEVT];
    int head;                   /* next slot to read */
    int count;                  /* queued events */
};

/* A bounds-checked reader over the op stream. Every read that would pass the
 * end sets err and yields zero, so a walk that touched the edge is caught by
 * checking err once at the end rather than at every field. */
struct rd {
    const uint8_t *b;
    uint32_t n;
    uint32_t pos;
    int err;
};

static uint8_t
rd_u8(struct rd *r)
{
    if (r->pos + 1 > r->n) {
        r->err = 1;
        return 0;
    }
    return r->b[r->pos++];
}

static uint16_t
rd_u16(struct rd *r)
{
    uint16_t lo = rd_u8(r);
    uint16_t hi = rd_u8(r);
    return (uint16_t)(lo | hi << 8);
}

static uint32_t
rd_u32(struct rd *r)
{
    uint32_t lo = rd_u16(r);
    uint32_t hi = rd_u16(r);
    return lo | hi << 16;
}

/* Consume a length-prefixed string (u16 count then that many bytes). */
static void
rd_str(struct rd *r)
{
    uint16_t len = rd_u16(r);
    if (r->pos + len > r->n) {
        r->err = 1;
        return;
    }
    r->pos += len;
}

/* Consume a style blob: a u16 presence mask then the present fields in bit
 * order. Mirrors the guest encoder exactly. */
static void
rd_style(struct rd *r)
{
    uint16_t mask = rd_u16(r);
    if (mask & DST_FILL)         rd_u32(r);
    if (mask & DST_STROKE)       rd_u32(r);
    if (mask & DST_STROKE_WIDTH) rd_u16(r);
    if (mask & DST_OPACITY)      rd_u8(r);
    if (mask & DST_FONT)         rd_u8(r);
}

/* Consume the params of a DOP_INTERACT op, which depend on the kind. */
static void
rd_interact(struct rd *r, uint16_t kind)
{
    switch (kind) {
    case DI_HOVER_FILL:
    case DI_PRESS_FILL:
        rd_u32(r);              /* to:color */
        break;
    case DI_DRAG:
        rd_u8(r);               /* axis */
        rd_u16(r);              /* track */
        break;
    case DI_XY:
    case DI_CLICK:
        break;
    default:
        r->err = 1;             /* unknown interaction kind */
        break;
    }
}

/* Queue one event for the guest to POLL, dropping the oldest if the queue is
 * full. */
static void
enqueue(struct drawdev *dd, uint32_t node, uint16_t type,
        const void *value, uint16_t vlen)
{
    if (vlen > DRAW_VALMAX)
        vlen = DRAW_VALMAX;
    if (dd->count == DRAW_MAXEVT) {
        dd->head = (dd->head + 1) % DRAW_MAXEVT;
        dd->count--;
    }
    int slot = (dd->head + dd->count) % DRAW_MAXEVT;
    struct draw_evt *e = &dd->q[slot];
    e->node = node;
    e->type = type;
    e->vlen = vlen;
    if (vlen)
        memcpy(e->value, value, vlen);
    dd->count++;
}

/* Walk a SUBMIT op stream. Returns 0 if every op is well formed, -1 otherwise.
 * When the self-test source is on, *click_node is set to the id of a node the
 * stream marks clickable (DOP_LISTEN for DEV_CLICK, or DOP_INTERACT DI_CLICK),
 * or left untouched when none is. */
static int
walk(const uint8_t *buf, uint32_t len, int want_click, uint32_t *click_node)
{
    struct rd r = { .b = buf, .n = len, .pos = 0, .err = 0 };

    while (r.pos < r.n) {
        uint8_t op = rd_u8(&r);
        switch (op) {
        case DOP_SURFACE:
            rd_u32(&r);                         /* id */
            rd_u16(&r); rd_u16(&r);             /* x, y */
            rd_u16(&r); rd_u16(&r);             /* w, h */
            rd_str(&r);                          /* title */
            break;
        case DOP_GROUP:
            rd_u32(&r); rd_u32(&r);             /* id, parent */
            rd_u16(&r); rd_u16(&r);             /* tx, ty */
            break;
        case DOP_RECT:
            rd_u32(&r); rd_u32(&r);             /* id, parent */
            rd_u16(&r); rd_u16(&r);             /* x, y */
            rd_u16(&r); rd_u16(&r); rd_u16(&r); /* w, h, rx */
            rd_style(&r);
            break;
        case DOP_CIRCLE:
            rd_u32(&r); rd_u32(&r);             /* id, parent */
            rd_u16(&r); rd_u16(&r); rd_u16(&r); /* cx, cy, r */
            rd_style(&r);
            break;
        case DOP_ELLIPSE:
            rd_u32(&r); rd_u32(&r);             /* id, parent */
            rd_u16(&r); rd_u16(&r);             /* cx, cy */
            rd_u16(&r); rd_u16(&r);             /* rx, ry */
            rd_style(&r);
            break;
        case DOP_LINE:
            rd_u32(&r); rd_u32(&r);             /* id, parent */
            rd_u16(&r); rd_u16(&r);             /* x1, y1 */
            rd_u16(&r); rd_u16(&r);             /* x2, y2 */
            rd_style(&r);
            break;
        case DOP_TEXT:
            rd_u32(&r); rd_u32(&r);             /* id, parent */
            rd_u16(&r); rd_u16(&r);             /* x, y */
            rd_u16(&r);                          /* size */
            rd_str(&r);                          /* s */
            rd_style(&r);
            break;
        case DOP_SET_TEXT:
            rd_u32(&r);                         /* id */
            rd_str(&r);                          /* s */
            break;
        case DOP_REMOVE:
            rd_u32(&r);                         /* id */
            break;
        case DOP_LISTEN: {
            uint32_t id = rd_u32(&r);
            uint16_t ev = rd_u16(&r);
            if (want_click && !r.err && ev == DEV_CLICK)
                *click_node = id;
            break;
        }
        case DOP_INTERACT: {
            uint32_t id = rd_u32(&r);
            uint16_t kind = rd_u16(&r);
            if (!r.err)
                rd_interact(&r, kind);
            if (want_click && !r.err && kind == DI_CLICK)
                *click_node = id;
            break;
        }
        default:
            return -1;                           /* unknown opcode */
        }
        if (r.err)
            return -1;
    }
    return 0;
}

struct drawdev *
draw_init(struct jotun_draw *chan, void (*wake)(void), int test)
{
    struct drawdev *dd = calloc(1, sizeof(*dd));
    if (!dd)
        return NULL;
    dd->chan = chan;
    dd->wake = wake;
    dd->test = test;
    return dd;
}

static void
handle_submit(struct drawdev *dd, struct jotun_draw *chan)
{
    uint32_t len = chan->len;
    if (len > JOTUN_DRAW_MSIZE) {
        chan->status = DRAW_EINVAL;
        return;
    }

    uint32_t click_node = 0;
    if (walk(chan->buf, len, dd->test, &click_node) != 0) {
        chan->status = DRAW_EINVAL;
        return;
    }
    chan->status = DRAW_OK;

    /* Self-test: a clickable node in the scene gets a synthetic click, routed
     * through the same evt-plus-wake path the browser front end will use. */
    if (dd->test && click_node) {
        enqueue(dd, click_node, DEV_CLICK, NULL, 0);
        __atomic_store_n(&chan->evt, 1, __ATOMIC_SEQ_CST);
        if (dd->wake)
            dd->wake();
    }
}

static void
handle_poll(struct drawdev *dd, struct jotun_draw *chan)
{
    if (dd->count == 0) {
        chan->status = DRAW_OK;
        chan->len = 0;
        return;
    }

    struct draw_evt *e = &dd->q[dd->head];
    dd->head = (dd->head + 1) % DRAW_MAXEVT;
    dd->count--;

    /* node:u32 type:u16 vlen:u16 value[vlen] */
    uint8_t *b = chan->buf;
    b[0] = (uint8_t)e->node;
    b[1] = (uint8_t)(e->node >> 8);
    b[2] = (uint8_t)(e->node >> 16);
    b[3] = (uint8_t)(e->node >> 24);
    b[4] = (uint8_t)e->type;
    b[5] = (uint8_t)(e->type >> 8);
    b[6] = (uint8_t)e->vlen;
    b[7] = (uint8_t)(e->vlen >> 8);
    if (e->vlen)
        memcpy(b + 8, e->value, e->vlen);
    chan->len = 8u + e->vlen;
    chan->status = DRAW_OK;
}

void
draw_handle(struct drawdev *dd, struct jotun_draw *chan)
{
    switch (chan->op) {
    case DRAW_SUBMIT:
        handle_submit(dd, chan);
        break;
    case DRAW_POLL:
        handle_poll(dd, chan);
        break;
    default:
        chan->status = DRAW_EINVAL;
        break;
    }
}

void
draw_shutdown(struct drawdev *dd)
{
    free(dd);
}
