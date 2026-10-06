/* draw.c : the host draw subsystem.
 *
 * The host is the authority on the guest's scene, not a relay. A SUBMIT carries
 * a chunk of structured draw ops; the host reads each op with a bounds-checked
 * cursor, validates every field, and applies it as a mutation on a retained
 * per-guest tree (create a node under a parent, set text, annotate interaction,
 * listen, remove). From that tree the host authors the SVG that a browser will
 * render: every attribute value is reconstructed from a validated number or an
 * escaped string, so there is no place for a guest to inject markup. The guest
 * never sends SVG and the host never forwards guest bytes verbatim.
 *
 * This step produces the canonical SVG and tracks scene and ids. It does not
 * yet ship that SVG anywhere: the WebSocket front end and the browser client
 * are M6.3. For bring-up, setting JOTUN_DRAW_SVG makes each SUBMIT write the
 * serialized scene to that path, and JOTUN_DRAW_TEST feeds a synthetic click
 * back on a clickable node so the submit/poll loop runs with no front end.
 *
 * On a malformed or semantically invalid op the whole SUBMIT returns
 * DRAW_EINVAL. Ops before the faulty one have already been applied, since the
 * retained tree is not rolled back here; every applied op was individually
 * validated, so the tree never holds anything unsafe. All-or-nothing rollback
 * against a hostile guest is M6.5. Node lookup is a linear scan, fine at the
 * node cap for correctness; an index is M6.6 scale work. */
#include "draw.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define DRAW_MAXEVT    16       /* queued input events before the oldest drops */
#define DRAW_VALMAX    64       /* event value bytes retained (matches guest) */

#define DRAW_MAXNODES  4096     /* nodes per guest (browser SVG DOM ceiling) */
#define DRAW_MAXSURF   16       /* surfaces (windows) per guest */
#define DRAW_STRCAP    4096     /* bytes kept from a title or text string */
#define DRAW_MAXINTER  4        /* interaction annotations per node */

/* The guest id namespaces browser DOM ids as qu-<guest>-<node>. One guest
 * domain exists today; multi-guest namespacing arrives with multiple domains. */
#define DRAW_GUEST     0

struct draw_evt {
    uint32_t node;
    uint16_t type;
    uint16_t vlen;
    uint8_t  value[DRAW_VALMAX];
};

/* The validated style of a shape, host side. mask selects which fields are
 * present (DST_* bits); colors are 0xRRGGBB or DRAW_COLOR_NONE. This mirrors
 * the guest's struct draw_style, but the host keeps its own copy rather than
 * sharing the guest header. */
struct draw_style {
    uint32_t mask;
    uint32_t fill;
    uint32_t stroke;
    uint16_t stroke_width;
    uint8_t  opacity;
    uint8_t  font;
};

/* One declarative interaction attached to a node. */
struct dinter {
    uint16_t kind;              /* DI_* */
    uint32_t to;                /* HOVER_FILL / PRESS_FILL target color */
    uint8_t  axis;              /* DRAG: 'h' or 'v' */
    uint16_t track;             /* DRAG: track length */
};

/* A retained scene node. Geometry is stored by type; text holds a surface
 * title or a text element's content. */
struct dnode {
    int      in_use;
    uint32_t id;
    uint8_t  type;              /* DOP_* */
    uint32_t parent;            /* 0 for a surface */
    uint32_t seq;               /* creation order, for stable sibling order */
    union {
        struct { int16_t x, y; uint16_t w, h; } surface;
        struct { int16_t tx, ty; } group;
        struct { int16_t x, y; uint16_t w, h, rx; } rect;
        struct { int16_t cx, cy; uint16_t r; } circle;
        struct { int16_t cx, cy; uint16_t rx, ry; } ellipse;
        struct { int16_t x1, y1, x2, y2; } line;
        struct { int16_t x, y; uint16_t size; } text;
    } geo;
    struct draw_style style;
    char    *text;              /* malloc'd, NUL-terminated, or NULL */
    struct dinter inter[DRAW_MAXINTER];
    int      ninter;
    int      click;             /* a click listener is attached */
    int      close;             /* a close listener is attached */
};

