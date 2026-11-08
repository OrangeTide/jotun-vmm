/* linuxefault.c : a syscall handed a bad pointer returns EFAULT, not a fault.
 *
 * The kernel range-checks every user pointer a syscall passes it (user_ok and
 * the copy_*_user helpers). A pointer outside the user window, or into the
 * stack guard band, is refused with EFAULT and the program keeps running. This
 * passes a kernel address as a write() buffer and a read() buffer and expects
 * EFAULT both times, with the guest surviving. A kernel that dereferenced the
 * pointer would read or write kernel memory or fault in ring 0 and halt.
 *
 * Positive test: it exits 0 when both calls report EFAULT. */
#include <unistd.h>
#include <errno.h>
#include <string.h>

static void
say(const char *s)
{
    write(1, s, strlen(s));
}

int
main(void)
{
    /* 0x1000 is inside the kernel band below the user window: off limits. */
    void *bad = (void *)0x1000UL;

    errno = 0;
    long w = write(1, bad, 16);
    int w_efault = (w < 0 && errno == EFAULT);

    errno = 0;
    long r = read(0, bad, 16);
    int r_efault = (r < 0 && errno == EFAULT);

    if (w_efault && r_efault) {
        say("efault: bad pointers rejected with EFAULT\n");
        return 0;
    }
    say("efault: a bad pointer was not rejected\n");
    return 1;
}
