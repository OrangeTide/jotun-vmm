/* p9hostdir.h : a 9P backend that serves a live host directory.
 *
 * Where p9smolvfs serves a content-addressed bundle, this serves a real
 * directory on the host filesystem, so a guest can graft in the host's own
 * runtime (for example /usr/lib and /lib64) without first packing it into a
 * bundle. It is confined to the directory it is opened on: a walk accepts only
 * single path components, never "." or "..", and a resolved path that escapes
 * the root (through a symlink) is refused. The manifest, not the guest, chooses
 * which host directory is exposed and whether it is writable. A writable mount
 * also serves create, write, truncate, and remove, so a guest's build output
 * can land on the host filesystem rather than in guest RAM. */
#ifndef P9HOSTDIR_H
#define P9HOSTDIR_H

#include "p9srv.h"

/* Open the host directory at `path`. writable enables the create/write/
 * truncate/remove ops; 0 serves it read-only. Returns a backend and sets *ctx,
 * or NULL if the path is not a directory. Pass both to p9srv_init or to
 * p9graft_add. Close with p9hostdir_close. */
const struct p9_backend *p9hostdir_open(const char *path, int writable,
                                        void **ctx);

void p9hostdir_close(void *ctx);

#endif /* P9HOSTDIR_H */
