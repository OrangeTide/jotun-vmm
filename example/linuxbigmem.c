/* linuxbigmem.c : a static Linux test that a single process can map and use more
 * than 2 GiB. The guest RAM and user window grew to 3.5 GiB precisely so a heavy
 * translation unit (V8's pipeline.cc needs ~2.4 GiB in cc1plus) fits; a 2.5 GiB
 * mapping here exceeds the old 2 GiB window and proves the new one. It maps a
 * large anonymous region, writes a known byte at every 64 KiB stride across the
 * whole span, reads them back, and reports. An ordinary Linux binary. */
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void
puts2(const char *s)
{
    write(1, s, strlen(s));
}

/* 2.5 GiB: over the old 2 GiB window, with headroom under the ~3.48 GiB window
 * for the program image, its stack, and the page tables backing the mapping. */
#define SPAN   (2560ull * 1024 * 1024)
#define STRIDE (64 * 1024)

int
main(void)
{
    void *p = mmap(NULL, SPAN, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        puts2("bigmem: mmap failed\n");
        return 1;
    }

    volatile unsigned char *b = p;
    /* Write a position-dependent byte at each stride, so a mis-mapped page (a
     * stale alias, a wrong physmap, an aliased frame) is caught on read-back. */
    for (uint64_t off = 0; off < SPAN; off += STRIDE)
        b[off] = (unsigned char)((off / STRIDE) & 0xff);

    for (uint64_t off = 0; off < SPAN; off += STRIDE) {
        if (b[off] != (unsigned char)((off / STRIDE) & 0xff)) {
            puts2("bigmem: readback mismatch\n");
            return 2;
        }
    }

    /* Touch the very top and bottom of the span explicitly. */
    b[0] = 0xA5;
    b[SPAN - 1] = 0x5A;
    if (b[0] != 0xA5 || b[SPAN - 1] != 0x5A) {
        puts2("bigmem: edge check failed\n");
        return 3;
    }

    munmap(p, SPAN);
    puts2("bigmem ok\n");
    return 0;
}
