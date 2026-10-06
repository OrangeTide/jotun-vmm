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
ov_list(void *ctx, const struct p9_node *dir, struct p9_dirent *ents, int max)
{
    struct p9overlay *ov = ctx;

    if (!is_synth(dir))
        return ov->under->list(ov->under_ctx, dir, ents, max);

    switch ((unsigned char)dir->handle[0]) {
    case OV_ROOT: {
        /* The bundle's root entries, plus a synthetic "net". */
        int n = ov->under->list(ov->under_ctx, &ov->under_root, ents, max - 1);
        if (n < 0)
            n = 0;
        memset(&ents[n], 0, sizeof ents[n]);
        strncpy(ents[n].name, "net", sizeof ents[n].name - 1);
        ents[n].is_dir = 1;
        ents[n].mode = 0555;
        ents[n].qid_path = OV_QID_BASE + 1;
        return n + 1;
    }
    case OV_NET: {
        int n = 0;
        if (max > 0) {
            memset(&ents[n], 0, sizeof ents[n]);
            strncpy(ents[n].name, "policy", sizeof ents[n].name - 1);
            ents[n].mode = 0444;
            ents[n].qid_path = OV_QID_BASE + 2;
            n++;
        }
        if (max > 1) {
            memset(&ents[n], 0, sizeof ents[n]);
            strncpy(ents[n].name, "conn", sizeof ents[n].name - 1);
            ents[n].is_dir = 1;
            ents[n].mode = 0555;
            ents[n].qid_path = OV_QID_BASE + 3;
            n++;
        }
        return n;
    }
    case OV_CONN: {
        struct net_conninfo conns[OV_MAXCONN];
        int cn = net_conn_snapshot(ov->net, conns, OV_MAXCONN);
        int n = 0;
        for (int i = 0; i < cn && n < max; i++) {
            memset(&ents[n], 0, sizeof ents[n]);
            snprintf(ents[n].name, sizeof ents[n].name, "%d", conns[i].handle);
            ents[n].mode = 0444;
            ents[n].qid_path = OV_QID_BASE + 0x100 + conns[i].handle;
            n++;
        }
        return n;
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

static const struct p9_backend overlay_backend = {
    .root = ov_root,
    .walk = ov_walk,
    .list = ov_list,
    .readf = ov_readf,
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
