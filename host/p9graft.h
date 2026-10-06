/* p9graft.h : a read-only 9P backend that grafts a second backend's tree at a
 * single top-level name inside a base backend's tree.
 *
 * The base is the guest's own filesystem; the graft is a shared tree (another
 * depot or a pinned snapshot) that appears as one directory, /<name>, in the
 * guest's root. Each operation routes to the backend that owns the node, and
 * qid paths are kept disjoint by a reserved high bit so a base file and a graft
 * file can never share a qid. This is how a guest sees both its own tree and a
 * shared tree at once, which serving a whole root (M10.4) cannot do. */
#ifndef P9GRAFT_H
#define P9GRAFT_H

#include "p9srv.h"

struct p9graft;

/* Wrap a base backend and graft `graft` at the top-level name `name` in the
 * base root. Returns an opaque context to pass to p9srv_init alongside
 * p9graft_backend(), or NULL on failure. The name must not already exist in the
 * base root; if it does, the graft shadows it. */
struct p9graft *p9graft_new(const struct p9_backend *base, void *base_ctx,
                            const struct p9_backend *graft, void *graft_ctx,
                            const char *name);

/* The backend vtable to register with p9srv_init. */
const struct p9_backend *p9graft_backend(void);

#endif /* P9GRAFT_H */
