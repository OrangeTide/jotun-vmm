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
#include "drawws.h"
#include "png.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#define DRAW_MAXEVT    16       /* queued input events before the oldest drops */
#define DRAW_VALMAX    64       /* event value bytes retained (matches guest) */

#define DRAW_MAXNODES  4096     /* nodes per guest (browser SVG DOM ceiling) */
#define DRAW_MAXSURF   16       /* surfaces (windows) per guest */
#define DRAW_MAXGUEST  8        /* sinks one front end composites (== MAX_GUESTS) */
#define DRAW_STRCAP    4096     /* bytes kept from a title or text string */
#define DRAW_MAXINTER  4        /* interaction annotations per node */
#define DRAW_MAXPTS    1024     /* points in a polyline */
#define DRAW_MAXSEG    512      /* segments in a path */
#define DRAW_IDHASH    8192     /* id index buckets (power of two, > 2x nodes) */
#define DRAW_IMG_MAXDIM  2048   /* max image width or height */
#define FB_MAXPATCH      16     /* overlay patches before a frame is re-based */
#define DRAW_IMG_MAXBYTES (4u * 1024 * 1024)    /* max framebuffer extent */
#define DRAW_DELTA_MAX   128    /* changed/removed nodes before a full push */

/* Browser DOM ids are namespaced qu-<guest>-<node>, where the guest field is
 * the owning guest's id (drawdev.gid). It keeps several guests' node ids
 * distinct in one client session and lets an inbound event name its guest. */

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

/* One validated path segment: the command and its fixed count of arguments. */
struct dseg {
    uint8_t cmd;                /* DPATH_* */
    uint8_t nargs;
    int16_t a[6];
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
        struct { int16_t x, y; uint16_t w, h; uint8_t fmt;
                 uint32_t stride; uint64_t gpa; } image;
    } geo;
    struct draw_style style;
    char    *text;              /* malloc'd, NUL-terminated, or NULL */

    int16_t *pts;               /* POLYLINE: 2*npts coordinates, malloc'd */
    int      npts;
    int      closed;            /* POLYLINE: polygon rather than open */
    struct dseg *segs;          /* PATH: nsegs segments, malloc'd */
    int      nsegs;

    struct dinter inter[DRAW_MAXINTER];
    int      ninter;
    int      click;             /* a click listener is attached */
    int      close;             /* a close listener is attached */
    int      hidden;            /* SET VISIBLE 0: emitted as display:none */

    uint8_t  change;            /* this submit: 0 none, 1 added, 2 modified */

    /* Pixel path. The framebuffer is shown as a base image plus small overlay
     * patches for damaged regions, so an unchanged background is not re-encoded
     * or re-sent each frame. Patches accumulate until a cap, then the frame is
     * re-based (one full re-encode) to bound the DOM and the total bytes. */
    int      img_dirty;         /* re-encode the whole frame (new or re-based) */
    char    *png_cache;         /* base64 PNG of the base frame */
    struct fbpatch {
        uint16_t x, y, w, h;    /* region in image pixels */
        char    *b64;           /* base64 PNG of the cropped region */
    } fb_patch[FB_MAXPATCH];
    int      fb_npatch;         /* overlay patches since the last re-base */
    uint32_t fb_patch_area;     /* total patched pixels since the last re-base */
    uint16_t dmg_x0, dmg_y0, dmg_x1, dmg_y1; /* union damage rect this submit */
    int      ndmg;              /* damage rects accumulated this submit */
    int      fb_delta_new;      /* patch index to send as an N delta, or -1 */

    /* Tree links (slot indices, -1 for none). Children are kept in creation
     * order so serialization is a single O(n) walk, no per-parent scan. */
    int      first_child, last_child;
    int      prev_sib, next_sib;
};

struct drawfe;                  /* the shared front end; defined after drawdev */

struct drawdev {
    struct jotun_draw *chan;
    int gid;                    /* owning guest id, namespaces DOM ids */
    void (*wake)(void *ctx);
    void *wake_ctx;
    int test;
    int bench;                  /* JOTUN_DRAW_BENCH: time apply and serialize */
    const char *svg_path;       /* JOTUN_DRAW_SVG, or NULL */
    struct drawfe *fe;          /* the shared front end this sink feeds, or NULL */

    /* Translate a guest-physical range to a host pointer for reading image
     * pixels, or NULL if the range is out of bounds. Supplied by the host. */
    draw_gpa_fn gpa;
    void *gpa_ctx;

    /* Inbound events are queued by the WS server thread and drained by the
     * vcpu thread in POLL, so the queue is guarded. */
    pthread_mutex_t qlock;
    struct draw_evt q[DRAW_MAXEVT];
    int head;
    int count;

    struct dnode nodes[DRAW_MAXNODES];
    int nnodes;                 /* live node count */
    int nsurf;                  /* live surface count */
    uint32_t seqctr;

    /* id -> slot+1 open-addressed index (0 is empty), so a lookup is O(1). */
    int idx[DRAW_IDHASH];
    /* Free-slot stack, so allocating a node is O(1) rather than a table scan. */
    int freelist[DRAW_MAXNODES];
    int nfree;
    /* The surface forest, in creation order (slot indices, -1 for none). */
    int root_first, root_last;
    int inited;                 /* freelist and roots laid out once */

    /* Per-submit change set, for incremental (delta) updates to the browser.
     * Added and modified nodes carry a flag; removed nodes are listed by id
     * here, since their slots are freed. Too many removals, or a whole-scene
     * change, falls back to a full document push. */
    uint32_t removed_ids[DRAW_DELTA_MAX];
    int nremoved;
    int nchanged;               /* added + modified nodes this submit */
    int delta_overflow;
};

