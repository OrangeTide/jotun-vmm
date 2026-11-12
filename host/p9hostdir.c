/* p9hostdir.c : a 9P backend over a live host directory.
 *
 * Read-only by default. A writable mount also serves create, write, truncate,
 * and remove, so a guest's build output lands on the host filesystem instead of
 * guest RAM. The same realpath confinement bounds every op to the mount root. */

#include "p9hostdir.h"

#include "jotun_9p.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct p9hostdir {
    char root[PATH_MAX];                /* canonical host directory root */
    int  writable;                      /* serve create/write/truncate/remove */
    /* A one-entry read cache: a library is read sequentially, so keeping the
     * last file open turns a run of reads into a single open. */
    char cur_handle[P9_HANDLE_MAX];
    int  cur_fd;
    /* A one-entry write cache, the write counterpart: a build tool writes a
     * file in a run of small chunks, so keeping it open avoids an open per
     * Twrite. */
    char cur_whandle[P9_HANDLE_MAX];
    int  cur_wfd;
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
    out->mtime = (uint32_t)st->st_mtime;        /* real host mtime, Unix seconds */
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

    char rel[P9_HANDLE_MAX];
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
hd_list(void *vctx, const struct p9_node *dir, p9_emit_fn emit, void *ectx)
{
    struct p9hostdir *c = vctx;
    char full[PATH_MAX];
    if (snprintf(full, sizeof full, "%s/%s", c->root, dir->handle)
        >= (int)sizeof full)
        return -1;

    DIR *d = opendir(full);
    if (!d)
        return -1;

    struct dirent *de;
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        char ep[PATH_MAX];
        if (snprintf(ep, sizeof ep, "%s/%s", full, de->d_name)
            >= (int)sizeof ep)
            continue;
        struct stat st;
        if (stat(ep, &st) != 0)
            continue;
        struct p9_dirent e;
        memset(&e, 0, sizeof e);
        strncpy(e.name, de->d_name, sizeof e.name - 1);
        e.is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
        e.mode = st.st_mode & 0777;
        e.length = e.is_dir ? 0 : (uint64_t)st.st_size;
        e.qid_path = (uint64_t)st.st_ino;
        e.mtime = (uint32_t)st.st_mtime;
        if (emit(ectx, &e) != 0)
            break;
    }
    closedir(d);
    return 0;
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

