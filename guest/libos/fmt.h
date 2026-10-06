/* fmt.h : a bounded printf-family formatter for the freestanding guest.
 *
 * The guest is one trust domain with no syscalls, so formatted output is never
 * a trap. The engine is a sink: vfctprintf drives a caller-supplied character
 * sink, and everything else is built on it. The libOS points the sink at the
 * console (con_printf in print.h); a bounded buffer sink gives snprintf. The
 * engine never allocates and never writes past what the sink accepts. */
#ifndef LIBOS_FMT_H
#define LIBOS_FMT_H

#include <stdarg.h>
#include <stddef.h>

/* Format into a character sink. out(c, arg) is called for each output byte in
 * order. Returns the length the fully-formatted output would have, whatever the
 * sink does with the bytes, so a bounded sink still reports the untruncated
 * length. */
int vfctprintf(void (*out)(char c, void *arg), void *arg,
               const char *fmt, va_list ap);

/* snprintf semantics: write at most size-1 bytes plus a terminating NUL into
 * dst (nothing at all when size is 0), and return the length the output would
 * have had with no truncation. */
int vsnprintf(char *dst, size_t size, const char *fmt, va_list ap);
int snprintf(char *dst, size_t size, const char *fmt, ...);

#endif /* LIBOS_FMT_H */