struct drawdev {
    struct jotun_draw *chan;
    void (*wake)(void);
    int test;
    const char *svg_path;       /* JOTUN_DRAW_SVG, or NULL */

    struct draw_evt q[DRAW_MAXEVT];
    int head;
    int count;

    struct dnode nodes[DRAW_MAXNODES];
    int nnodes;                 /* live node count */
    int nsurf;                  /* live surface count */
    uint32_t seqctr;
};

/****************************************************************
 * The op-stream reader: bounds-checked little-endian cursor
 ****************************************************************/

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

static int16_t
rd_i16(struct rd *r)
{
    return (int16_t)rd_u16(r);
}

/* Read a length-prefixed string into a fresh capped, NUL-terminated buffer. The
 * full declared length is always consumed so the cursor stays aligned even when
 * the value is truncated to the cap. Returns NULL on allocation failure, which
 * the caller treats as empty text rather than an error. */
static char *
rd_str_dup(struct rd *r)
{
    uint16_t len = rd_u16(r);
    if (r->pos + len > r->n) {
        r->err = 1;
        return NULL;
    }
    uint32_t keep = len > DRAW_STRCAP ? DRAW_STRCAP : len;
    char *s = malloc(keep + 1);
    if (s) {
        memcpy(s, r->b + r->pos, keep);
        s[keep] = '\0';
    }
    r->pos += len;
    return s;
}

/* Read a style block: a u16 presence mask then the present fields in bit
 * order. Colors are masked to 24 bits unless they are the "none" sentinel. */
static void
rd_style(struct rd *r, struct draw_style *st)
{
    memset(st, 0, sizeof *st);
    st->mask = rd_u16(r);
    if (st->mask & DST_FILL) {
        st->fill = rd_u32(r);
        if (st->fill != DRAW_COLOR_NONE)
            st->fill &= 0xffffffu;
    }
    if (st->mask & DST_STROKE) {
        st->stroke = rd_u32(r);
        if (st->stroke != DRAW_COLOR_NONE)
            st->stroke &= 0xffffffu;
    }
    if (st->mask & DST_STROKE_WIDTH) {
        uint16_t w = rd_u16(r);
        st->stroke_width = w > 1024 ? 1024 : w;     /* cap, per the vocabulary */
    }
    if (st->mask & DST_OPACITY)
        st->opacity = rd_u8(r);
    if (st->mask & DST_FONT) {
        uint8_t f = rd_u8(r);
        st->font = f > 2 ? 0 : f;                    /* range-check the enum */
    }
}

/****************************************************************
 * Scene tree
 ****************************************************************/

static int
find(struct drawdev *dd, uint32_t id)
{
    for (int i = 0; i < DRAW_MAXNODES; i++)
        if (dd->nodes[i].in_use && dd->nodes[i].id == id)
            return i;
    return -1;
}

/* A parent must be an existing surface or group. */
static int
parent_ok(struct drawdev *dd, uint32_t parent)
{
    int s = find(dd, parent);
    if (s < 0)
        return 0;
    uint8_t t = dd->nodes[s].type;
    return t == DOP_SURFACE || t == DOP_GROUP;
}

/* Create a node. Rejects id 0 (reserved), a duplicate id, or a full table.
 * Parent validation is the caller's, since a surface has none. Returns the new
 * node, or NULL. */
static struct dnode *
node_new(struct drawdev *dd, uint32_t id, uint8_t type, uint32_t parent)
{
    if (id == 0 || find(dd, id) >= 0 || dd->nnodes >= DRAW_MAXNODES)
        return NULL;
    for (int i = 0; i < DRAW_MAXNODES; i++) {
        struct dnode *n = &dd->nodes[i];
        if (n->in_use)
            continue;
        memset(n, 0, sizeof *n);
        n->in_use = 1;
        n->id = id;
        n->type = type;
        n->parent = parent;
        n->seq = dd->seqctr++;
        dd->nnodes++;
        return n;
    }
    return NULL;
}

