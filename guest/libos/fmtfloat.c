/* fmtfloat.c : floating-point conversions for the format engine.
 *
 * Split out from fmt.c so it can be built with the FPU enabled, like the
 * software rasterizer, while the rest of the formatter stays integer-only
 * under -mgeneral-regs-only. The guest enables the FPU at boot and preserves
 * FP state across task switches, so this code is safe once booted; nothing
 * should format a float before the FPU is live.
 *
 * Supports f e g and their uppercase forms, honouring the sign, field width,
 * precision, and the '-', '+', ' ', '0', '#' flags that fmt.c parses. Rounding
 * is round-half-to-even on the last kept digit. A double carries about 17
 * significant decimal digits, so beyond that fractional positions are rendered
 * as zeros, and a magnitude of 1e17 or more uses the exponential form even for
 * %f (its integer part no longer fits a 64-bit split). */
#include "fmtfloat.h"

#include <stdint.h>

#define FRAC_MAX    17          /* significant fraction digits a double holds */
#define PREC_MAX    300         /* cap on requested precision, bounds buffers */
#define F_WHOLE_MAX 1e17        /* at or above this, %f uses the %e form */

/* Powers of ten. The table is exact through 1e15 and the nearest double after;
 * that is good enough for scaling a fraction and normalising a mantissa. */
static double
pow10d(int e)
{
    static const double pos[] = {
        1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,
        1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18,
    };
    if (e >= 0 && e <= 18)
        return pos[e];
    double r = 1.0, base = (e < 0) ? 0.1 : 10.0;
    int n = (e < 0) ? -e : e;
    while (n) {
        if (n & 1)
            r *= base;
        base *= base;
        n >>= 1;
    }
    return r;
}

static double
dabs(double d)
{
    return d < 0.0 ? -d : d;
}

enum { FP_NORMAL, FP_INF, FP_NAN };

/* Classify by IEEE-754 bits, so no <math.h> and no FP compares are needed to
 * spot a NaN or infinity. Sets *neg from the sign bit (so -0.0 keeps its sign). */
static int
classify(double d, int *neg)
{
    union { double d; uint64_t u; } b = { .d = d };
    *neg = (int)(b.u >> 63);
    unsigned exp = (unsigned)((b.u >> 52) & 0x7ff);
    uint64_t man = b.u & 0xfffffffffffffull;
    if (exp == 0x7ff)
        return man ? FP_NAN : FP_INF;
    return FP_NORMAL;
}

/* Render v most-significant digit first into buf (not terminated). */
static int
u64toa(unsigned long long v, char *buf)
{
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    } while (v);
    for (int i = 0; i < n; i++)
        buf[i] = tmp[n - 1 - i];
    return n;
}

/* Round value (>= 0) to p fractional digits, splitting it into the integer
 * part and the p-digit fraction. p must be in [0, FRAC_MAX] so 10^p and both
 * parts fit in 64 bits. A fraction that rounds up to 10^p carries into the
 * integer part. Ties round to even. */
static void
split_round(double value, int p, unsigned long long *whole,
            unsigned long long *frac)
{
    double scale = pow10d(p);
    unsigned long long w = (unsigned long long)value;
    double f = (value - (double)w) * scale;
    unsigned long long fi = (unsigned long long)f;
    double rem = f - (double)fi;

    /* Round half to even: on an exact tie, round up only if the last kept
     * digit is odd. With no fraction (p == 0) that digit is the integer part. */
    unsigned long long last = (p == 0) ? w : fi;
    if (rem > 0.5 || (rem == 0.5 && (last & 1)))
        fi++;
    if ((double)fi >= scale) {      /* the fraction carried into the whole */
        fi = 0;
        w++;
    }
    *whole = w;
    *frac = fi;
}

/* Decompose v > 0 into a mantissa in [1, 10) and a power-of-ten exponent. For
 * v == 0 the mantissa is 0 and the exponent 0. */
static int
frexp10(double v, double *mant)
{
    int e = 0;
    double m = v;

    if (m > 0.0) {
        while (m >= 1e16) { m *= 1e-16; e += 16; }
        while (m >= 10.0) { m *= 0.1;   e += 1; }
        while (m < 1e-15) { m *= 1e16;  e -= 16; }
        while (m < 1.0)   { m *= 10.0;  e -= 1; }
    }
    *mant = m;
    return e;
}

/* Emit a p-digit fraction after the point: the real digits a double can hold,
 * zero-padded on the left to their place, then zeros for any positions past
 * what the value can represent. */
static void
emit_fraction(struct outctx *o, unsigned long long frac, int cprec, int prec)
{
    char fb[24];
    int fdig = u64toa(frac, fb);

    for (int i = fdig; i < cprec; i++)
        fmt_emit(o, '0');
    for (int i = 0; i < fdig; i++)
        fmt_emit(o, fb[i]);
    for (int i = cprec; i < prec; i++)
        fmt_emit(o, '0');
}

/* Fixed notation: [integer].[fraction], no sign. */
static void
render_fixed(struct outctx *o, double v, int prec, int alt)
{
    int cprec = prec > FRAC_MAX ? FRAC_MAX : prec;
    unsigned long long whole, frac;
    split_round(v, cprec, &whole, &frac);

    char ib[24];
    int idig = u64toa(whole, ib);
    for (int i = 0; i < idig; i++)
        fmt_emit(o, ib[i]);

    if (prec > 0 || alt)
        fmt_emit(o, '.');
    if (prec > 0)
        emit_fraction(o, frac, cprec, prec);
}

