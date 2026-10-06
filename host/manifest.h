/* manifest.h : the per-guest manifest the host reads at launch.
 *
 * The manifest is the sandbox's control surface. It names the bundle and the
 * ref to serve, the subtree within it the guest may see, and the init program
 * and arguments. Network and display rights join it in later milestones. */
#ifndef MANIFEST_H
#define MANIFEST_H

#define MANIFEST_MAXARG 16
#define MANIFEST_MAXNET 16

/* One "net allow <cidr> <ports> [proto]" rule, kept as unparsed strings; the
 * host turns these into the numeric net policy. */
struct manifest_net {
    char cidr[64];                      /* "a.b.c.d" or "a.b.c.d/prefix" */
    char ports[32];                     /* "p", "lo-hi", or "*" */
    char proto[8];                      /* "tcp", "udp", or "" for any */
};

struct manifest {
    char bundle[256];                   /* depot path */
    char origin[256];                   /* http origin to pull from, or "" */
    char key[128];                      /* trusted publisher key (hex), or "" */
    char label[64];                     /* topic label, or "" for the bare topic */
    char ref[64];                       /* ref name, default "root" */
    char subtree[256];                  /* guest root within the bundle */
    int  argc;
    char argv[MANIFEST_MAXARG][64];     /* init program and arguments */
    int  nnet;
    struct manifest_net net[MANIFEST_MAXNET];   /* network-allow rules */

    /* An optional read-only graft mount: a shared tree that appears as the
     * top-level directory mount_name in the guest root. mount_name is "" when
     * no mount is declared. */
    char mount_name[64];
    char mount_bundle[256];
    char mount_ref[64];
    char mount_subtree[256];
};

/* Parse the manifest file at path. Returns 0 on success, -1 on error. */
int manifest_load(const char *path, struct manifest *m);

#endif /* MANIFEST_H */