/* Remove a node and, recursively, everything parented to it. */
static void
node_remove(struct drawdev *dd, uint32_t id)
{
    int s = find(dd, id);
    if (s < 0)
        return;

    /* Children first: scan for anything whose parent is this node. */
    for (int i = 0; i < DRAW_MAXNODES; i++)
        if (dd->nodes[i].in_use && dd->nodes[i].parent == id)
            node_remove(dd, dd->nodes[i].id);

    struct dnode *n = &dd->nodes[s];
    if (n->type == DOP_SURFACE)
        dd->nsurf--;
    free(n->text);
    n->text = NULL;
    n->in_use = 0;
    dd->nnodes--;
}

/****************************************************************
 * Apply a SUBMIT op stream to the tree
 ****************************************************************/

/* Attach an interaction to a node, noting click and close listeners so the
 * emitter and the event layer can find them. Ignores extras past the cap. */
static void
node_interact(struct dnode *n, uint16_t kind, uint32_t to,
              uint8_t axis, uint16_t track)
{
    if (kind == DI_CLICK)
        n->click = 1;
    if (n->ninter < DRAW_MAXINTER) {
        struct dinter *it = &n->inter[n->ninter++];
        it->kind = kind;
        it->to = to;
        it->axis = axis;
        it->track = track;
    }
}

/* Walk and apply the stream. Returns 0 if every op was valid, -1 otherwise.
 * *click_node, when the self-test source is on, is set to the id of a node the
 * stream marks clickable. */
