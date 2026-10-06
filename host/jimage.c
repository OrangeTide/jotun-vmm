/* jimage.c : parse the jotun guest-image manifest object. */
#include "jimage.h"

#include <stdlib.h>
#include <string.h>

static void
copy_field(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    for (; src[i] && i < cap - 1; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* Trim leading and trailing blanks in place, returning the first non-blank. */
static char *
trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
    return s;
}

/* Split "arch <token> <hash>" (value already past the key) into one row. */
static void
parse_arch(struct jimage *im, char *val)
{
    if (im->narch >= JIMAGE_MAXARCH)
        return;

    char *tok = val;
    while (*val && *val != ' ' && *val != '\t')
        val++;
    if (*val)
        *val++ = '\0';
    while (*val == ' ' || *val == '\t')
        val++;
    if (!*tok || !*val)
        return;

    struct jimage_arch *a = &im->archs[im->narch++];
    copy_field(a->arch, sizeof a->arch, tok);
    copy_field(a->root, sizeof a->root, val);
}

int
jimage_parse(const char *text, size_t len, struct jimage *im)
{
    char line[512];
    size_t pos = 0;
    int seen_magic = 0;

    memset(im, 0, sizeof *im);

    while (pos < len) {
        /* Copy one line (up to the newline) into a bounded, NUL-terminated
         * buffer. Overlong lines are truncated; no field we read is that long. */
        size_t j = 0;
        while (pos < len && text[pos] != '\n') {
            if (j < sizeof line - 1)
                line[j++] = text[pos];
            pos++;
        }
        if (pos < len)
            pos++;                      /* step over the newline */
        line[j] = '\0';

        /* A folded continuation (leading whitespace) belongs to the previous
         * field, which this host does not retain, so drop it. The magic line is
         * never folded. */
        if (line[0] == ' ' || line[0] == '\t')
            continue;

        char *s = trim(line);
        if (*s == '\0')
            continue;                   /* blank line */

        /* Split the key from its value. */
        char *key = s;
        char *val = s;
        while (*val && *val != ' ' && *val != '\t')
            val++;
        if (*val)
            *val++ = '\0';
        while (*val == ' ' || *val == '\t')
            val++;

        if (!seen_magic) {
            if (strcmp(key, JIMAGE_MAGIC) != 0)
                return -1;
            im->version = (int)strtol(val, NULL, 10);
            if (im->version != JIMAGE_VERSION)
                return -1;
            seen_magic = 1;
            continue;
        }

        if (strcmp(key, "arch") == 0)
            parse_arch(im, val);
        else if (strcmp(key, "name") == 0)
            copy_field(im->name, sizeof im->name, val);
        else if (strcmp(key, "version") == 0)
            copy_field(im->ver, sizeof im->ver, val);
        /* other keys (upstream, note, ...) are informational and ignored */
    }

    return seen_magic ? 0 : -1;
}

const char *
jimage_root_for(const struct jimage *im, const char *arch)
{
    for (int i = 0; i < im->narch; i++)
        if (strcmp(im->archs[i].arch, arch) == 0)
            return im->archs[i].root;
    return NULL;
}
