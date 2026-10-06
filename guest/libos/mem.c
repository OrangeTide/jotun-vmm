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

void
meminit(void *base, size_t size)
{
    struct block *b = base;

    b->next = NULL;
    b->size = size;
    freelist = b;
}

void *
kmalloc(size_t size)
{
    struct block *b, **prev;
    size_t total;

    total = (size + sizeof(struct block) + ALIGN - 1) & ~((size_t)ALIGN - 1);

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
    return NULL;
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
    struct block *b, *cur, **prev;

    if (ptr == NULL)
        return;

    unsigned long irq = heap_acquire();
    b = (struct block *)((char *)ptr - sizeof(struct block));

    /* insert in address order */
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
    heap_release(irq);
}
