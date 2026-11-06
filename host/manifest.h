/* manifest.h : the per-guest manifest the host reads at launch.
 *
 * The manifest is the sandbox's control surface. It names the bundle and the
 * ref to serve, the subtree within it the guest may see, and the init program
 * and arguments. Network and display rights join it in later milestones. */
#ifndef MANIFEST_H
#define MANIFEST_H

#define MANIFEST_MAXARG 16
#define MANIFEST_MAXNET 16
#define MANIFEST_MAXMOUNT 8

/* One "net allow <cidr> <ports> [proto]" rule, kept as unparsed strings; the
 * host turns these into the numeric net policy. */
struct manifest_net {
    char cidr[64];                      /* "a.b.c.d" or "a.b.c.d/prefix" */
    char ports[32];                     /* "p", "lo-hi", or "*" */
    char proto[8];                      /* "tcp", "udp", or "" for any */
};

/* One "mount <name> <bundle> <ref> [subtree]" graft: a shared read-only tree
 * that appears as the top-level directory `name` in the guest root. Several may
 * be declared; the host chains them, so each adds one top-level name. */
struct manifest_mount {
    char name[64];                      /* top-level name in the guest root */
    char bundle[256];                   /* depot path to serve */
    char ref[64];                       /* ref name, default "root" */
    char subtree[256];                  /* subtree within that bundle, or "" */
};

struct manifest {
    char bundle[256];                   /* depot path */
    char origin[256];                   /* http origin to pull from, or "" */
    char key[128];                      /* trusted publisher key (hex), or "" */
    char label[64];                     /* topic label, or "" for the bare topic */
    char ref[64];                       /* ref name, default "root" */
    char subtree[256];                  /* guest root within the bundle */
    char name[64];                      /* this guest's network name, or "" */
    int  argc;
    char argv[MANIFEST_MAXARG][64];     /* init program and arguments */
    int  nnet;
    struct manifest_net net[MANIFEST_MAXNET];   /* network-allow rules */

    /* Read-only graft mounts: shared trees that appear as top-level directories
     * in the guest root, chained so each adds one name. nmount is 0 when none
     * are declared. */
    int  nmount;
    struct manifest_mount mount[MANIFEST_MAXMOUNT];
};

/* Parse the manifest file at path. Returns 0 on success, -1 on error. */
int manifest_load(const char *path, struct manifest *m);

#endif /* MANIFEST_H */
