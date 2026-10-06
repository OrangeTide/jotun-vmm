/* bundlepull.c : fetch and verify a signed bundle from an http origin. */
#define _GNU_SOURCE
#include "bundlepull.h"

#include "http.h"

#include "cas.h"
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

/* Ensure one object is present and verified in the local store, fetching it
 * from the origin if missing. Returns 0 on a verified object, -1 otherwise. */
static int
fetch_object(const struct http_url *origin, struct cas *store,
             struct cas_tree *ct, const char *hash, struct http_buf *buf)
{
    if (cas_exists(store, hash))
        return 0;

    char rel[CAS_HASH_HEX + 8];
    snprintf(rel, sizeof rel, "%c%c/%s", hash[0], hash[1], hash);
    int code = http_get(origin, rel, buf);
    if (code != 200) {
        fprintf(stderr, "bundle: fetch %s: http %d\n", hash, code);
        return -1;
    }
    if (write_loose(store, hash, buf->data, buf->len) != 0) {
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

/* Walk the object set reachable from root, fetching and verifying each missing
 * object. A tree or htree's children are enqueued after the node is present. */
static int
walk_objects(const struct http_url *origin, struct cas *store,
             struct cas_tree *ct, const char *root, struct http_buf *buf)
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

        if (fetch_object(origin, store, ct, hash, buf) != 0)
            goto done;

        char type[16];
        struct cas_file cf;
        if (cas_open_object(store, &cf, hash, type, sizeof type) != CAS_OK)
            continue;
        int is_tree = strcmp(type, "tree") == 0 || strcmp(type, "htree") == 0;
        cas_close(&cf);
        if (!is_tree)
            continue;

        struct cas_tree_dir dir;
        if (cas_tree_load(ct, hash, &dir) != CAS_OK) {
            fprintf(stderr, "bundle: cannot load tree %s\n", hash);
            goto done;
        }
        for (int i = 0; i < dir.count; i++) {
            int added = hset_add(&seen, dir.entries[i].hash);
            if (added < 0) {
                cas_tree_dir_free(&dir);
                goto done;
            }
            if (added == 0)
                continue;
            if (sp == scap) {
                size_t cap = scap * 2;
                void *p = realloc(stack, cap * sizeof *stack);
                if (!p) {
                    cas_tree_dir_free(&dir);
                    goto done;
                }
                stack = p;
                scap = cap;
            }
            memcpy(stack[sp++], dir.entries[i].hash, CAS_HASH_HEX + 1);
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

/* Fetch and verify the signed head record, returning the published root hash in
 * root_out (CAS_HASH_HEX + 1 bytes). The record's embedded key must equal the
 * trusted key pk. Returns 0 on a verified root, -1 otherwise. */
static int
resolve_signed_root(const struct http_url *origin, struct cas *store,
                    struct cas_tree *ct, const unsigned char *pk,
                    const char *label, struct http_buf *buf, char *root_out)
{
    if (!cas_sign_available()) {
        fprintf(stderr, "bundle: no signature backend compiled in\n");
        return -1;
    }

    unsigned char tid[CAS_HASH_LEN];
    cas_sign_topic_id(tid, pk);
    char refname[CAS_HASH_HEX + 2 + CAS_TOPIC_LABEL_MAX + 1];
    if (cas_topic_ref_name(refname, sizeof refname, tid, label) != CAS_OK) {
        fprintf(stderr, "bundle: bad topic label\n");
        return -1;
    }

    /* The ref file holds the head record's address. */
    char rel[sizeof refname + 16];
    snprintf(rel, sizeof rel, "refs/%s.root", refname);
    int code = http_get(origin, rel, buf);
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
    if (!cas_valid_hash(recaddr)) {
        fprintf(stderr, "bundle: malformed record address\n");
        return -1;
    }

    /* Fetch and content-verify the record object, then read its bytes. */
    if (fetch_object(origin, store, ct, recaddr, buf) != 0)
        return -1;

    char type[16];
    struct cas_file cf;
    if (cas_open_object(store, &cf, recaddr, type, sizeof type) != CAS_OK) {
        fprintf(stderr, "bundle: cannot read record %s\n", recaddr);
        return -1;
    }
    struct cas_vrec vrec;
    int dr = cas_vrec_decode(cf.data, cf.len, &vrec);
    cas_close(&cf);
    if (dr != CAS_OK) {
        fprintf(stderr, "bundle: record rejected: %s\n", cas_sign_strerror(dr));
        return -1;
    }

    /* Bind to the trusted publisher: the record's key must be the pinned one.
     * decode already proved topic_id == BLAKE2b(key) and the signature. */
    if (memcmp(vrec.pubkey, pk, CAS_SIGN_PUBKEY_LEN) != 0) {
        fprintf(stderr, "bundle: record signed by an untrusted key\n");
        return -1;
    }
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
    char root[CAS_HASH_HEX + 1];
    int rc = -1;

    if (resolve_signed_root(&origin, store, ct, pk, label, &buf, root) != 0)
        goto done;
    if (walk_objects(&origin, store, ct, root, &buf) != 0)
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
    http_buf_free(&buf);
    cas_tree_free(ct);
    cas_free(store);
    return rc;
}
