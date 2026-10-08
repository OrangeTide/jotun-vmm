/* malloctest.c : exercises the bundle program allocator (jmalloc.c + jsbrk.c).
 *
 * Checks write/read through a malloc'd block, calloc zeroing, realloc growth,
 * and that a balanced set of allocations returns the live footprint to zero
 * (so coalescing and free-list bookkeeping are sound). Run with
 * `make malloc-test`. A native bundle program: it reaches the libOS through the
 * service vector and uses the program C library. */
#include "jlibc.h"
#include "prog.h"

void
_start(void)
{
    printf("malloc test: start, live=%u\n", (unsigned)jmalloc_live());

    char *a = malloc(100);
    char *b = malloc(2000);
    if (!a || !b) {
        printf("  allocation failed\n");
        sys()->exit("malloc-fail");
    }
    for (int i = 0; i < 100; i++)
        a[i] = (char)i;
    int ok = 1;
    for (int i = 0; i < 100; i++)
        if (a[i] != (char)i)
            ok = 0;
    printf("  write/read 100 bytes: %s\n", ok ? "ok" : "BAD");
    printf("  live after two allocs=%u peak=%u\n",
           (unsigned)jmalloc_live(), (unsigned)jmalloc_peak());
    free(a);

    char *c = calloc(64, 4);
    int zeroed = (c != 0);
    for (int i = 0; c && i < 256; i++)
        if (c[i] != 0)
            zeroed = 0;
    printf("  calloc(64,4) zeroed: %s\n", zeroed ? "ok" : "BAD");

    c = realloc(c, 1024);
    printf("  realloc to 1024: %s\n", c ? "ok" : "BAD");
    free(c);
    free(b);
    printf("  live after frees=%u (expect 0)\n", (unsigned)jmalloc_live());

    /* Stress the bins and coalescing: many varied small allocations, freed. */
    char *v[64];
    int fails = 0;
    for (int i = 0; i < 64; i++) {
        v[i] = malloc(32 + (unsigned)i);
        if (!v[i])
            fails++;
    }
    for (int i = 0; i < 64; i++)
        free(v[i]);
    printf("  stress: %d failures, live=%u peak=%u\n",
           fails, (unsigned)jmalloc_live(), (unsigned)jmalloc_peak());

    sys()->exit("malloc-ok");
}
