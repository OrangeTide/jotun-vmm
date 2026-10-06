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
