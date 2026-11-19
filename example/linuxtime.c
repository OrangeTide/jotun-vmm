/* linuxtime.c : a static Linux test for the guest clock (preemption stages P1
 * and P2: a host-driven monotonic tick and a real clock read from it). Checks
 * that CLOCK_MONOTONIC advances and never goes backward, that CLOCK_REALTIME is
 * anchored at the guest epoch, and that gettimeofday and time agree with it.
 *
 * Run with the host timer on, which drives the clock:
 *   JOTUN_HZ=1000 make linux-example EXAMPLE_PROG=linuxtime
 * An ordinary Linux binary. */
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define GUEST_EPOCH 1735689600L         /* 2025-01-01 UTC; matches the guest */

static void
puts2(const char *s)
{
    write(1, s, strlen(s));
}

static int fails;

static void
check(int ok, const char *what)
{
    puts2(ok ? "[ok]   " : "[FAIL] ");
    puts2(what);
    puts2("\n");
    if (!ok)
        fails++;
}

static uint64_t
mono_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

int
main(void)
{
    /* Monotonic never goes backward across a run of reads. */
    uint64_t prev = mono_ns();
    int backward = 0;
    for (int i = 0; i < 2000; i++) {
        uint64_t now = mono_ns();
        if (now < prev)
            backward = 1;
        prev = now;
    }
    check(!backward, "CLOCK_MONOTONIC never goes backward");

    /* Monotonic advances across a busy interval: the host drives the clock in
     * wall time, so even this interrupts-off loop sees time pass. */
    uint64_t t0 = mono_ns();
    for (volatile unsigned long d = 0; d < 200000000UL; d++)
        ;
    uint64_t t1 = mono_ns();
    check(t1 > t0, "CLOCK_MONOTONIC advances over a busy loop");
    check(t1 - t0 < 60ULL * 1000000000ULL, "the elapsed time is sane (< 60 s)");

    /* Realtime is anchored at the guest epoch and nanoseconds are in range. */
    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);
    check(rt.tv_sec >= GUEST_EPOCH, "CLOCK_REALTIME is at or after the epoch");
    check(rt.tv_nsec >= 0 && rt.tv_nsec < 1000000000L, "realtime nsec in range");

    /* gettimeofday agrees with clock_gettime to within a couple of seconds. */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long dsec = tv.tv_sec - rt.tv_sec;
    if (dsec < 0)
        dsec = -dsec;
    check(dsec <= 2 && tv.tv_usec >= 0 && tv.tv_usec < 1000000L,
          "gettimeofday agrees with clock_gettime");

    /* time() agrees with the realtime seconds. */
    time_t tt = time(NULL);
    long dt = (long)tt - (long)rt.tv_sec;
    if (dt < 0)
        dt = -dt;
    check(tt >= GUEST_EPOCH && dt <= 2, "time() agrees with clock_gettime");

    puts2(fails ? "linuxtime: FAILED\n" : "linuxtime: all ok\n");
    return fails ? 1 : 0;
}
