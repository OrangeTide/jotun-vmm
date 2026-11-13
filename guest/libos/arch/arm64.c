/* arm64.c : arch console and halt for the AArch64 guest.
 *
 * The guest runs at EL1 with no port I/O, so the console is a memory-mapped
 * PL011-style UART. Halt is a store to the poweroff register, which the host
 * sees as an MMIO exit that stops the run. */
#include "arch.h"
#include "print.h"
#include "boot.h"
#include "jotun_abi.h"

#include <stdint.h>

/* The device MMIO band just above guest RAM; see JOTUN_* in abi/jotun_abi.h and
 * MMIO_UART_BASE/MMIO_POWEROFF in host/uart.h, which these must match. */
#define UART_BASE   0xE0000000ull
#define UART_DR     (UART_BASE + 0x00)      /* data register */
#define UART_FR     (UART_BASE + 0x18)      /* flag register */
#define FR_RXFE     (1u << 4)               /* receive FIFO empty */
#define FR_TXFF     (1u << 5)               /* transmit FIFO full */
#define POWEROFF    0xE0010000ull

void
con_raw_putc(char c)
{
    volatile unsigned int *fr = (volatile unsigned int *)UART_FR;
    volatile unsigned int *dr = (volatile unsigned int *)UART_DR;

    while (*fr & FR_TXFF)
        ;
    *dr = (unsigned int)(unsigned char)c;
}

