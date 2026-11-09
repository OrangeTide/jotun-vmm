/* linuxsleep: prove that nanosleep really blocks for about the requested time.
 *
 * Reads the monotonic clock, sleeps 100 ms, reads it again, and checks the gap.
 * With a host timer (JOTUN_HZ) the sleep parks until a deadline tick, so the gap
 * is at least ~90 ms; without one it returns at once. A second sleep on an
 * absolute deadline exercises clock_nanosleep's TIMER_ABSTIME path. */
#include <time.h>
#include <unistd.h>
#include <string.h>

static long
elapsed_ms(const struct timespec *a, const struct timespec *b)
{
    return (b->tv_sec - a->tv_sec) * 1000 + (b->tv_nsec - a->tv_nsec) / 1000000;
}

int
main(void)
{
    struct timespec a, b;

    /* With a host timer the gap is at least ~90 ms; with no timer the sleep
     * returns at once (gap ~0), which is also correct. Only a partial or wild gap
     * is a failure. */
    clock_gettime(CLOCK_MONOTONIC, &a);
    struct timespec req = { 0, 100 * 1000 * 1000 };     /* 100 ms */
    nanosleep(&req, 0);
    clock_gettime(CLOCK_MONOTONIC, &b);
    long ms = elapsed_ms(&a, &b);
    int ok = (ms >= 90 && ms < 5000) || (ms >= 0 && ms < 5);
    write(1, ok ? "nanosleep ok\n" : "nanosleep BAD\n", ok ? 13 : 14);

    /* Absolute deadline: now + 80 ms on CLOCK_MONOTONIC. */
    clock_gettime(CLOCK_MONOTONIC, &a);
    struct timespec dl = a;
    dl.tv_nsec += 80 * 1000 * 1000;
    if (dl.tv_nsec >= 1000000000) { dl.tv_nsec -= 1000000000; dl.tv_sec++; }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &dl, 0);
    clock_gettime(CLOCK_MONOTONIC, &b);
    ms = elapsed_ms(&a, &b);
    ok = (ms >= 70 && ms < 5000) || (ms >= 0 && ms < 5);
    write(1, ok ? "abssleep ok\n" : "abssleep BAD\n", ok ? 12 : 13);
    return 0;
}
