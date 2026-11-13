/* p9overlay.c : the /net overlay backend. */

#include "p9overlay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Synthetic node kinds, stored as the first byte of a node handle. Real
 * backends use printable handles (hashes or names), so these low bytes never
 * collide. A connection file carries its decimal handle after the tag byte. */
#define OV_ROOT     '\x01'
#define OV_NET      '\x02'
#define OV_POLICY   '\x03'
#define OV_CONN     '\x04'
#define OV_CONNF    '\x05'

/* Synthetic qid paths sit in a high reserved range, clear of backend qids. */
#define OV_QID_BASE 0xffffffffffff0000ull

#define OV_MAXCONN  16

struct p9overlay {
    const struct p9_backend *under;
    void *under_ctx;
    struct p9_node under_root;          /* cached, filled by ov_root */
    int under_root_ok;
    const struct net_policy *policy;
    struct netdev *net;
};

static int
is_synth(const struct p9_node *n)
{
    unsigned char c = (unsigned char)n->handle[0];
    return c >= 1 && c <= 5;
}

/* Fill a synthetic directory or file node. tag selects the kind; name is the
 * leaf name; qid is the reserved path; extra, for a connection file, is the
 * decimal handle appended after the tag byte. */
static void
synth_node(struct p9_node *out, char tag, int is_dir, const char *name,
           uint64_t qid, int extra)
{
    memset(out, 0, sizeof *out);
    out->is_dir = is_dir;
    out->mode = is_dir ? 0555 : 0444;
    out->length = 0;            /* the guest reads synthetic files to EOF */
    out->qid_path = qid;
    out->handle[0] = tag;
    if (tag == OV_CONNF)
        snprintf(out->handle + 1, sizeof out->handle - 1, "%d", extra);
    else
        out->handle[1] = '\0';
    strncpy(out->name, name, sizeof out->name - 1);
}

/* Render the policy as text into out. Returns the length. */
static int
render_policy(const struct p9overlay *ov, char *out, size_t cap)
{
    const struct net_policy *p = ov->policy;
    if (!p || p->n == 0)
        return snprintf(out, cap, "allow all\n");

    int w = 0;
    for (int i = 0; i < p->n && w < (int)cap; i++) {
        const struct net_rule *r = &p->rules[i];
        int prefix = __builtin_popcount(r->mask);
        const char *proto = r->proto == NET_PROTO_TCP ? "tcp"
                          : r->proto == NET_PROTO_UDP ? "udp" : "any";
        char ports[16];
        if (r->port_lo == 0 && r->port_hi == 65535)
            snprintf(ports, sizeof ports, "*");
        else if (r->port_lo == r->port_hi)
            snprintf(ports, sizeof ports, "%u", r->port_lo);
        else
            snprintf(ports, sizeof ports, "%u-%u", r->port_lo, r->port_hi);

        w += snprintf(out + w, cap - (size_t)w,
                      "allow %u.%u.%u.%u/%d %s %s\n",
                      (r->addr >> 24) & 0xff, (r->addr >> 16) & 0xff,
                      (r->addr >> 8) & 0xff, r->addr & 0xff,
                      prefix, ports, proto);
    }
    return w;
}

/* Render one connection file (named by handle) into out. */
static int
render_connf(struct p9overlay *ov, int handle, char *out, size_t cap)
{
    struct net_conninfo conns[OV_MAXCONN];
    int n = net_conn_snapshot(ov->net, conns, OV_MAXCONN);
    for (int i = 0; i < n; i++) {
        if (conns[i].handle != handle)
            continue;
        const char *proto = conns[i].proto == NET_PROTO_UDP ? "udp" : "tcp";
        return snprintf(out, cap, "%s %s\n", proto, conns[i].peer);
    }
    return 0;
}

static int
ov_root(void *ctx, struct p9_node *out)
{
    struct p9overlay *ov = ctx;
    if (!ov->under_root_ok) {
        if (ov->under->root(ov->under_ctx, &ov->under_root) != 0)
            return -1;
        ov->under_root_ok = 1;
    }
    synth_node(out, OV_ROOT, 1, "/", OV_QID_BASE + 0, 0);
    return 0;
}

static int
ov_walk(void *ctx, const struct p9_node *dir, const char *name,
        struct p9_node *out)
{
    struct p9overlay *ov = ctx;

    if (!is_synth(dir))
        return ov->under->walk(ov->under_ctx, dir, name, out);

    switch ((unsigned char)dir->handle[0]) {
    case OV_ROOT:
        if (strcmp(name, "net") == 0) {
            synth_node(out, OV_NET, 1, "net", OV_QID_BASE + 1, 0);
            return 0;
        }
        return ov->under->walk(ov->under_ctx, &ov->under_root, name, out);
    case OV_NET:
        if (strcmp(name, "policy") == 0) {
            synth_node(out, OV_POLICY, 0, "policy", OV_QID_BASE + 2, 0);
            return 0;
        }
        if (strcmp(name, "conn") == 0) {
            synth_node(out, OV_CONN, 1, "conn", OV_QID_BASE + 3, 0);
            return 0;
        }
        return -1;
    case OV_CONN: {
        /* A connection file named by its decimal handle, if it exists. */
        int h = atoi(name);
        struct net_conninfo conns[OV_MAXCONN];
        int n = net_conn_snapshot(ov->net, conns, OV_MAXCONN);
        for (int i = 0; i < n; i++) {
            if (conns[i].handle == h) {
                synth_node(out, OV_CONNF, 0, name, OV_QID_BASE + 0x100 + h, h);
                return 0;
            }
        }
        return -1;
    }
    default:
        return -1;
    }
}