int
con_raw_getc(void)
{
    volatile unsigned int *fr = (volatile unsigned int *)UART_FR;
    volatile unsigned int *dr = (volatile unsigned int *)UART_DR;

    if (*fr & FR_RXFE)
        return -1;
    return (int)(*dr & 0xff);
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
/* Implemented in linuxelf.c: print the faulting process's identity and arena. */
void linux_fault_ident(void);

/* An instruction-fetch abort taken from EL1h means the kernel tried to execute
 * in unmapped memory: a wild branch through a corrupted pointer or return
 * address. Called from abort_entry with the saved register frame (x0..x30 at
 * regs[0..30]) before pf_handle can mask it by mapping the page. Dumping every
 * register shows which one held the bad target, x30 shows the call site, and the
 * live SP_EL1 pins which stack it faulted on. */
void
arm64_wild_branch_report(const uint64_t *regs, uint64_t far, uint64_t elr,
                         uint64_t sp)
{
    con_puts("\n*** wild kernel branch ***\n  far=");
    con_puthex(far);
    con_puts(" elr=");
    con_puthex(elr);
    con_puts("\n  x0..x30:");
    for (int i = 0; i <= 30; i++) {
        con_puts(i % 4 == 0 ? "\n   " : " ");
        con_puthex(regs[i]);
    }
    con_puts("\n  fp(x29)=");
    con_puthex(regs[29]);
    con_puts(" sp_el1=");
    con_puthex(sp);
    linux_fault_ident();
    con_puts("\nhalted\n");
    arch_halt();
    for (;;)
        ;
}

void
arm64_fault_report(uint64_t far, uint64_t esr, uint64_t elr, uint64_t spsr,
                   uint64_t fp, uint64_t lr)
{
    con_puts("\n*** EL1 fault ***\n  esr=");
    con_puthex(esr);
    con_puts(" ec=");
    con_puthex((esr >> 26) & 0x3f);
    con_puts(" far=");
    con_puthex(far);
    con_puts("\n  elr=");
    con_puthex(elr);
    con_puts(" spsr=");
    con_puthex(spsr);
    con_puts(" from=");
    con_puts((spsr & 0xf) == 0x4 ? "EL1t(user)" : "EL1h(kernel)");
    /* Dump the words around the faulting PC: zeros mean an unfilled page
     * executed as code, non-zero garbage means a stale/wrong page. The fetch
     * that faulted proves the page is mapped, so this read is safe. */
    con_puts("\n  code@elr-4..+8=");
    for (int i = -1; i <= 2; i++) {
        con_puthex(*(const volatile uint32_t *)(uintptr_t)(elr + (int64_t)i * 4));
        con_putc(' ');
    }
    linux_fault_ident();
    /* The fault-time link register and a walk of the saved frame chain name the
     * kernel call path that branched to the bad PC. Each frame is {saved fp,
     * saved lr}; stop when fp leaves the kernel's identity region or is zero. */
    con_puts("\n  lr=");
    con_puthex(lr);
    con_puts("\n  frames:");
    for (int i = 0; i < 8 && fp >= 0x400000 && fp < 0x300000000ULL && (fp & 7) == 0; i++) {
        uint64_t slr = *(const volatile uint64_t *)(uintptr_t)(fp + 8);
        con_putc(' ');
        con_puthex(slr);
        fp = *(const volatile uint64_t *)(uintptr_t)fp;
    }
    con_puts("\nhalted\n");

    arch_halt();
    for (;;)
        ;
}

/* Make freshly written code executable (see arch.h). The instruction cache is
 * not coherent with data stores on AArch64, so clean the stores to the point of
 * unification, then invalidate the instruction cache over the range. A 64-byte
 * step covers the smallest cache line this guest runs on; cleaning a larger line
 * more than once is harmless. */
void
arch_sync_icache(void *addr, size_t len)
{
    uintptr_t base = (uintptr_t)addr & ~(uintptr_t)63;
    uintptr_t end = (uintptr_t)addr + len;
    for (uintptr_t p = base; p < end; p += 64)
        __asm__ volatile ("dc cvau, %0" : : "r"(p) : "memory");
    __asm__ volatile ("dsb ish" ::: "memory");
    for (uintptr_t p = base; p < end; p += 64)
        __asm__ volatile ("ic ivau, %0" : : "r"(p) : "memory");
    __asm__ volatile ("dsb ish; isb" ::: "memory");
}

/* Install the exception vectors (VBAR_EL1). The app runs at EL1t with IRQ
 * unmasked, so the preemption timer vectors through the 0x080 (current EL, SP_EL0)
 * IRQ slot into el1t_irq; the kernel runs with IRQs masked, so it is never
 * preempted. */
void
arch_traps_init(unsigned long id)
{
    (void)id;
    __asm__ volatile ("msr vbar_el1, %0; isb" : : "r"(arm64_vectors));
}

/* The scheduler's event entry (task.c), reached from the IRQ vector. */
extern void irq_event(unsigned cpl);

/* ---- GIC, used only on the KVM host ---------------------------------------
 * KVM's only interrupt-injection path is an in-kernel GIC, so on that host the
 * guest drives a minimal slice of it: enable the distributor and CPU interface,
 * configure the one shared peripheral interrupt the host pulses for the
 * preemption tick, and acknowledge each one. Two GIC generations are supported,
 * chosen by the host (boot_irq_model): GICv3 reaches the CPU interface through
 * system registers (ICC_*) and a per-cpu redistributor; GICv2 (for example the
 * GIC-400 on a Raspberry Pi) has an MMIO CPU interface (GICC) instead.
 * Hypervisor.framework has no in-guest controller and uses JOTUN_IRQ_LINE, so
 * none of this runs there.
 *
 * The GICv3 CPU interface is reached through system registers; spell them with
 * the raw S<op0>_<op1>_<Cn>_<Cm>_<op2> encodings so the code builds under either
 * assembler without a GIC-specific -march. */
#define ICC_IAR1_EL1    "S3_0_C12_C12_0"    /* acknowledge: read the pending INTID */
#define ICC_EOIR1_EL1   "S3_0_C12_C12_1"    /* end of interrupt: write the INTID */
#define ICC_PMR_EL1     "S3_0_C4_C6_0"      /* priority mask */
#define ICC_CTLR_EL1    "S3_0_C12_C12_4"    /* CPU interface control (EOImode) */
#define ICC_SRE_EL1     "S3_0_C12_C12_5"    /* system register enable */
#define ICC_IGRPEN1_EL1 "S3_0_C12_C12_7"    /* group 1 interrupt enable */

/* Distributor (GICD) registers, shared by both generations. */
#define GICD_CTLR       0x0000
#define GICD_IGROUPR    0x0080              /* 1 bit per intid (v3 grouping) */
#define GICD_ISENABLER  0x0100             /* 1 bit per intid */
#define GICD_IPRIORITYR 0x0400             /* 1 byte per intid */
#define GICD_ITARGETSR  0x0800             /* 1 byte per intid (v2 cpu mask) */
#define GICD_ICFGR      0x0c00             /* 2 bits per intid */
#define GICD_IROUTER    0x6000             /* 8 bytes per intid (v3 SPI routing) */

/* GICv3 redistributor. */
#define GICR_WAKER      0x0014
#define GICR_WAKER_PS   (1u << 1)          /* ProcessorSleep */
#define GICR_WAKER_CA   (1u << 2)          /* ChildrenAsleep */
#define GICD_CTLR_ARE        (1u << 4)     /* affinity routing enable (v3) */
#define GICD_CTLR_ENABLE_G1  (1u << 1)     /* enable group 1 (v3 single security) */

/* GICv2 MMIO CPU interface (GICC). */
#define GICC_CTLR       0x0000
#define GICC_PMR        0x0004
#define GICC_IAR        0x000c
#define GICC_EOIR       0x0010

#define SPI_INTID       JOTUN_EVENT_SPI

static inline void
mmio32(uint64_t addr, uint32_t v)
{
    *(volatile uint32_t *)(uintptr_t)addr = v;
}

static inline uint32_t
mmio32_r(uint64_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}

/* The interrupt-delivery model this host uses (boot_irq_model), cached so the
 * hot IRQ path does not reread the boot block each time. */
static uint32_t irq_model = JOTUN_IRQ_LINE;

/* Configure the tick SPI in the distributor: edge-triggered, a mid priority, and
 * enabled. Routing and grouping differ by generation, so the caller sets those. */
static void
gicd_spi_common(uint64_t gicd, unsigned id)
{
    uint64_t cfg = gicd + GICD_ICFGR + (id / 16) * 4;   /* 2 bits, edge == 0b10 */
    unsigned sh = (id % 16) * 2;
    mmio32(cfg, (mmio32_r(cfg) & ~(3u << sh)) | (2u << sh));
    *(volatile uint8_t *)(uintptr_t)(gicd + GICD_IPRIORITYR + id) = 0xa0;
    mmio32(gicd + GICD_ISENABLER + (id / 32) * 4, 1u << (id % 32));
}

/* GICv3: wake the redistributor, enable the distributor with affinity routing,
 * route the SPI to this cpu, and enable the system-register CPU interface. */
static void
gic_v3_init(void)
{
    uint64_t gicd = JOTUN_GICD_BASE, gicr = JOTUN_GICR_BASE;
    unsigned id = SPI_INTID;

    uint32_t w = mmio32_r(gicr + GICR_WAKER);
    mmio32(gicr + GICR_WAKER, w & ~GICR_WAKER_PS);
    while (mmio32_r(gicr + GICR_WAKER) & GICR_WAKER_CA)
        ;

    mmio32(gicd + GICD_CTLR, GICD_CTLR_ARE | GICD_CTLR_ENABLE_G1);
    mmio32(gicd + GICD_IGROUPR + (id / 32) * 4,
           mmio32_r(gicd + GICD_IGROUPR + (id / 32) * 4) | (1u << (id % 32)));
    *(volatile uint64_t *)(uintptr_t)(gicd + GICD_IROUTER + id * 8) = 0;
    gicd_spi_common(gicd, id);

    uint64_t v;
    __asm__ volatile ("mrs %0, " ICC_SRE_EL1 : "=r"(v));
    __asm__ volatile ("msr " ICC_SRE_EL1 ", %0; isb" : : "r"(v | 1));
    __asm__ volatile ("msr " ICC_PMR_EL1 ", %0" : : "r"((uint64_t)0xff));
    __asm__ volatile ("mrs %0, " ICC_CTLR_EL1 : "=r"(v));
    __asm__ volatile ("msr " ICC_CTLR_EL1 ", %0" : : "r"(v & ~(uint64_t)2)); /* EOImode=0 */
    __asm__ volatile ("msr " ICC_IGRPEN1_EL1 ", %0; isb" : : "r"((uint64_t)1));
}

/* GICv2: enable the distributor, route the SPI to cpu 0 through ITARGETSR, and
 * enable the MMIO CPU interface with all priorities unmasked. KVM's in-kernel
 * vGICv2 presents every interrupt as group 0 and signals group 0 to the cpu as
 * an FIQ, so the guest keeps FIQ unmasked and the EL1t FIQ vector also routes to
 * the preemption handler (see arm64_linuxsys.S). */
static void
gic_v2_init(void)
{
    uint64_t gicd = JOTUN_GICD_BASE, gicc = JOTUN_GICC_BASE;
    unsigned id = SPI_INTID;

    mmio32(gicd + GICD_CTLR, 1);                        /* enable the distributor */
    *(volatile uint8_t *)(uintptr_t)(gicd + GICD_ITARGETSR + id) = 0x01;  /* cpu 0 */
    gicd_spi_common(gicd, id);

    mmio32(gicc + GICC_PMR, 0xf0);                      /* unmask all priorities */
    mmio32(gicc + GICC_CTLR, 1);                        /* enable the CPU interface */
}

/* IRQ handler body (called from el1t_irq in arm64_linuxsys.S). The vector is
 * only reached from EL1t, so the interrupted level is always user (cpl 3): run
 * the device hooks and preempt the running thread. Acknowledge the interrupt
 * first, before any reschedule, so the tick is not re-taken the instant we
 * return to user with IRQs unmasked and so the next tick can be delivered.
 *   GICV3: read ICC_IAR1_EL1 to acknowledge, write ICC_EOIR1_EL1 to end it.
 *   GICV2: read GICC_IAR to acknowledge, write GICC_EOIR to end it.
 *   LINE : store to the event-EOI register so the host lowers the line it holds. */
void
arm64_irq_entry(void)
{
    if (irq_model == JOTUN_IRQ_GICV3) {
        uint64_t iar;
        __asm__ volatile ("mrs %0, " ICC_IAR1_EL1 : "=r"(iar));
        if ((iar & 0xffffff) >= 1020)       /* spurious / no pending */
            return;
        __asm__ volatile ("msr " ICC_EOIR1_EL1 ", %0" : : "r"(iar));
    } else if (irq_model == JOTUN_IRQ_GICV2) {
        uint32_t iar = mmio32_r(JOTUN_GICC_BASE + GICC_IAR);
        if ((iar & 0x3ff) >= 1020)          /* spurious / no pending */
            return;
        mmio32(JOTUN_GICC_BASE + GICC_EOIR, iar);
    } else {
        *(volatile uint32_t *)JOTUN_EVENT_EOI = 1;
    }
    irq_event(3);
}

/* Enable the interrupt controller for the preemption tick. On a KVM host bring
 * up the GIC of the generation the host created; on the line model
 * (Hypervisor.framework) there is no in-guest controller and nothing to do.
 * Matches the x86 arch_apic_init hook. */
void
arch_apic_init(void)
{
    irq_model = boot_irq_model();
    if (irq_model == JOTUN_IRQ_GICV3)
        gic_v3_init();
    else if (irq_model == JOTUN_IRQ_GICV2)
        gic_v2_init();
}

void
arch_set_kstack(uint64_t top)
{
    (void)top;
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

/* arm64 has no SMEP/SMAP. The EL0/EL1 split already keeps EL1 from executing an
 * EL0-only page, and user-memory containment would use PAN, a separate track
 * mirrored after the x86 sweep. A no-op for now so the shared init path links. */
void
arch_cpu_harden(void)
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
