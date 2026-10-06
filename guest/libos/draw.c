/* draw.c : the guest draw client.
 *
 * Ops are encoded little-endian straight into the channel buffer behind a
 * running cursor. When an op would not fit, the cursor is flushed first, so a
 * flush only ever happens between whole ops. draw_flush sends the chunk with
 * the net-style backpressure contract: DRAW_EAGAIN parks the task until a draw
 * event rouses it. One writer at a time is assumed for the scene (the demo and
 * early clients are single-task); the lock guards the channel transaction. */
#include "draw.h"

#include <stdint.h>

#include "arch.h"
#include "task.h"

/* Headroom reserved before each op. Strings are capped so no single op can
 * exceed this, which keeps a flush strictly between ops. */
#define DRAW_OP_MAX   512
#define DRAW_STR_MAX  256

static volatile int draw_waiting[TASK_MAX];
static volatile int draw_lock;
static uint32_t dpos;           /* cursor into the channel buffer */

static struct jotun_draw *
chan(void)
{
    return (struct jotun_draw *)(uintptr_t)JOTUN_DRAW_BASE;
}

static unsigned long
lock_chan(void)
{
    unsigned long f = arch_irq_save();
    while (__atomic_exchange_n(&draw_lock, 1, __ATOMIC_ACQUIRE))
        arch_cpu_relax();
    return f;
}

static void
unlock_chan(unsigned long f)
{
    __atomic_store_n(&draw_lock, 0, __ATOMIC_RELEASE);
    arch_irq_restore(f);
}

static void
doorbell(void)
{
    *(volatile uint32_t *)(uintptr_t)JOTUN_DRAW_DOORBELL = 1;
}

/* Encoding cursor. Writes clamp at the buffer end; overflow cannot happen
 * because each op reserves DRAW_OP_MAX via need() first. */
static void
du8(uint8_t v)
{
    if (dpos < JOTUN_DRAW_MSIZE)
        chan()->buf[dpos++] = v;
}

static void du16(uint16_t v) { du8((uint8_t)v); du8((uint8_t)(v >> 8)); }
static void du32(uint32_t v) { du16((uint16_t)v); du16((uint16_t)(v >> 16)); }
static void du64(uint64_t v) { du32((uint32_t)v); du32((uint32_t)(v >> 32)); }
static void di16(int v) { du16((uint16_t)(int16_t)v); }

static void
dstr(const char *s)
{
    uint16_t n = 0;
    while (s && s[n] != '\0' && n < DRAW_STR_MAX)
        n++;
    du16(n);
    for (uint16_t i = 0; i < n; i++)
        du8((uint8_t)s[i]);
}

static void
dstyle(const struct draw_style *st)
{
    uint16_t mask = st ? (uint16_t)st->mask : 0;

    du16(mask);
    if (mask & DST_FILL)         du32(st->fill);
    if (mask & DST_STROKE)       du32(st->stroke);
    if (mask & DST_STROKE_WIDTH) du16(st->stroke_width);
    if (mask & DST_OPACITY)      du8(st->opacity);
    if (mask & DST_FONT)         du8(st->font);
}

/* Reserve n bytes for the next op, flushing the current chunk if it would not
 * fit. A fixed op reserves the standard headroom; a variable op (polyline,
 * path) passes its computed size. */
static void
need_bytes(unsigned n)
{
    if (dpos + n > JOTUN_DRAW_MSIZE)
        draw_flush();
}

static void
need(void)
{
    need_bytes(DRAW_OP_MAX);
}

/* Fixed i16 argument count for a path command, or -1 if unknown. */
static int
seg_args(uint8_t cmd)
{
    switch (cmd) {
    case DPATH_M: case DPATH_L: return 2;
    case DPATH_H: case DPATH_V: return 1;
    case DPATH_C: return 6;
    case DPATH_Q: return 4;
    case DPATH_Z: return 0;
    default: return -1;
    }
}

void
draw_surface(uint32_t id, int x, int y, int w, int h, const char *title)
{
    need();
    du8(DOP_SURFACE); du32(id); di16(x); di16(y);
    du16((uint16_t)w); du16((uint16_t)h); dstr(title);
}

void
draw_group(uint32_t id, uint32_t parent, int tx, int ty)
{
    need();
    du8(DOP_GROUP); du32(id); du32(parent); di16(tx); di16(ty);
}

