/* p9.c : the guest 9P client.
 *
 * Each operation builds a T-message in the shared channel's request buffer,
 * rings the doorbell (a synchronous MMIO exit the host services inline), and
 * parses the reply from the response buffer. One request is outstanding at a
 * time, so a fixed tag is fine. */
#include "p9.h"

#include <stddef.h>

static struct jotun_p9 *
chan(void)
{
    return (struct jotun_p9 *)(uintptr_t)JOTUN_P9_BASE;
}

static void
doorbell(void)
{
    *(volatile uint32_t *)(uintptr_t)JOTUN_P9_DOORBELL = 1;
}

/* Send the built request and position in to parse the reply body, just past
 * the size/type/tag header. Returns the reply type, or -1 on error. */
static int
txn(struct p9_out *o, struct p9_in *in)
{
    struct jotun_p9 *c = chan();
    uint32_t sz = p9o_finish(o);

    if (!o->ok || sz == 0)
        return -1;
    c->req_len = sz;
    doorbell();

    p9i_init(in, c->rsp, c->rsp_len);
    (void)p9i_u32(in);              /* size */
    int type = p9i_u8(in);          /* type */
    (void)p9i_u16(in);              /* tag  */
    if (!in->ok)
        return -1;
    return type;
}

static void
begin(struct p9_out *o, uint8_t type)
{
    p9o_init(o, chan()->req, JOTUN_P9_MSIZE);
    p9o_hdr(o, type, 0);
}

int
p9_mount(void)
{
    struct p9_out o;
    struct p9_in in;

    begin(&o, P9_TVERSION);
    p9o_u32(&o, JOTUN_P9_MSIZE);
    p9o_str(&o, "9P2000");
    if (txn(&o, &in) != P9_RVERSION)
        return -1;

    begin(&o, P9_TATTACH);
    p9o_u32(&o, P9_ROOT_FID);
    p9o_u32(&o, P9_NOFID);          /* afid */
    p9o_str(&o, "jotun");           /* uname */
    p9o_str(&o, "");                /* aname */
    if (txn(&o, &in) != P9_RATTACH)
        return -1;
    return 0;
}

int
p9_walk(uint32_t fid, uint32_t newfid, int nwname, const char *const *names)
{
    struct p9_out o;
    struct p9_in in;

    if (nwname < 0 || nwname > P9_MAXWELEM)
        return -1;

    begin(&o, P9_TWALK);
    p9o_u32(&o, fid);
    p9o_u32(&o, newfid);
    p9o_u16(&o, (uint16_t)nwname);
    for (int i = 0; i < nwname; i++)
        p9o_str(&o, names[i]);
    if (txn(&o, &in) != P9_RWALK)
        return -1;

    uint16_t nwqid = p9i_u16(&in);
    /* Skip the returned qids; the client does not need them here. */
    for (int i = 0; i < nwqid; i++) {
        struct p9_qid q;
        p9i_qid(&in, &q);
    }
    if (!in.ok)
        return -1;
    return (int)nwqid;
}

int
p9_open(uint32_t fid)
{
    struct p9_out o;
    struct p9_in in;

    begin(&o, P9_TOPEN);
    p9o_u32(&o, fid);
    p9o_u8(&o, P9_OREAD);
    if (txn(&o, &in) != P9_ROPEN)
        return -1;
    return 0;
}

long
p9_read(uint32_t fid, uint64_t offset, void *buf, uint32_t count)
{
    struct p9_out o;
    struct p9_in in;
    uint8_t *out = buf;

    /* Bound the request to what a reply can carry. */
    uint32_t room = JOTUN_P9_MSIZE - 32;
    if (count > room)
        count = room;

    begin(&o, P9_TREAD);
    p9o_u32(&o, fid);
    p9o_u64(&o, offset);
    p9o_u32(&o, count);
    if (txn(&o, &in) != P9_RREAD)
        return -1;

    uint32_t n = p9i_u32(&in);
    if (!in.ok)
        return -1;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = p9i_u8(&in);
        if (out)
            out[i] = c;
    }
    if (!in.ok)
        return -1;
    return (long)n;
}

int
p9_clunk(uint32_t fid)
{
    struct p9_out o;
    struct p9_in in;

    begin(&o, P9_TCLUNK);
    p9o_u32(&o, fid);
    if (txn(&o, &in) != P9_RCLUNK)
        return -1;
    return 0;
}

/* Parse one stat structure, cursor positioned at its leading size[2] field. */
static int
parse_stat(struct p9_in *in, struct p9_stat *out)
{
    (void)p9i_u16(in);              /* size of the rest */
    (void)p9i_u16(in);              /* type */
    (void)p9i_u32(in);              /* dev */
    p9i_qid(in, &out->qid);
    out->mode = p9i_u32(in);
    (void)p9i_u32(in);              /* atime */
    (void)p9i_u32(in);              /* mtime */
    out->length = p9i_u64(in);
    p9i_str(in, out->name, sizeof out->name);
    p9i_str(in, NULL, 0);           /* uid */
    p9i_str(in, NULL, 0);           /* gid */
    p9i_str(in, NULL, 0);           /* muid */
    return in->ok;
}

int
p9_stat(uint32_t fid, struct p9_stat *out)
{
    struct p9_out o;
    struct p9_in in;

    begin(&o, P9_TSTAT);
    p9o_u32(&o, fid);
    if (txn(&o, &in) != P9_RSTAT)
        return -1;
    (void)p9i_u16(&in);             /* outer stat count */
    if (!parse_stat(&in, out))
        return -1;
    return 0;
}

int
p9_dir_next(const uint8_t *buf, uint32_t len, uint32_t *off, struct p9_stat *out)
{
    struct p9_in in;

    if (*off >= len)
        return 0;
    p9i_init(&in, buf, len);
    in.p = *off;
    if (!parse_stat(&in, out))
        return 0;
    *off = in.p;
    return 1;
}