static int
apply(struct drawdev *dd, const uint8_t *buf, uint32_t len, uint32_t *click_node)
{
    struct rd r = { .b = buf, .n = len, .pos = 0, .err = 0 };

    while (r.pos < r.n) {
        uint8_t op = rd_u8(&r);
        if (r.err)
            return -1;

        switch (op) {
        case DOP_SURFACE: {
            uint32_t id = rd_u32(&r);
            int16_t x = rd_i16(&r), y = rd_i16(&r);
            uint16_t w = rd_u16(&r), h = rd_u16(&r);
            char *title = rd_str_dup(&r);
            if (r.err || dd->nsurf >= DRAW_MAXSURF) {
                free(title);
                return -1;
            }
            struct dnode *n = node_new(dd, id, DOP_SURFACE, 0);
            if (!n) {
                free(title);
                return -1;
            }
            n->geo.surface.x = x;
            n->geo.surface.y = y;
            n->geo.surface.w = w;
            n->geo.surface.h = h;
            n->text = title;
            dd->nsurf++;
            break;
        }
        case DOP_GROUP: {
            uint32_t id = rd_u32(&r);
            uint32_t parent = rd_u32(&r);
            int16_t tx = rd_i16(&r), ty = rd_i16(&r);
            if (r.err || !parent_ok(dd, parent))
                return -1;
            struct dnode *n = node_new(dd, id, DOP_GROUP, parent);
            if (!n)
                return -1;
            n->geo.group.tx = tx;
            n->geo.group.ty = ty;
            break;
        }
        case DOP_RECT: {
            uint32_t id = rd_u32(&r);
            uint32_t parent = rd_u32(&r);
            int16_t x = rd_i16(&r), y = rd_i16(&r);
            uint16_t w = rd_u16(&r), h = rd_u16(&r), rx = rd_u16(&r);
            struct draw_style st;
            rd_style(&r, &st);
            if (r.err || !parent_ok(dd, parent))
                return -1;
            struct dnode *n = node_new(dd, id, DOP_RECT, parent);
            if (!n)
                return -1;
            n->geo.rect.x = x;
            n->geo.rect.y = y;
            n->geo.rect.w = w;
            n->geo.rect.h = h;
            n->geo.rect.rx = rx;
            n->style = st;
            break;
        }
        case DOP_CIRCLE: {
            uint32_t id = rd_u32(&r);
            uint32_t parent = rd_u32(&r);
            int16_t cx = rd_i16(&r), cy = rd_i16(&r);
            uint16_t rad = rd_u16(&r);
            struct draw_style st;
            rd_style(&r, &st);
            if (r.err || !parent_ok(dd, parent))
                return -1;
            struct dnode *n = node_new(dd, id, DOP_CIRCLE, parent);
            if (!n)
                return -1;
            n->geo.circle.cx = cx;
            n->geo.circle.cy = cy;
            n->geo.circle.r = rad;
            n->style = st;
            break;
        }
        case DOP_ELLIPSE: {
            uint32_t id = rd_u32(&r);
            uint32_t parent = rd_u32(&r);
            int16_t cx = rd_i16(&r), cy = rd_i16(&r);
            uint16_t rx = rd_u16(&r), ry = rd_u16(&r);
            struct draw_style st;
            rd_style(&r, &st);
            if (r.err || !parent_ok(dd, parent))
                return -1;
            struct dnode *n = node_new(dd, id, DOP_ELLIPSE, parent);
            if (!n)
                return -1;
            n->geo.ellipse.cx = cx;
            n->geo.ellipse.cy = cy;
            n->geo.ellipse.rx = rx;
            n->geo.ellipse.ry = ry;
            n->style = st;
            break;
        }
        case DOP_LINE: {
            uint32_t id = rd_u32(&r);
            uint32_t parent = rd_u32(&r);
            int16_t x1 = rd_i16(&r), y1 = rd_i16(&r);
            int16_t x2 = rd_i16(&r), y2 = rd_i16(&r);
            struct draw_style st;
            rd_style(&r, &st);
            if (r.err || !parent_ok(dd, parent))
                return -1;
            struct dnode *n = node_new(dd, id, DOP_LINE, parent);
            if (!n)
                return -1;
            n->geo.line.x1 = x1;
            n->geo.line.y1 = y1;
            n->geo.line.x2 = x2;
            n->geo.line.y2 = y2;
            n->style = st;
            break;
        }
        case DOP_TEXT: {
            uint32_t id = rd_u32(&r);
            uint32_t parent = rd_u32(&r);
            int16_t x = rd_i16(&r), y = rd_i16(&r);
            uint16_t size = rd_u16(&r);
            char *s = rd_str_dup(&r);
            struct draw_style st;
            rd_style(&r, &st);
            if (r.err || !parent_ok(dd, parent)) {
                free(s);
                return -1;
            }
            struct dnode *n = node_new(dd, id, DOP_TEXT, parent);
            if (!n) {
                free(s);
                return -1;
            }
            n->geo.text.x = x;
            n->geo.text.y = y;
            n->geo.text.size = size;
            n->text = s;
            n->style = st;
            break;
        }
        case DOP_SET_TEXT: {
            uint32_t id = rd_u32(&r);
            char *s = rd_str_dup(&r);
            if (r.err) {
                free(s);
                return -1;
            }
            int slot = find(dd, id);
            if (slot < 0 || dd->nodes[slot].type != DOP_TEXT) {
                free(s);
                return -1;
            }
            free(dd->nodes[slot].text);
            dd->nodes[slot].text = s;
            break;
        }
        case DOP_REMOVE: {
            uint32_t id = rd_u32(&r);
            if (r.err || find(dd, id) < 0)
                return -1;
            node_remove(dd, id);
            break;
        }
        case DOP_INTERACT: {
            uint32_t id = rd_u32(&r);
            uint16_t kind = rd_u16(&r);
            uint32_t to = 0;
            uint8_t axis = 0;
            uint16_t track = 0;
            switch (kind) {
            case DI_HOVER_FILL:
            case DI_PRESS_FILL:
                to = rd_u32(&r);
                if (to != DRAW_COLOR_NONE)
                    to &= 0xffffffu;
                break;
            case DI_DRAG:
                axis = rd_u8(&r);
                track = rd_u16(&r);
                if (axis != 'h' && axis != 'v')
                    r.err = 1;
                break;
            case DI_XY:
            case DI_CLICK:
                break;
            default:
                r.err = 1;
                break;
            }
            int slot = find(dd, id);
            if (r.err || slot < 0)
                return -1;
            node_interact(&dd->nodes[slot], kind, to, axis, track);
            if (click_node && kind == DI_CLICK)
                *click_node = id;
            break;
        }
        case DOP_LISTEN: {
            uint32_t id = rd_u32(&r);
            uint16_t ev = rd_u16(&r);
            int slot = find(dd, id);
            if (r.err || slot < 0)
                return -1;
            if (ev == DEV_CLICK) {
                dd->nodes[slot].click = 1;
                if (click_node)
                    *click_node = id;
            } else if (ev == DEV_CLOSE) {
                dd->nodes[slot].close = 1;
            }
            break;
        }
        default:
            /* SET, POLYLINE and PATH are in the vocabulary but have no guest
             * emitter yet; their host apply and SVG land with those wrappers.
             * Any other opcode is a protocol error. */
            return -1;
        }
        if (r.err)
            return -1;
    }
    return 0;
}

