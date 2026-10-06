/* mem.h : flat first-fit block allocator for the library OS.
 *
 * Adapted from the JOS kernel allocator. There is no virtual memory and no
 * MMU, so this manages one contiguous region of guest RAM directly. */
#ifndef LIBOS_MEM_H
#define LIBOS_MEM_H

#include <stddef.h>

/* Hand the allocator a contiguous region to manage. Call once at boot. */
void meminit(void *base, size_t size);

/* Allocate at least size bytes, or NULL if the region cannot satisfy it. */
void *kmalloc(size_t size);

/* Return a block from kmalloc. kfree(NULL) is a no-op. */
void kfree(void *ptr);

/* Total free bytes across the free list, including block headers. An idle
 * system returns to its initial value, so a churn test can assert no leak. */
size_t memfree(void);

#endif /* LIBOS_MEM_H */
