/* main.c : library OS entry and boot.
 *
 * The host loads this ELF and starts every vcpu at the same entry: the asm
 * _start (arch/<arch>_boot.S) sets up the call and jumps to kmain with the
 * vcpu id in the first argument. cpu 0 brings up the console, heap, 9P, and
 * namespace, then spawns init; the other cpus wait for that boot to finish and
 * join the scheduler. All cpus share one run queue, so tasks spread across
 * them. When the system quiesces every scheduler returns and each cpu halts.
 *
 * The guest is the Linux binary-compatibility emulation: init loads and runs an
 * unmodified Linux program (linuxelf.c). Networking (net.c, ns.c) backs the
 * guest's sockets and inter-guest links; 9P (p9.c) serves its filesystem. */
#include "arch.h"
#include "boot.h"
#include "linuxelf.h"
#include "mem.h"
#include "net.h"
#include "ns.h"
#include "p9.h"
#include "print.h"
#include "task.h"
#include "jotun_abi.h"
#include "vmspace.h"

/* The kernel heap sits in its own RAM band above the user window
 * ([VM_KHEAP_LO, VM_KHEAP_HI), see vmspace.h), not in .bss below the user's
 * load address, so kmalloc is not boxed into the few MiB under 0x400000. The
 * band is identity-mapped, and the host zero-fills guest RAM before entry, so
 * meminit can lay down the first block directly. */
static void
heap_init(void)
{
    meminit(phys_to_virt(VM_KHEAP_LO), (size_t)(VM_KHEAP_HI - VM_KHEAP_LO));
}

/* Allocate, free in a different order, and confirm the free total returns to
 * where it started. A leak or a coalescing bug shows up as a mismatch. */
static void
heap_selftest(void)
{
    size_t before = memfree();

    void *a = kmalloc(128);
    void *b = kmalloc(4096);
    void *c = kmalloc(32);
    kfree(b);
    kfree(a);
    kfree(c);

    if (memfree() == before)
        con_puts("heap ok\n");
    else
        con_puts("heap leak\n");
}

/* The init task: load and run the Linux init program. The boot arguments are
 * the init program and its arguments; argv[0] is the historical "bin" selector
 * and argv[1] the program, so the args a Linux process sees start at argv[2]
 * (build_stack in linuxelf.c skips the first two). */
static void
init_task(void)
{
    linux_elf_run((boot_argc() > 1) ? boot_argv(1) : "/bin/sh");
    con_puts("init: done\n");
}

/* Report the host-supplied boot parameters: RAM size and the argument vector,
 * with argv[0] the init program. */
static void
show_boot_params(void)
{
    if (boot_init() != 0) {
        con_puts("boot: no parameter block\n");
        return;
    }
    con_printf("boot: ram %lu bytes, argc %d\n",
               (unsigned long)boot_ram_size(), boot_argc());
    for (int i = 0; i < boot_argc(); i++)
        con_printf("  argv[%d] = %s\n", i, boot_argv(i));
}

/* Set once cpu 0 has finished shared bring-up and spawned init. Secondary cpus
 * spin on it so they never enter the scheduler against an empty queue and
 * mistake the not-yet-booted system for a quiesced one. */
static volatile int boot_ready;

void
kmain(unsigned long id)
{
    arch_mmu_init();            /* MMU on before the first lock (arm64 needs it) */
    cpu_init(id);
    arch_traps_init(id);        /* each cpu loads its own trap tables */
    arch_fpu_init();            /* enable FP/SIMD so Linux binaries can use it */
    arch_cpu_harden();          /* SMEP: ring 0 cannot execute a user page */
    boot_init();                /* parse the boot block before anything reads it;
                                   arch_apic_init needs the interrupt model */
    arch_apic_init();           /* enable the local APIC for the event vector */

    if (id == 0) {
        con_puts("libos up\n");
        heap_init();
        heap_selftest();
        show_boot_params();

        if (p9_mount() != 0)
            con_puts("9p: mount failed\n");
        else
            con_puts("9p: mounted\n");
        ns_init();
        net_init();

        task_spawn("init", init_task);
        __atomic_store_n(&boot_ready, 1, __ATOMIC_RELEASE);
    } else {
        while (!__atomic_load_n(&boot_ready, __ATOMIC_ACQUIRE))
            arch_cpu_relax();
    }

    sched_start();              /* runs until the system quiesces */

    if (id == 0) {
        con_puts("final table:\n");
        task_dump();
        con_puts("libos halting\n");
        /* Stop the VMM. A plain HLT would be serviced by the in-kernel APIC and
         * never reach the host (the timer keeps waking it), so signal shutdown
         * explicitly. */
        *(volatile uint32_t *)JOTUN_POWEROFF = 0;
    }
    arch_halt();
    for (;;)
        ;
}