static int
ov_emit_one(p9_emit_fn emit, void *ectx, const char *name, int is_dir,
            uint32_t mode, uint64_t qid_path)
{
    struct p9_dirent e;
    memset(&e, 0, sizeof e);
    strncpy(e.name, name, sizeof e.name - 1);
    e.is_dir = is_dir;
    e.mode = mode;
    e.qid_path = qid_path;
    return emit(ectx, &e);
}

static int
ov_list(void *ctx, const struct p9_node *dir, p9_emit_fn emit, void *ectx)
{
    struct p9overlay *ov = ctx;

    if (!is_synth(dir))
        return ov->under->list(ov->under_ctx, dir, emit, ectx);

    switch ((unsigned char)dir->handle[0]) {
    case OV_ROOT:
        /* The bundle's root entries, plus a synthetic "net". */
        ov->under->list(ov->under_ctx, &ov->under_root, emit, ectx);
        ov_emit_one(emit, ectx, "net", 1, 0555, OV_QID_BASE + 1);
        return 0;
    case OV_NET:
        ov_emit_one(emit, ectx, "policy", 0, 0444, OV_QID_BASE + 2);
        ov_emit_one(emit, ectx, "conn", 1, 0555, OV_QID_BASE + 3);
        return 0;
    case OV_CONN: {
        struct net_conninfo conns[OV_MAXCONN];
        int cn = net_conn_snapshot(ov->net, conns, OV_MAXCONN);
        for (int i = 0; i < cn; i++) {
            char nm[32];
            snprintf(nm, sizeof nm, "%d", conns[i].handle);
            if (ov_emit_one(emit, ectx, nm, 0, 0444,
                            OV_QID_BASE + 0x100 + conns[i].handle) != 0)
                break;
        }
        return 0;
    }
    default:
        return -1;
    }
}

static long
ov_readf(void *ctx, const struct p9_node *file, uint64_t off, void *buf,
         uint32_t n)
{
    struct p9overlay *ov = ctx;

    if (!is_synth(file))
        return ov->under->readf(ov->under_ctx, file, off, buf, n);

    char text[1024];
    int len = 0;
    switch ((unsigned char)file->handle[0]) {
    case OV_POLICY:
        len = render_policy(ov, text, sizeof text);
        break;
    case OV_CONNF:
        len = render_connf(ov, atoi(file->handle + 1), text, sizeof text);
        break;
    default:
        return -1;              /* a directory, or unknown */
    }
    if (len < 0)
        len = 0;
    if (off >= (uint64_t)len)
        return 0;
    uint64_t avail = (uint64_t)len - off;
    if (n > avail)
        n = (uint32_t)avail;
    memcpy(buf, text + off, n);
    return (long)n;
}

/* The write ops forward to the backend below for a real file, and refuse a
 * synthetic /net node (those are read-only). A read-only backend below leaves
 * its op NULL, which these guard. */
static long
ov_writef(void *ctx, const struct p9_node *file, uint64_t off, const void *buf,
          uint32_t n)
{
    struct p9overlay *ov = ctx;
    if (is_synth(file) || !ov->under->writef)
        return -1;
    return ov->under->writef(ov->under_ctx, file, off, buf, n);
}

static int
ov_create(void *ctx, const struct p9_node *dir, const char *name, uint32_t perm,
          struct p9_node *out)
{
    struct p9overlay *ov = ctx;
    if (is_synth(dir) || !ov->under->create)
        return -1;
    return ov->under->create(ov->under_ctx, dir, name, perm, out);
}

static int
ov_truncate(void *ctx, const struct p9_node *file, uint64_t len)
{
    struct p9overlay *ov = ctx;
    if (is_synth(file) || !ov->under->truncate)
        return -1;
    return ov->under->truncate(ov->under_ctx, file, len);
}

static int
ov_removef(void *ctx, const struct p9_node *node)
{
    struct p9overlay *ov = ctx;
    if (is_synth(node) || !ov->under->removef)
        return -1;
    return ov->under->removef(ov->under_ctx, node);
}

static int
ov_renamef(void *ctx, const struct p9_node *olddir, const char *oldname,
           const struct p9_node *newdir, const char *newname)
{
    struct p9overlay *ov = ctx;
    if (is_synth(olddir) || is_synth(newdir) || !ov->under->renamef)
        return -1;
    return ov->under->renamef(ov->under_ctx, olddir, oldname, newdir, newname);
}

static int
ov_openh(void *ctx, const struct p9_node *file, int for_write)
{
    struct p9overlay *ov = ctx;
    if (is_synth(file) || !ov->under->openh)
        return -1;              /* synthetic /net files are served via readf */
    return ov->under->openh(ov->under_ctx, file, for_write);
}

static const struct p9_backend overlay_backend = {
    .root = ov_root,
    .walk = ov_walk,
    .list = ov_list,
    .readf = ov_readf,
    .writef = ov_writef,
    .create = ov_create,
    .truncate = ov_truncate,
    .removef = ov_removef,
    .renamef = ov_renamef,
    .openh = ov_openh,
};

struct p9overlay *
p9overlay_new(const struct p9_backend *under, void *under_ctx,
              const struct net_policy *policy, struct netdev *net)
{
    struct p9overlay *ov = calloc(1, sizeof(*ov));
    if (!ov)
        return NULL;
    ov->under = under;
    ov->under_ctx = under_ctx;
    ov->policy = policy;
    ov->net = net;
    return ov;
}

const struct p9_backend *
p9overlay_backend(void)
{
    return &overlay_backend;
}
