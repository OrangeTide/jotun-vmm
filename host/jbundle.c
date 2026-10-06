/* jbundle.c : build a jotun guest image in a smolvfs depot.
 *
 * Each --arch token=dir mirrors a staging directory into the CAS as that
 * architecture's root tree, preserving nested directories (so boot/kernel is
 * where the host lifts it). The per-arch root hashes are then written into a
 * jimage manifest object, whose hash is the image identity, and a ref is
 * committed to that object. Objects shared across arches (identical config or
 * whole subtrees) are stored once by content address. */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cas.h"
#include "cas-tree.h"
#include "jimage.h"

static const char *progname = "jbundle";

static unsigned char *
read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "%s: cannot open %s\n", progname, path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    unsigned char *buf = malloc((size_t)n ? (size_t)n : 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "%s: short read on %s\n", progname, path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

/* Mirror the directory at path into the CAS and write its tree hash into
 * hash_out. Recurses into subdirectories; stores each regular file as a blob.
 * Entry uid/gid/mtime are zeroed so the same inputs always hash the same way,
 * which keeps images reproducible and maximises dedup across builds. Returns 0,
 * or -1 on error. */
static int
import_dir(struct cas_tree *ct, struct cas *store, const char *path,
           char *hash_out)
{
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "%s: cannot open directory %s\n", progname, path);
        return -1;
    }

    struct cas_tree_dir dir;
    cas_tree_dir_init(&dir);

    struct dirent *de;
    int rc = -1;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        char child[4096];
        snprintf(child, sizeof child, "%s/%s", path, de->d_name);

        struct stat st;
        if (stat(child, &st) != 0) {
            fprintf(stderr, "%s: cannot stat %s\n", progname, child);
            goto done;
        }

        struct cas_tree_entry e;
        memset(&e, 0, sizeof e);
        snprintf(e.name, sizeof e.name, "%s", de->d_name);

        if (S_ISDIR(st.st_mode)) {
            if (import_dir(ct, store, child, e.hash) != 0)
                goto done;
            e.mode = CAS_TREE_S_IFDIR | (st.st_mode & 0777);
        } else if (S_ISREG(st.st_mode)) {
            size_t len;
            unsigned char *data = read_file(child, &len);
            if (!data)
                goto done;
            int r = cas_put_object(store, "blob", data, len, e.hash);
            free(data);
            if (r != CAS_OK) {
                fprintf(stderr, "%s: cannot store %s: %s\n", progname, child,
                        cas_strerror(r));
                goto done;
            }
            e.mode = CAS_TREE_S_IFREG | (st.st_mode & 0777);
        } else {
            continue;                   /* skip symlinks, devices, sockets */
        }

        if (cas_tree_dir_add(&dir, &e) != CAS_OK) {
            fprintf(stderr, "%s: cannot add %s\n", progname, child);
            goto done;
        }
    }

    if (cas_tree_store(ct, &dir, hash_out) != CAS_OK) {
        fprintf(stderr, "%s: cannot store tree for %s\n", progname, path);
        goto done;
    }
    rc = 0;

done:
    cas_tree_dir_free(&dir);
    closedir(d);
    return rc;
}

static void
usage(void)
{
    fprintf(stderr,
            "usage: %s -d <depot> [--ref <name>] [--name <s>]\n"
            "         [--image-version <s>] [--upstream <s>] [--note <s>]\n"
            "         --arch <token>=<dir> [--arch <token>=<dir>]...\n",
            progname);
}

int
main(int argc, char **argv)
{
    const char *depot = NULL;
    const char *refname = "root";
    const char *name = NULL;
    const char *imgver = NULL;
    const char *upstream = NULL;
    const char *note = NULL;
    char tokens[JIMAGE_MAXARCH][JIMAGE_ARCHLEN];
    const char *dirs[JIMAGE_MAXARCH];
    int narch = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            depot = argv[++i];
        } else if (strcmp(argv[i], "--ref") == 0 && i + 1 < argc) {
            refname = argv[++i];
        } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else if (strcmp(argv[i], "--image-version") == 0 && i + 1 < argc) {
            imgver = argv[++i];
        } else if (strcmp(argv[i], "--upstream") == 0 && i + 1 < argc) {
            upstream = argv[++i];
        } else if (strcmp(argv[i], "--note") == 0 && i + 1 < argc) {
            note = argv[++i];
        } else if (strcmp(argv[i], "--arch") == 0 && i + 1 < argc) {
            char *spec = argv[++i];
            char *eq = strchr(spec, '=');
            if (!eq || narch >= JIMAGE_MAXARCH) {
                fprintf(stderr, "%s: bad --arch '%s'\n", progname, spec);
                return 1;
            }
            *eq = '\0';
            snprintf(tokens[narch], JIMAGE_ARCHLEN, "%s", spec);
            dirs[narch] = eq + 1;
            narch++;
        } else {
            usage();
            return 1;
        }
    }

    if (!depot || narch == 0) {
        usage();
        return 1;
    }

    struct cas *store = cas_new(depot);
    if (!store) {
        fprintf(stderr, "%s: cannot open depot %s\n", progname, depot);
        return 1;
    }
    struct cas_tree *ct = cas_tree_new(store);
    if (!ct) {
        cas_free(store);
        return 1;
    }

    /* Build the manifest text as each arch root is imported. */
    char text[4096];
    int n = snprintf(text, sizeof text, "%s %d\n", JIMAGE_MAGIC,
                     JIMAGE_VERSION);
    if (name)
        n += snprintf(text + n, sizeof text - n, "name %s\n", name);
    if (imgver)
        n += snprintf(text + n, sizeof text - n, "version %s\n", imgver);
    if (upstream)
        n += snprintf(text + n, sizeof text - n, "upstream %s\n", upstream);
    if (note)
        n += snprintf(text + n, sizeof text - n, "note %s\n", note);

    for (int i = 0; i < narch; i++) {
        char root[CAS_HASH_HEX + 1];
        if (import_dir(ct, store, dirs[i], root) != 0) {
            cas_tree_free(ct);
            cas_free(store);
            return 1;
        }
        fprintf(stderr, "%s: arch %s root %s\n", progname, tokens[i], root);
        n += snprintf(text + n, sizeof text - n, "arch %s %s\n", tokens[i],
                      root);
        if (n >= (int)sizeof text) {
            fprintf(stderr, "%s: manifest too large\n", progname);
            cas_tree_free(ct);
            cas_free(store);
            return 1;
        }
    }

    char jhash[CAS_HASH_HEX + 1];
    if (cas_put_object(store, JIMAGE_TYPE, text, (size_t)n, jhash) != CAS_OK) {
        fprintf(stderr, "%s: cannot store image manifest\n", progname);
        cas_tree_free(ct);
        cas_free(store);
        return 1;
    }
    if (cas_tree_ref_commit(ct, refname, jhash, "jbundle") != CAS_OK) {
        fprintf(stderr, "%s: cannot commit ref %s\n", progname, refname);
        cas_tree_free(ct);
        cas_free(store);
        return 1;
    }

    printf("%s\n", jhash);
    cas_tree_free(ct);
    cas_free(store);
    return 0;
}
