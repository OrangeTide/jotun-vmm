/* fmttest.c : a host check of the guest format engine against known output.
 *
 * The guest formatter (guest/libos/fmt.c and fmtfloat.c) is portable C, so it
 * builds for the host too. The Makefile compiles it here with snprintf renamed
 * to jsnprintf, so it coexists with the host libc, and this program asserts its
 * output byte for byte against literal expectations. It covers the integer
 * conversions and the floating point ones (f e g, with flags, width, precision,
 * rounding, and the infinity and NaN cases). Exits 0 on success, 1 on failure. */
#include <math.h>
#include <stdio.h>
#include <string.h>

int jsnprintf(char *dst, unsigned long size, const char *fmt, ...);

static int fails;

static void
ck(const char *got, const char *exp, const char *what)
{
    if (strcmp(got, exp) != 0) {
        fails++;
        printf("  FAIL %-14s got=[%s] expected=[%s]\n", what, got, exp);
    }
}

static void
cf(const char *fmt, double v, const char *exp)
{
    char buf[400];
    jsnprintf(buf, sizeof buf, fmt, v);
    ck(buf, exp, fmt);
}

static void
cd(const char *fmt, int v, const char *exp)
{
    char buf[64];
    jsnprintf(buf, sizeof buf, fmt, v);
    ck(buf, exp, fmt);
}

static void
cu(const char *fmt, unsigned v, const char *exp)
{
    char buf[64];
    jsnprintf(buf, sizeof buf, fmt, v);
    ck(buf, exp, fmt);
}

int
main(void)
{
    /* Integer conversions (guarding the existing engine). */
    cd("%d", -42, "-42");
    cd("%05d", 42, "00042");
    cd("%+d", 42, "+42");
    cd("%6d", 42, "    42");
    cd("%-6d|", 42, "42    |");
    cu("%x", 255u, "ff");
    cu("%#x", 255u, "0xff");
    cu("%o", 8u, "10");
    cu("%b", 5u, "101");
    cu("%u", 42u, "42");

    /* %f: default, precision, flags, width, zero-fill. */
    cf("%f", 3.14159, "3.141590");
    cf("%.2f", 3.14159, "3.14");
    cf("%.0f", 3.0, "3");
    cf("%.3f", 0.0, "0.000");
    cf("%f", -0.0, "-0.000000");
    cf("%+.3f", 1.0, "+1.000");
    cf("% .3f", 1.0, " 1.000");
    cf("%8.3f", 3.14, "   3.140");
    cf("%-8.3f|", 3.14, "3.140   |");
    cf("%08.3f", -3.14, "-003.140");
    cf("%#.0f", 5.0, "5.");
    cf("%.1f", 2.55, "2.5");       /* 2.55 is just under, rounds down */

    /* Round half to even on exact ties. */
    cf("%.0f", 0.5, "0");
    cf("%.0f", 1.5, "2");
    cf("%.0f", 2.5, "2");
    cf("%.0f", 3.5, "4");

    /* %e / %E. */
    cf("%e", 12345.678, "1.234568e+04");
    cf("%.2e", 0.00042, "4.20e-04");
    cf("%.0e", 7.0, "7e+00");
    cf("%E", 1.0, "1.000000E+00");
    cf("%e", 0.0, "0.000000e+00");

    /* %g / %G: style choice and trailing-zero removal. */
    cf("%g", 100000.0, "100000");
    cf("%g", 1000000.0, "1e+06");
    cf("%g", 0.0001, "0.0001");
    cf("%g", 0.00001, "1e-05");
    cf("%g", 3.14, "3.14");
    cf("%.3g", 3.14159, "3.14");
    cf("%g", 0.0, "0");
    cf("%G", 0.000001, "1E-06");
    cf("%#g", 1.5, "1.50000");      /* '#' keeps trailing zeros */

    /* Infinity and NaN. */
    cf("%f", INFINITY, "inf");
    cf("%f", -INFINITY, "-inf");
    cf("%F", INFINITY, "INF");
    cf("%e", INFINITY, "inf");
    cf("%g", NAN, "nan");
    cf("%6.2f", INFINITY, "   inf");

    if (fails == 0)
        printf("fmttest: ok\n");
    else
        printf("fmttest: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
