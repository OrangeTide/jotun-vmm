/* manifest.h : the per-guest manifest the host reads at launch.
 *
 * The manifest is the sandbox's control surface. It names the bundle and the
 * ref to serve, the subtree within it the guest may see, and the init program
 * and arguments. Network and display rights join it in later milestones. */
#ifndef MANIFEST_H
#define MANIFEST_H

#define MANIFEST_MAXARG 16

struct manifest {
    char bundle[256];                   /* depot path */
    char ref[64];                       /* ref name, default "root" */
    char subtree[256];                  /* guest root within the bundle */
    int  argc;
    char argv[MANIFEST_MAXARG][64];     /* init program and arguments */
};

/* Parse the manifest file at path. Returns 0 on success, -1 on error. */
int manifest_load(const char *path, struct manifest *m);

#endif /* MANIFEST_H */