/****************************************************************
 * SVG serialization: the host authors every byte
 ****************************************************************/

struct sbuf {
    char *p;
    size_t len, cap;
};

static void
sb_need(struct sbuf *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap)
        return;
    size_t cap = b->cap ? b->cap * 2 : 4096;
    while (cap < b->len + extra + 1)
        cap *= 2;
    char *np = realloc(b->p, cap);
    if (!np)
        return;                 /* on OOM, further writes are dropped */
    b->p = np;
    b->cap = cap;
}

static void
sb_puts(struct sbuf *b, const char *s)
{
    size_t n = strlen(s);
    sb_need(b, n);
    if (b->len + n + 1 <= b->cap) {
        memcpy(b->p + b->len, s, n);
        b->len += n;
        b->p[b->len] = '\0';
    }
}

static void
sb_printf(struct sbuf *b, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0)
        sb_puts(b, tmp);
}

/* Append s with XML special characters escaped, so guest text can never open a
 * tag, an attribute, or an entity. */
static void
sb_escape(struct sbuf *b, const char *s)
{
    if (!s)
        return;
    for (; *s; s++) {
        switch (*s) {
        case '&':  sb_puts(b, "&amp;");  break;
        case '<':  sb_puts(b, "&lt;");   break;
        case '>':  sb_puts(b, "&gt;");   break;
        case '"':  sb_puts(b, "&quot;"); break;
        case '\'': sb_puts(b, "&#39;");  break;
        default: {
            char c[2] = { *s, '\0' };
            sb_puts(b, c);
            break;
        }
        }
    }
}

static void
emit_color(struct sbuf *b, uint32_t c)
{
    if (c == DRAW_COLOR_NONE)
        sb_puts(b, "none");
    else
        sb_printf(b, "#%06x", c & 0xffffffu);
}

/* Emit the present style fields as SVG presentation attributes. */
static void
emit_style(struct sbuf *b, const struct draw_style *s)
{
    static const char *fonts[] = { "sans-serif", "serif", "monospace" };

    if (s->mask & DST_FILL) {
        sb_puts(b, " fill=\"");
        emit_color(b, s->fill);
        sb_puts(b, "\"");
    }
    if (s->mask & DST_STROKE) {
        sb_puts(b, " stroke=\"");
        emit_color(b, s->stroke);
        sb_puts(b, "\"");
    }
    if (s->mask & DST_STROKE_WIDTH)
        sb_printf(b, " stroke-width=\"%u\"", s->stroke_width);
    if (s->mask & DST_OPACITY)
        sb_printf(b, " opacity=\"%g\"", s->opacity / 255.0);
    if (s->mask & DST_FONT)
        sb_printf(b, " font-family=\"%s\"", fonts[s->font <= 2 ? s->font : 0]);
}

/* Emit the open-tag attributes for interactions the client reads as data (drag,
 * xy, click, close). Hover and press fills are SMIL children, emitted after. */
static void
emit_inter_attrs(struct sbuf *b, const struct dnode *n)
{
    for (int i = 0; i < n->ninter; i++) {
        const struct dinter *it = &n->inter[i];
        if (it->kind == DI_DRAG)
            sb_printf(b, " data-qu-drag=\"%c\" data-qu-track=\"%u\"",
                      it->axis, it->track);
        else if (it->kind == DI_XY)
            sb_puts(b, " data-qu-xy=\"1\"");
    }
    if (n->click)
        sb_puts(b, " data-qu-listen=\"click\"");
    if (n->close)
        sb_puts(b, " data-qu-listen=\"close\"");
}

