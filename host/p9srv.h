/* p9srv.h : a minimal read-only 9P2000 server.
 *
 * The server owns the protocol, the fid table, and message encoding. It is
 * backend-agnostic: a backend resolves the root, walks a name, lists a
 * directory, and reads a file's bytes. The in-memory backend and the smolvfs
 * backend both implement this interface. */
#ifndef P9SRV_H
#define P9SRV_H

#include <stdint.h>

/* The backend handle buffer. It holds either a 64-hex content hash (the CAS
 * backends) or a path relative to the mount root (the host-directory backend).
 * A real source tree nests deep, so this must be large enough for a full
 * relative path, not just a hash: Node's checkout reaches past 170 bytes. */
#define P9_HANDLE_MAX 1024

/* A resolved node. The backend fills every field when it resolves a node; the
 * server treats handle as opaque and hands it back on later calls. */
struct p9_node {
    int      is_dir;
    uint32_t mode;                  /* permission bits, low 9 */
    uint64_t length;                /* file size, 0 for directories */
    uint64_t qid_path;              /* stable unique id for the qid */
    uint32_t mtime;                 /* modification time, Unix seconds (0 if none) */
    char     handle[P9_HANDLE_MAX]; /* backend handle (hash or relative path) */
    char     name[256];             /* leaf name, for stat */
};

/* One directory entry, returned by the list op. */
struct p9_dirent {
    char     name[256];
    int      is_dir;
    uint32_t mode;
    uint64_t length;
    uint64_t qid_path;
    uint32_t mtime;                 /* modification time, Unix seconds (0 if none) */
};

/* A directory listing streams its entries to this callback, one at a time, so
 * no fixed-size array bounds a listing. Returns 0 to continue, -1 to stop
 * early. The pointed-to entry is valid only for the call. */
typedef int (*p9_emit_fn)(void *emit_ctx, const struct p9_dirent *de);

/* The backend interface. root and walk return 0 or -1; list emits each entry
 * and returns 0 or -1; readf returns a byte count or -1.
 *
 * The write ops (writef, create, truncate, removef, renamef) are NULL for a
 * read-only backend; the server reports a read-only error when a T-message needs
 * one that is absent. create makes a child of dir: a directory when perm has
 * P9_DMDIR, else a regular file, and fills out with the new node. */
struct p9_backend {
    int  (*root)(void *ctx, struct p9_node *out);
    int  (*walk)(void *ctx, const struct p9_node *dir, const char *name,
                 struct p9_node *out);
    int  (*list)(void *ctx, const struct p9_node *dir,
                 p9_emit_fn emit, void *emit_ctx);
    long (*readf)(void *ctx, const struct p9_node *file, uint64_t off,
                  void *buf, uint32_t n);
    long (*writef)(void *ctx, const struct p9_node *file, uint64_t off,
                   const void *buf, uint32_t n);
    int  (*create)(void *ctx, const struct p9_node *dir, const char *name,
                   uint32_t perm, struct p9_node *out);
    int  (*truncate)(void *ctx, const struct p9_node *file, uint64_t len);
    int  (*removef)(void *ctx, const struct p9_node *node);
    /* Move oldname under olddir to newname under newdir, replacing an existing
     * destination. Both directories belong to this backend (the server and the
     * graft wrapper enforce that). NULL on a read-only backend. Returns 0 or -1. */
    int  (*renamef)(void *ctx, const struct p9_node *olddir, const char *oldname,
                    const struct p9_node *newdir, const char *newname);
    /* Optional. Open a regular file and return a host file descriptor the server
     * holds for the fid's lifetime, reading and writing it with pread/pwrite and
     * closing it on clunk. A held descriptor keeps the file's inode alive after
     * its name is removed, so a program that unlinks a file it still has open
     * (a shell's self-cleaning here-document temp, and much of autoconf) keeps
     * reading and writing it. A backend with no host file (the CAS bundle) leaves
     * this NULL and the server falls back to readf/writef by path. Returns an fd
     * >= 0, or -1 to decline (the server then uses readf/writef). for_write opens
     * read-write so one descriptor serves a create-write-read sequence. */
    int  (*openh)(void *ctx, const struct p9_node *file, int for_write);
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
        int            hostfd;      /* backend's held host fd, or -1 (use readf/writef) */
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
