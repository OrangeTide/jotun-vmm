/* p9smolvfs.h : a 9P backend that serves a smolvfs bundle. */
#ifndef P9SMOLVFS_H
#define P9SMOLVFS_H

#include "p9srv.h"

/* Open a smolvfs depot and resolve ref to its root tree. On success returns
 * the backend and stores a context pointer in *ctx_out. Returns NULL if the
 * depot cannot be opened or the ref does not resolve. */
const struct p9_backend *p9smolvfs_open(const char *depot, const char *ref,
                                        void **ctx_out);

#endif /* P9SMOLVFS_H */
