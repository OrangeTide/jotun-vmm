/* p9.h : the guest 9P client, over the shared 9P channel.
 *
 * One outstanding request at a time: each call builds a T-message, rings the
 * doorbell, and parses the reply. Fids are chosen by the caller. The root fid
 * is established by p9_mount. */
#ifndef LIBOS_P9_H
#define LIBOS_P9_H

#include <stdint.h>

#include "jotun_9p.h"

#define P9_ROOT_FID 0u

/* A decoded stat entry, the fields a read-only client needs. */
struct p9_stat {
    struct p9_qid qid;
    uint32_t      mode;
    uint64_t      length;
    char          name[64];
};

/* Negotiate the version and attach the root fid (P9_ROOT_FID). Returns 0 on
 * success, -1 on any protocol error. */
int p9_mount(void);

/* Walk from fid along nwname path components into newfid. Returns the number
 * of components walked (equal to nwname on full success), or -1 on error. */
int p9_walk(uint32_t fid, uint32_t newfid, int nwname, const char *const *names);

/* Open fid for reading. Returns 0 or -1. */
int p9_open(uint32_t fid);

/* Read up to count bytes at offset into buf. Returns the byte count, or -1. */
long p9_read(uint32_t fid, uint64_t offset, void *buf, uint32_t count);

/* Release a fid. Returns 0 or -1. */
int p9_clunk(uint32_t fid);

/* Stat fid into out. Returns 0 or -1. */
int p9_stat(uint32_t fid, struct p9_stat *out);

/* Decode one stat entry from a directory-read buffer at *off, advancing *off.
 * Returns 1 if an entry was decoded, 0 at the end of the buffer. Used to walk
 * the concatenated stat entries a directory read returns. */
int p9_dir_next(const uint8_t *buf, uint32_t len, uint32_t *off, struct p9_stat *out);

#endif /* LIBOS_P9_H */
