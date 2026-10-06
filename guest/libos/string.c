/* string.c : freestanding mem and string primitives.
 *
 * Plain byte-wise implementations. They stay call-free (no self-recursion into
 * memcpy/memset) because the guest builds with -fno-builtin and without an
 * optimizer that would rewrite these loops into library calls. If the guest
 * optimization level is ever raised, compile this file with
 * -fno-tree-loop-distribute-patterns (gcc) to keep that guarantee. */
#include "string.h"

void *
memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
}

void *
memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    if (d < s) {
        for (size_t i = 0; i < n; i++)
            d[i] = s[i];
    } else if (d > s) {
        for (size_t i = n; i > 0; i--)
            d[i - 1] = s[i - 1];
    }
    return dst;
}

void *
memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;

    for (size_t i = 0; i < n; i++)
        d[i] = (unsigned char)c;
    return dst;
}

int
memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a;
    const unsigned char *y = b;

    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i])
            return (int)x[i] - (int)y[i];
    return 0;
}

size_t
strlen(const char *s)
{
    size_t n = 0;

    while (s[n] != '\0')
        n++;
    return n;
}

int
strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int
strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];

        if (ca != cb)
            return (int)ca - (int)cb;
        if (ca == '\0')
            break;
    }
    return 0;
}

size_t
strlcpy(char *dst, const char *src, size_t size)
{
    if (src == NULL)
        src = "";

    size_t slen = strlen(src);
    if (size > 0) {
        size_t n = slen < size - 1 ? slen : size - 1;

        for (size_t i = 0; i < n; i++)
            dst[i] = src[i];
        dst[n] = '\0';
    }
    return slen;
}
