/* main.c : library OS entry and boot.
 *
 * The host loads this ELF, sets the vcpu PC to _start and SP to the top of a
 * per-vcpu stack, then runs. This is the greenfield 64-bit boot path: no
 * privilege split and no asm boot stub, since the host already hands us a
 * usable stack. We bring up the console, exercise the allocator, and halt.
 *
 * Later M2 steps add the scheduler and context switch; for now _start is the
 * whole of the init task. */
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

/* Preemption demo: two CPU-bound tasks that never yield. With the host timer
 * (JOTUN_HZ) their letters interleave; without it, one runs to completion
 * before the other. */
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

/* The init task: either the preemption demo or the startup script, selected by
 * the first boot argument (default "rc"). */
static void
init_task(void)
{
    const char *script = (boot_argc() > 0) ? boot_argv(0) : "rc";

    if (script[0] == 's' && script[1] == 'p') {         /* "spin" */
        spin_demo();
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

void
_start(void)
{
    con_puts("libos up\n");
    heap_selftest();
    show_boot_params();
    arch_traps_init();          /* trap tables ready before any task runs */
    svc_publish();

    if (p9_mount() != 0)
        con_puts("9p: mount failed\n");
    else
        con_puts("9p: mounted\n");
    ns_init();

    task_spawn("init", init_task);
    sched_start();

    con_puts("final table:\n");
    task_dump();

    con_puts("libos halting\n");
    arch_halt();
    for (;;)
        ;
}