/* Exponential notation: d.ddd e(+/-)NN, no sign on the mantissa. */
static void
render_exp(struct outctx *o, double v, int prec, int alt, int upper)
{
    double m;
    int e = frexp10(v, &m);
    int cprec = prec > FRAC_MAX ? FRAC_MAX : prec;
    unsigned long long whole, frac;

    split_round(m, cprec, &whole, &frac);
    if (whole >= 10) {              /* 9.99.. rounded up to 10.0 */
        whole = 1;
        frac = 0;
        e++;
    }

    fmt_emit(o, (char)('0' + (int)whole));
    if (prec > 0 || alt)
        fmt_emit(o, '.');
    if (prec > 0)
        emit_fraction(o, frac, cprec, prec);

    fmt_emit(o, upper ? 'E' : 'e');
    fmt_emit(o, e < 0 ? '-' : '+');
    unsigned long long ae = (unsigned long long)(e < 0 ? -e : e);
    char eb[8];
    int ed = u64toa(ae, eb);
    for (int i = ed; i < 2; i++)    /* at least two exponent digits */
        fmt_emit(o, '0');
    for (int i = 0; i < ed; i++)
        fmt_emit(o, eb[i]);
}

/* A sink that captures into a fixed buffer, for the %g pass that strips
 * trailing zeros before emitting. */
struct capture {
    char *buf;
    int   cap;
    int   len;
};

static void
capture_out(char c, void *arg)
{
    struct capture *c2 = arg;
    if (c2->len < c2->cap)
        c2->buf[c2->len++] = c;
}

/* Drop %g's trailing zeros (and a now-trailing point) from the fraction,
 * keeping any exponent suffix. Returns the new length. */
static int
strip_zeros(char *b, int len)
{
    int epos = len;
    for (int i = 0; i < len; i++)
        if (b[i] == 'e' || b[i] == 'E') {
            epos = i;
            break;
        }
    int dot = -1;
    for (int i = 0; i < epos; i++)
        if (b[i] == '.') {
            dot = i;
            break;
        }
    if (dot < 0)
        return len;

    int end = epos;
    while (end - 1 > dot && b[end - 1] == '0')
        end--;
    if (end - 1 == dot)
        end = dot;              /* nothing left after the point */

    int gap = epos - end;
    if (gap > 0) {
        for (int i = epos; i < len; i++)
            b[i - gap] = b[i];
        len -= gap;
    }
    return len;
}

/* General notation: pick fixed or exponential by the decimal exponent, as C's
 * %g does, then strip trailing zeros unless '#' was given. */
static void
render_g(struct outctx *o, double v, int prec, int alt, int upper)
{
    int P = prec == 0 ? 1 : prec;
    if (P > FRAC_MAX)
        P = FRAC_MAX;

    double m;
    int e = frexp10(v, &m);
    unsigned long long w, f;
    split_round(m, P - 1, &w, &f);      /* P-1 in [0, 16], valid for split */
    int X = (w >= 10) ? e + 1 : e;      /* exponent after rounding to P digits */

    char gb[64];
    struct capture cap = { gb, (int)sizeof gb, 0 };
    struct outctx go = { capture_out, &cap, 0 };

    if (X >= -4 && X < P)
        render_fixed(&go, v, P - 1 - X, alt);
    else
        render_exp(&go, v, P - 1, alt, upper);

    int len = cap.len;
    if (!alt)
        len = strip_zeros(gb, len);
    for (int i = 0; i < len; i++)
        fmt_emit(o, gb[i]);
}

void
fmt_float(struct outctx *o, int flags, int width, int prec, char conv,
          va_list *ap)
{
    double d = va_arg(*ap, double);
    int upper = (conv >= 'A' && conv <= 'Z');
    char lc = (char)(upper ? conv + 32 : conv);

    int neg;
    int cls = classify(d, &neg);
    char sign = neg ? '-'
              : (flags & F_PLUS)  ? '+'
              : (flags & F_SPACE) ? ' '
              : 0;

    /* Render the magnitude (no sign) into a buffer, then pad the field. */
    char buf[352];
    struct capture cap = { buf, (int)sizeof buf, 0 };
    struct outctx bo = { capture_out, &cap, 0 };
    int zeroflag = flags & F_ZERO;

    if (cls != FP_NORMAL) {
        const char *s = (cls == FP_NAN) ? (upper ? "NAN" : "nan")
                                        : (upper ? "INF" : "inf");
        for (int i = 0; i < 3; i++)
            capture_out(s[i], &cap);
        zeroflag = 0;               /* inf/nan are space-padded, never zero */
    } else {
        double v = dabs(d);
        if (prec < 0)
            prec = 6;
        if (prec > PREC_MAX)
            prec = PREC_MAX;
        if (lc == 'g')
            render_g(&bo, v, prec, flags & F_ALT, upper);
        else if (lc == 'e')
            render_exp(&bo, v, prec, flags & F_ALT, upper);
        else if (v >= F_WHOLE_MAX)  /* %f with too large an integer part */
            render_exp(&bo, v, prec, flags & F_ALT, upper);
        else
            render_fixed(&bo, v, prec, flags & F_ALT);
    }

    int content = cap.len;
    int blen = (sign ? 1 : 0) + content;
    int pad = width > blen ? width - blen : 0;
    int zeropad = 0;

    if (zeroflag && !(flags & F_LEFT)) {
        zeropad = pad;
        pad = 0;
    }
    if (!(flags & F_LEFT))
        while (pad-- > 0)
            fmt_emit(o, ' ');
    if (sign)
        fmt_emit(o, sign);
    while (zeropad-- > 0)
        fmt_emit(o, '0');
    for (int i = 0; i < content; i++)
        fmt_emit(o, buf[i]);
    if (flags & F_LEFT)
        while (pad-- > 0)
            fmt_emit(o, ' ');
}
