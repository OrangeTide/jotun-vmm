/* mem.h : flat first-fit block allocator for the library OS.
 *
 * Adapted from the JOS kernel allocator. There is no virtual memory and no
 * MMU, so this manages one contiguous region of guest RAM directly. */
#ifndef LIBOS_MEM_H
#define LIBOS_MEM_H

#include <stddef.h>

/* Hand the allocator a contiguous region to manage. Call once at boot. */
void meminit(void *base, size_t size);

/* Add another contiguous region to the free pool. Like meminit, but augments
 * the pool rather than replacing it, so the heap can grow after boot. The
 * region is inserted in address order and coalesces with an adjacent block. */
void memadd(void *base, size_t size);

/* Register a callback kmalloc invokes when the current free pool cannot
 * satisfy a request. It should map and add at least `need` bytes as one
 * contiguous region (via memadd) and return the bytes added, or 0 if it cannot
 * grow. kmalloc retries the request after a successful growth. */
void mem_set_grower(size_t (*grow)(size_t need));

/* Allocate at least size bytes, or NULL if the region cannot satisfy it. */
void *kmalloc(size_t size);

/* Return a block from kmalloc. kfree(NULL) is a no-op. */
void kfree(void *ptr);

/* Total free bytes across the free list, including block headers. An idle
 * system returns to its initial value, so a churn test can assert no leak. */
size_t memfree(void);

#endif /* LIBOS_MEM_H */
