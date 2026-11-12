/* x86_64.c : arch console and halt for the 64-bit x86 guest.
 *
 * The host runs this guest in long mode and emulates a 16550 UART on COM1, so
 * the console is port I/O. Halt is HLT, which the host sees as a HLT exit. */
#include "arch.h"
#include "print.h"

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
 * return slot. RFLAGS starts with IF clear (0x2) so the libOS kernel runs
 * non-preemptibly: the scheduler, context switches, and syscall handlers are
 * never interrupted by the event timer, so they need no extra locking. Only a
 * Linux process in user mode runs with IF set (linux_jump), so the timer
 * preempts a user compute loop and nothing else. The entry slot is 16-aligned so
 * that after the ret, rsp is 8 mod 16, as the SysV ABI promises at entry. */
void *
stack_init(void *stack, size_t size, void (*entry)(void))
{
    uintptr_t top = ((uintptr_t)stack + size) & ~(uintptr_t)15;
    uint64_t *entry_slot = (uint64_t *)(top - 16);
    uint64_t *sp = entry_slot - 7;

    sp[0] = 0x2;                    /* RFLAGS: IF clear, reserved bit 1 */
    for (int i = 1; i < 7; i++)
        sp[i] = 0;                  /* r15, r14, r13, r12, rbp, rbx */
    *entry_slot = (uint64_t)(uintptr_t)entry;
    return sp;
}

/****************************************************************
 * Traps: a GDT the interrupt path can reload CS/SS from, an IDT with the
 * event gate, and interrupt-mask helpers.
 ****************************************************************/

/* Up to this many vcpus get their own TSS slot in the GDT. Mirrors MAX_CPUS in
 * task.c; the Linux-compat path runs single-vcpu, so this is headroom. */
#define ARCH_MAX_CPUS 8

/* GDT: null, the kernel code/data pair (0x08/0x10), the user code/data pair at
 * DPL 3 (0x20/0x28) with a reserved slot (0x18) before them, then two slots per
 * cpu for that cpu's 64-bit TSS descriptor (filled at runtime). The user pair
 * lets a Linux process run in CPL 3: the return path builds an IRETQ frame with
 * CS 0x2b and SS 0x23 (the user selectors with RPL 3). The slot order matches
 * the SYSRET rule (SS at base+8, CS at base+16, base 0x18) so a later SYSRET
 * fast path needs no reshuffle. The vcpu runs with the kernel selectors cached
 * from KVM; the table must exist in memory so an interrupt or iret can reload a
 * selector without faulting, and the TSS descriptors give each cpu an
 * interrupt-stack table and a ring-0 stack for faults taken in CPL 3. */
static uint64_t gdt[6 + 2 * ARCH_MAX_CPUS] __attribute__((aligned(16))) = {
    0x0000000000000000ull,          /* 0x00 null */
    0x00af9a000000ffffull,          /* 0x08 kernel code: present, DPL0, L=1 */
    0x00cf92000000ffffull,          /* 0x10 kernel data: present, DPL0, DB=1 */
    0x0000000000000000ull,          /* 0x18 reserved: the SYSRET selector base */
    0x00cff2000000ffffull,          /* 0x20 user data: present, DPL3, DB=1 */
    0x00affa000000ffffull,          /* 0x28 user code: present, DPL3, L=1 */
};

/* The GDT selector of cpu id's TSS descriptor. */
#define TSS_SEL(id) ((uint16_t)((6 + 2 * (id)) * 8))

/* The 64-bit task state segment. rsp0 is the stack the CPU switches to when an
 * interrupt or exception is taken from CPL 3 (a Linux process) and the vector
 * carries no IST; without it a fault in user code would push its frame onto the
 * user rsp or, if that is bad, triple fault. The IST array still gives chosen
 * vectors their own stack: #PF so its handler can demand-map and return even
 * when the faulting stack is the problem, and #DF as a last resort. rsp1/rsp2
 * stay unused (no CPL 1 or 2). */
struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];                /* ist[0] is IST1, ist[6] is IST7 */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

/* IST stack indices used in the IDT gates. IST1 carries #PF so its handler can
 * demand-map and return even when the faulting stack is the problem; IST2
 * carries #DF so any other bad-stack fault is reported rather than tripled. */
#define IST_PF 1
#define IST_DF 2
#define IST_STACK_SZ 8192

static struct tss cpu_tss[ARCH_MAX_CPUS];
static uint8_t ist_pf_stack[ARCH_MAX_CPUS][IST_STACK_SZ] __attribute__((aligned(16)));
static uint8_t ist_df_stack[ARCH_MAX_CPUS][IST_STACK_SZ] __attribute__((aligned(16)));
static uint8_t rsp0_stack[ARCH_MAX_CPUS][IST_STACK_SZ] __attribute__((aligned(16)));

/* Write a 64-bit available-TSS descriptor (type 0x9, present, DPL0) into the
 * two GDT slots at index `slot`. */
static void
set_tss_desc(int slot, uint64_t base, uint32_t limit)
{
    gdt[slot] = (limit & 0xffffULL)
              | ((base & 0xffffffULL) << 16)
              | (0x89ULL << 40)
              | (((uint64_t)(limit >> 16) & 0xfULL) << 48)
              | (((base >> 24) & 0xffULL) << 56);
    gdt[slot + 1] = (base >> 32) & 0xffffffffULL;
}

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

/* The 32 CPU-exception stub entry points, from arch/x86_64_fault.S. */
extern void (*exc_stub_table[32])(void);

/* The page-fault handler entry (arch/x86_64_fault.S). */
extern void pf_isr(void);

/* The register and trap frame fault_report receives, matching the push order
 * in exc_common (GP registers first, then the vector and error code the stub
 * added, then the CPU's own trap frame). */
struct faultframe {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vec, err;
    uint64_t rip, cs, rflags, rsp, ss;
};

static const char *const exc_names[32] = {
    "#DE divide error", "#DB debug", "NMI", "#BP breakpoint",
    "#OF overflow", "#BR bound range", "#UD invalid opcode",
    "#NM device not available", "#DF double fault", "coprocessor overrun",
    "#TS invalid TSS", "#NP segment not present", "#SS stack fault",
    "#GP general protection", "#PF page fault", "reserved",
    "#MF x87 fp", "#AC alignment check", "#MC machine check",
    "#XM SIMD fp", "#VE virtualization", "#CP control protection",
    "reserved", "reserved", "reserved", "reserved", "reserved", "reserved",
    "#HV hypervisor", "#VC vmm communication", "#SX security", "reserved",
};

static void
put_byte(uint8_t b)
{
    static const char hx[] = "0123456789abcdef";
    con_putc(hx[b >> 4]);
    con_putc(hx[b & 0xf]);
}

/* Print the fault and halt. Entered only on a CPU exception, which a healthy
 * guest never takes; it exists so a foreign Linux binary that faults leaves a
 * readable trail instead of a silent triple fault. */
