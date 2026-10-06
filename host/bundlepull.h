/* bundlepull.h : fetch and verify a signed bundle from an http origin.
 *
 * Pulls a bundle from a static http origin into a local cache depot, fetching
 * only the objects it lacks, verifying every object against its content hash,
 * and verifying the published root against a trusted publisher key before the
 * root is trusted. On success the cache depot serves the bundle under a local
 * ref, exactly as a local depot does, so the caller opens it unchanged.
 *
 * The transport is plain http; authenticity rests on the signature, not the
 * transport. Any failure (transport, a missing or wrong signature, a corrupt
 * object) returns an error, and the caller must refuse to boot. */
#ifndef BUNDLEPULL_H
#define BUNDLEPULL_H

/* origin_url : "http://host[:port][/base]" serving the depot as static files.
 * pubkey_hex : the trusted publisher public key, 64 hex characters.
 * label      : optional topic label, or NULL for the bare topic.
 * cache_dir  : local depot directory, created if absent.
 * ref_name   : local ref the verified root is committed under (e.g. "root").
 *
 * Returns 0 on success (cache_dir now resolves ref_name to the verified root),
 * or -1 on any failure. */
int bundle_pull(const char *origin_url, const char *pubkey_hex,
                const char *label, const char *cache_dir, const char *ref_name);

#endif /* BUNDLEPULL_H */
