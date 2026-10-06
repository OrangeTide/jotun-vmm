/* p9smolvfs.h : a 9P backend that serves a smolvfs bundle. */
#ifndef P9SMOLVFS_H
#define P9SMOLVFS_H

#include <stddef.h>

#include "p9srv.h"

/* Open a smolvfs depot, resolve ref to its root tree, and confine the served
 * root to the given subtree (empty or "/" serves the whole tree). ref may name
 * a ref, a pinned root tree hash, or a jimage manifest object; a jimage selects
 * the root tree matching `arch` (uname-style, e.g. "x86_64"). On success
 * returns the backend and stores a context pointer in *ctx_out. Returns NULL if
 * the depot cannot be opened, the ref does not resolve, the image has no such
 * arch, or the subtree does not exist or is not a directory. */
const struct p9_backend *p9smolvfs_open(const char *depot, const char *ref,
                                        const char *subtree, const char *arch,
                                        void **ctx_out);

/* Read the whole blob at a slash-separated path under the served root into a
 * freshly malloc'd buffer. On success *out holds the bytes and *len the length;
 * the caller frees *out. Returns 0, or -1 if the path is missing, names a
 * directory, or on error. Used to lift the kernel image out of the bundle. */
int p9smolvfs_readfile(void *ctx, const char *path, void **out, size_t *len);

/* Release a context returned by p9smolvfs_open. */
void p9smolvfs_close(void *ctx);

#endif /* P9SMOLVFS_H */
