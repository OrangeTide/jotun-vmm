/* ns.c : namespace resolution over the single 9P tree.
 *
 * Each mount maps a mountpoint prefix to a source subpath of the server tree.
 * Resolving a path picks the longest matching mountpoint, replaces it with the
 * source subpath, and walks the result from the 9P root. */
#include "ns.h"

#include "p9.h"
#include "string.h"

#define NS_MAX      16
#define NS_NAMELEN  64

struct ns_ent {
    char at[NS_NAMELEN];
    char src[NS_NAMELEN];
};

static struct ns_ent ents[NS_MAX];
static int nents;

void
ns_init(void)
{
    strlcpy(ents[0].at, "/", NS_NAMELEN);
    ents[0].src[0] = '\0';
    nents = 1;
}

int
ns_mount(const char *at, const char *src)
{
    if (nents >= NS_MAX)
        return -1;
    strlcpy(ents[nents].at, at, NS_NAMELEN);
    strlcpy(ents[nents].src, src, NS_NAMELEN);
    nents++;
    return 0;
}

int
ns_bind(const char *src, const char *at)
{
    return ns_mount(at, src);
}

/* Is at a path-boundary prefix of path? "/" matches everything. */
static int
is_prefix(const char *at, const char *path)
{
    int la = strlen(at);

    if (la == 1 && at[0] == '/')
        return 1;
    for (int i = 0; i < la; i++)
        if (at[i] != path[i])
            return 0;
    return path[la] == '\0' || path[la] == '/';
}

/* Rewrite path to its effective server path through the longest mount. */
static void
ns_resolve(const char *path, char *out, int cap)
{
    int best = -1, bestlen = -1;

    for (int i = 0; i < nents; i++) {
        if (is_prefix(ents[i].at, path)) {
            int l = strlen(ents[i].at);
            if (l > bestlen) {
                bestlen = l;
                best = i;
            }
        }
    }
    if (best < 0) {
        strlcpy(out, path, cap);
        return;
    }

    const char *rem;
    if (bestlen == 1 && ents[best].at[0] == '/')
        rem = (path[0] == '/') ? path + 1 : path;
    else {
        rem = path + bestlen;
        if (*rem == '/')
            rem++;
    }

    int n = 0;
    const char *src = ents[best].src;
    for (int i = 0; src[i] && n < cap - 1; i++)
        out[n++] = src[i];
    if (*rem) {
        if (n > 0 && out[n - 1] != '/' && n < cap - 1)
            out[n++] = '/';
        while (*rem && n < cap - 1)
            out[n++] = *rem++;
    }
    out[n] = '\0';
}

/* Walk a server path into `fid`. A 9P Twalk carries at most P9_MAXWELEM (16)
 * path elements, so a deeper path (a build tree reaches 17+: .deps mirrors the
 * absolute object path) is walked in several messages, each continuing from the
 * fid the previous one established by walking it in place (the server reuses the
 * same fid slot when newfid == fid). Without this, a path past 16 components
 * walked only its first 16 and stopped at an interior directory, so a create
 * under it reported EISDIR. Components are taken 16 at a time, reusing one
 * buffer rather than storing the whole path, so the stack stays small. A ".."
 * that pops across a chunk boundary is not modelled; callers pass keys already
 * normalized (resolve_path/norm_key collapse "." and ".."), so this does not
 * arise for real paths. */
int
ns_walk(const char *path, uint32_t fid)
{
    char eff[256];
    char comps[P9_MAXWELEM][NS_NAMELEN];
    const char *names[P9_MAXWELEM];

    ns_resolve(path, eff, sizeof eff);

    uint32_t base = P9_ROOT_FID;
    int any = 0;
    const char *p = eff;
    for (;;) {
        int n = 0;
        while (*p && n < P9_MAXWELEM) {
            while (*p == '/')
                p++;
            if (!*p)
                break;
            int i = 0;
            while (*p && *p != '/') {
                if (i < NS_NAMELEN - 1)
                    comps[n][i++] = *p;
                p++;
            }
            comps[n][i] = '\0';
            if (comps[n][0] == '.' && comps[n][1] == '\0')
                continue;                       /* "." : current directory */
            if (comps[n][0] == '.' && comps[n][1] == '.' && comps[n][2] == '\0') {
                if (n > 0)                      /* ".." within this chunk */
                    n--;
                continue;
            }
            names[n] = comps[n];
            n++;
        }
        if (n == 0)
            break;                              /* no (more) components */
        if (p9_walk(base, fid, n, names) != n)
            return -1;                          /* a component was not found */
        base = fid;                             /* continue in place from here */
        any = 1;
    }
    if (!any)                                   /* empty path: clone the root */
        return p9_walk(P9_ROOT_FID, fid, 0, names) == 0 ? 0 : -1;
    return 0;
}
