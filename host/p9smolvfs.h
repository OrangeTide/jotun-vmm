/* p9smolvfs.h : a 9P backend that serves a smolvfs bundle. */
#ifndef P9SMOLVFS_H
#define P9SMOLVFS_H

#include "p9srv.h"

/* Open a smolvfs depot, resolve ref to its root tree, and confine the served
 * root to the given subtree (empty or "/" serves the whole tree). On success
 * returns the backend and stores a context pointer in *ctx_out. Returns NULL
 * if the depot cannot be opened, the ref does not resolve, or the subtree does
 * not exist or is not a directory. */
const struct p9_backend *p9smolvfs_open(const char *depot, const char *ref,
                                        const char *subtree, void **ctx_out);

#endif /* P9SMOLVFS_H */
