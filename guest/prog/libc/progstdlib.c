/* progstdlib.c : small numeric parsing for bundle programs. */
#include "jlibc.h"

#include <limits.h>

static int
is_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v'
        || c == '\f';
}

long
strtol(const char *s, char **end, int base)
{
    const char *p = s;

    while (is_space((unsigned char)*p))
        p++;

    int neg = 0;
    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }

    if ((base == 0 || base == 16) && p[0] == '0'
        && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    } else if (base == 0 && p[0] == '0') {
        base = 8;               /* leave p at the '0', consumed as a digit */
    } else if (base == 0) {
        base = 10;
    }

    /* The largest magnitude the result can hold: LONG_MAX, or one more when
     * negative so LONG_MIN is representable. */
    unsigned long cutoff = neg ? (unsigned long)LONG_MAX + 1UL
                               : (unsigned long)LONG_MAX;
    unsigned long acc = 0;
    int any = 0, overflow = 0;

    for (;; p++) {
        int c = (unsigned char)*p;
        int d;

        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'z')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z')
            d = c - 'A' + 10;
        else
            break;
        if (d >= base)
            break;

        any = 1;
        if (!overflow) {
            if (acc > (cutoff - (unsigned long)d) / (unsigned long)base)
                overflow = 1;
            else
                acc = acc * (unsigned long)base + (unsigned long)d;
        }
    }

    if (end)
        *end = (char *)(any ? p : s);
    if (overflow)
        return neg ? LONG_MIN : LONG_MAX;
    if (neg)
        return (acc == (unsigned long)LONG_MAX + 1UL) ? LONG_MIN : -(long)acc;
    return (long)acc;
}

int
atoi(const char *s)
{
    return (int)strtol(s, NULL, 10);
}
