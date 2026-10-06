/* manifest.c : parse the per-guest manifest.
 *
 * One directive per line: "key value". Blank lines and lines beginning with
 * '#' are ignored. Keys: bundle, ref, subtree, and init (whose value is the
 * whole argument vector). Unknown keys are ignored so the format can grow. */
#include "manifest.h"

#include <stdio.h>
#include <string.h>

static void
copy_field(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    for (; src[i] && i < cap - 1; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* Trim leading whitespace and a trailing newline in place. */
static char *
trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
    return s;
}

int
manifest_load(const char *path, struct manifest *m)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    memset(m, 0, sizeof *m);
    copy_field(m->ref, sizeof m->ref, "root");

    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (*s == '\0' || *s == '#')
            continue;

        /* Split off the key. */
        char *key = s;
        char *val = s;
        while (*val && *val != ' ' && *val != '\t')
            val++;
        if (*val)
            *val++ = '\0';
        while (*val == ' ' || *val == '\t')
            val++;

        if (strcmp(key, "net") == 0) {
            /* "net allow <cidr> <ports> [proto]". Tokenize the value. */
            char *tok[4];
            int nt = 0;
            char *p = val;
            while (*p && nt < 4) {
                while (*p == ' ' || *p == '\t')
                    p++;
                if (!*p)
                    break;
                tok[nt++] = p;
                while (*p && *p != ' ' && *p != '\t')
                    p++;
                if (*p)
                    *p++ = '\0';
            }
            if (nt >= 3 && strcmp(tok[0], "allow") == 0
                && m->nnet < MANIFEST_MAXNET) {
                struct manifest_net *r = &m->net[m->nnet++];
                copy_field(r->cidr, sizeof r->cidr, tok[1]);
                copy_field(r->ports, sizeof r->ports, tok[2]);
                copy_field(r->proto, sizeof r->proto, nt >= 4 ? tok[3] : "");
            }
        } else if (strcmp(key, "bundle") == 0) {
            copy_field(m->bundle, sizeof m->bundle, val);
        } else if (strcmp(key, "ref") == 0) {
            copy_field(m->ref, sizeof m->ref, val);
        } else if (strcmp(key, "subtree") == 0) {
            copy_field(m->subtree, sizeof m->subtree, val);
        } else if (strcmp(key, "init") == 0) {
            /* Tokenize the rest into the argument vector. */
            char *p = val;
            while (*p && m->argc < MANIFEST_MAXARG) {
                while (*p == ' ' || *p == '\t')
                    p++;
                if (!*p)
                    break;
                char *tok = p;
                while (*p && *p != ' ' && *p != '\t')
                    p++;
                if (*p)
                    *p++ = '\0';
                copy_field(m->argv[m->argc], sizeof m->argv[0], tok);
                m->argc++;
            }
        }
    }

    fclose(f);
    return m->bundle[0] ? 0 : -1;
}
