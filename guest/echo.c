/* echo.c : M1 transport proof. A freestanding guest that sends a message over
 * the ring, checks the host's echo, and on a match sends a second message, so
 * the host prints both and the round trip is visible in each direction. */

#include <stdint.h>
#include "jotun_ring.h"

static int
equal(const char *a, const uint8_t *b, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++)
        if ((uint8_t)a[i] != b[i])
            return 0;
    return 1;
}

static uint16_t
slen(const char *s)
{
    uint16_t n = 0;
    while (s[n])
        n++;
    return n;
}

static void
done(void)
{
#if defined(__aarch64__)
    /* MMIO poweroff, matching MMIO_POWEROFF in host/uart.h. */
    *(volatile unsigned int *)(uintptr_t)0x09010000ull = 1;
#else
    __asm__ volatile ("hlt");
#endif
    for (;;)
        ;
}

void
_start(void)
{
    static const char msg[] = "hello from the ring";
    uint8_t reply[JOTUN_MSG_MAX];

    uint16_t n = slen(msg);
    jotun_send(msg, n);

    uint16_t got = jotun_recv(reply, sizeof(reply));
    if (got == n && equal(msg, reply, n)) {
        static const char ok[] = "VERIFIED";
        jotun_send(ok, slen(ok));
    }

    done();
}
