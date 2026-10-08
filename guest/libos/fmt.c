/* fmt.c : the bounded printf-family format engine.
 *
 * One parse pass over the format string. Each conversion is rendered into a
 * small local digit buffer, then emitted through the sink with the requested
 * sign, prefix, precision zeros, and field padding. Supports the flags
 * '-', '0', '+', ' ', '#'; a numeric or '*' width and precision; the length
 * modifiers h, hh, l, ll, z, t, j, L; and the conversions d i u o x X b c s p %.
 * The floating-point conversions f F e E g G are handled by fmtfloat.c, which
 * is built with the FPU enabled. No allocation and no unbounded temporaries. */
#include "fmt.h"
#include "fmtfloat.h"

#include <stdint.h>

/* struct outctx and the F_* flag bits are shared with fmtfloat.c, in fmtfloat.h
 * (the floating-point conversions live there so they can be built with the FPU
 * on). emit() is the local spelling of fmt_emit for this file's call sites. */
static void
emit(struct outctx *o, char c)
{
    fmt_emit(o, c);
}

/* Length modifiers. The guest is LP64, so long and size_t are both 64-bit. */
enum { LEN_INT, LEN_LONG, LEN_LLONG, LEN_SIZE };

static int
read_uint(const char **p)
{
    int n = 0;

    while (**p >= '0' && **p <= '9') {
        n = n * 10 + (**p - '0');
        (*p)++;
    }
    return n;
}

/* Render v in base into buf (not terminated), most-significant digit first.
 * Returns the digit count, which is at least one. */
static int
utoa(unsigned long long v, unsigned base, int upper, char *buf)
{
    static const char lower[] = "0123456789abcdef";
    static const char upperd[] = "0123456789ABCDEF";
    const char *d = upper ? upperd : lower;
    char tmp[64];
    int n = 0;

    do {
        tmp[n++] = d[v % base];
        v /= base;
    } while (v);

    for (int i = 0; i < n; i++)
        buf[i] = tmp[n - 1 - i];
    return n;
}

/* Emit a rendered integer with its sign, prefix, precision zeros, and field
 * padding. digits/ndig is the bare number; sign is 0 or one of '+', '-', ' ';
 * prefix is "", "0x", "0X", or "0b". */
static void
emit_int(struct outctx *o, const char *digits, int ndig, int flags, int width,
         int prec, const char *prefix, int preflen, char sign)
{
    int zeros = (prec > ndig) ? prec - ndig : 0;
    int body = (sign ? 1 : 0) + preflen + zeros + ndig;
    int pad = (width > body) ? width - body : 0;
    int zeropad = 0;

    /* The '0' flag zero-fills the field, but an explicit precision overrides
     * it for integers, and left-justify always uses spaces on the right. */
    if ((flags & F_ZERO) && !(flags & F_LEFT) && prec < 0) {
        zeropad = pad;
        pad = 0;
    }

    if (!(flags & F_LEFT))
        while (pad-- > 0)
            emit(o, ' ');
    if (sign)
        emit(o, sign);
    for (int i = 0; i < preflen; i++)
        emit(o, prefix[i]);
    while (zeropad-- > 0)
        emit(o, '0');
    while (zeros-- > 0)
        emit(o, '0');
    for (int i = 0; i < ndig; i++)
        emit(o, digits[i]);
    if (flags & F_LEFT)
        while (pad-- > 0)
            emit(o, ' ');
}

static void
emit_str(struct outctx *o, const char *s, int flags, int width, int prec)
{
    int len = 0;

    if (s == NULL)
        s = "(null)";
    while (s[len] != '\0' && (prec < 0 || len < prec))
        len++;

    int pad = (width > len) ? width - len : 0;

    if (!(flags & F_LEFT))
        while (pad-- > 0)
            emit(o, ' ');
    for (int i = 0; i < len; i++)
        emit(o, s[i]);
    if (flags & F_LEFT)
        while (pad-- > 0)
            emit(o, ' ');
}

