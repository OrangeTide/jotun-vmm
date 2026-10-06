/* x86_64.c : arch console and halt for the 64-bit x86 guest.
 *
 * The host runs this guest in long mode and emulates a 16550 UART on COM1, so
 * the console is port I/O. Halt is HLT, which the host sees as a HLT exit. */
#include "arch.h"

#define COM1        0x3f8
#define LSR_THRE    0x20        /* transmit holding register empty */

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

void
con_putc(char c)
{
    while (!(inb(COM1 + 5) & LSR_THRE))
        ;
    outb(COM1, (unsigned char)c);
}

void
arch_halt(void)
{
    for (;;)
        __asm__ volatile ("hlt");
}
