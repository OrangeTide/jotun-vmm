/* p9srv.h : a minimal read-only 9P2000 server.
 *
 * The server owns the protocol, the fid table, and message encoding. It is
 * backend-agnostic: a backend resolves the root, walks a name, lists a
 * directory, and reads a file's bytes. The in-memory backend and the smolvfs
 * backend both implement this interface. */
#ifndef P9SRV_H
#define P9SRV_H

#include <stdint.h>

/* A resolved node. The backend fills every field when it resolves a node; the
 * server treats handle as opaque and hands it back on later calls. */
struct p9_node {
    int      is_dir;
    uint32_t mode;                  /* permission bits, low 9 */
    uint64_t length;                /* file size, 0 for directories */
    uint64_t qid_path;              /* stable unique id for the qid */
    char     handle[65];            /* backend handle (a 64-hex hash fits) */
    char     name[256];             /* leaf name, for stat */
};

/* One directory entry, returned by the list op. */
struct p9_dirent {
    char     name[256];
    int      is_dir;
    uint32_t mode;
    uint64_t length;
    uint64_t qid_path;
};

/* A directory listing streams its entries to this callback, one at a time, so
 * no fixed-size array bounds a listing. Returns 0 to continue, -1 to stop
 * early. The pointed-to entry is valid only for the call. */
typedef int (*p9_emit_fn)(void *emit_ctx, const struct p9_dirent *de);

/* The backend interface. root and walk return 0 or -1; list emits each entry
 * and returns 0 or -1; readf returns a byte count or -1. */
struct p9_backend {
    int  (*root)(void *ctx, struct p9_node *out);
    int  (*walk)(void *ctx, const struct p9_node *dir, const char *name,
                 struct p9_node *out);
    int  (*list)(void *ctx, const struct p9_node *dir,
                 p9_emit_fn emit, void *emit_ctx);
    long (*readf)(void *ctx, const struct p9_node *file, uint64_t off,
                  void *buf, uint32_t n);
};

/* Must cover the guest's 9P fid pool (GFID_MAX in guest/libos/linuxelf.c) plus
 * its interp-loader fid and a scratch fid, with headroom. */
#define P9_MAXFID 160

struct p9srv {
    const struct p9_backend *be;
    void *ctx;
    struct {
        int            used;
        int            open;
        uint32_t       fid;
        struct p9_node node;
    } fids[P9_MAXFID];
    /* A directory read serializes the whole listing once (at offset 0) into
     * this growable buffer, then serves byte ranges of it across the reads
     * that walk the directory. Keyed by the fid it was built for; a read at
     * offset 0, or of a different fid, rebuilds it. So a listing is bounded
     * only by host memory, not by a fixed entry count or message size. */
    uint8_t  *dirbuf;
    uint32_t  dirbuf_cap;
    uint32_t  dirbuf_len;
    uint32_t  dir_fid;
    int       dir_valid;
};

void p9srv_init(struct p9srv *s, const struct p9_backend *be, void *ctx);

/* Process one T-message in req[reqlen] and write the R-message into rsp, which
 * has rspcap bytes. Returns the reply length. */
uint32_t p9srv_handle(struct p9srv *s, const uint8_t *req, uint32_t reqlen,
                      uint8_t *rsp, uint32_t rspcap);

#endif /* P9SRV_H */
