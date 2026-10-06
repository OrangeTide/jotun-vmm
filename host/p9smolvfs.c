/* p9smolvfs.c : a 9P backend backed by a smolvfs content-addressed bundle.
 *
 * A node's backend handle is the object's 64-hex hash. Directories are tree
 * objects; files are blobs. The qid path is derived from the hash, which is
 * stable per content. File sizes are not stored in the tree, so a file's
 * length is obtained by opening its blob. */
#include "p9smolvfs.h"

#include <string.h>

#include "cas.h"
#include "cas-tree.h"

struct smolctx {
    struct cas      *store;
    struct cas_tree *ct;
    char            root[CAS_HASH_HEX + 1];
};

static struct smolctx ctx;

/* Fold the leading hex of a content hash into a 64-bit qid path. */
static uint64_t
qid_of(const char *hash)
{
    uint64_t v = 0;

    for (int i = 0; i < 16 && hash[i]; i++) {
        char c = hash[i];
        int d = 0;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        v = (v << 4) | (uint64_t)d;
    }
    return v;
}

static int
blob_len(struct smolctx *c, const char *hash, uint64_t *out)
{
    struct cas_file cf;

    if (cas_open(c->store, &cf, hash) != CAS_OK)
        return -1;
    *out = cf.len;
    cas_close(&cf);
    return 0;
}

static void
fill_node(struct smolctx *c, struct p9_node *n, const char *name,
          int mode, const char *hash)
{
    int isdir = (mode & CAS_TREE_S_IFMT) == CAS_TREE_S_IFDIR;

    memset(n, 0, sizeof *n);
    n->is_dir = isdir;
    n->mode = (uint32_t)(mode & 0777);
    n->qid_path = qid_of(hash);
    strncpy(n->handle, hash, sizeof n->handle - 1);
    strncpy(n->name, name, sizeof n->name - 1);
    if (!isdir)
        blob_len(c, hash, &n->length);
}

static int
sv_root(void *vctx, struct p9_node *out)
{
    struct smolctx *c = vctx;

    memset(out, 0, sizeof *out);
    out->is_dir = 1;
    out->mode = 0555;
    out->qid_path = qid_of(c->root);
    strncpy(out->handle, c->root, sizeof out->handle - 1);
    strcpy(out->name, "/");
    return 0;
}

static int
sv_walk(void *vctx, const struct p9_node *dir, const char *name,
        struct p9_node *out)
{
    struct smolctx *c = vctx;
    struct cas_tree_entry e;

    if (!dir->is_dir)
        return -1;
    if (cas_tree_lookup(c->ct, dir->handle, name, &e) != CAS_OK)
        return -1;
    fill_node(c, out, e.name, e.mode, e.hash);
    return 0;
}

static int
sv_list(void *vctx, const struct p9_node *dir, struct p9_dirent *ents, int max)
{
    struct smolctx *c = vctx;
    struct cas_tree_dir d;

    if (!dir->is_dir)
        return -1;
    if (cas_tree_load(c->ct, dir->handle, &d) != CAS_OK)
        return -1;

    int n = 0;
    for (int i = 0; i < d.count && n < max; i++) {
        struct cas_tree_entry *e = &d.entries[i];
        int isdir = (e->mode & CAS_TREE_S_IFMT) == CAS_TREE_S_IFDIR;

        memset(&ents[n], 0, sizeof ents[n]);
        strncpy(ents[n].name, e->name, sizeof ents[n].name - 1);
        ents[n].is_dir = isdir;
        ents[n].mode = (uint32_t)(e->mode & 0777);
        ents[n].qid_path = qid_of(e->hash);
        if (!isdir)
            blob_len(c, e->hash, &ents[n].length);
        n++;
    }
    cas_tree_dir_free(&d);
    return n;
}

static long
sv_readf(void *vctx, const struct p9_node *file, uint64_t off, void *buf,
         uint32_t n)
{
    struct smolctx *c = vctx;
    struct cas_file cf;
    long r = 0;

    if (cas_open(c->store, &cf, file->handle) != CAS_OK)
        return -1;
    if (off < cf.len) {
        uint64_t avail = cf.len - off;
        if (n > avail)
            n = (uint32_t)avail;
        memcpy(buf, cf.data + off, n);
        r = (long)n;
    }
    cas_close(&cf);
    return r;
}

static const struct p9_backend sv_backend = {
    .root = sv_root,
    .walk = sv_walk,
    .list = sv_list,
    .readf = sv_readf,
};

const struct p9_backend *
p9smolvfs_open(const char *depot, const char *ref, void **ctx_out)
{
    ctx.store = cas_new(depot);
    if (!ctx.store)
        return NULL;
    ctx.ct = cas_tree_new(ctx.store);
    if (!ctx.ct) {
        cas_free(ctx.store);
        ctx.store = NULL;
        return NULL;
    }
    if (cas_tree_ref_read(ctx.ct, ref, ctx.root) != CAS_OK) {
        cas_tree_free(ctx.ct);
        cas_free(ctx.store);
        ctx.store = NULL;
        return NULL;
    }
    *ctx_out = &ctx;
    return &sv_backend;
}
