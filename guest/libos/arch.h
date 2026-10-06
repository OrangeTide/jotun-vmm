/* arch.h : the arch interface the library OS core depends on.
 *
 * Each supported host provides these in its arch/<arch>.c. The core and the
 * portable helpers call only through this seam, so bringing up a new arch is a
 * matter of supplying a console byte sink and a way to stop the machine. */
#ifndef LIBOS_ARCH_H
#define LIBOS_ARCH_H

/* Write one byte to the console, blocking until the device accepts it. */
void con_putc(char c);

/* Stop the vcpu for good. Does not return. On x86-64 this is HLT; on arm64 it
 * is a store to the poweroff register. */
void arch_halt(void);

#endif /* LIBOS_ARCH_H */
