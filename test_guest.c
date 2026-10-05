/* test_guest.c : bare-metal 32-bit guest that greets over COM1 and halts.
 *
 * Built freestanding for i386 and run by the VMM in 32-bit protected mode with
 * flat segments and paging off. It talks to the emulated 16550 UART on COM1,
 * then halts, which the VMM sees as a HLT exit. */

#define COM1 0x3f8
#define LSR_THRE 0x20       /* transmit holding register empty */

static inline void
outb(unsigned short port, unsigned char val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline unsigned char
inb(unsigned short port)
{
    unsigned char val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static void
uart_putc(char c)
{
    while (!(inb(COM1 + 5) & LSR_THRE))
        ;
    outb(COM1, (unsigned char)c);
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
    uart_puts("Hello from the guest via the 16550 UART!\n");
    __asm__ volatile ("hlt");
    for (;;)
        ;
}
