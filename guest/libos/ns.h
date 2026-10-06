/* ns.h : a minimal per-guest namespace over the single 9P tree.
 *
 * Paths resolve through a table of mounts, each grafting a source subpath of
 * the 9P tree onto a mountpoint prefix, longest prefix winning. bind and mount
 * are the same operation here, since there is one filesystem; additional
 * services (net, display) become real mounts in later milestones. A single
 * global namespace for now; per-process namespaces are a later refinement. */
#ifndef LIBOS_NS_H
#define LIBOS_NS_H

#include <stdint.h>

/* Reset the namespace to a single mount of the 9P root at "/". */
void ns_init(void);

/* Graft the source subpath src onto the mountpoint at. Returns 0 or -1. */
int ns_mount(const char *at, const char *src);

/* bind src onto at: accessing at then yields src. Returns 0 or -1. */
int ns_bind(const char *src, const char *at);

/* Resolve path through the namespace and walk it from the 9P root into fid.
 * Returns 0 on a full walk, -1 otherwise. */
int ns_walk(const char *path, uint32_t fid);

#endif /* LIBOS_NS_H */