void
fault_report(struct faultframe *f)
{
    uint64_t cr0, cr4, cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));

    con_puts("\n*** CPU exception ");
    con_putu(f->vec);
    if (f->vec < 32) {
        con_puts(" (");
        con_puts(exc_names[f->vec]);
        con_puts(")");
    }
    con_puts(" ***\n  err=");
    con_puthex(f->err);
    con_puts(" rip=");
    con_puthex(f->rip);
    con_puts(" cs=");
    con_puthex(f->cs);
    con_puts(" rflags=");
    con_puthex(f->rflags);
    con_puts("\n  rsp=");
    con_puthex(f->rsp);
    con_puts(" rbp=");
    con_puthex(f->rbp);
    con_puts(" cr2=");
    con_puthex(cr2);
    con_puts("\n  rax=");
    con_puthex(f->rax);
    con_puts(" rbx=");
    con_puthex(f->rbx);
    con_puts(" rcx=");
    con_puthex(f->rcx);
    con_puts(" rdx=");
    con_puthex(f->rdx);
    con_puts("\n  rsi=");
    con_puthex(f->rsi);
    con_puts(" rdi=");
    con_puthex(f->rdi);
    con_puts("\n  r8 =");
    con_puthex(f->r8);
    con_puts(" r9 =");
    con_puthex(f->r9);
    con_puts(" r10=");
    con_puthex(f->r10);
    con_puts(" r11=");
    con_puthex(f->r11);
    con_puts("\n  r12=");
    con_puthex(f->r12);
    con_puts(" r13=");
    con_puthex(f->r13);
    con_puts(" r14=");
    con_puthex(f->r14);
    con_puts(" r15=");
    con_puthex(f->r15);

    /* CPU feature state. If OSXSAVE is clear, xgetbv itself is #UD, so only read
     * XCR0 when it is set. A VEX/EVEX opcode faulting with XCR0 missing its AVX
     * bits is the classic "glibc IFUNC picked an instruction the guest never
     * enabled" trap. */
    con_puts("\n  cr0=");
    con_puthex(cr0);
    con_puts(" cr4=");
    con_puthex(cr4);
    con_puts(" xcr0=");
    if (cr4 & (1ULL << 18)) {
        uint32_t lo, hi;
        __asm__ volatile ("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        con_puthex(((uint64_t)hi << 32) | lo);
    } else {
        con_puts("(osxsave off)");
    }

    /* The bytes at RIP. For #UD this classifies the fault at a glance: 0f 0b is
     * a deliberate ud2 (abort/trap, the real cause is upstream), a c5/c4/62
     * prefix is a VEX/EVEX (AVX) instruction, anything else is likely a wild
     * jump. The CPU fetched the faulting instruction, so RIP is mapped; stop at
     * the page boundary so the dump never touches a second, possibly unmapped
     * page. */
    if (f->vec == 6 || f->vec == 14 || f->vec == 13) {
        const uint8_t *p = (const uint8_t *)(uintptr_t)f->rip;
        uint64_t page_end = (f->rip | 0xFFFULL) + 1;
        con_puts("\n  code=");
        for (int i = 0; i < 16 && f->rip + (uint64_t)i < page_end; i++) {
            put_byte(p[i]);
            con_putc(' ');
        }
    }
    con_puts("\nhalted\n");

    for (;;)
        __asm__ volatile ("cli; hlt");
}

static void
idt_set(int vec, void (*handler)(void), int ist)
{
    uintptr_t a = (uintptr_t)handler;
    idt[vec].off_lo = (uint16_t)a;
    idt[vec].sel = 0x08;            /* kernel code selector */
    idt[vec].ist = (uint8_t)(ist & 0x7);
    idt[vec].attr = 0x8e;           /* present, DPL0, 64-bit interrupt gate */
    idt[vec].off_mid = (uint16_t)(a >> 16);
    idt[vec].off_hi = (uint32_t)(a >> 32);
    idt[vec].zero = 0;
}