int
vfctprintf(void (*out)(char c, void *arg), void *arg, const char *fmt,
           va_list ap0)
{
    struct outctx o = { out, arg, 0 };

    /* Work from a local va_list so its address has type va_list *, which the
     * float helper needs to advance it. Taking &ap0 would not: a va_list
     * parameter has already decayed where va_list is an array type. */
    va_list ap;
    va_copy(ap, ap0);

    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            emit(&o, *p);
            continue;
        }
        p++;                        /* past '%' */

        /* Flags. */
        int flags = 0;
        for (;; p++) {
            if (*p == '-')      flags |= F_LEFT;
            else if (*p == '0') flags |= F_ZERO;
            else if (*p == '+') flags |= F_PLUS;
            else if (*p == ' ') flags |= F_SPACE;
            else if (*p == '#') flags |= F_ALT;
            else                break;
        }

        /* Width: a number, or '*' to take it from the argument list. A
         * negative '*' width means left-justify with its magnitude. */
        int width = 0;
        if (*p == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                flags |= F_LEFT;
                width = -width;
            }
            p++;
        } else {
            width = read_uint(&p);
        }

        /* Precision: '.' then a number or '*'. A negative '*' precision is
         * taken as absent. */
        int prec = -1;
        if (*p == '.') {
            p++;
            if (*p == '*') {
                prec = va_arg(ap, int);
                p++;
            } else {
                prec = read_uint(&p);
            }
            if (prec < 0)
                prec = -1;
        }

        /* Length modifier. */
        int len = LEN_INT;
        if (*p == 'l') {
            p++;
            if (*p == 'l') { len = LEN_LLONG; p++; }
            else           { len = LEN_LONG; }
        } else if (*p == 'h') {
            p++;
            if (*p == 'h') p++;     /* hh: still read as int */
        } else if (*p == 'z' || *p == 't') {
            len = LEN_SIZE;
            p++;
        } else if (*p == 'j') {
            len = LEN_LLONG;
            p++;
        } else if (*p == 'L') {
            p++;                    /* long double: rendered as double */
        }

        char digits[64];
        int ndig;

        switch (*p) {
        case 'd':
        case 'i': {
            long long sv;
            switch (len) {
            case LEN_LLONG: sv = va_arg(ap, long long); break;
            case LEN_LONG:  sv = va_arg(ap, long); break;
            case LEN_SIZE:  sv = (long long)va_arg(ap, size_t); break;
            default:        sv = va_arg(ap, int); break;
            }
            unsigned long long uv;
            char sign = 0;
            if (sv < 0) {
                uv = (unsigned long long)(-(sv + 1)) + 1ull;    /* no UB at MIN */
                sign = '-';
            } else {
                uv = (unsigned long long)sv;
                if (flags & F_PLUS)       sign = '+';
                else if (flags & F_SPACE) sign = ' ';
            }
            ndig = utoa(uv, 10, 0, digits);
            if (prec == 0 && uv == 0)
                ndig = 0;
            emit_int(&o, digits, ndig, flags, width, prec, "", 0, sign);
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X':
        case 'b': {
            unsigned long long uv;
            switch (len) {
            case LEN_LLONG: uv = va_arg(ap, unsigned long long); break;
            case LEN_LONG:  uv = va_arg(ap, unsigned long); break;
            case LEN_SIZE:  uv = va_arg(ap, size_t); break;
            default:        uv = va_arg(ap, unsigned int); break;
            }
            unsigned base = 10;
            int upper = 0;
            const char *prefix = "";
            int preflen = 0;
            if (*p == 'o') {
                base = 8;
            } else if (*p == 'x' || *p == 'X') {
                base = 16;
                upper = (*p == 'X');
                if ((flags & F_ALT) && uv != 0) {
                    prefix = upper ? "0X" : "0x";
                    preflen = 2;
                }
            } else if (*p == 'b') {
                base = 2;
                if ((flags & F_ALT) && uv != 0) {
                    prefix = "0b";
                    preflen = 2;
                }
            }
            ndig = utoa(uv, base, upper, digits);
            if (prec == 0 && uv == 0)
                ndig = 0;
            /* '#' octal guarantees a leading zero. */
            if ((flags & F_ALT) && base == 8 && (ndig == 0 || digits[0] != '0')
                && prec <= ndig)
                prec = ndig + 1;
            emit_int(&o, digits, ndig, flags, width, prec, prefix, preflen, 0);
            break;
        }
        case 'p': {
            uintptr_t pv = (uintptr_t)va_arg(ap, void *);
            ndig = utoa((unsigned long long)pv, 16, 0, digits);
            emit_int(&o, digits, ndig, flags, width, prec, "0x", 2, 0);
            break;
        }
        case 'c': {
            char ch = (char)va_arg(ap, int);
            int pad = (width > 1) ? width - 1 : 0;
            if (!(flags & F_LEFT))
                while (pad-- > 0)
                    emit(&o, ' ');
            emit(&o, ch);
            if (flags & F_LEFT)
                while (pad-- > 0)
                    emit(&o, ' ');
            break;
        }
        case 's':
            emit_str(&o, va_arg(ap, const char *), flags, width, prec);
            break;
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
            /* Floating point lives in its own FPU-enabled unit; it reads the
             * double from ap, advancing it for us. */
            fmt_float(&o, flags, width, prec, *p, &ap);
            break;
        case '%':
            emit(&o, '%');
            break;
        case '\0':
            /* A trailing '%' with no conversion: emit it and stop. */
            emit(&o, '%');
            va_end(ap);
            return o.count;
        default:
            /* Unknown conversion: pass it through literally. */
            emit(&o, '%');
            emit(&o, *p);
            break;
        }
    }
    va_end(ap);
    return o.count;
}

struct bufsink {
    char *dst;
    size_t cap;     /* bytes available for data, i.e. size - 1 */
    size_t pos;
};

static void
buf_out(char c, void *arg)
{
    struct bufsink *b = arg;

    if (b->pos < b->cap)
        b->dst[b->pos] = c;
    b->pos++;
}

int
vsnprintf(char *dst, size_t size, const char *fmt, va_list ap)
{
    struct bufsink b = { dst, size > 0 ? size - 1 : 0, 0 };
    int n = vfctprintf(buf_out, &b, fmt, ap);

    if (size > 0) {
        size_t term = (b.pos < b.cap) ? b.pos : b.cap;
        dst[term] = '\0';
    }
    return n;
}

int
snprintf(char *dst, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst, size, fmt, ap);
    va_end(ap);
    return n;
}
