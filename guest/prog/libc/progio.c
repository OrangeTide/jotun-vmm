/* progio.c : program-side formatted output, through the service vector.
 *
 * The format engine (fmt.c) is shared with the libOS; only the sink differs. A
 * program has no console primitive of its own, so the sink calls the service
 * vector's putc, the one output path the kernel publishes to programs. */
#include "jlibc.h"

#include "prog.h"

static void
putc_sink(char c, void *arg)
{
    (void)arg;
    sys()->putc(c);
}

int
vprintf(const char *fmt, va_list ap)
{
    return vfctprintf(putc_sink, NULL, fmt, ap);
}

int
printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfctprintf(putc_sink, NULL, fmt, ap);
    va_end(ap);
    return n;
}

int
putchar(int c)
{
    sys()->putc((char)c);
    return c;
}

int
puts(const char *s)
{
    sys()->puts(s);
    sys()->putc('\n');
    return 0;
}
