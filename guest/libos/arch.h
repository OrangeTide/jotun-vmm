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
 * return into the context stored there. Preserves the interrupt-enable state
 * per context, so a resumed context keeps its own masking. */
void ctx_switch(void **oldsp, void *newsp);

/* Install the trap tables (GDT and IDT on x86) and the event interrupt gate.
 * Must run before any task starts, since a task runs with interrupts enabled
 * and a tick could arrive. On arm64 this is a stub for now. */
void arch_traps_init(void);

/* Enable the floating-point and SIMD unit on this cpu, so code built with the
 * FPU on (the guest rasterizer) can run. The rest of the library OS is built
 * integer-only and never touches these registers; the only place FP state
 * crosses a context boundary is a task switch, which each arch preserves (the
 * x86 interrupt path saves it around the handler, and the arm64 cooperative
 * switch saves the callee-saved vector registers). Must run once per cpu at
 * boot, before any task starts. */
void arch_fpu_init(void);

/* Enable and disable asynchronous interrupts on this cpu. */
void arch_irq_enable(void);
void arch_irq_disable(void);

/* Save-and-disable interrupts, returning the prior state, and restore it.
 * Used to guard scheduler-critical regions against preemption. */
unsigned long arch_irq_save(void);
void arch_irq_restore(unsigned long flags);

/* Per-cpu base pointer: set it once per vcpu at boot, then read it to find
 * this cpu's private data. Backed by the GS base on x86 and TPIDR_EL1 on
 * arm64. */
void arch_set_percpu(void *p);
void *arch_get_percpu(void);

/* Spin-wait hint for a contended lock (pause / yield). */
void arch_cpu_relax(void);

#endif /* LIBOS_ARCH_H */