/* Build root/<handle> into full. Returns 0, or -1 if it does not fit. */
static int
host_path(const struct p9hostdir *c, const char *handle, char *full,
          size_t cap)
{
    int n = handle[0] ? snprintf(full, cap, "%s/%s", c->root, handle)
                      : snprintf(full, cap, "%s", c->root);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

/* Drop a cached fd (read or write) that names this handle, so a later read or
 * stat does not see a stale descriptor after the file changed or went away. */
static void
invalidate(struct p9hostdir *c, const char *handle)
{
    if (c->cur_fd >= 0 && strcmp(c->cur_handle, handle) == 0) {
        close(c->cur_fd);
        c->cur_fd = -1;
        c->cur_handle[0] = '\0';
    }
    if (c->cur_wfd >= 0 && strcmp(c->cur_whandle, handle) == 0) {
        close(c->cur_wfd);
        c->cur_wfd = -1;
        c->cur_whandle[0] = '\0';
    }
}

static long
hd_writef(void *vctx, const struct p9_node *file, uint64_t off, const void *buf,
          uint32_t nbytes)
{
    struct p9hostdir *c = vctx;
    if (!c->writable)
        return -1;

    if (c->cur_wfd < 0 || strcmp(c->cur_whandle, file->handle) != 0) {
        if (c->cur_wfd >= 0)
            close(c->cur_wfd);
        c->cur_wfd = -1;
        c->cur_whandle[0] = '\0';
        char full[PATH_MAX];
        if (host_path(c, file->handle, full, sizeof full) != 0)
            return -1;
        c->cur_wfd = open(full, O_WRONLY);
        if (c->cur_wfd < 0)
            return -1;
        strncpy(c->cur_whandle, file->handle, sizeof c->cur_whandle - 1);
        c->cur_whandle[sizeof c->cur_whandle - 1] = '\0';
    }

    ssize_t w = pwrite(c->cur_wfd, buf, nbytes, (off_t)off);
    return w < 0 ? -1 : w;
}

static int
hd_create(void *vctx, const struct p9_node *dir, const char *name,
          uint32_t perm, struct p9_node *out)
{
    struct p9hostdir *c = vctx;
    if (!c->writable || !valid_name(name))
        return -1;

    /* A single leaf name joined onto an already-confined directory handle stays
     * within the root (valid_name rejects "/", "." and ".."), so the new path
     * needs no realpath (the target may not exist yet). */
    char rel[P9_HANDLE_MAX];
    int rl = dir->handle[0]
        ? snprintf(rel, sizeof rel, "%s/%s", dir->handle, name)
        : snprintf(rel, sizeof rel, "%s", name);
    if (rl < 0 || rl >= (int)sizeof rel || (size_t)rl >= sizeof out->handle)
        return -1;

    char full[PATH_MAX];
    if (host_path(c, rel, full, sizeof full) != 0)
        return -1;

    unsigned mode = perm & 0777u;
    if (perm & P9_DMDIR) {
        if (mkdir(full, mode ? mode : 0755) != 0 && errno != EEXIST)
            return -1;
    } else {
        int fd = open(full, O_WRONLY | O_CREAT | O_TRUNC, mode ? mode : 0644);
        if (fd < 0)
            return -1;
        close(fd);
    }

    struct stat st;
    if (stat(full, &st) != 0)
        return -1;
    fill_node(out, rel, name, &st);
    return 0;
}

static int
hd_truncate(void *vctx, const struct p9_node *file, uint64_t len)
{
    struct p9hostdir *c = vctx;
    if (!c->writable)
        return -1;
    char full[PATH_MAX];
    if (host_path(c, file->handle, full, sizeof full) != 0)
        return -1;
    invalidate(c, file->handle);
    return truncate(full, (off_t)len) == 0 ? 0 : -1;
}

/* Open a real host descriptor for a file, which the server holds for the fid's
 * lifetime (reads/writes go through it with pread/pwrite). Holding the fd is
 * what gives open-unlink semantics: once it is open, removing the name leaves
 * the inode reachable through this descriptor until it is closed. The handle was
 * confined to the root at walk time, so it needs no further realpath here. */
static int
hd_openh(void *vctx, const struct p9_node *file, int for_write)
{
    struct p9hostdir *c = vctx;
    if (file->is_dir || (for_write && !c->writable))
        return -1;
    char full[PATH_MAX];
    if (host_path(c, file->handle, full, sizeof full) != 0)
        return -1;
    return open(full, for_write ? O_RDWR : O_RDONLY);
}

static int
hd_removef(void *vctx, const struct p9_node *node)
{
    struct p9hostdir *c = vctx;
    if (!c->writable)
        return -1;
    char full[PATH_MAX];
    if (host_path(c, node->handle, full, sizeof full) != 0)
        return -1;
    invalidate(c, node->handle);
    if (node->is_dir)
        return rmdir(full) == 0 ? 0 : -1;
    return unlink(full) == 0 ? 0 : -1;
}

static const struct p9_backend hostdir_backend = {
    .root = hd_root,
    .walk = hd_walk,
    .list = hd_list,
    .readf = hd_readf,
    .writef = hd_writef,
    .create = hd_create,
    .truncate = hd_truncate,
    .removef = hd_removef,
    .openh = hd_openh,
};

const struct p9_backend *
p9hostdir_open(const char *path, int writable, void **ctxout)
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
    c->writable = writable;
    c->cur_fd = -1;
    c->cur_wfd = -1;
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
    if (c->cur_wfd >= 0)
        close(c->cur_wfd);
    free(c);
}
