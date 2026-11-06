/* p9hostdir.c : a read-only 9P backend over a live host directory. */

#include "p9hostdir.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct p9hostdir {
    char root[PATH_MAX];                /* canonical host directory root */
    /* A one-entry read cache: a library is read sequentially, so keeping the
     * last file open turns a run of reads into a single open. */
    char cur_handle[65];
    int  cur_fd;
};

/* A walk accepts only a single path component, never one that could escape the
 * root by name alone. Symlink escapes are caught separately by realpath. */
static int
valid_name(const char *n)
{
    if (!n[0] || strcmp(n, ".") == 0 || strcmp(n, "..") == 0)
        return 0;
    for (const char *p = n; *p; p++)
        if (*p == '/')
            return 0;
    return 1;
}

/* True when real, a canonical path, lies within root (also canonical). */
static int
within(const char *root, const char *real)
{
    size_t rl = strlen(root);
    if (strncmp(real, root, rl) != 0)
        return 0;
    return real[rl] == '\0' || real[rl] == '/';
}

/* The host handle (a path relative to the root; "" is the root itself) is the
 * node's handle. Fill a node from a stat of it. */
static void
fill_node(struct p9_node *out, const char *rel, const char *name,
          const struct stat *st)
{
    out->is_dir = S_ISDIR(st->st_mode) ? 1 : 0;
    out->mode = st->st_mode & 0777;
    out->length = out->is_dir ? 0 : (uint64_t)st->st_size;
    out->qid_path = (uint64_t)st->st_ino;       /* host inode: stable, unique */
    strncpy(out->handle, rel, sizeof out->handle - 1);
    out->handle[sizeof out->handle - 1] = '\0';
    strncpy(out->name, name, sizeof out->name - 1);
    out->name[sizeof out->name - 1] = '\0';
}

static int
hd_root(void *vctx, struct p9_node *out)
{
    struct p9hostdir *c = vctx;
    struct stat st;
    if (stat(c->root, &st) != 0)
        return -1;
    fill_node(out, "", "/", &st);
    return 0;
}

static int
hd_walk(void *vctx, const struct p9_node *dir, const char *name,
        struct p9_node *out)
{
    struct p9hostdir *c = vctx;
    if (!valid_name(name))
        return -1;

    char rel[256];
    int rl = dir->handle[0]
        ? snprintf(rel, sizeof rel, "%s/%s", dir->handle, name)
        : snprintf(rel, sizeof rel, "%s", name);
    if (rl < 0 || rl >= (int)sizeof rel || (size_t)rl >= sizeof out->handle)
        return -1;                      /* must fit the node handle */

    char full[PATH_MAX];
    if (snprintf(full, sizeof full, "%s/%s", c->root, rel) >= (int)sizeof full)
        return -1;

    /* Resolve symlinks and refuse anything that leaves the root. */
    char real[PATH_MAX];
    if (!realpath(full, real) || !within(c->root, real))
        return -1;

    struct stat st;
    if (stat(real, &st) != 0)
        return -1;
    fill_node(out, rel, name, &st);
    return 0;
}

static int
hd_list(void *vctx, const struct p9_node *dir, struct p9_dirent *ents, int max)
{
    struct p9hostdir *c = vctx;
    char full[PATH_MAX];
    if (snprintf(full, sizeof full, "%s/%s", c->root, dir->handle)
        >= (int)sizeof full)
        return -1;

    DIR *d = opendir(full);
    if (!d)
        return -1;

    int n = 0;
    struct dirent *de;
    while (n < max && (de = readdir(d))) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        char ep[PATH_MAX];
        if (snprintf(ep, sizeof ep, "%s/%s", full, de->d_name)
            >= (int)sizeof ep)
            continue;
        struct stat st;
        if (stat(ep, &st) != 0)
            continue;
        memset(&ents[n], 0, sizeof ents[n]);
        strncpy(ents[n].name, de->d_name, sizeof ents[n].name - 1);
        ents[n].is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
        ents[n].mode = st.st_mode & 0777;
        ents[n].length = ents[n].is_dir ? 0 : (uint64_t)st.st_size;
        ents[n].qid_path = (uint64_t)st.st_ino;
        n++;
    }
    closedir(d);
    return n;
}

static long
hd_readf(void *vctx, const struct p9_node *file, uint64_t off, void *buf,
         uint32_t nbytes)
{
    struct p9hostdir *c = vctx;

    if (c->cur_fd < 0 || strcmp(c->cur_handle, file->handle) != 0) {
        if (c->cur_fd >= 0)
            close(c->cur_fd);
        c->cur_fd = -1;
        c->cur_handle[0] = '\0';
        char full[PATH_MAX];
        if (snprintf(full, sizeof full, "%s/%s", c->root, file->handle)
            >= (int)sizeof full)
            return -1;
        c->cur_fd = open(full, O_RDONLY);
        if (c->cur_fd < 0)
            return -1;
        strncpy(c->cur_handle, file->handle, sizeof c->cur_handle - 1);
        c->cur_handle[sizeof c->cur_handle - 1] = '\0';
    }

    ssize_t r = pread(c->cur_fd, buf, nbytes, (off_t)off);
    return r < 0 ? -1 : r;
}

static const struct p9_backend hostdir_backend = {
    .root = hd_root,
    .walk = hd_walk,
    .list = hd_list,
    .readf = hd_readf,
};

const struct p9_backend *
p9hostdir_open(const char *path, void **ctxout)
{
    char real[PATH_MAX];
    struct stat st;
    if (!realpath(path, real) || stat(real, &st) != 0 || !S_ISDIR(st.st_mode))
        return NULL;

    struct p9hostdir *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    strncpy(c->root, real, sizeof c->root - 1);
    c->root[sizeof c->root - 1] = '\0';
    c->cur_fd = -1;
    *ctxout = c;
    return &hostdir_backend;
}

void
p9hostdir_close(void *ctx)
{
    struct p9hostdir *c = ctx;
    if (!c)
        return;
    if (c->cur_fd >= 0)
        close(c->cur_fd);
    free(c);
}
