/* test_guest_arm64.c : bare-metal AArch64 guest, MMIO console.
 *
 * Built freestanding for arm64 and run by the VMM at EL1. It has no port I/O,
 * so it talks to a memory-mapped PL011-style UART, then stores to the poweroff
 * register, which the VMM sees as an MMIO exit that stops the run. */

#define UART_BASE 0x09000000ull
#define UART_DR   (UART_BASE + 0x00)    /* data register */
#define UART_FR   (UART_BASE + 0x18)    /* flag register */
#define FR_TXFF   (1u << 5)             /* transmit FIFO full */
#define POWEROFF  0x09010000ull

static void
uart_putc(char c)
{
    volatile unsigned int *fr = (volatile unsigned int *)UART_FR;
    volatile unsigned int *dr = (volatile unsigned int *)UART_DR;
    while (*fr & FR_TXFF)
        ;
    *dr = (unsigned int)(unsigned char)c;
}

static void
uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

void
_start(void)
{
    uart_puts("Hello from the arm64 guest via MMIO UART!\n");
    *(volatile unsigned int *)POWEROFF = 1;
    for (;;)
        ;
}
