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
#include "mem.h"
#include "print.h"
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

/* Two demo tasks that interleave by yielding. Each prints a few times so the
 * round-robin order is visible, then returns, which exits through task_exit. */
static void
task_a(void)
{
    for (int i = 0; i < 3; i++) {
        con_puts("  task A\n");
        task_yield();
    }
}

static void
task_b(void)
{
    for (int i = 0; i < 3; i++) {
        con_puts("  task B\n");
        task_yield();
    }
}

void
_start(void)
{
    con_puts("libos up\n");
    heap_selftest();

    task_spawn("task-a", task_a);
    task_spawn("task-b", task_b);
    con_puts("tasks spawned:\n");
    task_dump();

    con_puts("scheduling:\n");
    sched_start();

    con_puts("all tasks done:\n");
    task_dump();

    con_puts("libos halting\n");
    arch_halt();
    for (;;)
        ;
}
