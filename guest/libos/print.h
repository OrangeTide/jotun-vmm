/* print.h : portable console helpers built on con_putc.
 *
 * These are arch-neutral, so they live in a shared header as small inlines
 * rather than in any arch file. */
#ifndef LIBOS_PRINT_H
#define LIBOS_PRINT_H

#include <stdint.h>
#include "arch.h"

static inline void
con_puts(const char *s)
{
    while (*s)
        con_putc(*s++);
}

/* Print a 64-bit value in decimal. */
static inline void
con_putu(uint64_t v)
{
    char buf[20];
    int n = 0;

    if (v == 0) {
        con_putc('0');
        return;
    }
    while (v > 0) {
        buf[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    }
    while (n > 0)
        con_putc(buf[--n]);
}

/* Print a 64-bit value as "0x" followed by 16 hex digits. */
static inline void
con_puthex(uint64_t v)
{
    static const char digits[] = "0123456789abcdef";
    con_putc('0');
    con_putc('x');
    for (int shift = 60; shift >= 0; shift -= 4)
        con_putc(digits[(v >> shift) & 0xf]);
}

#endif /* LIBOS_PRINT_H */
