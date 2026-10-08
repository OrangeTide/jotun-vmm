/* linux_arch.h : arch-neutral front door for the Linux-compat backend.
 *
 * linuxelf.c holds the arch-neutral core (VFS, heap arena, process and thread
 * model, syscall dispatch). The machine-specific parts (syscall numbers, the
 * user trapframe, entering user code, the thread pointer, and arming the trap)
 * come from the per-arch backend this selects. The vmspace (page tables) has
 * its own front door, vmspace.h. */
#ifndef LIBOS_LINUX_ARCH_H
#define LIBOS_LINUX_ARCH_H

#if defined(__x86_64__)
#include "arch/x86_64_linuxabi.h"
#elif defined(__aarch64__)
#include "arch/arm64_linuxabi.h"
#else
#error "no Linux-compat backend for this architecture"
#endif

#endif /* LIBOS_LINUX_ARCH_H */
