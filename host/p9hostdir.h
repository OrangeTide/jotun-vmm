/* p9hostdir.h : a read-only 9P backend that serves a live host directory.
 *
 * Where p9smolvfs serves a content-addressed bundle, this serves a real
 * directory on the host filesystem, so a guest can graft in the host's own
 * runtime (for example /usr/lib and /lib64) without first packing it into a
 * bundle. It is read-only and confined to the directory it is opened on: a
 * walk accepts only single path components, never "." or "..", and a resolved
 * path that escapes the root (through a symlink) is refused. The manifest, not
 * the guest, chooses which host directory is exposed. */
#ifndef P9HOSTDIR_H
#define P9HOSTDIR_H

#include "p9srv.h"

/* Open the host directory at `path`, read-only. Returns a backend and sets
 * *ctx, or NULL if the path is not a directory. Pass both to p9srv_init or to
 * p9graft_add. Close with p9hostdir_close. */
const struct p9_backend *p9hostdir_open(const char *path, void **ctx);

void p9hostdir_close(void *ctx);

#endif /* P9HOSTDIR_H */
