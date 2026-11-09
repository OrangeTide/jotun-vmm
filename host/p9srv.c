/* p9srv.c : the read-only 9P2000 protocol server.
 *
 * One request is handled at a time. The server decodes a T-message, consults
 * the backend, and encodes the R-message. Directory reads serialize the
 * backend's listing into 9P stat structures and serve byte ranges of that. */
#include "p9srv.h"

#include <stdlib.h>
#include <string.h>

#include "jotun_9p.h"

void
p9srv_init(struct p9srv *s, const struct p9_backend *be, void *ctx)
{
    memset(s, 0, sizeof(*s));
    s->be = be;
    s->ctx = ctx;
}

static int
find_fid(struct p9srv *s, uint32_t fid)
{
    for (int i = 0; i < P9_MAXFID; i++)
        if (s->fids[i].used && s->fids[i].fid == fid)
            return i;
    return -1;
}

static int
alloc_fid(struct p9srv *s, uint32_t fid)
{
    int i = find_fid(s, fid);
    if (i >= 0)
        return i;
    for (i = 0; i < P9_MAXFID; i++) {
        if (!s->fids[i].used) {
            s->fids[i].used = 1;
            s->fids[i].open = 0;
            s->fids[i].fid = fid;
            return i;
        }
    }
    return -1;
}

static void
put_qid(struct p9_out *o, const struct p9_node *n)
{
    struct p9_qid q = {
        .type = n->is_dir ? P9_QTDIR : P9_QTFILE,
        .version = 0,
        .path = n->qid_path,
    };
    p9o_qid(o, &q);
}

/* Serialize one stat structure (its own size[2] prefix included). */
static void
put_stat(struct p9_out *o, const char *name, int is_dir, uint32_t mode,
         uint64_t length, uint64_t qid_path)
{
    uint16_t namelen = (uint16_t)strlen(name);
    uint16_t sz = (uint16_t)(47 + namelen);     /* bytes after this size field */
    struct p9_qid q = {
        .type = is_dir ? P9_QTDIR : P9_QTFILE,
        .version = 0,
        .path = qid_path,
    };

    p9o_u16(o, sz);
    p9o_u16(o, 0);                  /* type */
    p9o_u32(o, 0);                  /* dev */
    p9o_qid(o, &q);
    p9o_u32(o, (mode & 0777u) | (is_dir ? P9_DMDIR : 0u));
    p9o_u32(o, 0);                  /* atime */
    p9o_u32(o, 0);                  /* mtime */
    p9o_u64(o, length);
    p9o_str(o, name);
    p9o_str(o, "");                 /* uid */
    p9o_str(o, "");                 /* gid */
    p9o_str(o, "");                 /* muid */
}

/* Append one serialized stat entry to the server's growable directory buffer.
 * The emit_ctx is the p9srv. Grows the buffer as needed; -1 only if out of
 * memory, so a listing is bounded by host memory, not a fixed count. */
static int
dir_ser_emit(void *vctx, const struct p9_dirent *de)
{
    struct p9srv *s = vctx;
    uint8_t tmp[384];                   /* one stat: 2 + 47 + name(<=255) */
    struct p9_out o;

    p9o_init(&o, tmp, sizeof tmp);
    put_stat(&o, de->name, de->is_dir, de->mode, de->length, de->qid_path);
    if (!o.ok)
        return 0;                       /* skip an entry that will not fit */

    if (s->dirbuf_len + o.n > s->dirbuf_cap) {
        uint32_t ncap = s->dirbuf_cap ? s->dirbuf_cap : 65536;
        while (ncap < s->dirbuf_len + o.n)
            ncap *= 2;
        uint8_t *nb = realloc(s->dirbuf, ncap);
        if (!nb)
            return -1;
        s->dirbuf = nb;
        s->dirbuf_cap = ncap;
    }
    memcpy(s->dirbuf + s->dirbuf_len, tmp, o.n);
    s->dirbuf_len += o.n;
    return 0;
}