/* Emit the SMIL <set> children for hover/press fill interactions. */
static void
emit_inter_smil(struct sbuf *b, const struct dnode *n)
{
    for (int i = 0; i < n->ninter; i++) {
        const struct dinter *it = &n->inter[i];
        const char *begin, *end;
        if (it->kind == DI_HOVER_FILL) {
            begin = "mouseover";
            end = "mouseout";
        } else if (it->kind == DI_PRESS_FILL) {
            begin = "mousedown";
            end = "mouseup";
        } else {
            continue;
        }
        sb_puts(b, "<set attributeName=\"fill\" to=\"");
        emit_color(b, it->to);
        sb_printf(b, "\" begin=\"%s\" end=\"%s\"/>", begin, end);
    }
}

static void emit_node(struct sbuf *b, struct drawdev *dd, const struct dnode *n);

/* Emit every child of parent id in creation order: repeatedly pick the child
 * with the smallest seq greater than the last emitted. O(n^2) over the tree,
 * which is fine at the node cap; an index is M6.6 work. */
static void
emit_children_ordered(struct sbuf *b, struct drawdev *dd, uint32_t parent)
{
    int have_last = 0;
    uint32_t last = 0;
    for (;;) {
        const struct dnode *pick = NULL;
        for (int i = 0; i < DRAW_MAXNODES; i++) {
            const struct dnode *c = &dd->nodes[i];
            if (!c->in_use || c->parent != parent)
                continue;
            if (have_last && c->seq <= last)
                continue;
            if (!pick || c->seq < pick->seq)
                pick = c;
        }
        if (!pick)
            break;
        emit_node(b, dd, pick);
        last = pick->seq;
        have_last = 1;
    }
}

static void
emit_node(struct sbuf *b, struct drawdev *dd, const struct dnode *n)
{
    switch (n->type) {
    case DOP_SURFACE:
        sb_printf(b, "<g id=\"qu-%d-%u\" class=\"qu-surface\""
                  " transform=\"translate(%d,%d)\">",
                  DRAW_GUEST, n->id, n->geo.surface.x, n->geo.surface.y);
        sb_printf(b, "<rect class=\"qu-frame\" x=\"0\" y=\"0\""
                  " width=\"%u\" height=\"%u\" fill=\"#ffffff\""
                  " stroke=\"#888888\"/>",
                  n->geo.surface.w, n->geo.surface.h);
        sb_printf(b, "<rect class=\"qu-titlebar\" x=\"0\" y=\"0\""
                  " width=\"%u\" height=\"18\" fill=\"#dddddd\"/>",
                  n->geo.surface.w);
        sb_puts(b, "<text class=\"qu-title\" x=\"4\" y=\"13\""
                " font-family=\"sans-serif\" font-size=\"12\">");
        sb_escape(b, n->text);
        sb_puts(b, "</text>");
        sb_printf(b, "<text class=\"qu-close\" x=\"%d\" y=\"13\""
                  " font-family=\"sans-serif\" font-size=\"12\""
                  " data-qu-listen=\"close\">x</text>",
                  n->geo.surface.w - 12);
        emit_children_ordered(b, dd, n->id);
        sb_puts(b, "</g>");
        break;
    case DOP_GROUP:
        sb_printf(b, "<g id=\"qu-%d-%u\" transform=\"translate(%d,%d)\">",
                  DRAW_GUEST, n->id, n->geo.group.tx, n->geo.group.ty);
        emit_children_ordered(b, dd, n->id);
        sb_puts(b, "</g>");
        break;
    case DOP_RECT:
        sb_printf(b, "<rect id=\"qu-%d-%u\" x=\"%d\" y=\"%d\""
                  " width=\"%u\" height=\"%u\" rx=\"%u\"",
                  DRAW_GUEST, n->id, n->geo.rect.x, n->geo.rect.y,
                  n->geo.rect.w, n->geo.rect.h, n->geo.rect.rx);
        emit_style(b, &n->style);
        emit_inter_attrs(b, n);
        sb_puts(b, ">");
        emit_inter_smil(b, n);
        sb_puts(b, "</rect>");
        break;
    case DOP_CIRCLE:
        sb_printf(b, "<circle id=\"qu-%d-%u\" cx=\"%d\" cy=\"%d\" r=\"%u\"",
                  DRAW_GUEST, n->id, n->geo.circle.cx, n->geo.circle.cy,
                  n->geo.circle.r);
        emit_style(b, &n->style);
        emit_inter_attrs(b, n);
        sb_puts(b, ">");
        emit_inter_smil(b, n);
        sb_puts(b, "</circle>");
        break;
    case DOP_ELLIPSE:
        sb_printf(b, "<ellipse id=\"qu-%d-%u\" cx=\"%d\" cy=\"%d\""
                  " rx=\"%u\" ry=\"%u\"",
                  DRAW_GUEST, n->id, n->geo.ellipse.cx, n->geo.ellipse.cy,
                  n->geo.ellipse.rx, n->geo.ellipse.ry);
        emit_style(b, &n->style);
        emit_inter_attrs(b, n);
        sb_puts(b, ">");
        emit_inter_smil(b, n);
        sb_puts(b, "</ellipse>");
        break;
    case DOP_LINE:
        sb_printf(b, "<line id=\"qu-%d-%u\" x1=\"%d\" y1=\"%d\""
                  " x2=\"%d\" y2=\"%d\"",
                  DRAW_GUEST, n->id, n->geo.line.x1, n->geo.line.y1,
                  n->geo.line.x2, n->geo.line.y2);
        emit_style(b, &n->style);
        emit_inter_attrs(b, n);
        sb_puts(b, ">");
        emit_inter_smil(b, n);
        sb_puts(b, "</line>");
        break;
    case DOP_TEXT:
        sb_printf(b, "<text id=\"qu-%d-%u\" x=\"%d\" y=\"%d\" font-size=\"%u\"",
                  DRAW_GUEST, n->id, n->geo.text.x, n->geo.text.y,
                  n->geo.text.size);
        emit_style(b, &n->style);
        emit_inter_attrs(b, n);
        sb_puts(b, ">");
        sb_escape(b, n->text);
        sb_puts(b, "</text>");
        break;
    default:
        break;
    }
}

