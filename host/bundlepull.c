/* bundlepull.c : fetch and verify a signed bundle from an http origin. */
#define _GNU_SOURCE
#include "bundlepull.h"

#include "http.h"

#include "jimage.h"

#include "cas.h"
#include "cas-pack.h"
#include "cas-sign.h"
#include "cas-topic.h"
#include "cas-tree.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PULL_MAXOBJ  200000     /* object-count cap, guards a hostile ref */
#define PULL_MAXROT  16         /* key-rotation hop cap, guards a cycle */
#define PACK_REL     "pack.dat" /* the origin's packfile, if it has one */
#define ROT_LABEL    "successor" /* reserved topic label for a key rotation */

/****************************************************************
 * A set of hashes seen this run, so each is fetched and expanded once.
 * Open addressing over a power-of-two table of 1-based indices into keys.
 ****************************************************************/

struct hset {
    char   (*keys)[CAS_HASH_HEX + 1];
    size_t   nkeys, kcap;
    uint32_t *slots;                /* 0 empty, else key index + 1 */
    size_t    nslots;
};

static uint32_t
hhash(const char *s)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < CAS_HASH_HEX; i++)
        h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

static int
hset_grow(struct hset *s)
{
    size_t n = s->nslots ? s->nslots * 2 : 1024;
    uint32_t *slots = calloc(n, sizeof *slots);
    if (!slots)
        return -1;
    for (size_t i = 0; i < s->nslots; i++) {
        uint32_t v = s->slots[i];
        if (!v)
            continue;
        size_t j = hhash(s->keys[v - 1]) & (n - 1);
        while (slots[j])
            j = (j + 1) & (n - 1);
        slots[j] = v;
    }
    free(s->slots);
    s->slots = slots;
    s->nslots = n;
    return 0;
}

/* Add hash to the set. Returns 1 if newly added, 0 if already present, -1 oom. */
static int
hset_add(struct hset *s, const char *hash)
{
    if (s->nslots == 0 || s->nkeys * 10 >= s->nslots * 7)
        if (hset_grow(s) != 0)
            return -1;
    size_t j = hhash(hash) & (s->nslots - 1);
    while (s->slots[j]) {
        if (memcmp(s->keys[s->slots[j] - 1], hash, CAS_HASH_HEX) == 0)
            return 0;
        j = (j + 1) & (s->nslots - 1);
    }
    if (s->nkeys == s->kcap) {
        size_t cap = s->kcap ? s->kcap * 2 : 1024;
        void *k = realloc(s->keys, cap * sizeof *s->keys);
        if (!k)
            return -1;
        s->keys = k;
        s->kcap = cap;
    }
    memcpy(s->keys[s->nkeys], hash, CAS_HASH_HEX);
    s->keys[s->nkeys][CAS_HASH_HEX] = '\0';
    s->slots[j] = (uint32_t)(s->nkeys + 1);
    s->nkeys++;
    return 1;
}

static void
hset_free(struct hset *s)
{
    free(s->keys);
    free(s->slots);
}

/****************************************************************
 * Object fetch and verify
 ****************************************************************/

