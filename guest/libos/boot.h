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

/* Number of writable-mount guest prefixes the host declared. */
int boot_nrwmount(void);

/* The i-th writable-mount prefix (for example "/out"), or NULL if out of range.
 * Points into the boot block, mapped for the life of the guest. */
const char *boot_rwmount(int i);

/* The preemption-timer rate in ticks per second, or 0 when no timer is running
 * (then there is no host-driven clock and callers fall back to the cycle
 * counter). Constant for the life of the guest. */
uint32_t boot_timer_hz(void);

/* The current monotonic tick count. The host advances it in wall time, so a
 * fresh read reflects elapsed real time even across an interrupts-off stretch.
 * Zero when no timer is running. */
uint64_t boot_timer_ticks(void);

/* How the preemption tick is delivered to this (arm64) guest: JOTUN_IRQ_LINE or
 * JOTUN_IRQ_GICV3 (jotun_abi.h). Chosen by the host backend; the arm64 IRQ path
 * reads it to pick its acknowledge mechanism. */
uint32_t boot_irq_model(void);

/* The host's real wall-clock time at boot, in Unix seconds, or 0 when the host
 * supplied none. The realtime clock bases itself on this. */
uint64_t boot_unix_time(void);

/* Number of environment strings the host passed (after the arguments), and the
 * i-th one ("NAME=value"), or NULL if out of range. The host supplies these
 * only for an interactive guest; otherwise the count is 0. */
int boot_envc(void);
const char *boot_envp(int i);

/* Nonzero when the host wired this guest's console to a live terminal: stdin
 * delivers keystrokes and the init program should run a tty line discipline. */
int boot_console_interactive(void);

#endif /* LIBOS_BOOT_H */
