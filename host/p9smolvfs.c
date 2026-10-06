/* p9smolvfs.c : a 9P backend backed by a smolvfs content-addressed bundle.
 *
 * A node's backend handle is the object's 64-hex hash. Directories are tree
 * objects; files are blobs. The qid path is derived from the hash, which is
 * stable per content. File sizes are not stored in the tree, so a file's
 * length is obtained by opening its blob. */
#include "p9smolvfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cas.h"
#include "cas-tree.h"
#include "jimage.h"

/* Walk a slash-separated subtree from the given tree hash, following only
 * directories, and write the resulting tree hash into cur. Returns 0 on
 * success, -1 if a component is missing or is not a directory. */
static int
descend_subtree(struct cas_tree *ct, char *cur, const char *subtree)
{
    char buf[256];
    size_t i = 0;
    for (; subtree[i] && i < sizeof buf - 1; i++)
        buf[i] = subtree[i];
    buf[i] = '\0';

    char *p = buf;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        char *comp = p;
        while (*p && *p != '/')
            p++;
        if (*p)
            *p++ = '\0';

        struct cas_tree_entry e;
        if (cas_tree_lookup(ct, cur, comp, &e) != CAS_OK)
            return -1;
        if ((e.mode & CAS_TREE_S_IFMT) != CAS_TREE_S_IFDIR)
            return -1;
        for (int k = 0; k <= CAS_HASH_HEX; k++)
            cur[k] = e.hash[k];
    }
    return 0;
}

struct smolctx {
    struct cas      *store;
    struct cas_tree *ct;
    char            root[CAS_HASH_HEX + 1];
};

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

/* A ref is either a ref name (which its owner may advance) or a fixed 64-hex
 * root hash that pins an immutable snapshot. Pinning lets a guest sharing
 * another guest's depot serve a stable tree even as the owner commits new
 * roots. Return 1 if s is exactly a content-hash hex string. */
static int
is_root_hash(const char *s)
{
    size_t n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
              || (c >= 'A' && c <= 'F')))
            return 0;
    }
    return n == CAS_HASH_HEX;
}

/* If c->root names a jimage manifest object, replace it with the root tree for
 * `arch`. Leaves c->root untouched when it is an ordinary tree. Returns 0 on
 * success, -1 if the object is a jimage this host cannot use (bad format or no
 * row for arch). */
static int
resolve_image(struct smolctx *c, const char *arch)
{
    struct cas_file cf;
    char type[CAS_TYPE_MAX + 1] = "";

    if (cas_open_object(c->store, &cf, c->root, type, sizeof type) != CAS_OK)
        return 0;                       /* not readable as a typed object: treat as a tree */
    if (strcmp(type, JIMAGE_TYPE) != 0) {
        cas_close(&cf);
        return 0;                       /* an ordinary tree root */
    }

    struct jimage im;
    int ok = jimage_parse((const char *)cf.data, cf.len, &im) == 0;
    cas_close(&cf);
    if (!ok) {
        fprintf(stderr, "[9p] image manifest is malformed or unsupported\n");
        return -1;
    }

    const char *r = jimage_root_for(&im, arch ? arch : "");
    if (!r) {
        fprintf(stderr, "[9p] image has no variant for arch '%s'\n",
                arch ? arch : "");
        return -1;
    }
    strncpy(c->root, r, sizeof c->root - 1);
    c->root[sizeof c->root - 1] = '\0';
    return 0;
}

const struct p9_backend *
p9smolvfs_open(const char *depot, const char *ref, const char *subtree,
               const char *arch, void **ctx_out)
{
    struct smolctx *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;

    c->store = cas_new(depot);
    if (!c->store) {
        free(c);
        return NULL;
    }
    c->ct = cas_tree_new(c->store);
    if (!c->ct) {
        cas_free(c->store);
        free(c);
        return NULL;
    }
    if (ref && is_root_hash(ref)) {
        strncpy(c->root, ref, sizeof c->root - 1);          /* pinned snapshot */
        c->root[sizeof c->root - 1] = '\0';
    } else if (cas_tree_ref_read(c->ct, ref, c->root) != CAS_OK) {
        goto fail;
    }

    /* The resolved object is either a root tree (serve it) or a jimage manifest
     * that points at a root tree per arch (select this host's). */
    if (resolve_image(c, arch) != 0)
        goto fail;

    /* Confirm the root names a real tree. A ref name always resolves to a
     * committed tree, but a pinned or image-selected hash is caller-supplied,
     * so a mistyped one fails here rather than mounting an empty, broken root. */
    struct cas_tree_dir d;
    if (cas_tree_load(c->ct, c->root, &d) != CAS_OK)
        goto fail;
    cas_tree_dir_free(&d);

    /* Confine the served root to the manifest's subtree. */
    if (subtree && descend_subtree(c->ct, c->root, subtree) != 0)
        goto fail;

    *ctx_out = c;
    return &sv_backend;

fail:
    cas_tree_free(c->ct);
    cas_free(c->store);
    free(c);
    return NULL;
}

void
p9smolvfs_close(void *vctx)
{
    struct smolctx *c = vctx;
    if (!c)
        return;
    cas_tree_free(c->ct);
    cas_free(c->store);
    free(c);
}

int
p9smolvfs_readfile(void *vctx, const char *path, void **out, size_t *len)
{
    struct smolctx *c = vctx;
    char cur[CAS_HASH_HEX + 1];
    char buf[256];
    size_t i = 0;

    for (; path[i] && i < sizeof buf - 1; i++)
        buf[i] = path[i];
    buf[i] = '\0';

    for (int k = 0; k <= CAS_HASH_HEX; k++)
        cur[k] = c->root[k];

    /* Walk the path. Every component but the last is a directory to descend;
     * the last names the file whose blob we return. */
    char *p = buf;
    struct cas_tree_entry e;
    int have = 0;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        char *comp = p;
        while (*p && *p != '/')
            p++;
        if (*p)
            *p++ = '\0';

        if (cas_tree_lookup(c->ct, cur, comp, &e) != CAS_OK)
            return -1;
        have = 1;
        for (int k = 0; k <= CAS_HASH_HEX; k++)
            cur[k] = e.hash[k];
    }
    if (!have || (e.mode & CAS_TREE_S_IFMT) == CAS_TREE_S_IFDIR)
        return -1;

    struct cas_file cf;
    if (cas_open(c->store, &cf, cur) != CAS_OK)
        return -1;
    void *b = malloc(cf.len ? cf.len : 1);
    if (!b) {
        cas_close(&cf);
        return -1;
    }
    memcpy(b, cf.data, cf.len);
    *out = b;
    *len = cf.len;
    cas_close(&cf);
    return 0;
}
