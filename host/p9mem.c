/* p9mem.c : an in-memory 9P backend, for bringing up the 9P path.
 *
 * A tiny fixed tree: a root directory holding a couple of text files. It lets
 * the transport, the protocol server, and the guest client be exercised end to
 * end before the smolvfs-backed backend replaces it. */
#include "p9mem.h"

#include <string.h>

struct memfile {
    const char *name;
    const char *data;
};

static const struct memfile files[] = {
    { "hello.txt", "hello from the 9P server\n" },
    { "motd",      "jotun guest filesystem is live\n" },
};
#define NFILES ((int)(sizeof files / sizeof files[0]))

/* Node handles: "/" is the root directory; a file's handle is its name. The
 * qid path is a small fixed id (0 for the root, index+1 for files). */

static int
mem_root(void *ctx, struct p9_node *out)
{
    (void)ctx;
    memset(out, 0, sizeof *out);
    out->is_dir = 1;
    out->mode = 0555;
    out->qid_path = 0;
    strcpy(out->handle, "/");
    strcpy(out->name, "/");
    return 0;
}

static int
mem_walk(void *ctx, const struct p9_node *dir, const char *name,
         struct p9_node *out)
{
    (void)ctx;
    if (!dir->is_dir)
        return -1;
    for (int i = 0; i < NFILES; i++) {
        if (strcmp(name, files[i].name) == 0) {
            memset(out, 0, sizeof *out);
            out->is_dir = 0;
            out->mode = 0444;
            out->length = strlen(files[i].data);
            out->qid_path = (uint64_t)(i + 1);
            strncpy(out->handle, files[i].name, sizeof out->handle - 1);
            strncpy(out->name, files[i].name, sizeof out->name - 1);
            return 0;
        }
    }
    return -1;
}

static int
mem_list(void *ctx, const struct p9_node *dir, struct p9_dirent *ents, int max)
{
    (void)ctx;
    if (!dir->is_dir)
        return -1;
    int n = 0;
    for (int i = 0; i < NFILES && n < max; i++) {
        memset(&ents[n], 0, sizeof ents[n]);
        strncpy(ents[n].name, files[i].name, sizeof ents[n].name - 1);
        ents[n].is_dir = 0;
        ents[n].mode = 0444;
        ents[n].length = strlen(files[i].data);
        ents[n].qid_path = (uint64_t)(i + 1);
        n++;
    }
    return n;
}

static long
mem_readf(void *ctx, const struct p9_node *file, uint64_t off, void *buf,
          uint32_t n)
{
    (void)ctx;
    for (int i = 0; i < NFILES; i++) {
        if (strcmp(file->handle, files[i].name) == 0) {
            uint64_t len = strlen(files[i].data);
            if (off >= len)
                return 0;
            uint64_t avail = len - off;
            if (n > avail)
                n = (uint32_t)avail;
            memcpy(buf, files[i].data + off, n);
            return (long)n;
        }
    }
    return -1;
}

static const struct p9_backend mem_backend = {
    .root = mem_root,
    .walk = mem_walk,
    .list = mem_list,
    .readf = mem_readf,
};

const struct p9_backend *
p9mem_backend(void)
{
    return &mem_backend;
}
