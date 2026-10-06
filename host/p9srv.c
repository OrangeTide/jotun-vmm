/* p9srv.c : the read-only 9P2000 protocol server.
 *
 * One request is handled at a time. The server decodes a T-message, consults
 * the backend, and encodes the R-message. Directory reads serialize the
 * backend's listing into 9P stat structures and serve byte ranges of that. */
#include "p9srv.h"

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
        (void)p9i_u8(&in);          /* mode; read-only server ignores it */
        int fi = find_fid(s, fid);
        if (fi < 0)
            return reply_error(rsp, rspcap, tag, "bad fid");
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
            /* Serialize the whole listing, then serve the requested slice. */
            static uint8_t dirbuf[JOTUN_P9_MSIZE];
            struct p9_out d;
            struct p9_dirent ents[64];
            int ne = s->be->list(s->ctx, n, ents, 64);
            if (ne < 0)
                return reply_error(rsp, rspcap, tag, "list failed");
            p9o_init(&d, dirbuf, sizeof dirbuf);
            for (int k = 0; k < ne; k++)
                put_stat(&d, ents[k].name, ents[k].is_dir, ents[k].mode,
                         ents[k].length, ents[k].qid_path);
            uint32_t total = d.n;
            uint32_t start = (offset < total) ? (uint32_t)offset : total;
            uint32_t avail = total - start;
            if (count > avail)
                count = avail;
            p9o_u32(&o, count);
            p9o_bytes(&o, dirbuf + start, count);
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
    default:
        return reply_error(rsp, rspcap, tag, "unsupported message");
    }
}
