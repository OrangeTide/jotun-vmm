/* jotun_9p.h : the 9P2000 transport channel and a small wire codec.
 *
 * Shared verbatim by the host 9P server and the freestanding guest 9P client.
 * It uses only fixed-width integer types and byte-wise access, no libc, so the
 * guest can include it.
 *
 * Transport: one shared buffer pair in guest RAM holds a single outstanding
 * request and its reply. The guest writes a T-message into req[] with its
 * length in req_len, stores to the 9P doorbell (a synchronous MMIO exit), and
 * the host processes it inline and writes the R-message into rsp[]/rsp_len
 * before the store returns. Pipelining with many tags is a later milestone;
 * this first cut is one message at a time. */

#ifndef JOTUN_9P_H
#define JOTUN_9P_H

#include <stdint.h>

/* Low-memory map: the 9P channel sits above the boot block (jotun_boot.h),
 * below the guest image. The doorbell is an unmapped MMIO address near the
 * ring doorbell (jotun_abi.h). */
#define JOTUN_P9_BASE       0x8000u
#define JOTUN_P9_DOORBELL   0x0a000008ull
#define JOTUN_P9_MSIZE      8192u

struct jotun_p9 {
    uint32_t req_len;               /* bytes of a T-message in req */
    uint32_t rsp_len;              /* bytes of an R-message in rsp */
    uint8_t  req[JOTUN_P9_MSIZE];
    uint8_t  rsp[JOTUN_P9_MSIZE];
};

/* 9P2000 message types. */
enum {
    P9_TVERSION = 100, P9_RVERSION = 101,
    P9_TATTACH  = 104, P9_RATTACH  = 105,
    P9_RERROR   = 107,
    P9_TWALK    = 110, P9_RWALK    = 111,
    P9_TOPEN    = 112, P9_ROPEN    = 113,
    P9_TREAD    = 116, P9_RREAD    = 117,
    P9_TCLUNK   = 120, P9_RCLUNK   = 121,
    P9_TSTAT    = 124, P9_RSTAT    = 125,
};

/* qid.type and stat.mode high bits. */
#define P9_QTDIR    0x80u
#define P9_QTFILE   0x00u
#define P9_DMDIR    0x80000000u

#define P9_NOFID    0xffffffffu
#define P9_NOTAG    0xffffu
#define P9_OREAD    0u              /* open mode: read only */
#define P9_MAXWELEM 16              /* max walk name elements */

/* A qid is 13 wire bytes: type[1] version[4] path[8]. */
struct p9_qid {
    uint8_t  type;
    uint32_t version;
    uint64_t path;
};

/****************************************************************
 * Build cursor: append fields, little-endian, bounds-checked.
 ****************************************************************/

struct p9_out {
    uint8_t  *b;
    uint32_t cap;
    uint32_t n;
    int      ok;                    /* cleared on overflow */
};

static inline void
p9o_init(struct p9_out *o, uint8_t *buf, uint32_t cap)
{
    o->b = buf;
    o->cap = cap;
    o->n = 0;
    o->ok = 1;
}

static inline void
p9o_u8(struct p9_out *o, uint8_t v)
{
    if (!o->ok || o->n + 1 > o->cap) { o->ok = 0; return; }
    o->b[o->n++] = v;
}

static inline void
p9o_u16(struct p9_out *o, uint16_t v)
{
    p9o_u8(o, (uint8_t)v);
    p9o_u8(o, (uint8_t)(v >> 8));
}

static inline void
p9o_u32(struct p9_out *o, uint32_t v)
{
    p9o_u16(o, (uint16_t)v);
    p9o_u16(o, (uint16_t)(v >> 16));
}

static inline void
p9o_u64(struct p9_out *o, uint64_t v)
{
    p9o_u32(o, (uint32_t)v);
    p9o_u32(o, (uint32_t)(v >> 32));
}

static inline void
p9o_bytes(struct p9_out *o, const uint8_t *d, uint32_t len)
{
    if (!o->ok || o->n + len > o->cap) { o->ok = 0; return; }
    for (uint32_t i = 0; i < len; i++)
        o->b[o->n++] = d[i];
}

/* A 9P string: len[2] followed by the bytes, no terminator. */
static inline void
p9o_str(struct p9_out *o, const char *s)
{
    uint32_t len = 0;
    while (s[len] != '\0')
        len++;
    p9o_u16(o, (uint16_t)len);
    p9o_bytes(o, (const uint8_t *)s, len);
}

static inline void
p9o_qid(struct p9_out *o, const struct p9_qid *q)
{
    p9o_u8(o, q->type);
    p9o_u32(o, q->version);
    p9o_u64(o, q->path);
}

/* Write a message header (size placeholder, type, tag) at the front. Call
 * before appending the body, then p9o_finish to fill in size. */
static inline void
p9o_hdr(struct p9_out *o, uint8_t type, uint16_t tag)
{
    p9o_u32(o, 0);                  /* size, patched by p9o_finish */
    p9o_u8(o, type);
    p9o_u16(o, tag);
}

static inline uint32_t
p9o_finish(struct p9_out *o)
{
    if (!o->ok)
        return 0;
    uint32_t sz = o->n;
    o->b[0] = (uint8_t)sz;
    o->b[1] = (uint8_t)(sz >> 8);
    o->b[2] = (uint8_t)(sz >> 16);
    o->b[3] = (uint8_t)(sz >> 24);
    return sz;
}

/****************************************************************
 * Parse cursor: read fields, little-endian, bounds-checked.
 ****************************************************************/

struct p9_in {
    const uint8_t *b;
    uint32_t len;
    uint32_t p;
    int      ok;                    /* cleared on underflow */
};

static inline void
p9i_init(struct p9_in *in, const uint8_t *buf, uint32_t len)
{
    in->b = buf;
    in->len = len;
    in->p = 0;
    in->ok = 1;
}

static inline uint8_t
p9i_u8(struct p9_in *in)
{
    if (!in->ok || in->p + 1 > in->len) { in->ok = 0; return 0; }
    return in->b[in->p++];
}

static inline uint16_t
p9i_u16(struct p9_in *in)
{
    uint16_t a = p9i_u8(in);
    uint16_t b = p9i_u8(in);
    return (uint16_t)(a | (b << 8));
}

static inline uint32_t
p9i_u32(struct p9_in *in)
{
    uint32_t a = p9i_u16(in);
    uint32_t b = p9i_u16(in);
    return a | (b << 16);
}

static inline uint64_t
p9i_u64(struct p9_in *in)
{
    uint64_t a = p9i_u32(in);
    uint64_t b = p9i_u32(in);
    return a | (b << 32);
}

/* Read a 9P string into dst (NUL-terminated, truncated to dstcap). Returns the
 * on-wire length. */
static inline uint16_t
p9i_str(struct p9_in *in, char *dst, uint32_t dstcap)
{
    uint16_t len = p9i_u16(in);
    uint32_t i = 0;
    for (; i < len; i++) {
        uint8_t c = p9i_u8(in);
        if (dst && i + 1 < dstcap)
            dst[i] = (char)c;
    }
    if (dst && dstcap > 0)
        dst[(i < dstcap) ? i : dstcap - 1] = '\0';
    return len;
}

static inline void
p9i_qid(struct p9_in *in, struct p9_qid *q)
{
    q->type = p9i_u8(in);
    q->version = p9i_u32(in);
    q->path = p9i_u64(in);
}

#endif /* JOTUN_9P_H */