/* Serialize the whole scene to one SVG document. Returns a malloc'd NUL-
 * terminated string the caller frees, or NULL. */
static char *
serialize(struct drawdev *dd)
{
    /* Canvas size: enough to contain every surface. */
    int cw = 1, ch = 1;
    for (int i = 0; i < DRAW_MAXNODES; i++) {
        const struct dnode *n = &dd->nodes[i];
        if (!n->in_use || n->type != DOP_SURFACE)
            continue;
        int right = n->geo.surface.x + n->geo.surface.w;
        int bottom = n->geo.surface.y + n->geo.surface.h;
        if (right > cw)
            cw = right;
        if (bottom > ch)
            ch = bottom;
    }

    struct sbuf b = { 0 };
    sb_printf(&b, "<svg xmlns=\"http://www.w3.org/2000/svg\""
              " width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\">",
              cw, ch, cw, ch);
    emit_children_ordered(&b, dd, 0);       /* surfaces have parent 0 */
    sb_puts(&b, "</svg>");
    return b.p;
}

/****************************************************************
 * Event queue (host to guest), unchanged from the mechanism step
 ****************************************************************/

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

/****************************************************************
 * Channel handling
 ****************************************************************/

struct drawdev *
draw_init(struct jotun_draw *chan, void (*wake)(void), int test)
{
    struct drawdev *dd = calloc(1, sizeof(*dd));
    if (!dd)
        return NULL;
    dd->chan = chan;
    dd->wake = wake;
    dd->test = test;
    dd->svg_path = getenv("JOTUN_DRAW_SVG");
    return dd;
}

static void
dump_svg(struct drawdev *dd)
{
    if (!dd->svg_path)
        return;
    char *svg = serialize(dd);
    if (!svg)
        return;
    FILE *f = fopen(dd->svg_path, "w");
    if (f) {
        fputs(svg, f);
        fputc('\n', f);
        fclose(f);
    }
    free(svg);
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
    int rc = apply(dd, chan->buf, len, dd->test ? &click_node : NULL);

    /* Author the SVG snapshot regardless, so a partial scene from a rejected
     * stream is still inspectable during bring-up. */
    dump_svg(dd);

    if (rc != 0) {
        chan->status = DRAW_EINVAL;
        return;
    }
    chan->status = DRAW_OK;

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
    if (!dd)
        return;
    for (int i = 0; i < DRAW_MAXNODES; i++)
        free(dd->nodes[i].text);
    free(dd);
}