/* The session front end: one WebSocket transport (drawws) shared by every
 * guest, and the set of sinks registered by guest id so an inbound event can be
 * routed to the guest it names. One guest feeds it today; compositing several
 * guests' scenes into one document arrives with M10.1c. */
struct drawfe {
    struct drawws *ws;                      /* the one transport, or NULL */
    struct drawdev *sinks[DRAW_MAXGUEST];   /* registered sinks, by gid */
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

static uint64_t
rd_u64(struct rd *r)
{
    uint64_t lo = rd_u32(r);
    uint64_t hi = rd_u32(r);
    return lo | hi << 32;
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
 *
 * An id-to-slot hash index makes lookup O(1); a free-slot stack makes
 * allocation O(1); and sibling links keep children in creation order so the
 * whole scene serializes in one O(n) walk with no per-parent scan.
 ****************************************************************/

/* Lay out the free list and empty the forest. Done once, lazily, so a
 * calloc'd drawdev (every link zero) becomes a valid empty tree. */
static void
tree_init(struct drawdev *dd)
{
    if (dd->inited)
        return;
    for (int i = 0; i < DRAW_MAXNODES; i++)
        dd->freelist[i] = DRAW_MAXNODES - 1 - i;    /* pop low slots first */
    dd->nfree = DRAW_MAXNODES;
    dd->root_first = dd->root_last = -1;
    dd->inited = 1;
}

static unsigned
idx_hash(uint32_t id)
{
    return (id * 2654435761u) & (DRAW_IDHASH - 1);
}

/* Return the slot holding id, or -1. */
static int
find(struct drawdev *dd, uint32_t id)
{
    for (unsigned i = idx_hash(id); dd->idx[i]; i = (i + 1) & (DRAW_IDHASH - 1))
        if (dd->nodes[dd->idx[i] - 1].id == id)
            return dd->idx[i] - 1;
    return -1;
}

static void
idx_insert(struct drawdev *dd, uint32_t id, int slot)
{
    unsigned i = idx_hash(id);
    while (dd->idx[i])
        i = (i + 1) & (DRAW_IDHASH - 1);
    dd->idx[i] = slot + 1;
}

/* Remove id from the index with backward-shift deletion, which keeps the
 * table free of tombstones so probe chains stay short. */
static void
idx_remove(struct drawdev *dd, uint32_t id)
{
    unsigned mask = DRAW_IDHASH - 1;
    unsigned i = idx_hash(id);
    while (dd->idx[i] && dd->nodes[dd->idx[i] - 1].id != id)
        i = (i + 1) & mask;
    if (!dd->idx[i])
        return;

    for (;;) {
        dd->idx[i] = 0;
        unsigned j = i;
        for (;;) {
            j = (j + 1) & mask;
            if (!dd->idx[j])
                return;
            unsigned k = idx_hash(dd->nodes[dd->idx[j] - 1].id);
            /* Move j back to i only if k is not within the (i, j] window. */
            if (i <= j ? (k > i && k <= j) : (k > i || k <= j))
                continue;
            dd->idx[i] = dd->idx[j];
            i = j;
            break;
        }
    }
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

/* Append slot to the tail of a sibling list (the parent's children, or the
 * surface forest when parent_slot is -1). */
static void
link_child(struct drawdev *dd, int parent_slot, int slot)
{
    int *first = parent_slot < 0 ? &dd->root_first : &dd->nodes[parent_slot].first_child;
    int *last = parent_slot < 0 ? &dd->root_last : &dd->nodes[parent_slot].last_child;
    dd->nodes[slot].prev_sib = *last;
    dd->nodes[slot].next_sib = -1;
    if (*last >= 0)
        dd->nodes[*last].next_sib = slot;
    else
        *first = slot;
    *last = slot;
}

/* Detach slot from its sibling list. */
static void
unlink_child(struct drawdev *dd, int parent_slot, int slot)
{
    int *first = parent_slot < 0 ? &dd->root_first : &dd->nodes[parent_slot].first_child;
    int *last = parent_slot < 0 ? &dd->root_last : &dd->nodes[parent_slot].last_child;
    int p = dd->nodes[slot].prev_sib, n = dd->nodes[slot].next_sib;
    if (p >= 0)
        dd->nodes[p].next_sib = n;
    else
        *first = n;
    if (n >= 0)
        dd->nodes[n].prev_sib = p;
    else
        *last = p;
}

/* Create a node. Rejects id 0 (reserved), a duplicate id, or a full table.
 * Parent validation is the caller's, since a surface has none. Returns the new
 * node, or NULL. */
static struct dnode *
node_new(struct drawdev *dd, uint32_t id, uint8_t type, uint32_t parent)
{
    tree_init(dd);
    if (id == 0 || find(dd, id) >= 0 || dd->nfree == 0)
        return NULL;

    int slot = dd->freelist[--dd->nfree];
    struct dnode *n = &dd->nodes[slot];
    memset(n, 0, sizeof *n);
    n->in_use = 1;
    n->id = id;
    n->type = type;
    n->parent = parent;
    n->seq = dd->seqctr++;
    n->first_child = n->last_child = -1;
    n->prev_sib = n->next_sib = -1;
    n->fb_delta_new = -1;
    n->change = 1;              /* added this submit */
    dd->nchanged++;
    dd->nnodes++;

    idx_insert(dd, id, slot);
    link_child(dd, parent ? find(dd, parent) : -1, slot);
    return n;
}

/* Remove a node and, recursively, everything parented to it. */
static void
node_remove(struct drawdev *dd, uint32_t id)
{
    int s = find(dd, id);
    if (s < 0)
        return;

    /* Children first, walking the sibling list and capturing the next link
     * before the child unlinks itself. */
    for (int c = dd->nodes[s].first_child; c >= 0; ) {
        int next = dd->nodes[c].next_sib;
        node_remove(dd, dd->nodes[c].id);
        c = next;
    }

    struct dnode *n = &dd->nodes[s];
    unlink_child(dd, n->parent ? find(dd, n->parent) : -1, s);
    idx_remove(dd, id);
    if (n->type == DOP_SURFACE)
        dd->nsurf--;
    free(n->text);
    free(n->pts);
    free(n->segs);
    free(n->png_cache);
    for (int i = 0; i < n->fb_npatch; i++)
        free(n->fb_patch[i].b64);
    n->text = NULL;
    n->pts = NULL;
    n->segs = NULL;
    n->png_cache = NULL;
    n->fb_npatch = 0;
    n->fb_patch_area = 0;
    n->ndmg = 0;
    n->fb_delta_new = -1;
    n->in_use = 0;
    dd->freelist[dd->nfree++] = s;
    dd->nnodes--;
}

/* Flag a node as modified this submit, unless it is newly added (its full
 * fragment will carry the change). */
static void
mark_modified(struct drawdev *dd, int slot)
{
    if (dd->nodes[slot].change == 0) {
        dd->nodes[slot].change = 2;
        dd->nchanged++;
    }
}

/* Record a top-level removal for the delta, or overflow to a full push. */
static void
record_removed(struct drawdev *dd, uint32_t id)
{
    if (dd->nremoved < DRAW_DELTA_MAX)
        dd->removed_ids[dd->nremoved++] = id;
    else
        dd->delta_overflow = 1;
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

/* Fixed i16 argument count for a path command, or -1 if the command is unknown. */
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

/* Apply a single-attribute SET to a node. Geometry attributes apply only to the
 * node kinds that have the field; style attributes and VISIBLE apply to any
 * node. Returns 0, or -1 if the attribute does not apply to this node. */
static int
set_attr(struct dnode *n, uint16_t attr, long v)
{
    switch (attr) {
    case DSA_FILL:
        n->style.mask |= DST_FILL;
        n->style.fill = (uint32_t)v == DRAW_COLOR_NONE ? DRAW_COLOR_NONE
                                                       : (uint32_t)v & 0xffffffu;
        return 0;
    case DSA_STROKE:
        n->style.mask |= DST_STROKE;
        n->style.stroke = (uint32_t)v == DRAW_COLOR_NONE ? DRAW_COLOR_NONE
                                                        : (uint32_t)v & 0xffffffu;
        return 0;
    case DSA_STROKE_WIDTH:
        n->style.mask |= DST_STROKE_WIDTH;
        n->style.stroke_width = (uint16_t)(v > 1024 ? 1024 : v);
        return 0;
    case DSA_OPACITY:
        n->style.mask |= DST_OPACITY;
        n->style.opacity = (uint8_t)v;
        return 0;
    case DSA_FONT:
        n->style.mask |= DST_FONT;
        n->style.font = (uint8_t)(v > 2 ? 0 : v);
        return 0;
    case DSA_VISIBLE:
        n->hidden = !v;
        return 0;
    default:
        break;                      /* geometry: handled per node kind below */
    }

    switch (n->type) {
    case DOP_SURFACE:
        switch (attr) {
        case DSA_X: n->geo.surface.x = (int16_t)v; return 0;
        case DSA_Y: n->geo.surface.y = (int16_t)v; return 0;
        case DSA_W: n->geo.surface.w = (uint16_t)v; return 0;
        case DSA_H: n->geo.surface.h = (uint16_t)v; return 0;
        }
        break;
    case DOP_GROUP:
        switch (attr) {
        case DSA_X: n->geo.group.tx = (int16_t)v; return 0;
        case DSA_Y: n->geo.group.ty = (int16_t)v; return 0;
        }
        break;
    case DOP_RECT:
        switch (attr) {
        case DSA_X: n->geo.rect.x = (int16_t)v; return 0;
        case DSA_Y: n->geo.rect.y = (int16_t)v; return 0;
        case DSA_W: n->geo.rect.w = (uint16_t)v; return 0;
        case DSA_H: n->geo.rect.h = (uint16_t)v; return 0;
        }
        break;
    case DOP_CIRCLE:
        switch (attr) {
        case DSA_CX: n->geo.circle.cx = (int16_t)v; return 0;
        case DSA_CY: n->geo.circle.cy = (int16_t)v; return 0;
        case DSA_R:  n->geo.circle.r = (uint16_t)v; return 0;
        }
        break;
    case DOP_ELLIPSE:
        switch (attr) {
        case DSA_CX: n->geo.ellipse.cx = (int16_t)v; return 0;
        case DSA_CY: n->geo.ellipse.cy = (int16_t)v; return 0;
        case DSA_RX: n->geo.ellipse.rx = (uint16_t)v; return 0;
        case DSA_RY: n->geo.ellipse.ry = (uint16_t)v; return 0;
        }
        break;
    case DOP_LINE:
        switch (attr) {
        case DSA_X1: n->geo.line.x1 = (int16_t)v; return 0;
        case DSA_Y1: n->geo.line.y1 = (int16_t)v; return 0;
        case DSA_X2: n->geo.line.x2 = (int16_t)v; return 0;
        case DSA_Y2: n->geo.line.y2 = (int16_t)v; return 0;
        }
        break;
    case DOP_TEXT:
        switch (attr) {
        case DSA_X: n->geo.text.x = (int16_t)v; return 0;
        case DSA_Y: n->geo.text.y = (int16_t)v; return 0;
        }
        break;
    }
    return -1;                      /* attribute does not apply to this node */
}

/* Read a SET value at the width its attribute implies, into *out. Returns 0, or
 * -1 for an unknown attribute. */
static int
read_set_value(struct rd *r, uint16_t attr, long *out)
{
    switch (attr) {
    case DSA_X: case DSA_Y: case DSA_CX: case DSA_CY:
    case DSA_X1: case DSA_Y1: case DSA_X2: case DSA_Y2:
        *out = rd_i16(r);
        return 0;
    case DSA_W: case DSA_H: case DSA_R: case DSA_RX: case DSA_RY:
    case DSA_STROKE_WIDTH:
        *out = rd_u16(r);
        return 0;
    case DSA_FILL: case DSA_STROKE:
        *out = (long)rd_u32(r);
        return 0;
    case DSA_OPACITY: case DSA_VISIBLE: case DSA_FONT:
        *out = rd_u8(r);
        return 0;
    default:
        return -1;
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
        case DOP_POLYLINE: {
            uint32_t id = rd_u32(&r);
            uint32_t parent = rd_u32(&r);
            uint8_t closed = rd_u8(&r);
            uint16_t npts = rd_u16(&r);
            if (r.err || npts < 2 || npts > DRAW_MAXPTS)
                return -1;
            int16_t *pts = malloc((size_t)npts * 2 * sizeof *pts);
            if (!pts)
                return -1;
            for (int i = 0; i < npts * 2; i++)
                pts[i] = rd_i16(&r);
            struct draw_style st;
            rd_style(&r, &st);
            if (r.err || !parent_ok(dd, parent)) {
                free(pts);
                return -1;
            }
            struct dnode *n = node_new(dd, id, DOP_POLYLINE, parent);
            if (!n) {
                free(pts);
                return -1;
            }
            n->pts = pts;
            n->npts = npts;
            n->closed = closed ? 1 : 0;
            n->style = st;
            break;
        }
        case DOP_PATH: {
            uint32_t id = rd_u32(&r);
            uint32_t parent = rd_u32(&r);
            uint16_t nseg = rd_u16(&r);
            if (r.err || nseg == 0 || nseg > DRAW_MAXSEG)
                return -1;
            struct dseg *segs = malloc((size_t)nseg * sizeof *segs);
            if (!segs)
                return -1;
            for (int i = 0; i < nseg; i++) {
                uint8_t cmd = rd_u8(&r);
                int na = seg_args(cmd);
                if (r.err || na < 0) {
                    free(segs);
                    return -1;
                }
                segs[i].cmd = cmd;
                segs[i].nargs = (uint8_t)na;
                for (int k = 0; k < na; k++)
                    segs[i].a[k] = rd_i16(&r);
            }
            struct draw_style st;
            rd_style(&r, &st);
            if (r.err || !parent_ok(dd, parent)) {
                free(segs);
                return -1;
            }
            struct dnode *n = node_new(dd, id, DOP_PATH, parent);
            if (!n) {
                free(segs);
                return -1;
            }
            n->segs = segs;
            n->nsegs = nseg;
            n->style = st;
            break;
        }
        case DOP_SET: {
            uint32_t id = rd_u32(&r);
            uint16_t attr = rd_u16(&r);
            long v = 0;
            if (read_set_value(&r, attr, &v) != 0 || r.err)
                return -1;
            int slot = find(dd, id);
            if (slot < 0 || set_attr(&dd->nodes[slot], attr, v) != 0)
                return -1;
            mark_modified(dd, slot);
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
        case DOP_IMAGE: {
            uint32_t id = rd_u32(&r);
            uint32_t parent = rd_u32(&r);
            int16_t x = rd_i16(&r), y = rd_i16(&r);
            uint16_t w = rd_u16(&r), h = rd_u16(&r);
            uint8_t fmt = rd_u8(&r);
            uint32_t stride = rd_u32(&r);
            uint64_t gpa = rd_u64(&r);
            if (r.err || !parent_ok(dd, parent))
                return -1;
            /* Validate geometry; the pixels themselves are bounds-checked at
             * read time through the address translator. */
            if (fmt != DIMG_RGBA8888 || w == 0 || h == 0
                || w > DRAW_IMG_MAXDIM || h > DRAW_IMG_MAXDIM)
                return -1;
            if (stride == 0)
                stride = (uint32_t)w * 4;
            if (stride < (uint32_t)w * 4
                || (uint64_t)stride * h > DRAW_IMG_MAXBYTES)
                return -1;
            /* Re-submitting an existing image id updates the node and marks it
             * dirty, so a new frame from the guest flows as one node update. */
            struct dnode *n;
            int slot = find(dd, id);
            if (slot >= 0) {
                if (dd->nodes[slot].type != DOP_IMAGE)
                    return -1;
                n = &dd->nodes[slot];
                mark_modified(dd, slot);
            } else {
                n = node_new(dd, id, DOP_IMAGE, parent);
                if (!n)
                    return -1;
            }
            n->geo.image.x = x;
            n->geo.image.y = y;
            n->geo.image.w = w;
            n->geo.image.h = h;
            n->geo.image.fmt = fmt;
            n->geo.image.stride = stride;
            n->geo.image.gpa = gpa;
            n->img_dirty = 1;   /* re-base the whole frame */
            n->ndmg = 0;        /* a full submit supersedes pending damage */
            break;
        }
        case DOP_IMAGE_DAMAGE: {
            uint32_t id = rd_u32(&r);
            uint16_t x = rd_u16(&r), y = rd_u16(&r);
            uint16_t w = rd_u16(&r), h = rd_u16(&r);
            if (r.err)
                return -1;
            int slot = find(dd, id);
            if (slot < 0 || dd->nodes[slot].type != DOP_IMAGE)
                return -1;
            struct dnode *n = &dd->nodes[slot];
            /* Clamp the region to the image; an empty region is a no-op. */
            uint16_t iw = n->geo.image.w, ih = n->geo.image.h;
            if (x >= iw || y >= ih || w == 0 || h == 0)
                break;
            uint32_t x1 = (uint32_t)x + w, y1 = (uint32_t)y + h;
            if (x1 > iw) x1 = iw;
            if (y1 > ih) y1 = ih;
            if (n->ndmg == 0) {
                n->dmg_x0 = x; n->dmg_y0 = y;
                n->dmg_x1 = (uint16_t)x1; n->dmg_y1 = (uint16_t)y1;
            } else {
                if (x < n->dmg_x0) n->dmg_x0 = x;
                if (y < n->dmg_y0) n->dmg_y0 = y;
                if (x1 > n->dmg_x1) n->dmg_x1 = (uint16_t)x1;
                if (y1 > n->dmg_y1) n->dmg_y1 = (uint16_t)y1;
            }
            n->ndmg++;
            mark_modified(dd, slot);
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
            mark_modified(dd, slot);
            break;
        }
        case DOP_REMOVE: {
            uint32_t id = rd_u32(&r);
            if (r.err || find(dd, id) < 0)
                return -1;
            record_removed(dd, id);
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
            mark_modified(dd, slot);
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
            mark_modified(dd, slot);
            break;
        }
        default:
            return -1;              /* unknown opcode: a protocol error */
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

/* The attributes a shape's open tag ends with: style, interaction, and the
 * hidden flag. */
static void
emit_common(struct sbuf *b, const struct dnode *n)
{
    emit_style(b, &n->style);
    emit_inter_attrs(b, n);
    if (n->hidden)
        sb_puts(b, " display=\"none\"");
}

/* Emit one framebuffer overlay patch: a cropped image positioned within the
 * image node's translated group, at the region's pixel offset. */
static void
emit_fb_patch(struct sbuf *b, const struct fbpatch *p)
{
    sb_printf(b, "<image x=\"%u\" y=\"%u\" width=\"%u\" height=\"%u\""
              " xlink:href=\"data:image/png;base64,",
              p->x, p->y, p->w, p->h);
    sb_puts(b, p->b64);
    sb_puts(b, "\"/>");
}

static void emit_node(struct sbuf *b, struct drawdev *dd, struct dnode *n);

/* Emit a sibling list in creation order, following the next links. O(children),
 * so the whole scene serializes in one O(n) pass. */
static void
emit_siblings(struct sbuf *b, struct drawdev *dd, int head)
{
    for (int s = head; s >= 0; s = dd->nodes[s].next_sib)
        emit_node(b, dd, &dd->nodes[s]);
}

static void
emit_node(struct sbuf *b, struct drawdev *dd, struct dnode *n)
{
    switch (n->type) {
    case DOP_SURFACE:
        sb_printf(b, "<g id=\"qu-%d-%u\" class=\"qu-surface\""
                  " transform=\"translate(%d,%d)\"%s>",
                  dd->gid, n->id, n->geo.surface.x, n->geo.surface.y,
                  n->hidden ? " display=\"none\"" : "");
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
        emit_siblings(b, dd, n->first_child);
        sb_puts(b, "</g>");
        break;
    case DOP_GROUP:
        sb_printf(b, "<g id=\"qu-%d-%u\" transform=\"translate(%d,%d)\"%s>",
                  dd->gid, n->id, n->geo.group.tx, n->geo.group.ty,
                  n->hidden ? " display=\"none\"" : "");
        emit_siblings(b, dd, n->first_child);
        sb_puts(b, "</g>");
        break;
    case DOP_RECT:
        sb_printf(b, "<rect id=\"qu-%d-%u\" x=\"%d\" y=\"%d\""
                  " width=\"%u\" height=\"%u\" rx=\"%u\"",
                  dd->gid, n->id, n->geo.rect.x, n->geo.rect.y,
                  n->geo.rect.w, n->geo.rect.h, n->geo.rect.rx);
        emit_common(b, n);
        sb_puts(b, ">");
        emit_inter_smil(b, n);
        sb_puts(b, "</rect>");
        break;
    case DOP_CIRCLE:
        sb_printf(b, "<circle id=\"qu-%d-%u\" cx=\"%d\" cy=\"%d\" r=\"%u\"",
                  dd->gid, n->id, n->geo.circle.cx, n->geo.circle.cy,
                  n->geo.circle.r);
        emit_common(b, n);
        sb_puts(b, ">");
        emit_inter_smil(b, n);
        sb_puts(b, "</circle>");
        break;
    case DOP_ELLIPSE:
        sb_printf(b, "<ellipse id=\"qu-%d-%u\" cx=\"%d\" cy=\"%d\""
                  " rx=\"%u\" ry=\"%u\"",
                  dd->gid, n->id, n->geo.ellipse.cx, n->geo.ellipse.cy,
                  n->geo.ellipse.rx, n->geo.ellipse.ry);
        emit_common(b, n);
        sb_puts(b, ">");
        emit_inter_smil(b, n);
        sb_puts(b, "</ellipse>");
        break;
    case DOP_LINE:
        sb_printf(b, "<line id=\"qu-%d-%u\" x1=\"%d\" y1=\"%d\""
                  " x2=\"%d\" y2=\"%d\"",
                  dd->gid, n->id, n->geo.line.x1, n->geo.line.y1,
                  n->geo.line.x2, n->geo.line.y2);
        emit_common(b, n);
        sb_puts(b, ">");
        emit_inter_smil(b, n);
        sb_puts(b, "</line>");
        break;
    case DOP_POLYLINE: {
        const char *tag = n->closed ? "polygon" : "polyline";
        sb_printf(b, "<%s id=\"qu-%d-%u\" points=\"", tag, dd->gid, n->id);
        for (int i = 0; i < n->npts; i++)
            sb_printf(b, "%s%d,%d", i ? " " : "", n->pts[2 * i], n->pts[2 * i + 1]);
        sb_puts(b, "\"");
        emit_common(b, n);
        sb_puts(b, ">");
        emit_inter_smil(b, n);
        sb_printf(b, "</%s>", tag);
        break;
    }
    case DOP_PATH:
        sb_printf(b, "<path id=\"qu-%d-%u\" d=\"", dd->gid, n->id);
        for (int i = 0; i < n->nsegs; i++) {
            const struct dseg *s = &n->segs[i];
            sb_printf(b, "%s%c", i ? " " : "", s->cmd);
            for (int k = 0; k < s->nargs; k++)
                sb_printf(b, " %d", s->a[k]);
        }
        sb_puts(b, "\"");
        emit_common(b, n);
        sb_puts(b, ">");
        emit_inter_smil(b, n);
        sb_puts(b, "</path>");
        break;
    case DOP_TEXT:
        sb_printf(b, "<text id=\"qu-%d-%u\" x=\"%d\" y=\"%d\" font-size=\"%u\"",
                  dd->gid, n->id, n->geo.text.x, n->geo.text.y,
                  n->geo.text.size);
        emit_common(b, n);
        sb_puts(b, ">");
        sb_escape(b, n->text);
        sb_puts(b, "</text>");
        break;
    case DOP_IMAGE: {
        /* The pixels were resolved before serialization (fb_resolve). Emit the
         * node as a group holding the base image and any overlay patches, so a
         * later submit can append a patch under this same group. A node with no
         * base (an out-of-range buffer) is dropped. */
        if (!n->png_cache)
            break;
        sb_printf(b, "<g id=\"qu-%d-%u\" transform=\"translate(%d,%d)\"",
                  dd->gid, n->id, n->geo.image.x, n->geo.image.y);
        emit_common(b, n);
        sb_printf(b, "><image x=\"0\" y=\"0\" width=\"%u\" height=\"%u\""
                  " xlink:href=\"data:image/png;base64,",
                  n->geo.image.w, n->geo.image.h);
        sb_puts(b, n->png_cache);
        sb_puts(b, "\"/>");
        for (int i = 0; i < n->fb_npatch; i++)
            emit_fb_patch(b, &n->fb_patch[i]);
        sb_puts(b, "</g>");
        break;
    }
    default:
        break;
    }
}

/****************************************************************
 * Pixel path: resolve framebuffers into base images and patches
 ****************************************************************/

/* Read an image node's pixels from guest RAM, bounds-checked through the
 * address translator. Returns NULL if the declared buffer is out of range. */
static const uint8_t *
fb_pixels(struct drawdev *dd, struct dnode *n)
{
    uint64_t extent = (uint64_t)n->geo.image.stride * n->geo.image.h;
    return dd->gpa ? dd->gpa(dd->gpa_ctx, n->geo.image.gpa, extent) : NULL;
}

/* Re-encode the whole frame as the base image and drop every overlay patch. */
static void
fb_rebase(struct drawdev *dd, struct dnode *n)
{
    const uint8_t *px = fb_pixels(dd, n);
    char *b64 = px ? png_base64_rgba(px, n->geo.image.w, n->geo.image.h,
                                     (int)n->geo.image.stride) : NULL;
    free(n->png_cache);
    n->png_cache = b64;
    for (int i = 0; i < n->fb_npatch; i++) {
        free(n->fb_patch[i].b64);
        n->fb_patch[i].b64 = NULL;
    }
    n->fb_npatch = 0;
    n->fb_patch_area = 0;
}

/* Resolve one image node's pixels for this submit into a base image and overlay
 * patches, and record how the delta should carry the change:
 *   fb_delta_new >= 0   one appended patch, sent as an N under the node
 *   fb_delta_new == -1  the whole node (re-based or newly added), sent in full
 * Runs once per submit, before the scene is serialized. */
static void
fb_resolve(struct drawdev *dd, struct dnode *n)
{
    n->fb_delta_new = -1;
    if (n->img_dirty || !n->png_cache) {
        fb_rebase(dd, n);
        n->img_dirty = 0;
        n->ndmg = 0;
        return;
    }
    if (n->ndmg == 0)
        return;
    uint16_t rx = n->dmg_x0, ry = n->dmg_y0;
    uint16_t rw = (uint16_t)(n->dmg_x1 - n->dmg_x0);
    uint16_t rh = (uint16_t)(n->dmg_y1 - n->dmg_y0);
    n->ndmg = 0;
    uint32_t area = (uint32_t)rw * rh;
    uint32_t frame = (uint32_t)n->geo.image.w * n->geo.image.h;
    /* Past the patch cap, or once the accumulated patch area would reach a full
     * frame, re-base: one full re-encode is cheaper than more overlays and it
     * keeps the DOM bounded. A read failure falls back to a re-base too. */
    const uint8_t *px;
    if (n->fb_npatch >= FB_MAXPATCH || n->fb_patch_area + area >= frame
        || !(px = fb_pixels(dd, n))) {
        fb_rebase(dd, n);
        return;
    }
    uint32_t stride = n->geo.image.stride;
    const uint8_t *crop = px + (size_t)ry * stride + (size_t)rx * 4;
    char *b64 = png_base64_rgba(crop, rw, rh, (int)stride);
    if (!b64) {
        fb_rebase(dd, n);
        return;
    }
    int i = n->fb_npatch++;
    n->fb_patch[i].x = rx;
    n->fb_patch[i].y = ry;
    n->fb_patch[i].w = rw;
    n->fb_patch[i].h = rh;
    n->fb_patch[i].b64 = b64;
    n->fb_patch_area += area;
    n->fb_delta_new = i;
}

/* Resolve every image node that needs it before the scene is serialized. */
static void
fb_resolve_all(struct drawdev *dd)
{
    for (int i = 0; i < DRAW_MAXNODES; i++) {
        struct dnode *n = &dd->nodes[i];
        if (n->in_use && n->type == DOP_IMAGE
            && (n->img_dirty || n->ndmg || !n->png_cache))
            fb_resolve(dd, n);
    }
}

/* Canvas size: enough to contain every surface. Surfaces are the forest roots,
 * so walk the root list rather than scanning the whole table. */
static void
canvas_size(struct drawdev *dd, int *cw, int *ch)
{
    *cw = 1;
    *ch = 1;
    for (int s = dd->root_first; s >= 0; s = dd->nodes[s].next_sib) {
        const struct dnode *n = &dd->nodes[s];
        int right = n->geo.surface.x + n->geo.surface.w;
        int bottom = n->geo.surface.y + n->geo.surface.h;
        if (right > *cw)
            *cw = right;
        if (bottom > *ch)
            *ch = bottom;
    }
}

/* Serialize the whole scene to one SVG document. Returns a malloc'd NUL-
 * terminated string the caller frees, or NULL. */
static char *
serialize(struct drawdev *dd)
{
    int cw, ch;
    canvas_size(dd, &cw, &ch);

    struct sbuf b = { 0 };
    sb_printf(&b, "<svg xmlns=\"http://www.w3.org/2000/svg\""
              " xmlns:xlink=\"http://www.w3.org/1999/xlink\""
              " width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\">",
              cw, ch, cw, ch);
    emit_siblings(&b, dd, dd->root_first);
    sb_puts(&b, "</svg>");
    return b.p;
}

/* Walk the forest in document order emitting the changed nodes as delta
 * records. A changed node's whole fragment (its subtree) is emitted once, so
 * descendants it already covers are suppressed. */
static void
emit_changes(struct sbuf *b, struct drawdev *dd, int head, int suppressed)
{
    for (int s = head; s >= 0; s = dd->nodes[s].next_sib) {
        struct dnode *n = &dd->nodes[s];
        int emitted = 0;
        if (!suppressed && n->change) {
            if (n->type == DOP_IMAGE && n->change == 2 && n->fb_delta_new >= 0) {
                /* Only a region changed: append the patch under the image group
                 * rather than re-sending the whole frame. */
                sb_printf(b, "\nN %u ", n->id);
                emit_fb_patch(b, &n->fb_patch[n->fb_delta_new]);
            } else if (n->change == 1) {
                sb_printf(b, "\nN %u ", n->parent);     /* add under parent */
                emit_node(b, dd, n);
            } else {
                sb_puts(b, "\nU ");                      /* replace by id */
                emit_node(b, dd, n);
            }
            emitted = 1;
        }
        emit_changes(b, dd, n->first_child, suppressed || emitted);
    }
}

/* Build an incremental update: a canvas record, the removals, then the changed
 * nodes. Records are newline-separated; fragments contain no newlines. Returns
 * a malloc'd string the caller frees. */
static char *
build_delta(struct drawdev *dd)
{
    int cw, ch;
    canvas_size(dd, &cw, &ch);

    struct sbuf b = { 0 };
    sb_printf(&b, "!%d %d", cw, ch);
    for (int i = 0; i < dd->nremoved; i++)
        sb_printf(&b, "\nR %u", dd->removed_ids[i]);
    emit_changes(&b, dd, dd->root_first, 0);
    return b.p;
}

/* Clear the per-submit change set once the update has been built. */
static void
clear_changes(struct drawdev *dd)
{
    for (int i = 0; i < DRAW_MAXNODES; i++) {
        dd->nodes[i].change = 0;
        dd->nodes[i].fb_delta_new = -1;
    }
    dd->nremoved = 0;
    dd->nchanged = 0;
    dd->delta_overflow = 0;
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
    pthread_mutex_lock(&dd->qlock);
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
    pthread_mutex_unlock(&dd->qlock);
}

/* Deliver one inbound event to a sink: queue it and rouse that guest, the same
 * evt-plus-wake path the self-test source uses. */
static void
deliver_event(struct drawdev *dd, uint32_t node, uint16_t type,
              const char *value, int vlen)
{
    if (vlen < 0)
        vlen = 0;
    enqueue(dd, node, type, value, (uint16_t)vlen);
    __atomic_store_n(&dd->chan->evt, 1, __ATOMIC_SEQ_CST);
    if (dd->wake)
        dd->wake(dd->wake_ctx);
}

/* Inbound event from the browser (on the WS server thread), routed by the front
 * end to the guest it targets. The browser does not yet tag the event with its
 * guest, so it is delivered to the first registered sink; per-guest routing by
 * the DOM id's guest field is M10.2. */
static void
fe_on_event(void *ctx, uint32_t node, uint16_t type, const char *value, int vlen)
{
    struct drawfe *fe = ctx;

    for (int i = 0; i < DRAW_MAXGUEST; i++) {
        if (fe->sinks[i]) {
            deliver_event(fe->sinks[i], node, type, value, vlen);
            return;
        }
    }
}

/****************************************************************
 * Channel handling
 ****************************************************************/

/* Start the shared session front end bound to 127.0.0.1:port, or return NULL
 * when port is 0 (headless). One of these backs the whole process; guests
 * attach their sinks to it in draw_init. */
struct drawfe *
drawfe_start(int port)
{
    if (port <= 0)
        return NULL;

    struct drawfe *fe = calloc(1, sizeof(*fe));
    if (!fe)
        return NULL;
    fe->ws = drawws_start(port, fe_on_event, fe);
    if (!fe->ws) {
        free(fe);
        return NULL;
    }
    return fe;
}

void
drawfe_stop(struct drawfe *fe)
{
    if (!fe)
        return;
    drawws_stop(fe->ws);
    free(fe);
}

/* Push a guest's authored scene to the browser through the front end. Today a
 * single guest's full document and delta are forwarded unchanged; compositing
 * several guests' scenes into one document arrives with M10.1c. */
static void
drawfe_publish(struct drawfe *fe, const char *full, const char *delta)
{
    if (fe && fe->ws)
        drawws_update(fe->ws, full, delta);
}

struct drawdev *
draw_init(struct jotun_draw *chan, int gid, struct drawfe *fe,
          void (*wake)(void *ctx), void *wake_ctx, int test,
          draw_gpa_fn gpa, void *gpa_ctx)
{
    struct drawdev *dd = calloc(1, sizeof(*dd));
    if (!dd)
        return NULL;
    dd->chan = chan;
    dd->gid = gid;
    dd->fe = fe;
    dd->wake = wake;
    dd->wake_ctx = wake_ctx;
    dd->test = test;
    dd->gpa = gpa;
    dd->gpa_ctx = gpa_ctx;
    dd->bench = getenv("JOTUN_DRAW_BENCH") != NULL;
    dd->svg_path = getenv("JOTUN_DRAW_SVG");
    pthread_mutex_init(&dd->qlock, NULL);
    tree_init(dd);              /* so an empty forest has root_first == -1 */

    /* Register as the front end's sink for this guest, so inbound events reach
     * it. The front end's lifecycle is owned by the session, not the sink. */
    if (fe && gid >= 0 && gid < DRAW_MAXGUEST)
        fe->sinks[gid] = dd;
    return dd;
}

static void
dump_svg(struct drawdev *dd, const char *svg)
{
    FILE *f = fopen(dd->svg_path, "w");
    if (f) {
        fputs(svg, f);
        fputc('\n', f);
        fclose(f);
    }
}

static void
handle_submit(struct drawdev *dd, struct jotun_draw *chan)
{
    uint32_t len = chan->len;
    if (len > JOTUN_DRAW_MSIZE) {
        chan->status = DRAW_EINVAL;
        return;
    }

    struct timespec t0, t1, t2;
    if (dd->bench)
        clock_gettime(CLOCK_MONOTONIC, &t0);

    uint32_t click_node = 0;
    int rc = apply(dd, chan->buf, len, dd->test ? &click_node : NULL);

    if (dd->bench)
        clock_gettime(CLOCK_MONOTONIC, &t1);

    /* Resolve framebuffers into base images and overlay patches before the
     * scene is authored, so a region-only change re-encodes just that region. */
    fb_resolve_all(dd);

    /* Author the scene once: push it to the browser and, when requested, drop a
     * snapshot to disk. A partial scene from a rejected stream is still shown,
     * so it is inspectable during bring-up. */
    char *svg = serialize(dd);

    if (dd->bench) {
        clock_gettime(CLOCK_MONOTONIC, &t2);
        long au = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000;
        long su = (t2.tv_sec - t1.tv_sec) * 1000000L + (t2.tv_nsec - t1.tv_nsec) / 1000;
        fprintf(stderr, "[draw bench] nodes=%d svg=%zu apply=%ld us serialize=%ld us\n",
                dd->nnodes, svg ? strlen(svg) : 0, au, su);
    }

    /* Push the authored scene. The full document keeps new browsers correct and
     * backs the on-disk snapshot; a small change set also produces a delta the
     * front end sends in place of the whole document. A large change set or an
     * overflow of tracked removals falls back to the full push. */
    if (svg) {
        char *delta = NULL;
        int overflow = dd->delta_overflow
            || dd->nchanged + dd->nremoved > DRAW_DELTA_MAX;
        if (!overflow && (dd->nchanged || dd->nremoved))
            delta = build_delta(dd);
        drawfe_publish(dd->fe, svg, delta);
        if (dd->svg_path)
            dump_svg(dd, svg);
        free(delta);
        free(svg);
    }
    clear_changes(dd);

    if (rc != 0) {
        chan->status = DRAW_EINVAL;
        return;
    }
    chan->status = DRAW_OK;

    if (dd->test && click_node) {
        enqueue(dd, click_node, DEV_CLICK, NULL, 0);
        __atomic_store_n(&chan->evt, 1, __ATOMIC_SEQ_CST);
        if (dd->wake)
            dd->wake(dd->wake_ctx);
    }
}

static void
handle_poll(struct drawdev *dd, struct jotun_draw *chan)
{
    struct draw_evt ev;
    pthread_mutex_lock(&dd->qlock);
    if (dd->count == 0) {
        pthread_mutex_unlock(&dd->qlock);
        chan->status = DRAW_OK;
        chan->len = 0;
        return;
    }
    ev = dd->q[dd->head];
    dd->head = (dd->head + 1) % DRAW_MAXEVT;
    dd->count--;
    pthread_mutex_unlock(&dd->qlock);

    struct draw_evt *e = &ev;

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
    /* The front end (dd->fe) is owned and stopped by the session, not here. */
    if (dd->fe && dd->gid >= 0 && dd->gid < DRAW_MAXGUEST
        && dd->fe->sinks[dd->gid] == dd)
        dd->fe->sinks[dd->gid] = NULL;
    for (int i = 0; i < DRAW_MAXNODES; i++) {
        free(dd->nodes[i].text);
        free(dd->nodes[i].pts);
        free(dd->nodes[i].segs);
    }
    pthread_mutex_destroy(&dd->qlock);
    free(dd);
}
