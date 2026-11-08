/* linuxstackov.c : deliberately overflow the main-thread stack to prove the
 * guard band below it faults cleanly.
 *
 * The libOS gives a Linux process an 8 MiB stack reservation at the top of the
 * user window, with a 1 MiB unmapped guard band just below it that pf_handle
 * refuses to demand-page (see guest/libos/linuxelf.c, pf_guard_lo). This program
 * recurses with a large per-frame footprint, touching each frame so the stack
 * grows monotonically down. Once it walks past the 8 MiB reservation the next
 * write lands in the guard and must take a fatal fault, instead of silently
 * growing into the mmap arena the way the old 256 KiB stack did. A correct guest
 * never lets this program print "SURVIVED": it dies in the guard first. */
#include <stddef.h>
#include <unistd.h>

/* Report the current depth and the address of a local, so the run shows the
 * stack marching down toward the reservation floor before it faults. */
static void
report(int depth, void *frame)
{
    char buf[64];
    unsigned long a = (unsigned long)frame;
    int n = 0;
    buf[n++] = 'd'; buf[n++] = '=';
    /* depth in decimal */
    char tmp[16]; int t = 0;
    if (depth == 0) tmp[t++] = '0';
    for (int d = depth; d > 0; d /= 10) tmp[t++] = '0' + d % 10;
    while (t > 0) buf[n++] = tmp[--t];
    buf[n++] = ' '; buf[n++] = 's'; buf[n++] = 'p'; buf[n++] = '=';
    buf[n++] = '0'; buf[n++] = 'x';
    for (int s = 28; s >= 0; s -= 4) {   /* 32-bit VA is plenty here */
        int nib = (int)((a >> s) & 0xf);
        buf[n++] = nib < 10 ? '0' + nib : 'a' + nib - 10;
    }
    buf[n++] = '\n';
    write(1, buf, (size_t)n);
}

static int
descend(int depth)
{
    /* A fat, touched frame: 8 KiB forces the stack down two pages per call and
     * the writes fault each page in, so growth is real, not just rsp motion. */
    volatile char frame[8192];
    frame[0] = (char)depth;
    frame[sizeof frame - 1] = (char)depth;

    if ((depth & 0xff) == 0)
        report(depth, (void *)frame);

    /* Recurse forever; the guard fault is the only way out. */
    int r = descend(depth + 1);
    return r + frame[0];
}

int
main(void)
{
    write(1, "stackov: descending\n", 20);
    int r = descend(0);
    /* Unreachable on a correct guest. */
    write(1, "stackov: SURVIVED\n", 18);
    return r & 1;
}
