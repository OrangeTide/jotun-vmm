/* x86_64.c : arch console and halt for the 64-bit x86 guest.
 *
 * The host runs this guest in long mode and emulates a 16550 UART on COM1, so
 * the console is port I/O. Halt is HLT, which the host sees as a HLT exit. */
#include "arch.h"

#include <stdint.h>

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

/* The ctx_switch restore pops six callee-saved registers (r15, r14, r13, r12,
 * rbp, rbx) and then rets. Lay those down with the entry as the return slot.
 * The entry slot is 16-aligned so that after the ret, rsp is 8 mod 16, which
 * is what the SysV ABI promises a function at its entry. */
void *
stack_init(void *stack, size_t size, void (*entry)(void))
{
    uintptr_t top = ((uintptr_t)stack + size) & ~(uintptr_t)15;
    uint64_t *entry_slot = (uint64_t *)(top - 16);
    uint64_t *sp = entry_slot - 6;

    for (int i = 0; i < 6; i++)
        sp[i] = 0;
    *entry_slot = (uint64_t)(uintptr_t)entry;
    return sp;
}
