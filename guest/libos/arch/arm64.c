/* arm64.c : arch console and halt for the AArch64 guest.
 *
 * The guest runs at EL1 with no port I/O, so the console is a memory-mapped
 * PL011-style UART. Halt is a store to the poweroff register, which the host
 * sees as an MMIO exit that stops the run. */
#include "arch.h"

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