/* Build an R-message header into o. */
static void
reply_hdr(struct p9_out *o, uint8_t *rsp, uint32_t rspcap, uint8_t type,
          uint16_t tag)
{
    p9o_init(o, rsp, rspcap);
    p9o_hdr(o, type, tag);
}

static uint32_t
reply_error(uint8_t *rsp, uint32_t rspcap, uint16_t tag, const char *msg)
{
    struct p9_out o;

    reply_hdr(&o, rsp, rspcap, P9_RERROR, tag);
    p9o_str(&o, msg);
    return p9o_finish(&o);
}

uint32_t
p9srv_handle(struct p9srv *s, const uint8_t *req, uint32_t reqlen,
             uint8_t *rsp, uint32_t rspcap)
{
    struct p9_in in;
    struct p9_out o;

    p9i_init(&in, req, reqlen);
    (void)p9i_u32(&in);             /* size */
    uint8_t type = p9i_u8(&in);
    uint16_t tag = p9i_u16(&in);
    if (!in.ok)
        return reply_error(rsp, rspcap, P9_NOTAG, "short message");

    switch (type) {
    case P9_TVERSION: {
        uint32_t msize = p9i_u32(&in);
        char ver[32];
        p9i_str(&in, ver, sizeof ver);
        if (msize > JOTUN_P9_MSIZE)
            msize = JOTUN_P9_MSIZE;
        /* A new session: drop any existing fids. */
        for (int i = 0; i < P9_MAXFID; i++)
            s->fids[i].used = 0;
        reply_hdr(&o, rsp, rspcap, P9_RVERSION, tag);
        p9o_u32(&o, msize);
        p9o_str(&o, "9P2000");
        return p9o_finish(&o);
    }
    case P9_TATTACH: {
        uint32_t fid = p9i_u32(&in);
        (void)p9i_u32(&in);         /* afid */
        char uname[64], aname[64];
        p9i_str(&in, uname, sizeof uname);
        p9i_str(&in, aname, sizeof aname);
        if (!in.ok)
            return reply_error(rsp, rspcap, tag, "bad Tattach");
        int i = alloc_fid(s, fid);
        if (i < 0)
            return reply_error(rsp, rspcap, tag, "no fids");
        if (s->be->root(s->ctx, &s->fids[i].node) != 0) {
            s->fids[i].used = 0;
            return reply_error(rsp, rspcap, tag, "no root");
        }
        reply_hdr(&o, rsp, rspcap, P9_RATTACH, tag);
        put_qid(&o, &s->fids[i].node);
        return p9o_finish(&o);
    }
    case P9_TWALK: {
        uint32_t fid = p9i_u32(&in);
        uint32_t newfid = p9i_u32(&in);
        uint16_t nwname = p9i_u16(&in);
        if (nwname > P9_MAXWELEM)
            return reply_error(rsp, rspcap, tag, "too many names");
        int fi = find_fid(s, fid);
        if (fi < 0)
            return reply_error(rsp, rspcap, tag, "bad fid");

        struct p9_node cur = s->fids[fi].node;
        struct p9_qid qids[P9_MAXWELEM];
        int nq = 0;
        for (int k = 0; k < nwname; k++) {
            char name[256];
            p9i_str(&in, name, sizeof name);
            if (!in.ok)
                return reply_error(rsp, rspcap, tag, "bad Twalk");
            struct p9_node next;
            if (!cur.is_dir || s->be->walk(s->ctx, &cur, name, &next) != 0)
                break;
            cur = next;
            qids[nq].type = cur.is_dir ? P9_QTDIR : P9_QTFILE;
            qids[nq].version = 0;
            qids[nq].path = cur.qid_path;
            nq++;
        }
        /* A walk that matched no leading component at all is an error; a
         * partial walk returns the qids it managed and sets no new fid. */
        if (nwname > 0 && nq == 0)
            return reply_error(rsp, rspcap, tag, "not found");
        if (nq == nwname) {
            int ni = alloc_fid(s, newfid);
            if (ni < 0)
                return reply_error(rsp, rspcap, tag, "no fids");
            s->fids[ni].node = cur;
            s->fids[ni].open = 0;
        }
        reply_hdr(&o, rsp, rspcap, P9_RWALK, tag);
        p9o_u16(&o, (uint16_t)nq);
        for (int k = 0; k < nq; k++)
            p9o_qid(&o, &qids[k]);
        return p9o_finish(&o);
    }
    case P9_TOPEN: {
        uint32_t fid = p9i_u32(&in);
        uint8_t mode = p9i_u8(&in);
        int fi = find_fid(s, fid);
        if (fi < 0)
            return reply_error(rsp, rspcap, tag, "bad fid");
        /* A write-intent open needs a writable backend. OTRUNC empties an
         * existing file here, since the overlay's create path only reaches a
         * file that already exists through a plain open. */
        int wants_write = (mode & 3) == P9_OWRITE || (mode & 3) == P9_ORDWR
                          || (mode & P9_OTRUNC);
        if (wants_write && !s->be->writef)
            return reply_error(rsp, rspcap, tag, "read-only");
        if ((mode & P9_OTRUNC) && !s->fids[fi].node.is_dir) {
            if (!s->be->truncate ||
                s->be->truncate(s->ctx, &s->fids[fi].node, 0) != 0)
                return reply_error(rsp, rspcap, tag, "truncate failed");
            s->fids[fi].node.length = 0;
        }
        s->fids[fi].open = 1;
        reply_hdr(&o, rsp, rspcap, P9_ROPEN, tag);
        put_qid(&o, &s->fids[fi].node);
        p9o_u32(&o, 0);             /* iounit */
        return p9o_finish(&o);
    }
    case P9_TREAD: {
        uint32_t fid = p9i_u32(&in);
        uint64_t offset = p9i_u64(&in);
        uint32_t count = p9i_u32(&in);
        int fi = find_fid(s, fid);
        if (fi < 0)
            return reply_error(rsp, rspcap, tag, "bad fid");
        struct p9_node *n = &s->fids[fi].node;

        /* Bound the reply to the buffer: header(7) + count[4] = 11 bytes. */
        uint32_t room = (rspcap > 11) ? rspcap - 11 : 0;
        if (count > room)
            count = room;

        reply_hdr(&o, rsp, rspcap, P9_RREAD, tag);

        if (n->is_dir) {
            /* Serialize the whole listing once, at the start of a read (offset
             * 0) or when the cache holds a different fid, then serve the
             * requested byte slice of it. A read that splits a stat entry at
             * its end is fine: the client consumes only whole entries and
             * re-reads from the next entry's offset. */
            if (offset == 0 || !s->dir_valid || s->dir_fid != fid) {
                s->dirbuf_len = 0;
                if (s->be->list(s->ctx, n, dir_ser_emit, s) != 0)
                    return reply_error(rsp, rspcap, tag, "list failed");
                s->dir_fid = fid;
                s->dir_valid = 1;
            }
            uint32_t total = s->dirbuf_len;
            uint32_t start = (offset < total) ? (uint32_t)offset : total;
            uint32_t avail = total - start;
            if (count > avail)
                count = avail;
            p9o_u32(&o, count);
            p9o_bytes(&o, s->dirbuf + start, count);
        } else {
            uint8_t tmp[JOTUN_P9_MSIZE];
            if (count > sizeof tmp)
                count = sizeof tmp;
            long got = s->be->readf(s->ctx, n, offset, tmp, count);
            if (got < 0)
                return reply_error(rsp, rspcap, tag, "read failed");
            p9o_u32(&o, (uint32_t)got);
            p9o_bytes(&o, tmp, (uint32_t)got);
        }
        return p9o_finish(&o);
    }
    case P9_TCLUNK: {
        uint32_t fid = p9i_u32(&in);
        int fi = find_fid(s, fid);
        if (fi >= 0)
            s->fids[fi].used = 0;
        if (s->dir_valid && s->dir_fid == fid)
            s->dir_valid = 0;           /* its cached listing is now stale */
        reply_hdr(&o, rsp, rspcap, P9_RCLUNK, tag);
        return p9o_finish(&o);
    }
    case P9_TSTAT: {
        uint32_t fid = p9i_u32(&in);
        int fi = find_fid(s, fid);
        if (fi < 0)
            return reply_error(rsp, rspcap, tag, "bad fid");
        struct p9_node *n = &s->fids[fi].node;
        const char *name = n->name[0] ? n->name : "/";
        reply_hdr(&o, rsp, rspcap, P9_RSTAT, tag);
        /* Rstat carries a 2-byte count before the stat structure. */
        uint16_t namelen = (uint16_t)strlen(name);
        p9o_u16(&o, (uint16_t)(49 + namelen));
        put_stat(&o, name, n->is_dir, n->mode, n->length, n->qid_path);
        return p9o_finish(&o);
    }
    case P9_TCREATE: {
        /* Create a child of the directory fid points at, then make fid refer to
         * the new file, open for writing (9P2000 semantics). perm's P9_DMDIR
         * bit selects a directory. */
        uint32_t fid = p9i_u32(&in);
        char name[256];
        p9i_str(&in, name, sizeof name);
        uint32_t perm = p9i_u32(&in);
        (void)p9i_u8(&in);          /* mode; we always open the result writable */
        if (!in.ok)
            return reply_error(rsp, rspcap, tag, "bad Tcreate");
        int fi = find_fid(s, fid);
        if (fi < 0)
            return reply_error(rsp, rspcap, tag, "bad fid");
        if (!s->be->create)
            return reply_error(rsp, rspcap, tag, "read-only");
        struct p9_node nn;
        if (s->be->create(s->ctx, &s->fids[fi].node, name, perm, &nn) != 0)
            return reply_error(rsp, rspcap, tag, "create failed");
        s->fids[fi].node = nn;
        s->fids[fi].open = 1;
        reply_hdr(&o, rsp, rspcap, P9_RCREATE, tag);
        put_qid(&o, &s->fids[fi].node);
        p9o_u32(&o, 0);             /* iounit */
        return p9o_finish(&o);
    }
    case P9_TWRITE: {
        uint32_t fid = p9i_u32(&in);
        uint64_t offset = p9i_u64(&in);
        uint32_t count = p9i_u32(&in);
        if (!in.ok || in.p + count > in.len)
            return reply_error(rsp, rspcap, tag, "bad Twrite");
        const uint8_t *data = in.b + in.p;      /* the payload follows count */
        int fi = find_fid(s, fid);
        if (fi < 0)
            return reply_error(rsp, rspcap, tag, "bad fid");
        if (!s->be->writef)
            return reply_error(rsp, rspcap, tag, "read-only");
        long wrote = s->be->writef(s->ctx, &s->fids[fi].node, offset, data, count);
        if (wrote < 0)
            return reply_error(rsp, rspcap, tag, "write failed");
        if (offset + (uint64_t)wrote > s->fids[fi].node.length)
            s->fids[fi].node.length = offset + (uint64_t)wrote;
        reply_hdr(&o, rsp, rspcap, P9_RWRITE, tag);
        p9o_u32(&o, (uint32_t)wrote);
        return p9o_finish(&o);
    }
    case P9_TREMOVE: {
        /* Remove the file the fid names, then clunk the fid (9P2000: Tremove
         * always clunks, success or not). */
        uint32_t fid = p9i_u32(&in);
        int fi = find_fid(s, fid);
        if (fi < 0)
            return reply_error(rsp, rspcap, tag, "bad fid");
        int rc = s->be->removef ? s->be->removef(s->ctx, &s->fids[fi].node) : -1;
        s->fids[fi].used = 0;
        if (s->dir_valid && s->dir_fid == fid)
            s->dir_valid = 0;
        if (rc != 0)
            return reply_error(rsp, rspcap, tag, "remove failed");
        reply_hdr(&o, rsp, rspcap, P9_RREMOVE, tag);
        return p9o_finish(&o);
    }
    default:
        return reply_error(rsp, rspcap, tag, "unsupported message");
    }
}
