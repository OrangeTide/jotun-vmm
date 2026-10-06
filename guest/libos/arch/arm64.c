/* arm64.c : arch console and halt for the AArch64 guest.
 *
 * The guest runs at EL1 with no port I/O, so the console is a memory-mapped
 * PL011-style UART. Halt is a store to the poweroff register, which the host
 * sees as an MMIO exit that stops the run. */
#include "arch.h"

#include <stdint.h>

#define UART_BASE   0x09000000ull
#define UART_DR     (UART_BASE + 0x00)      /* data register */
#define UART_FR     (UART_BASE + 0x18)      /* flag register */
#define FR_TXFF     (1u << 5)               /* transmit FIFO full */
#define POWEROFF    0x09010000ull

void
con_putc(char c)
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

/* ctx_switch saves twelve 8-byte slots: x19-x28, then x29 (fp) and x30 (lr).
 * The restore loads them and rets to x30, so put entry in the x30 slot (the
 * twelfth word). The frame is a multiple of 16 bytes, so sp stays aligned. */
void *
stack_init(void *stack, size_t size, void (*entry)(void))
{
    uintptr_t top = ((uintptr_t)stack + size) & ~(uintptr_t)15;
    uint64_t *sp = (uint64_t *)top - 12;

    for (int i = 0; i < 12; i++)
        sp[i] = 0;
    sp[11] = (uint64_t)(uintptr_t)entry;    /* x30: ret lands here */
    return sp;
}

/* arm64 preemption is deferred: injecting IRQs without a vGIC (KVM) and the
 * HVF path both need hardware to validate, and the guest runs at EL1 with DAIF
 * masked. The trap table (VBAR_EL1 exception vectors) is set up when that work
 * lands. The mask helpers below still work, so the scheduler's critical-region
 * guards are correct, they simply never see an asynchronous interrupt yet. */
void
arch_traps_init(void)
{
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
