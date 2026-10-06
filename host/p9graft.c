/* p9graft.c : graft a second backend's tree at a name in a base backend. */

#include "p9graft.h"

#include <stdlib.h>
#include <string.h>

/* The top bit of a qid path tags which tree a node belongs to: set for the
 * graft, clear for the base. The wrapper rewrites every node's qid path as it
 * passes, so routing is reliable (the top bit) and the two trees' qids are
 * disjoint for the client. The underlying backends key off the opaque handle,
 * not the qid path, so rewriting the qid does not disturb delegation. */
#define GRAFT_BIT (1ull << 63)

struct p9graft {
    const struct p9_backend *base;
    void *base_ctx;
    const struct p9_backend *graft;
    void *graft_ctx;
    char name[256];                 /* mount leaf name in the base root */

    struct p9_node base_root;       /* cached base root, qid cleared */
    int base_root_ok;
    struct p9_node graft_root;      /* cached graft root, qid tagged */
    int graft_root_ok;
};

static int
is_graft(const struct p9_node *n)
{
    return (n->qid_path & GRAFT_BIT) != 0;
}

static int
ensure_base_root(struct p9graft *g)
{
    if (!g->base_root_ok) {
        if (g->base->root(g->base_ctx, &g->base_root) != 0)
            return -1;
        g->base_root.qid_path &= ~GRAFT_BIT;
        g->base_root_ok = 1;
    }
    return 0;
}

static int
ensure_graft_root(struct p9graft *g)
{
    if (!g->graft_root_ok) {
        if (g->graft->root(g->graft_ctx, &g->graft_root) != 0)
            return -1;
        g->graft_root.qid_path |= GRAFT_BIT;
        g->graft_root_ok = 1;
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

    if (is_graft(dir)) {
        if (g->graft->walk(g->graft_ctx, dir, name, out) != 0)
            return -1;
        out->qid_path |= GRAFT_BIT;
        return 0;
    }

    if (ensure_base_root(g) != 0)
        return -1;

    /* The mount name, walked from the base root, enters the graft tree. */
    if (strcmp(dir->handle, g->base_root.handle) == 0
        && strcmp(name, g->name) == 0) {
        if (ensure_graft_root(g) != 0)
            return -1;
        *out = g->graft_root;
        strncpy(out->name, g->name, sizeof out->name - 1);
        out->name[sizeof out->name - 1] = '\0';
        return 0;
    }

    if (g->base->walk(g->base_ctx, dir, name, out) != 0)
        return -1;
    out->qid_path &= ~GRAFT_BIT;
    return 0;
}

static int
gr_list(void *ctx, const struct p9_node *dir, struct p9_dirent *ents, int max)
{
    struct p9graft *g = ctx;

    if (is_graft(dir)) {
        int n = g->graft->list(g->graft_ctx, dir, ents, max);
        for (int i = 0; i < n; i++)
            ents[i].qid_path |= GRAFT_BIT;
        return n;
    }

    if (ensure_base_root(g) != 0)
        return -1;
    int at_root = strcmp(dir->handle, g->base_root.handle) == 0;

    /* Keep one slot for the injected mount entry when listing the base root. */
    int n = g->base->list(g->base_ctx, dir, ents, at_root ? max - 1 : max);
    if (n < 0)
        n = 0;
    for (int i = 0; i < n; i++)
        ents[i].qid_path &= ~GRAFT_BIT;

    if (at_root && n < max) {
        if (ensure_graft_root(g) != 0)
            return -1;
        memset(&ents[n], 0, sizeof ents[n]);
        strncpy(ents[n].name, g->name, sizeof ents[n].name - 1);
        ents[n].is_dir = 1;
        ents[n].mode = g->graft_root.mode ? g->graft_root.mode : 0555;
        ents[n].qid_path = g->graft_root.qid_path;      /* already tagged */
        n++;
    }
    return n;
}

static long
gr_readf(void *ctx, const struct p9_node *file, uint64_t off, void *buf,
         uint32_t n)
{
    struct p9graft *g = ctx;

    if (is_graft(file))
        return g->graft->readf(g->graft_ctx, file, off, buf, n);
    return g->base->readf(g->base_ctx, file, off, buf, n);
}

static const struct p9_backend graft_backend = {
    .root = gr_root,
    .walk = gr_walk,
    .list = gr_list,
    .readf = gr_readf,
};

struct p9graft *
p9graft_new(const struct p9_backend *base, void *base_ctx,
            const struct p9_backend *graft, void *graft_ctx, const char *name)
{
    struct p9graft *g = calloc(1, sizeof *g);
    if (!g)
        return NULL;
    g->base = base;
    g->base_ctx = base_ctx;
    g->graft = graft;
    g->graft_ctx = graft_ctx;
    strncpy(g->name, name, sizeof g->name - 1);
    return g;
}

const struct p9_backend *
p9graft_backend(void)
{
    return &graft_backend;
}
