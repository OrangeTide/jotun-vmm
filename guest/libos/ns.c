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

/* Split a slash-separated path into components; returns the count. */
static int
split_path(const char *path, char comps[][NS_NAMELEN], int maxc)
{
    int n = 0;
    const char *p = path;

    while (*p && n < maxc) {
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
        n++;
    }
    return n;
}

int
ns_walk(const char *path, uint32_t fid)
{
    char eff[128];
    char comps[P9_MAXWELEM][NS_NAMELEN];
    const char *names[P9_MAXWELEM];

    ns_resolve(path, eff, sizeof eff);
    int n = split_path(eff, comps, P9_MAXWELEM);
    for (int i = 0; i < n; i++)
        names[i] = comps[i];
    return (p9_walk(P9_ROOT_FID, fid, n, names) == n) ? 0 : -1;
}
