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
con_raw_putc(char c)
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

/* The ctx_switch restore pops RFLAGS then six callee-saved registers (r15,
 * r14, r13, r12, rbp, rbx) and rets. Lay those down with the entry as the
 * return slot. RFLAGS starts with IF set (0x202) so a fresh task runs with
 * interrupts enabled and is preemptible. The entry slot is 16-aligned so that
 * after the ret, rsp is 8 mod 16, as the SysV ABI promises at entry. */
void *
stack_init(void *stack, size_t size, void (*entry)(void))
{
    uintptr_t top = ((uintptr_t)stack + size) & ~(uintptr_t)15;
    uint64_t *entry_slot = (uint64_t *)(top - 16);
    uint64_t *sp = entry_slot - 7;

    sp[0] = 0x202;                  /* RFLAGS: IF set, reserved bit 1 */
    for (int i = 1; i < 7; i++)
        sp[i] = 0;                  /* r15, r14, r13, r12, rbp, rbx */
    *entry_slot = (uint64_t)(uintptr_t)entry;
    return sp;
}

/****************************************************************
 * Traps: a GDT the interrupt path can reload CS/SS from, an IDT with the
 * event gate, and interrupt-mask helpers.
 ****************************************************************/

/* GDT: null, 64-bit code (0x08), data (0x10). The vcpu already runs with these
 * cached from KVM; the table must exist in memory so an interrupt or iret can
 * reload the selectors without faulting. */
static uint64_t gdt[3] __attribute__((aligned(16))) = {
    0x0000000000000000ull,          /* null */
    0x00af9a000000ffffull,          /* code: present, DPL0, L=1 */
    0x00cf92000000ffffull,          /* data: present, DPL0, DB=1 */
};

struct idt_gate {
    uint16_t off_lo;
    uint16_t sel;
    uint8_t  ist;
    uint8_t  attr;
    uint16_t off_mid;
    uint32_t off_hi;
    uint32_t zero;
} __attribute__((packed));

static struct idt_gate idt[256] __attribute__((aligned(16)));

struct desc_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

extern void isr_event(void);        /* arch/x86_64_isr.S */

static void
idt_set(int vec, void (*handler)(void))
{
    uintptr_t a = (uintptr_t)handler;
    idt[vec].off_lo = (uint16_t)a;
    idt[vec].sel = 0x08;            /* kernel code selector */
    idt[vec].ist = 0;
    idt[vec].attr = 0x8e;           /* present, DPL0, 64-bit interrupt gate */
    idt[vec].off_mid = (uint16_t)(a >> 16);
    idt[vec].off_hi = (uint32_t)(a >> 32);
    idt[vec].zero = 0;
}

void
arch_traps_init(void)
{
    struct desc_ptr gp = { sizeof gdt - 1, (uint64_t)(uintptr_t)gdt };
    __asm__ volatile ("lgdt %0" : : "m"(gp));

    idt_set(0x20, isr_event);       /* JOTUN_EVENT_VECTOR */
    struct desc_ptr ip = { sizeof idt - 1, (uint64_t)(uintptr_t)idt };
    __asm__ volatile ("lidt %0" : : "m"(ip));
}

/* Enable SSE: clear CR0.EM (no x87 emulation), set CR0.MP, and set CR4.OSFXSR
 * so SSE instructions and FXSAVE/FXRSTOR work. SSE exceptions stay masked in
 * MXCSR and CR4.OSXMMEXCPT is left clear, so no #XM handler is needed. FNINIT
 * puts the x87/MMX state in a known state. */
void
arch_fpu_init(void)
{
    unsigned long cr0, cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1UL << 2);             /* EM = 0 */
    cr0 |= (1UL << 1);              /* MP = 1 */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1UL << 9);              /* OSFXSR = 1 */
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));
    __asm__ volatile ("fninit");
    unsigned int mxcsr = 0x1f80;    /* all SSE exceptions masked */
    __asm__ volatile ("ldmxcsr %0" : : "m"(mxcsr));
}

void
arch_irq_enable(void)
{
    __asm__ volatile ("sti" ::: "memory");
}

void
arch_irq_disable(void)
{
    __asm__ volatile ("cli" ::: "memory");
}

unsigned long
arch_irq_save(void)
{
    unsigned long flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

void
arch_irq_restore(unsigned long flags)
{
    __asm__ volatile ("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

#define MSR_GS_BASE 0xc0000101u

void
arch_set_percpu(void *p)
{
    uint64_t v = (uint64_t)(uintptr_t)p;
    __asm__ volatile ("wrmsr"
                      : : "c"(MSR_GS_BASE), "a"((uint32_t)v),
                          "d"((uint32_t)(v >> 32)));
}

void *
arch_get_percpu(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_GS_BASE));
    return (void *)(uintptr_t)(((uint64_t)hi << 32) | lo);
}

void
arch_cpu_relax(void)
{
    __asm__ volatile ("pause" ::: "memory");
}