void
draw_rect(uint32_t id, uint32_t parent, int x, int y, int w, int h,
          int rx, const struct draw_style *st)
{
    need();
    du8(DOP_RECT); du32(id); du32(parent); di16(x); di16(y);
    du16((uint16_t)w); du16((uint16_t)h); du16((uint16_t)rx); dstyle(st);
}

void
draw_circle(uint32_t id, uint32_t parent, int cx, int cy, int r,
            const struct draw_style *st)
{
    need();
    du8(DOP_CIRCLE); du32(id); du32(parent); di16(cx); di16(cy);
    du16((uint16_t)r); dstyle(st);
}

void
draw_ellipse(uint32_t id, uint32_t parent, int cx, int cy, int rx, int ry,
             const struct draw_style *st)
{
    need();
    du8(DOP_ELLIPSE); du32(id); du32(parent); di16(cx); di16(cy);
    du16((uint16_t)rx); du16((uint16_t)ry); dstyle(st);
}

void
draw_line(uint32_t id, uint32_t parent, int x1, int y1, int x2, int y2,
          const struct draw_style *st)
{
    need();
    du8(DOP_LINE); du32(id); du32(parent); di16(x1); di16(y1);
    di16(x2); di16(y2); dstyle(st);
}

/* Cap on points/segments per op, so one op fits a channel chunk with headroom
 * for the header and style. */
#define DRAW_PTS_MAX  1024
#define DRAW_SEG_MAX  512

void
draw_polyline(uint32_t id, uint32_t parent, int closed,
              const int16_t *pts, int n, const struct draw_style *st)
{
    if (n < 0)
        n = 0;
    if (n > DRAW_PTS_MAX)
        n = DRAW_PTS_MAX;
    /* op = tag + id + parent + closed + n + 4*n point bytes + style */
    need_bytes((unsigned)(1 + 4 + 4 + 1 + 2 + 4 * n + 32));
    du8(DOP_POLYLINE); du32(id); du32(parent);
    du8((uint8_t)(closed ? 1 : 0)); du16((uint16_t)n);
    for (int i = 0; i < 2 * n; i++)
        di16(pts[i]);
    dstyle(st);
}

void
draw_path(uint32_t id, uint32_t parent, const struct draw_seg *segs, int n,
          const struct draw_style *st)
{
    if (n < 0)
        n = 0;
    if (n > DRAW_SEG_MAX)
        n = DRAW_SEG_MAX;
    /* Worst case 1 cmd byte + 6 args each; reserve for that plus header/style. */
    need_bytes((unsigned)(1 + 4 + 4 + 2 + n * (1 + 12) + 32));
    du8(DOP_PATH); du32(id); du32(parent); du16((uint16_t)n);
    for (int i = 0; i < n; i++) {
        int na = seg_args(segs[i].cmd);
        if (na < 0)
            na = 0;                 /* unknown command: host will reject it */
        du8(segs[i].cmd);
        for (int k = 0; k < na; k++)
            di16(segs[i].a[k]);
    }
    dstyle(st);
}

void
draw_text(uint32_t id, uint32_t parent, int x, int y, int size,
          const char *s, const struct draw_style *st)
{
    need();
    du8(DOP_TEXT); du32(id); du32(parent); di16(x); di16(y);
    du16((uint16_t)size); dstr(s); dstyle(st);
}

void
draw_set_text(uint32_t id, const char *s)
{
    need();
    du8(DOP_SET_TEXT); du32(id); dstr(s);
}

void
draw_image(uint32_t id, uint32_t parent, int x, int y, int w, int h,
           const void *pixels, int stride)
{
    need();
    du8(DOP_IMAGE); du32(id); du32(parent); di16(x); di16(y);
    du16((uint16_t)w); du16((uint16_t)h);
    du8(DIMG_RGBA8888);
    du32((uint32_t)(stride ? stride : w * 4));
    du64((uint64_t)(uintptr_t)pixels);
}

void
draw_set(uint32_t id, int attr, long value)
{
    need();
    du8(DOP_SET); du32(id); du16((uint16_t)attr);
    switch (attr) {
    case DSA_X: case DSA_Y: case DSA_CX: case DSA_CY:
    case DSA_X1: case DSA_Y1: case DSA_X2: case DSA_Y2:
        di16((int)value); break;
    case DSA_W: case DSA_H: case DSA_R: case DSA_RX: case DSA_RY:
    case DSA_STROKE_WIDTH:
        du16((uint16_t)value); break;
    case DSA_FILL: case DSA_STROKE:
        du32((uint32_t)value); break;
    case DSA_OPACITY: case DSA_VISIBLE: case DSA_FONT:
        du8((uint8_t)value); break;
    default:
        break;                      /* unknown attr: no value, host rejects */
    }
}