void
arch_traps_init(unsigned long id)
{
    struct desc_ptr gp = { sizeof gdt - 1, (uint64_t)(uintptr_t)gdt };
    __asm__ volatile ("lgdt %0" : : "m"(gp));

    /* This cpu's TSS: rsp0 is the ring-0 stack for a fault taken in CPL 3, the
     * IST slots point at its private fault stacks, rsp1/rsp2 stay zero (no CPL
     * 1 or 2). iomap_base past the limit means no I/O permission bitmap. */
    if (id < ARCH_MAX_CPUS) {
        struct tss *t = &cpu_tss[id];
        for (unsigned i = 0; i < sizeof *t; i++)
            ((uint8_t *)t)[i] = 0;
        t->rsp[0] =
            (uint64_t)(uintptr_t)(rsp0_stack[id] + IST_STACK_SZ);
        t->ist[IST_PF - 1] =
            (uint64_t)(uintptr_t)(ist_pf_stack[id] + IST_STACK_SZ);
        t->ist[IST_DF - 1] =
            (uint64_t)(uintptr_t)(ist_df_stack[id] + IST_STACK_SZ);
        t->iomap_base = (uint16_t)sizeof *t;
        set_tss_desc(6 + 2 * (int)id, (uint64_t)(uintptr_t)t, sizeof *t - 1);
        __asm__ volatile ("ltr %0" : : "r"(TSS_SEL(id)));
    }

    /* CPU exception vectors first, so a fault during bring-up is reported
     * rather than escalating to a silent triple fault. */
    for (int v = 0; v < 32; v++)
        idt_set(v, exc_stub_table[v], 0);

    /* Page faults get a handler that demand-maps the Linux user window (and
     * reports anything else), on a private stack so a fault caused by a bad
     * user rsp is still handled. Double faults get their own stack too, so any
     * other bad-stack fault is reported instead of tripling. */
    idt_set(14, pf_isr, IST_PF);
    idt_set(8, exc_stub_table[8], IST_DF);

    idt_set(0x20, isr_event, 0);    /* JOTUN_EVENT_VECTOR */
    struct desc_ptr ip = { sizeof idt - 1, (uint64_t)(uintptr_t)idt };
    __asm__ volatile ("lidt %0" : : "m"(ip));
}

/* Whether the interrupt save path uses XSAVE (wider vector state, including
 * AVX) rather than FXSAVE. Read by the interrupt stub in x86_64_isr.S, so it is
 * a plain global, not static. Set by arch_fpu_init when XSAVE is available. */
uint8_t fpu_use_xsave;
/* The XSAVE area size for the enabled XCR0 state, for a sanity check against the
 * fixed buffer the interrupt stub carves. */
static uint32_t xsave_area_size;

static void
cpuid(uint32_t leaf, uint32_t subleaf,
      uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf), "c"(subleaf));
}

/* Enable SSE and, where the CPU supports it, AVX. Clear CR0.EM (no x87
 * emulation) and set CR0.MP; set CR4.OSFXSR so SSE and FXSAVE/FXRSTOR work. SSE
 * exceptions stay masked in MXCSR and CR4.OSXMMEXCPT is left clear, so no #XM
 * handler is needed. FNINIT puts the x87/MMX state in a known state.
 *
 * When XSAVE is present, also set CR4.OSXSAVE and enable x87, SSE, and (if the
 * CPU has it) AVX in XCR0. That lets a Linux program's glibc pick its AVX code
 * paths, and tells the interrupt save path to use XSAVE so the wider vector
 * state survives a preemptive switch (the fallback is SSE-only FXSAVE). AVX-512
 * is intentionally left out of XCR0 to keep the save area small and bounded. */
/* x86 long mode always runs with paging on (the host sets up the initial page
 * tables), so the MMU needs no early bring-up here. */
void
arch_mmu_init(void)
{
}

/* x86 instruction and data caches are coherent, so freshly written code needs no
 * cache maintenance to become executable (see arch.h). */
void
arch_sync_icache(void *addr, size_t len)
{
    (void)addr;
    (void)len;
}

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

    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (!(c & (1u << 26)))          /* no XSAVE: SSE-only FXSAVE path stands */
        return;

    cr4 |= (1UL << 18);             /* OSXSAVE: XSAVE/XGETBV/XSETBV and AVX */
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));

    uint64_t xcr0 = 0x3;            /* x87 + SSE */
    if (c & (1u << 28))             /* AVX */
        xcr0 |= 0x4;
    __asm__ volatile ("xsetbv"
                      : : "c"(0u), "a"((uint32_t)xcr0), "d"((uint32_t)(xcr0 >> 32)));

    cpuid(0xD, 0, &a, &b, &c, &d);
    xsave_area_size = b;            /* bytes needed for the enabled XCR0 state */
    /* The interrupt stub carves a fixed 1024-byte, 64-byte-aligned area. x87 +
     * SSE + AVX needs 832, so this always fits; guard anyway, and if some CPU
     * ever reports more, drop AVX and keep the SSE-only FXSAVE path. */
    if (xsave_area_size > 1024) {
        __asm__ volatile ("xsetbv"
                          : : "c"(0u), "a"(3u), "d"(0u));   /* XCR0 = x87 + SSE */
        return;                     /* fpu_use_xsave stays 0 */
    }
    fpu_use_xsave = 1;
}

