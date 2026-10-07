/* print.h : portable console helpers built on con_putc.
 *
 * These are arch-neutral, so they live in a shared header as small inlines
 * rather than in any arch file. */
#ifndef LIBOS_PRINT_H
#define LIBOS_PRINT_H

#include <stdarg.h>
#include <stdint.h>
#include "arch.h"
#include "fmt.h"

/* Write one byte to the console: always to the arch raw sink, and also to a
 * registered sink, if any. Defined in print.c. */
void con_putc(char c);

/* Register (or clear, with NULL) a sink that receives a copy of every console
 * byte. A guest terminal uses it to mirror console output on screen. */
void con_set_sink(void (*fn)(char));

static inline void
con_puts(const char *s)
{
    while (*s)
        con_putc(*s++);
}

/* The console as a format sink: one byte at a time to con_putc. */
static inline void
con_sink(char c, void *arg)
{
    (void)arg;
    con_putc(c);
}

/* Formatted console output. Nothing is buffered, so there is no length cap;
 * each byte goes straight to con_putc. Returns the number of bytes written. */
static inline int
con_vprintf(const char *fmt, va_list ap)
{
    return vfctprintf(con_sink, NULL, fmt, ap);
}

static inline int
con_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfctprintf(con_sink, NULL, fmt, ap);
    va_end(ap);
    return n;
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