/* Write a fetched loose object verbatim into the store, atomically. */
static int
write_loose(struct cas *store, const char *hash, const char *data, size_t len)
{
    const char *base = cas_basedir(store);
    char dir[2048], tmp[2112], final[2112];
    snprintf(dir, sizeof dir, "%s/%c%c", base, hash[0], hash[1]);
    mkdir(dir, 0755);                           /* ignore EEXIST */
    snprintf(final, sizeof final, "%s/%s", dir, hash);
    snprintf(tmp, sizeof tmp, "%s/.pull-XXXXXX", dir);

    int fd = mkstemp(tmp);
    if (fd < 0)
        return -1;
    for (size_t off = 0; off < len; ) {
        ssize_t w = write(fd, data + off, len - off);
        if (w <= 0) {
            close(fd);
            unlink(tmp);
            return -1;
        }
        off += (size_t)w;
    }
    if (fsync(fd) != 0 || close(fd) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, final) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* Store bytes as a loose object and verify them against the hash. The bytes are
 * an on-disk object image (data region followed by the 64-byte trailer), which
 * is exactly what a loose file holds, whether it came from a loose GET or was
 * carved out of a packfile. Returns 0 on a verified object, -1 otherwise. */
static int
store_and_verify(struct cas *store, struct cas_tree *ct, const char *hash,
                 const char *data, size_t len)
{
    if (write_loose(store, hash, data, len) != 0) {
        fprintf(stderr, "bundle: cannot store object %s\n", hash);
        return -1;
    }
    int fs = cas_fsck_object(store, hash);
    if (fs == CAS_FSCK_REENCODED)
        fs = (cas_tree_verify(ct, hash) == CAS_OK) ? CAS_FSCK_OK : CAS_FSCK_CORRUPT;
    if (fs != CAS_FSCK_OK) {
        fprintf(stderr, "bundle: object %s failed verification (fsck %d)\n",
                hash, fs);
        cas_remove(store, hash);
        return -1;
    }
    return 0;
}

/****************************************************************
 * Remote packfile index (byte-range transport)
 *
 * When the origin serves a packfile, the client reads only the extents it
 * needs. It range-fetches the footer (the last block) for the entry count, then
 * the sorted index, then each missing object's own [data | trailer] span. An
 * object's on-disk span in the pack is byte-identical to its loose file, so it
 * verifies through the same path.
 ****************************************************************/

struct pent {
    unsigned char hash[CAS_HASH_LEN];
    uint64_t offset;                    /* start of this object's trailer */
    uint64_t stored;                    /* on-disk data length, 0 = header len */
};

struct pack_index {
    int          present;               /* a usable pack was found */
    struct pent *ent;                   /* sorted by hash, count entries */
    uint64_t     count;
};

static uint64_t
rd_le64(const unsigned char *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static void
pack_index_free(struct pack_index *idx)
{
    free(idx->ent);
    idx->ent = NULL;
    idx->present = 0;
    idx->count = 0;
}

/* Best-effort: load the origin pack's index via byte ranges. A missing pack or
 * a server that ignores Range leaves idx->present 0, and the pull falls back to
 * loose object GETs. */
static void
pack_index_load(const struct http_url *origin, struct pack_index *idx,
                struct http_buf *buf)
{
    memset(idx, 0, sizeof *idx);

    long total = -1;
    int code = http_get_range(origin, PACK_REL, -CAS_PACK_BLOCK,
                              CAS_PACK_BLOCK, buf, &total);
    if (code != 206 || buf->len < CAS_PACK_BLOCK || total < 0)
        return;                         /* no pack, or no range support */

    const unsigned char *footer = (const unsigned char *)buf->data
                                   + (buf->len - CAS_PACK_BLOCK);
    static const unsigned char magic[] = CAS_PACK_FOOTER_MAGIC_V1;
    if (memcmp(footer, magic, CAS_PACK_MAGIC_LEN) != 0)
        return;
    uint64_t count = rd_le64(footer + CAS_PACK_MAGIC_LEN);
    if (count == 0 || count > PULL_MAXOBJ)
        return;

    long index_bytes = (long)count * CAS_PACK_BLOCK;
    long index_off = total - CAS_PACK_BLOCK - index_bytes;
    if (index_off < 0)
        return;
    code = http_get_range(origin, PACK_REL, index_off, index_bytes, buf, NULL);
    if (code != 206 || (long)buf->len < index_bytes)
        return;

    struct pent *ent = calloc(count, sizeof *ent);
    if (!ent)
        return;
    for (uint64_t i = 0; i < count; i++) {
        const unsigned char *e = (const unsigned char *)buf->data
                                 + i * CAS_PACK_BLOCK;
        memcpy(ent[i].hash, e, CAS_HASH_LEN);
        ent[i].offset = rd_le64(e + CAS_HASH_LEN);
        ent[i].stored = rd_le64(e + CAS_HASH_LEN + 8);
    }
    idx->ent = ent;
    idx->count = count;
    idx->present = 1;
    fprintf(stderr, "bundle: pack index: %llu objects\n",
            (unsigned long long)count);
}

/* Binary-search the index (sorted by raw hash) for a hex hash. */
static const struct pent *
pack_index_find(const struct pack_index *idx, const char *hash)
{
    if (!idx->present)
        return NULL;
    unsigned char want[CAS_HASH_LEN];
    if (cas_hex_decode(hash, CAS_HASH_HEX, want, sizeof want) != 0)
        return NULL;
    uint64_t lo = 0, hi = idx->count;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        int c = memcmp(want, idx->ent[mid].hash, CAS_HASH_LEN);
        if (c == 0)
            return &idx->ent[mid];
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

/* Carve one object out of the remote pack and store it. The object image is its
 * data region followed by its 64-byte trailer, at [offset - datalen, offset+64)
 * in the pack. Returns 0 on a verified object, -1 otherwise. */
static int
fetch_from_pack(const struct http_url *origin, struct cas *store,
                struct cas_tree *ct, const struct pent *e, const char *hash,
                struct http_buf *buf)
{
    long off = (long)e->offset;
    long datalen = (long)e->stored;

    if (datalen == 0) {
        /* Old pack without stored_size: read the trailer for the plaintext
         * length, then the data region. */
        if (http_get_range(origin, PACK_REL, off, CAS_PACK_BLOCK, buf, NULL)
                != 206 || buf->len < CAS_PACK_BLOCK)
            return -1;
        const char *h = buf->data + CAS_PACK_MAGIC_LEN;     /* "type len\0" */
        const char *sp = memchr(h, ' ', CAS_PACK_HEADER_LEN);
        if (!sp)
            return -1;
        datalen = strtol(sp + 1, NULL, 10);
        if (datalen < 0)
            return -1;
    }

    long start = off - datalen;
    long span = datalen + CAS_PACK_BLOCK;       /* data || trailer */
    if (start < 0)
        return -1;
    if (http_get_range(origin, PACK_REL, start, span, buf, NULL) != 206
        || (long)buf->len != span) {
        fprintf(stderr, "bundle: pack extent for %s failed\n", hash);
        return -1;
    }
    return store_and_verify(store, ct, hash, buf->data, buf->len);
}

/* Ensure one object is present and verified in the local store, fetching it
 * from the origin if missing: from the packfile when the index has it, else as
 * a loose object. Returns 0 on a verified object, -1 otherwise. */
static int
fetch_object(const struct http_url *origin, struct cas *store,
             struct cas_tree *ct, const struct pack_index *idx,
             const char *hash, struct http_buf *buf)
{
    if (cas_exists(store, hash))
        return 0;

    const struct pent *e = pack_index_find(idx, hash);
    if (e)
        return fetch_from_pack(origin, store, ct, e, hash, buf);

    char rel[CAS_HASH_HEX + 8];
    snprintf(rel, sizeof rel, "%c%c/%s", hash[0], hash[1], hash);
    int code = http_get(origin, rel, buf);
    if (code != 200) {
        fprintf(stderr, "bundle: fetch %s: http %d\n", hash, code);
        return -1;
    }
    return store_and_verify(store, ct, hash, buf->data, buf->len);
}

/* Enqueue a child hash for the walk: add it to the seen set, and if newly seen,
 * push it on the work stack (growing it as needed). Returns 0, or -1 on oom. */
static int
enqueue_child(struct hset *seen, char (**stack)[CAS_HASH_HEX + 1],
              size_t *sp, size_t *scap, const char *hash)
{
    int added = hset_add(seen, hash);
    if (added < 0)
        return -1;
    if (added == 0)
        return 0;
    if (*sp == *scap) {
        size_t cap = *scap * 2;
        void *p = realloc(*stack, cap * sizeof **stack);
        if (!p)
            return -1;
        *stack = p;
        *scap = cap;
    }
    memcpy((*stack)[(*sp)++], hash, CAS_HASH_HEX + 1);
    return 0;
}

/* Walk the object set reachable from root, fetching and verifying each missing
 * object. A tree or htree's children are enqueued after the node is present; a
 * jimage's per-arch root trees are enqueued the same way, so a multi-arch image
 * pulls every arch and the content-addressed objects shared between arches
 * transfer once. */
static int
walk_objects(const struct http_url *origin, struct cas *store,
             struct cas_tree *ct, const struct pack_index *idx,
             const char *root, struct http_buf *buf)
{
    struct hset seen = { 0 };
    char (*stack)[CAS_HASH_HEX + 1] = NULL;
    size_t sp = 0, scap = 0;
    int rc = -1;

    if (hset_add(&seen, root) < 0)
        goto done;
    scap = 1024;
    stack = malloc(scap * sizeof *stack);
    if (!stack)
        goto done;
    memcpy(stack[sp++], root, CAS_HASH_HEX + 1);

    size_t processed = 0;
    while (sp > 0) {
        if (++processed > PULL_MAXOBJ) {
            fprintf(stderr, "bundle: object set exceeds cap\n");
            goto done;
        }
        char hash[CAS_HASH_HEX + 1];
        memcpy(hash, stack[--sp], sizeof hash);

        if (fetch_object(origin, store, ct, idx, hash, buf) != 0)
            goto done;

        char type[16];
        struct cas_file cf;
        if (cas_open_object(store, &cf, hash, type, sizeof type) != CAS_OK)
            continue;

        /* A jimage names a root tree per architecture; enqueue each one. */
        if (strcmp(type, "jimage") == 0) {
            struct jimage im;
            int ok = jimage_parse((const char *)cf.data, cf.len, &im) == 0;
            cas_close(&cf);
            if (!ok) {
                fprintf(stderr, "bundle: cannot parse jimage %s\n", hash);
                goto done;
            }
            for (int i = 0; i < im.narch; i++)
                if (enqueue_child(&seen, &stack, &sp, &scap,
                                  im.archs[i].root) != 0)
                    goto done;
            continue;
        }

        int is_tree = strcmp(type, "tree") == 0 || strcmp(type, "htree") == 0;
        cas_close(&cf);
        if (!is_tree)
            continue;

        struct cas_tree_dir dir;
        if (cas_tree_load(ct, hash, &dir) != CAS_OK) {
            fprintf(stderr, "bundle: cannot load tree %s\n", hash);
            goto done;
        }
        for (int i = 0; i < dir.count; i++)
            if (enqueue_child(&seen, &stack, &sp, &scap,
                              dir.entries[i].hash) != 0) {
                cas_tree_dir_free(&dir);
                goto done;
            }
        cas_tree_dir_free(&dir);
    }
    rc = 0;
done:
    free(stack);
    hset_free(&seen);
    return rc;
}

/****************************************************************
 * Signed-root resolution
 ****************************************************************/

/* Fetch a version record by its object address, verifying its bytes against the
 * hash and decoding it (which checks topic binding and the signature). The
 * record is self-verifying: topic_id == BLAKE2b(embedded key). Returns 0 with
 * out filled, -1 otherwise. */
static int
load_record(const struct http_url *origin, struct cas *store,
            struct cas_tree *ct, const struct pack_index *idx,
            const char *addr, struct http_buf *buf, struct cas_vrec *out)
{
    if (!cas_valid_hash(addr)) {
        fprintf(stderr, "bundle: malformed record address\n");
        return -1;
    }
    if (fetch_object(origin, store, ct, idx, addr, buf) != 0)
        return -1;

    char type[16];
    struct cas_file cf;
    if (cas_open_object(store, &cf, addr, type, sizeof type) != CAS_OK) {
        fprintf(stderr, "bundle: cannot read record %s\n", addr);
        return -1;
    }
    int dr = cas_vrec_decode(cf.data, cf.len, out);
    cas_close(&cf);
    if (dr != CAS_OK) {
        fprintf(stderr, "bundle: record rejected: %s\n", cas_sign_strerror(dr));
        return -1;
    }
    return 0;
}

/* Fetch and verify the signed head record for a topic (pk) and label. The ref
 * names the head record's address; the record's embedded key must equal pk, so
 * it is bound to the trusted publisher. Returns 0 on a verified record, 1 if
 * the topic has no ref yet (ordinary, not an error), -1 otherwise. */
static int
fetch_topic_record(const struct http_url *origin, struct cas *store,
                   struct cas_tree *ct, const struct pack_index *idx,
                   const unsigned char *pk, const char *label,
                   struct http_buf *buf, struct cas_vrec *out)
{
    unsigned char tid[CAS_HASH_LEN];
    cas_sign_topic_id(tid, pk);
    char refname[CAS_HASH_HEX + 2 + CAS_TOPIC_LABEL_MAX + 1];
    if (cas_topic_ref_name(refname, sizeof refname, tid, label) != CAS_OK) {
        fprintf(stderr, "bundle: bad topic label\n");
        return -1;
    }

    char rel[sizeof refname + 16];
    snprintf(rel, sizeof rel, "refs/%s.root", refname);
    int code = http_get(origin, rel, buf);
    if (code == 404)
        return 1;                       /* no such topic ref */
    if (code != 200) {
        fprintf(stderr, "bundle: ref %s: http %d\n", refname, code);
        return -1;
    }
    char recaddr[CAS_HASH_HEX + 1];
    const char *p = buf->data;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (strlen(p) < CAS_HASH_HEX) {
        fprintf(stderr, "bundle: malformed ref value\n");
        return -1;
    }
    memcpy(recaddr, p, CAS_HASH_HEX);
    recaddr[CAS_HASH_HEX] = '\0';

    if (load_record(origin, store, ct, idx, recaddr, buf, out) != 0)
        return -1;

    /* Bind to the trusted publisher: the record's key must be the pinned one.
     * decode already proved topic_id == BLAKE2b(key) and the signature. */
    if (memcmp(out->pubkey, pk, CAS_SIGN_PUBKEY_LEN) != 0) {
        fprintf(stderr, "bundle: record signed by an untrusted key\n");
        return -1;
    }
    return 0;
}

/* Follow a chain of key-rotation endorsements. A publisher retiring a key
 * publishes a record on that key's reserved "successor" topic whose root names
 * the successor's own head record. Because the endorsement is signed by the
 * retiring key, a client that trusts that key learns the successor's key from
 * the record it points at (that record is signed by the successor and embeds
 * its key), without trusting the transport. pk is advanced in place to the
 * active key. Returns 0 on success, -1 on error. */
static int
follow_rotations(const struct http_url *origin, struct cas *store,
                 struct cas_tree *ct, const struct pack_index *idx,
                 unsigned char *pk, struct http_buf *buf)
{
    for (int hop = 0; hop < PULL_MAXROT; hop++) {
        struct cas_vrec endorse;
        int r = fetch_topic_record(origin, store, ct, idx, pk, ROT_LABEL,
                                   buf, &endorse);
        if (r == 1)
            return 0;                   /* no successor: pk is the active key */
        if (r != 0)
            return -1;

        /* The endorsement names the successor's head record; load it and adopt
         * the key that signed it. */
        struct cas_vrec succ;
        if (load_record(origin, store, ct, idx, endorse.root, buf, &succ) != 0)
            return -1;
        if (memcmp(succ.pubkey, pk, CAS_SIGN_PUBKEY_LEN) == 0)
            return 0;                   /* endorses its own key: stop */
        memcpy(pk, succ.pubkey, CAS_SIGN_PUBKEY_LEN);

        char hex[CAS_HASH_HEX + 1];
        cas_hex_encode(pk, CAS_SIGN_PUBKEY_LEN, hex);
        fprintf(stderr, "bundle: key rotated to %s\n", hex);
    }
    fprintf(stderr, "bundle: too many key rotations\n");
    return -1;
}

/* Resolve the signed root a topic publishes. pk must already be the active key
 * (after any rotations). Returns 0 with root_out (CAS_HASH_HEX + 1) filled. */
static int
resolve_signed_root(const struct http_url *origin, struct cas *store,
                    struct cas_tree *ct, const struct pack_index *idx,
                    const unsigned char *pk, const char *label,
                    struct http_buf *buf, char *root_out)
{
    struct cas_vrec vrec;
    int r = fetch_topic_record(origin, store, ct, idx, pk, label, buf, &vrec);
    if (r == 1) {
        fprintf(stderr, "bundle: no published ref for this key\n");
        return -1;
    }
    if (r != 0)
        return -1;
    if (!cas_valid_hash(vrec.root)) {
        fprintf(stderr, "bundle: record names an invalid root\n");
        return -1;
    }
    memcpy(root_out, vrec.root, CAS_HASH_HEX + 1);
    return 0;
}

/****************************************************************
 * Entry point
 ****************************************************************/

int
bundle_pull(const char *origin_url, const char *pubkey_hex,
            const char *label, const char *cache_dir, const char *ref_name)
{
    struct http_url origin;
    if (http_url_parse(origin_url, &origin) != 0) {
        fprintf(stderr, "bundle: bad origin url '%s'\n", origin_url);
        return -1;
    }
    if (!pubkey_hex || strlen(pubkey_hex) != CAS_HASH_HEX) {
        fprintf(stderr, "bundle: trusted key must be %d hex chars\n", CAS_HASH_HEX);
        return -1;
    }
    if (!cas_sign_available()) {
        fprintf(stderr, "bundle: no signature backend compiled in\n");
        return -1;
    }
    unsigned char pk[CAS_SIGN_PUBKEY_LEN];
    if (cas_hex_decode(pubkey_hex, CAS_HASH_HEX, pk, sizeof pk) != 0) {
        fprintf(stderr, "bundle: trusted key is not valid hex\n");
        return -1;
    }

    struct cas *store = cas_new(cache_dir);
    if (!store) {
        fprintf(stderr, "bundle: cannot open cache '%s'\n", cache_dir);
        return -1;
    }
    struct cas_tree *ct = cas_tree_new(store);
    if (!ct) {
        cas_free(store);
        return -1;
    }

    struct http_buf buf = { 0 };
    struct pack_index idx;
    char root[CAS_HASH_HEX + 1];
    int rc = -1;

    /* Load the origin's packfile index once (best effort); object fetches below
     * read only the extents they need from it, falling back to loose GETs. */
    pack_index_load(&origin, &idx, &buf);

    /* Follow any key-rotation endorsements from the pinned key to the active
     * one, then resolve and fetch under that key. */
    if (follow_rotations(&origin, store, ct, &idx, pk, &buf) != 0)
        goto done;
    if (resolve_signed_root(&origin, store, ct, &idx, pk, label, &buf, root) != 0)
        goto done;
    if (walk_objects(&origin, store, ct, &idx, root, &buf) != 0)
        goto done;

    /* Everything reachable from the verified root is present; record the ref so
     * the depot resolves it and a later run resumes from here. */
    if (cas_tree_ref_commit(ct, ref_name, root, "pulled from origin") != CAS_OK) {
        fprintf(stderr, "bundle: cannot commit ref '%s'\n", ref_name);
        goto done;
    }
    fprintf(stderr, "bundle: pulled and verified root %s\n", root);
    rc = 0;
done:
    pack_index_free(&idx);
    http_buf_free(&buf);
    cas_tree_free(ct);
    cas_free(store);
    return rc;
}
