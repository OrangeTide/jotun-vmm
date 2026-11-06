/* p9graft.c : graft other backends' trees at names in a base backend. */

#include "p9graft.h"

#include <stdlib.h>
#include <string.h>

/* The high bits of a qid path carry a tree id: 0 for the base, 1..N for the
 * grafts. The wrapper rewrites every node's qid as it passes, so routing reads
 * the id back from the node, and the trees' qids stay disjoint for the client.
 * The underlying backends key off the opaque handle, not the qid path, so
 * rewriting the qid does not disturb delegation. Four bits allow 15 grafts. */
#define GRAFT_SHIFT 60
#define GRAFT_IDMASK 0xfull
#define GRAFT_QMASK  ((1ull << GRAFT_SHIFT) - 1)

#define GRAFT_MAX 15

struct graft_entry {
    const struct p9_backend *be;
    void *ctx;
    char name[256];
    struct p9_node root;            /* cached graft root, qid tagged */
    int root_ok;
};

struct p9graft {
    const struct p9_backend *base;
    void *base_ctx;
    struct p9_node base_root;       /* cached base root, qid tagged id 0 */
    int base_root_ok;
    struct graft_entry g[GRAFT_MAX];
    int ng;
};

static unsigned
tree_id(const struct p9_node *n)
{
    return (unsigned)((n->qid_path >> GRAFT_SHIFT) & GRAFT_IDMASK);
}

/* Tag a node's qid with a tree id, replacing the high id bits. */
static void
set_tree_id(struct p9_node *n, unsigned id)
{
    n->qid_path = (n->qid_path & GRAFT_QMASK) | ((uint64_t)id << GRAFT_SHIFT);
}

static int
ensure_base_root(struct p9graft *g)
{
    if (!g->base_root_ok) {
        if (g->base->root(g->base_ctx, &g->base_root) != 0)
            return -1;
        set_tree_id(&g->base_root, 0);
        g->base_root_ok = 1;
    }
    return 0;
}

static int
ensure_graft_root(struct p9graft *g, int i)
{
    struct graft_entry *e = &g->g[i];
    if (!e->root_ok) {
        if (e->be->root(e->ctx, &e->root) != 0)
            return -1;
        set_tree_id(&e->root, (unsigned)(i + 1));
        e->root_ok = 1;
    }
    return 0;
}

static int
gr_root(void *ctx, struct p9_node *out)
{
    struct p9graft *g = ctx;
    if (ensure_base_root(g) != 0)
        return -1;
    *out = g->base_root;
    return 0;
}

static int
gr_walk(void *ctx, const struct p9_node *dir, const char *name,
        struct p9_node *out)
{
    struct p9graft *g = ctx;
    unsigned id = tree_id(dir);

    if (id != 0) {                  /* inside a graft: delegate to its backend */
        struct graft_entry *e = &g->g[id - 1];
        if (e->be->walk(e->ctx, dir, name, out) != 0)
            return -1;
        set_tree_id(out, id);
        return 0;
    }

    if (ensure_base_root(g) != 0)
        return -1;

    /* A graft name, walked from the base root, enters that graft's tree. */
    if (strcmp(dir->handle, g->base_root.handle) == 0) {
        for (int i = 0; i < g->ng; i++) {
            if (strcmp(name, g->g[i].name) != 0)
                continue;
            if (ensure_graft_root(g, i) != 0)
                return -1;
            *out = g->g[i].root;
            strncpy(out->name, g->g[i].name, sizeof out->name - 1);
            out->name[sizeof out->name - 1] = '\0';
            return 0;
        }
    }

    if (g->base->walk(g->base_ctx, dir, name, out) != 0)
        return -1;
    set_tree_id(out, 0);
    return 0;
}

static int
gr_list(void *ctx, const struct p9_node *dir, struct p9_dirent *ents, int max)
{
    struct p9graft *g = ctx;
    unsigned id = tree_id(dir);

    if (id != 0) {
        struct graft_entry *e = &g->g[id - 1];
        int n = e->be->list(e->ctx, dir, ents, max);
        for (int i = 0; i < n; i++)
            ents[i].qid_path = (ents[i].qid_path & GRAFT_QMASK)
                             | ((uint64_t)id << GRAFT_SHIFT);
        return n;
    }

    if (ensure_base_root(g) != 0)
        return -1;
    int at_root = strcmp(dir->handle, g->base_root.handle) == 0;

    /* Keep one slot per graft for the injected entries when listing the root. */
    int reserve = at_root ? g->ng : 0;
    int n = g->base->list(g->base_ctx, dir, ents, max - reserve);
    if (n < 0)
        n = 0;
    for (int i = 0; i < n; i++)
        ents[i].qid_path &= GRAFT_QMASK;        /* tree id 0 */

    if (at_root) {
        for (int i = 0; i < g->ng && n < max; i++) {
            if (ensure_graft_root(g, i) != 0)
                return -1;
            memset(&ents[n], 0, sizeof ents[n]);
            strncpy(ents[n].name, g->g[i].name, sizeof ents[n].name - 1);
            ents[n].is_dir = 1;
            ents[n].mode = g->g[i].root.mode ? g->g[i].root.mode : 0555;
            ents[n].qid_path = g->g[i].root.qid_path;   /* already tagged */
            n++;
        }
    }
    return n;
}

static long
gr_readf(void *ctx, const struct p9_node *file, uint64_t off, void *buf,
         uint32_t n)
{
    struct p9graft *g = ctx;
    unsigned id = tree_id(file);

    if (id != 0)
        return g->g[id - 1].be->readf(g->g[id - 1].ctx, file, off, buf, n);
    return g->base->readf(g->base_ctx, file, off, buf, n);
}

static const struct p9_backend graft_backend = {
    .root = gr_root,
    .walk = gr_walk,
    .list = gr_list,
    .readf = gr_readf,
};

struct p9graft *
p9graft_new(const struct p9_backend *base, void *base_ctx)
{
    struct p9graft *g = calloc(1, sizeof *g);
    if (!g)
        return NULL;
    g->base = base;
    g->base_ctx = base_ctx;
    return g;
}

int
p9graft_add(struct p9graft *g, const struct p9_backend *graft, void *graft_ctx,
            const char *name)
{
    if (g->ng >= GRAFT_MAX)
        return -1;
    struct graft_entry *e = &g->g[g->ng];
    e->be = graft;
    e->ctx = graft_ctx;
    strncpy(e->name, name, sizeof e->name - 1);
    e->name[sizeof e->name - 1] = '\0';
    e->root_ok = 0;
    g->ng++;
    return 0;
}

const struct p9_backend *
p9graft_backend(void)
{
    return &graft_backend;
}
