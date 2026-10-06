/* boot.h : access to the host-supplied boot parameters.
 *
 * The host places a jotun_bootinfo block in guest RAM before the vcpu starts.
 * These helpers read it; they are valid only after boot_init has validated the
 * block. */
#ifndef LIBOS_BOOT_H
#define LIBOS_BOOT_H

#include <stdint.h>

/* Validate and latch the boot block. Returns 0 on success, -1 if the block is
 * absent or its magic or version does not match. */
int boot_init(void);

/* Total guest RAM in bytes, or 0 if boot_init failed. */
uint64_t boot_ram_size(void);

/* Number of argument strings the host passed. */
int boot_argc(void);

/* The i-th argument string, or NULL if i is out of range. Points into the boot
 * block, which stays mapped for the life of the guest. */
const char *boot_argv(int i);

#endif /* LIBOS_BOOT_H */
