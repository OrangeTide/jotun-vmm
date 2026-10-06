/* main.c : library OS entry and boot.
 *
 * The host loads this ELF and starts every vcpu at the same entry: the asm
 * _start (arch/<arch>_boot.S) sets up the call and jumps to kmain with the
 * vcpu id in the first argument. cpu 0 brings up the console, heap, 9P, and
 * namespace, then spawns init; the other cpus wait for that boot to finish and
 * join the scheduler. All cpus share one run queue, so tasks spread across
 * them. When the system quiesces every scheduler returns and each cpu halts. */
#include "arch.h"
#include "boot.h"
#include "jsh.h"
#include "mem.h"
#include "ns.h"
#include "p9.h"
#include "print.h"
#include "prog.h"
#include "task.h"

/* The heap lives in .bss. Guest RAM is zero-filled by the host before entry,
 * so no explicit clearing is needed before meminit lays down the first block. */
static unsigned char heap[1u * 1024 * 1024];

/* Allocate, free in a different order, and confirm the free total returns to
 * where it started. A leak or a coalescing bug shows up as a mismatch. */
static void
heap_selftest(void)
{
    meminit(heap, sizeof heap);
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

/* A CPU-bound worker: loop without ever yielding, printing its letter each
 * pass. Only a timer preemption can interleave two of these, so the output
 * shows whether preemption is working. */
static void
spinner(char letter)
{
    for (int i = 0; i < 6; i++) {
        con_putc(letter);
        con_putc(' ');
        for (volatile unsigned long d = 0; d < 20000000ul; d++)
            ;           /* busy-wait, no yield */
    }
}

static void spin_a(void) { spinner('A'); task_exits("a"); }
static void spin_b(void) { spinner('B'); task_exits("b"); }

/* Preemption demo: two CPU-bound tasks that never yield. With one vcpu and the
 * host timer (JOTUN_HZ) their letters interleave through preemption; with two
 * vcpus they run at the same time, one per cpu. Without either, one runs to
 * completion before the other. */
static void
spin_demo(void)
{
    con_puts("spin: two cpu-bound tasks\n");
    task_spawn("spin-a", spin_a);
    task_spawn("spin-b", spin_b);

    char status[STATUS_MAX];
    while (task_await(status, sizeof status) >= 0)
        ;
    con_puts("\nspin: done\n");
}

/* Shared counter guarded by a spinlock, hammered by several worker tasks. On
 * two vcpus the increments genuinely race, so a correct final total is
 * evidence the lock (and the memory the cpus share) behaves. */
#define SMP_WORKERS 4
#define SMP_ROUNDS  50000

static volatile int count_lock;
static volatile unsigned long counter;

static void
count_lock_acquire(void)
{
    while (__atomic_exchange_n(&count_lock, 1, __ATOMIC_ACQUIRE))
        arch_cpu_relax();
}

static void
count_lock_release(void)
{
    __atomic_store_n(&count_lock, 0, __ATOMIC_RELEASE);
}

static void
count_worker(void)
{
    for (int i = 0; i < SMP_ROUNDS; i++) {
        count_lock_acquire();
        counter++;
        count_lock_release();
    }
    task_exits(NULL);
}

/* Spawn SMP_WORKERS counting tasks and check the total once they finish. */
static void
smp_demo(void)
{
    con_puts("smp: locked-counter test\n");
    counter = 0;
    for (int i = 0; i < SMP_WORKERS; i++)
        task_spawn("count", count_worker);

    char status[STATUS_MAX];
    while (task_await(status, sizeof status) >= 0)
        ;

    con_puts("smp: counter ");
    con_putu(counter);
    con_puts(counter == (unsigned long)SMP_WORKERS * SMP_ROUNDS
             ? " ok\n" : " WRONG\n");
}

/* The init task: either the preemption demo or the startup script, selected by
 * the first boot argument (default "rc"). */
static void
init_task(void)
{
    const char *script = (boot_argc() > 0) ? boot_argv(0) : "rc";

    if (script[0] == 's' && script[1] == 'p') {         /* "spin" */
        spin_demo();
    } else if (script[0] == 's' && script[1] == 'm') {  /* "smp" */
        spin_demo();
        smp_demo();
    } else {
        con_puts("init: running script /");
        con_puts(script);
        con_putc('\n');
        jsh_run(script);
    }
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
    con_puts("boot: ram ");
    con_putu(boot_ram_size());
    con_puts(" bytes, argc ");
    con_putu((uint64_t)boot_argc());
    con_putc('\n');
    for (int i = 0; i < boot_argc(); i++) {
        con_puts("  argv[");
        con_putu((uint64_t)i);
        con_puts("] = ");
        con_puts(boot_argv(i));
        con_putc('\n');
    }
}

/* Set once cpu 0 has finished shared bring-up and spawned init. Secondary cpus
 * spin on it so they never enter the scheduler against an empty queue and
 * mistake the not-yet-booted system for a quiesced one. */
static volatile int boot_ready;

void
kmain(unsigned long id)
{
    cpu_init(id);
    arch_traps_init();          /* each cpu loads its own trap tables */

    if (id == 0) {
        con_puts("libos up\n");
        heap_selftest();
        show_boot_params();
        svc_publish();

        if (p9_mount() != 0)
            con_puts("9p: mount failed\n");
        else
            con_puts("9p: mounted\n");
        ns_init();

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
    }
    arch_halt();
    for (;;)
        ;
}
