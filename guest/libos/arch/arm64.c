/* arm64.c : arch console and halt for the AArch64 guest.
 *
 * The guest runs at EL1 with no port I/O, so the console is a memory-mapped
 * PL011-style UART. Halt is a store to the poweroff register, which the host
 * sees as an MMIO exit that stops the run. */
#include "arch.h"
#include "print.h"

#include <stdint.h>

#define UART_BASE   0x09000000ull
#define UART_DR     (UART_BASE + 0x00)      /* data register */
#define UART_FR     (UART_BASE + 0x18)      /* flag register */
#define FR_TXFF     (1u << 5)               /* transmit FIFO full */
#define POWEROFF    0x09010000ull

void
con_raw_putc(char c)
{
    volatile unsigned int *fr = (volatile unsigned int *)UART_FR;
    volatile unsigned int *dr = (volatile unsigned int *)UART_DR;

    while (*fr & FR_TXFF)
        ;
    *dr = (unsigned int)(unsigned char)c;
}

void
arch_halt(void)
{
    *(volatile unsigned int *)POWEROFF = 1;
    for (;;)
        ;
}

/* ctx_switch saves twenty 8-byte slots: x19-x28, x29 (fp), x30 (lr), then the
 * eight callee-saved vector registers d8-d15. The restore loads them and rets
 * to x30, so put entry in the x30 slot (the twelfth word) and zero the rest,
 * including the vector slots. The frame is a multiple of 16 bytes, so sp stays
 * aligned. */
void *
stack_init(void *stack, size_t size, void (*entry)(void))
{
    uintptr_t top = ((uintptr_t)stack + size) & ~(uintptr_t)15;
    uint64_t *sp = (uint64_t *)top - 20;

    for (int i = 0; i < 20; i++)
        sp[i] = 0;
    sp[11] = (uint64_t)(uintptr_t)entry;    /* x30: ret lands here */
    return sp;
}

/* The exception vector table (arch/arm64_linuxsys.S). Installing it lets the
 * Linux-compat guest take SVC traps and demand-map faults; for other guests it
 * only means a stray fault is reported instead of silently escaping. */
extern char arm64_vectors[];

/* Print an unrecoverable fault and halt. Entered from the exception vectors on a
 * fault that is not a demand-mappable page or on an SError; a healthy guest
 * never gets here. It exists so a foreign Linux binary that faults leaves a
 * readable trail instead of vanishing. */
void
arm64_fault_report(uint64_t far, uint64_t esr, uint64_t elr)
{
    con_puts("\n*** EL1 fault ***\n  esr=");
    con_puthex(esr);
    con_puts(" ec=");
    con_puthex((esr >> 26) & 0x3f);
    con_puts(" far=");
    con_puthex(far);
    con_puts("\n  elr=");
    con_puthex(elr);
    con_puts("\nhalted\n");

    arch_halt();
    for (;;)
        ;
}

/* arm64 preemption is deferred: injecting IRQs without a vGIC (KVM) and the HVF
 * path both need hardware to validate, and the guest runs at EL1 with DAIF
 * masked. So the vectors handle only synchronous exceptions (SVC and faults);
 * IRQ/FIQ slots just hang. The mask helpers below still work, so the scheduler's
 * critical-region guards are correct, they simply never see an asynchronous
 * interrupt yet. */
void
arch_traps_init(unsigned long id)
{
    (void)id;
    __asm__ volatile ("msr vbar_el1, %0; isb" : : "r"(arm64_vectors));
}

/* Enable FP/SIMD at EL1: set CPACR_EL1.FPEN to 0b11 so FP and Advanced SIMD
 * instructions do not trap, then synchronize with an ISB. */
void
arch_fpu_init(void)
{
    uint64_t v;
    __asm__ volatile ("mrs %0, cpacr_el1" : "=r"(v));
    v |= (3UL << 20);               /* FPEN = 0b11 */
    __asm__ volatile ("msr cpacr_el1, %0; isb" : : "r"(v));
}

void
arch_irq_enable(void)
{
    __asm__ volatile ("msr daifclr, #2" ::: "memory");
}

void
arch_irq_disable(void)
{
    __asm__ volatile ("msr daifset, #2" ::: "memory");
}

unsigned long
arch_irq_save(void)
{
    unsigned long f;
    __asm__ volatile ("mrs %0, daif; msr daifset, #2" : "=r"(f) :: "memory");
    return f;
}

void
arch_irq_restore(unsigned long flags)
{
    __asm__ volatile ("msr daif, %0" : : "r"(flags) : "memory");
}

void
arch_set_percpu(void *p)
{
    __asm__ volatile ("msr tpidr_el1, %0" : : "r"(p) : "memory");
}

void *
arch_get_percpu(void)
{
    void *p;
    __asm__ volatile ("mrs %0, tpidr_el1" : "=r"(p));
    return p;
}

void
arch_cpu_relax(void)
{
    __asm__ volatile ("yield" ::: "memory");
}
