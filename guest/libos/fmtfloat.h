/* fmtfloat.h : the format engine's floating-point rendering, split out so it
 * can be built with the FPU enabled while fmt.c stays integer-only.
 *
 * fmt.c is compiled -mgeneral-regs-only like the rest of the guest, which
 * forbids the FP and SIMD instructions that even reading a double argument
 * needs. The float conversions therefore live in their own translation unit,
 * compiled with the FPU on (as the rasterizer is), and fmt.c calls into it. */
#ifndef FMTFLOAT_H
#define FMTFLOAT_H

#include <stdarg.h>

/* The format engine's output sink, shared between fmt.c and fmtfloat.c. */
struct outctx {
    void (*out)(char c, void *arg);
    void *arg;
    int count;
};

static inline void
fmt_emit(struct outctx *o, char c)
{
    o->out(c, o->arg);
    o->count++;
}

/* Conversion flags, shared so fmt.c passes them through unchanged. */
#define F_LEFT  0x01    /* '-' : left-justify in the field */
#define F_ZERO  0x02    /* '0' : pad with zeros, not spaces */
#define F_PLUS  0x04    /* '+' : always show a sign */
#define F_SPACE 0x08    /* ' ' : space before a non-negative value */
#define F_ALT   0x10    /* '#' : keep the decimal point and trailing zeros */

/* Render one floating conversion (conv is one of f F e E g G) into the sink,
 * taking the double from *ap and advancing the caller's va_list. prec is the
 * requested precision, or negative when the format omitted it. The FPU must be
 * live, which the guest guarantees once booted. */
void fmt_float(struct outctx *o, int flags, int width, int prec, char conv,
               va_list *ap);

#endif /* FMTFLOAT_H */
