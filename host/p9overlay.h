/* p9overlay.h : a read-only 9P backend that overlays a synthetic /net tree on
 * another backend.
 *
 * It serves /net (and its children /net/policy and /net/conn) from the net
 * provider and the policy, and delegates every other path to an underlying
 * backend (the bundle). This is how the guest sees its network rights and live
 * connections as files without the net data path going through 9P. */
#ifndef P9OVERLAY_H
#define P9OVERLAY_H

#include "net.h"
#include "p9srv.h"

struct p9overlay;

/* Wrap an underlying backend. under/under_ctx is the delegated filesystem (the
 * bundle); policy and net supply the /net contents. Returns an opaque context
 * to pass to p9srv_init alongside p9overlay_backend(), or NULL on failure. */
struct p9overlay *p9overlay_new(const struct p9_backend *under, void *under_ctx,
                                const struct net_policy *policy,
                                struct netdev *net);

/* The backend vtable to register with p9srv_init. */
const struct p9_backend *p9overlay_backend(void);

#endif /* P9OVERLAY_H */
