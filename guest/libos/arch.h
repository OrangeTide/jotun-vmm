/* arch.h : the arch interface the library OS core depends on.
 *
 * Each supported host provides these in its arch/<arch>.c. The core and the
 * portable helpers call only through this seam, so bringing up a new arch is a
 * matter of supplying a console byte sink and a way to stop the machine. */
#ifndef LIBOS_ARCH_H
#define LIBOS_ARCH_H

#include <stddef.h>

/* Write one byte to the console, blocking until the device accepts it. */
void con_putc(char c);

/* Stop the vcpu for good. Does not return. On x86-64 this is HLT; on arm64 it
 * is a store to the poweroff register. */
void arch_halt(void);

/* Lay out a fresh stack so the first switch into it "returns" to entry with a
 * clean callee-saved set. Returns the stack pointer to hand to ctx_switch.
 * entry must not return; a task's entry is a trampoline that exits instead. */
void *stack_init(void *stack, size_t size, void (*entry)(void));

/* Save the current callee-saved context to *oldsp (unless oldsp is NULL, as on
 * the first switch away from a context never to be resumed), load newsp, and
 * return into the context stored there. */
void ctx_switch(void **oldsp, void *newsp);

#endif /* LIBOS_ARCH_H */
