/* mem.c : flat first-fit block allocator, adapted from the JOS kernel.
 *
 * One contiguous region, a first-fit free list, allocations aligned up to
 * ALIGN. kfree inserts in address order and coalesces with both neighbours,
 * so an idle system returns to a single free block. */
#include "mem.h"

#include <stdint.h>

#include "arch.h"

/* Allocation granularity. Every block is a multiple of this, which keeps
 * headers aligned and bounds the number of free-list fragments. */
#define ALIGN 64

struct block {
    struct block    *next;
    size_t          size;       /* including this header */
};

static struct block *freelist;

/* Invoked when the free pool cannot satisfy a request; grows the heap. NULL
 * until a backend registers one, so the boot/native path behaves as a fixed
 * region. */
static size_t (*mem_grow)(size_t need);

/* The free list is shared by every task on every cpu. A spinlock serialises
 * cross-cpu access; masking interrupts as well keeps a tick on the same cpu
 * from re-entering the allocator while the lock is held (which would spin
 * forever against itself). */
static volatile int heap_lock;

static unsigned long
heap_acquire(void)
{
    unsigned long f = arch_irq_save();
    while (__atomic_exchange_n(&heap_lock, 1, __ATOMIC_ACQUIRE))
        arch_cpu_relax();
    return f;
}

static void
heap_release(unsigned long f)
{
    __atomic_store_n(&heap_lock, 0, __ATOMIC_RELEASE);
    arch_irq_restore(f);
}

/* Insert b into the free list in address order and coalesce with either
 * neighbour it abuts. The caller holds the heap lock. Shared by kfree, which
 * returns a block, and memadd, which contributes a fresh region. */
static void
free_insert(struct block *b)
{
    struct block *cur, **prev;

    for (prev = &freelist; (cur = *prev) != NULL; prev = &cur->next) {
        if (b < cur)
            break;
    }
    b->next = cur;
    *prev = b;

    /* coalesce with the following block */
    if (cur != NULL && (char *)b + b->size == (char *)cur) {
        b->size += cur->size;
        b->next = cur->next;
    }
    /* coalesce with the preceding block */
    if (prev != &freelist) {
        struct block *p = freelist;
        while (p->next != b && p->next != NULL)
            p = p->next;
        if ((char *)p + p->size == (char *)b) {
            p->size += b->size;
            p->next = b->next;
        }
    }
}

void
meminit(void *base, size_t size)
{
    struct block *b = base;

    b->next = NULL;
    b->size = size;
    freelist = b;
}

void
memadd(void *base, size_t size)
{
    if (size < sizeof(struct block))
        return;

    struct block *b = base;
    b->size = size;

    unsigned long irq = heap_acquire();
    free_insert(b);
    heap_release(irq);
}

void
mem_set_grower(size_t (*grow)(size_t need))
{
    mem_grow = grow;
}

void *
kmalloc(size_t size)
{
    struct block *b, **prev;
    size_t total;

    total = (size + sizeof(struct block) + ALIGN - 1) & ~((size_t)ALIGN - 1);

    for (;;) {
        unsigned long irq = heap_acquire();
        for (prev = &freelist; (b = *prev) != NULL; prev = &b->next) {
            if (b->size < total)
                continue;
            if (b->size - total >= sizeof(struct block) + ALIGN) {
                /* split: carve the request off the front, leave the rest */
                struct block *rem = (struct block *)((char *)b + total);
                rem->next = b->next;
                rem->size = b->size - total;
                b->size = total;
                *prev = rem;
            } else {
                *prev = b->next;
            }
            heap_release(irq);
            return (char *)b + sizeof(struct block);
        }
        heap_release(irq);

        /* Nothing fit. Ask the backend to grow the heap by at least this
         * request and retry; give up when it cannot. memadd runs inside the
         * grower, so the lock must be released first. */
        if (!mem_grow || mem_grow(total) == 0)
            return NULL;
    }
}

size_t
memfree(void)
{
    struct block *b;
    size_t total = 0;

    for (b = freelist; b != NULL; b = b->next)
        total += b->size;
    return total;
}

void
kfree(void *ptr)
{
    if (ptr == NULL)
        return;

    struct block *b = (struct block *)((char *)ptr - sizeof(struct block));

    unsigned long irq = heap_acquire();
    free_insert(b);
    heap_release(irq);
}
