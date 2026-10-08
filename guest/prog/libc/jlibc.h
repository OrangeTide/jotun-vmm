/* jlibc.h : the small C library linked into bundle programs.
 *
 * A bundle program is a standalone freestanding ELF that reaches the libOS only
 * through the service vector (prog.h, struct jotun_svc). Rather than grow that
 * ABI with every convenience, the common pieces are compiled into a small
 * library and linked into each program: the formatter and string primitives
 * shared with the libOS, plus program-side printf that routes output through
 * the service vector's putc, and numeric parsing. Not a full libc. */
#ifndef JOTUN_PROG_LIBC_H
#define JOTUN_PROG_LIBC_H

#include <stdarg.h>
#include <stddef.h>

#include "fmt.h"        /* snprintf, vsnprintf, vfctprintf */
#include "string.h"     /* mem{cpy,move,set,cmp}, str{len,cmp,ncmp}, strlcpy */

/* Formatted output to the console, routed through the service vector's putc.
 * Unbuffered, so there is no length cap; each returns the byte count written. */
int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int putchar(int c);
int puts(const char *s);       /* writes s then a newline, like C puts */

/* Numeric parsing. strtol honours leading space, an optional sign, a 0x/0
 * prefix for base 0 or 16, bases 2..36, and clamps overflow to LONG_MIN or
 * LONG_MAX with end left past the consumed digits. */
long strtol(const char *s, char **end, int base);
int atoi(const char *s);

/* Dynamic memory, over a reserved region in the program's own image (jsbrk.c).
 * Standard semantics; malloc/calloc/realloc return NULL when the heap is
 * exhausted. jmalloc_live/jmalloc_peak report the current and high-water bytes
 * handed out, for leak hunting in tests. */
void *malloc(size_t nbytes);
void free(void *ap);
void *calloc(size_t n, size_t size);
void *realloc(void *ap, size_t nbytes);
size_t jmalloc_live(void);
size_t jmalloc_peak(void);

#endif /* JOTUN_PROG_LIBC_H */
