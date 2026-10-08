/* jsbrk.c : the memory seam for the program allocator.
 *
 * jsbrk grows or shrinks the program break by `incr` bytes and returns the old
 * break, or (void *)-1 if the request would leave the heap. The heap is a
 * reserved .bss array the linker places in the program image, so a bundle
 * program gets a working malloc without any new service-vector ABI: the memory
 * is simply part of the program's own address space, zeroed at load. Lifted
 * from skjegg's libc/sbrk.c (CC0), renamed and trimmed. */
#include <stddef.h>
#include <stdint.h>

#ifndef JLIBC_HEAP_SIZE
#define JLIBC_HEAP_SIZE (256 * 1024)        /* fits the 2 MiB program window */
#endif

/* An array of the widest scalar, so the region base is 8-byte aligned. */
static long long heap_store[(JLIBC_HEAP_SIZE + sizeof(long long) - 1)
                            / sizeof(long long)];

static char *heap_brk;                      /* current break, lazily initialized */

void *
jsbrk(long incr)
{
    uintptr_t raw = (uintptr_t)(char *)heap_store;
    char *base = (char *)((raw + 7u) & ~(uintptr_t)7u);
    char *limit = (char *)heap_store + sizeof heap_store;
    char *old, *next;

    if (heap_brk == 0)
        heap_brk = base;
    old = heap_brk;
    next = old + incr;

    if (next < base || next > limit)
        return (void *)-1;
    heap_brk = next;
    return old;
}