void
draw_remove(uint32_t id)
{
    need();
    du8(DOP_REMOVE); du32(id);
}

void
draw_listen(uint32_t id, int event)
{
    need();
    du8(DOP_LISTEN); du32(id); du16((uint16_t)event);
}

void
draw_click(uint32_t id)
{
    need();
    du8(DOP_INTERACT); du32(id); du16(DI_CLICK);
}

void
draw_hover_fill(uint32_t id, uint32_t to)
{
    need();
    du8(DOP_INTERACT); du32(id); du16(DI_HOVER_FILL); du32(to);
}

void
draw_drag(uint32_t id, int vertical, int track)
{
    need();
    du8(DOP_INTERACT); du32(id); du16(DI_DRAG);
    du8(vertical ? 'v' : 'h'); du16((uint16_t)track);
}

void
draw_xy(uint32_t id)
{
    need();
    du8(DOP_INTERACT); du32(id); du16(DI_XY);
}

unsigned
draw_event_norm(const struct draw_event *ev, int idx)
{
    int off = idx * 2;
    if (off < 0 || off + 2 > ev->vlen)
        return 0;
    return (unsigned)(uint8_t)ev->value[off]
         | (unsigned)(uint8_t)ev->value[off + 1] << 8;
}

void
draw_flush(void)
{
    if (dpos == 0)
        return;

    int me = task_self();
    __atomic_store_n(&draw_waiting[me], 1, __ATOMIC_SEQ_CST);
    for (;;) {
        unsigned long f = lock_chan();
        struct jotun_draw *c = chan();
        c->op = DRAW_SUBMIT;
        c->len = dpos;
        doorbell();
        int st = c->status;
        unlock_chan(f);
        if (st != DRAW_EAGAIN)
            break;                  /* OK or error: stop retrying */
        task_block();               /* outbound full: wait for room */
    }
    __atomic_store_n(&draw_waiting[me], 0, __ATOMIC_SEQ_CST);
    dpos = 0;
}

int
draw_next_event(struct draw_event *ev)
{
    draw_flush();                   /* make sure the scene is sent first */

    int me = task_self();
    __atomic_store_n(&draw_waiting[me], 1, __ATOMIC_SEQ_CST);
    for (;;) {
        unsigned long f = lock_chan();
        struct jotun_draw *c = chan();
        c->op = DRAW_POLL;
        doorbell();
        int st = c->status;
        uint32_t len = c->len;

        if (st == DRAW_OK && len >= 8) {
            /* node:u32 type:u16 vlen:u16 value[vlen] */
            const uint8_t *b = c->buf;
            ev->node = (uint32_t)b[0] | (uint32_t)b[1] << 8 |
                       (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
            ev->type = (uint16_t)(b[4] | b[5] << 8);
            uint16_t vlen = (uint16_t)(b[6] | b[7] << 8);
            uint16_t room = sizeof ev->value - 1;
            if (vlen > room)
                vlen = room;
            if (8u + vlen > len)
                vlen = (uint16_t)(len - 8);
            for (uint16_t i = 0; i < vlen; i++)
                ev->value[i] = (char)b[8 + i];
            ev->value[vlen] = '\0';
            ev->vlen = vlen;
            unlock_chan(f);
            __atomic_store_n(&draw_waiting[me], 0, __ATOMIC_SEQ_CST);
            return 0;
        }
        unlock_chan(f);

        if (st != DRAW_OK) {
            __atomic_store_n(&draw_waiting[me], 0, __ATOMIC_SEQ_CST);
            return -1;
        }
        task_block();               /* no event yet: wait */
    }
}

static void
draw_wake_waiters(void)
{
    if (!__atomic_exchange_n(&chan()->evt, 0, __ATOMIC_SEQ_CST))
        return;
    for (int i = 0; i < TASK_MAX; i++)
        if (__atomic_load_n(&draw_waiting[i], __ATOMIC_SEQ_CST))
            task_wake(i);
}

void
draw_init(void)
{
    task_add_event_hook(draw_wake_waiters);
}
