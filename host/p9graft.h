/* p9graft.h : a read-only 9P backend that grafts other backends' trees at
 * top-level names inside a base backend's tree.
 *
 * The base is the guest's own filesystem; each graft is a shared tree (another
 * depot or a pinned snapshot) that appears as one directory, /<name>, in the
 * guest's root. Several grafts can share one wrapper, so a guest sees its own
 * tree and a whole runtime of shared trees at once, which serving a single root
 * (M10.4) cannot do. Each operation routes to the backend that owns the node:
 * the high bits of every qid path carry a small tree id (0 for the base, 1..N
 * for the grafts), kept disjoint so nodes from different trees never collide. */
#ifndef P9GRAFT_H
#define P9GRAFT_H

#include "p9srv.h"

struct p9graft;

/* Wrap a base backend. Returns an opaque context, or NULL on failure. Add
 * grafts with p9graft_add, then pass the context to p9srv_init alongside
 * p9graft_backend(). */
struct p9graft *p9graft_new(const struct p9_backend *base, void *base_ctx);

/* Graft `graft` at the top-level name `name` in the base root. Returns 0, or -1
 * if the graft table is full. The name must not already exist in the base root;
 * if it does, the graft shadows it. Order matters only for listing; walks match
 * by name. */
int p9graft_add(struct p9graft *g, const struct p9_backend *graft,
                void *graft_ctx, const char *name);

/* The backend vtable to register with p9srv_init. */
const struct p9_backend *p9graft_backend(void);

#endif /* P9GRAFT_H */