/* Enable the CPU's ring-3 containment features that back privilege separation.
 * SMEP (CR4 bit 20) stops ring 0 from executing an instruction fetched from a
 * page mapped user, so corrupted kernel control flow cannot jump into
 * user-controlled code. All kernel code lives below VM_USER_LO in supervisor
 * pages, so nothing the kernel executes carries the user bit and SMEP changes no
 * legitimate behavior. SMAP (CR4 bit 21) would also stop ring 0 reading or
 * writing user pages outside a stac/clac window; it is left off until every
 * kernel access to user memory is bracketed, or it would fault immediately.
 * Each feature is gated on its CPUID.7.0:EBX bit so a CPU without it still
 * boots. Per-cpu; call once on each. */
void
arch_cpu_harden(void)
{
    uint32_t a, b, c, d;
    cpuid(7, 0, &a, &b, &c, &d);
    if (b & (1u << 7)) {                /* SMEP supported */
        unsigned long cr4;
        __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1UL << 20);             /* SMEP */
        __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));
    }
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

/****************************************************************
 * Local APIC (x2APIC mode). The host gives the VM an in-kernel APIC and
 * delivers the event vector to it (preemption ticks and device events). In
 * x2APIC mode the APIC is reached through MSRs, not an MMIO page, so a
 * freestanding guest needs no device mapping: enabling it and sending EOI are
 * just wrmsr. The event ISR writes EOI (see x86_64_isr.S, which reads
 * apic_enabled) so the APIC will deliver the next interrupt.
 ****************************************************************/
#define MSR_APIC_BASE  0x1Bu
#define MSR_X2APIC_SVR 0x80Fu       /* spurious-interrupt vector register */

/* Nonzero once this build enabled the x2APIC; read by the ISR (plain global, so
 * not static) to decide whether to send EOI. */
uint8_t apic_enabled;

static uint64_t
rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void
wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ volatile ("wrmsr"
                      : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

/* Enable this cpu's local APIC in x2APIC mode so it can receive the event
 * vector. Called once per cpu during bring-up. If the CPU has no x2APIC the
 * APIC stays off and the timer only advances the host-maintained clock. */
void
arch_apic_init(void)
{
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (!(c & (1u << 21)))              /* no x2APIC */
        return;
    wrmsr(MSR_APIC_BASE, rdmsr(MSR_APIC_BASE) | (1u << 10) | (1u << 11));
    wrmsr(MSR_X2APIC_SVR, 0x1FF);       /* software enable, spurious vector 0xFF */
    apic_enabled = 1;
}

/* Point this cpu's TSS.rsp0 at a thread's kernel stack, so an interrupt taken
 * from ring 3 lands there rather than on the shared fault stack. Find the
 * running cpu's TSS through the task register (TR) and the GDT descriptor it
 * selects, so no cpu-id plumbing is needed. */
void
arch_set_kstack(uint64_t top)
{
    uint16_t tr;
    __asm__ volatile ("str %0" : "=r"(tr));
    unsigned slot = (unsigned)(tr >> 3);
    uint64_t lo = gdt[slot], hi = gdt[slot + 1];
    uint64_t base = ((lo >> 16) & 0xffffffULL)
                  | (((lo >> 56) & 0xffULL) << 24)
                  | ((hi & 0xffffffffULL) << 32);
    ((struct tss *)(uintptr_t)base)->rsp[0] = top;
}
