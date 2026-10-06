/* string.h : freestanding mem and string primitives for the guest.
 *
 * The guest builds -ffreestanding -nostdlib -fno-builtin. A freestanding target
 * must still provide memcpy, memmove, memset, and memcmp, because the compiler
 * may lower a struct copy or an array initializer into a call to one of them
 * even where the source never names it. These are that required set plus the
 * handful of string helpers the libOS was open-coding. */
#ifndef LIBOS_STRING_H
#define LIBOS_STRING_H

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);

/* Bounded, always-terminated copy, like strlcpy: copies at most size-1 bytes
 * and always NUL-terminates when size is non-zero. A NULL src is treated as the
 * empty string. Returns the length of src, so truncation shows as a return
 * value >= size. */
size_t strlcpy(char *dst, const char *src, size_t size);

#endif /* LIBOS_STRING_H */
