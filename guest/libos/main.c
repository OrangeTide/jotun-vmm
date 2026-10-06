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

/* worker_a: print, drain any notes posted to it, yield once, then exit with a
 * status string. */
static void
worker_a(void)
{
    char note[NOTE_MAX];

    con_puts("  A: running\n");
    while (task_recv_note(note, sizeof note)) {
        con_puts("  A: note '");
        con_puts(note);
        con_puts("'\n");
    }
    task_yield();
    task_exits("a-ok");
}

/* The program worker_b exec's into. It runs on a fresh stack as the same task,
 * then exits with its own status. */
static void
worker_b2(void)
{
    con_puts("  B2: running after exec\n");
    task_yield();
    task_exits("b-done");
}

/* worker_b: replace itself with worker_b2. task_exec does not return. */
static void
worker_b(void)
{
    con_puts("  B: exec-ing into B2\n");
    task_exec(worker_b2);
}

/* The init task: spawn the workers, send one a note, then await both and
 * report each child's string exit status. */
static void
init_task(void)
{
    con_puts("init: starting\n");

    int a = task_spawn("worker-a", worker_a);
    task_spawn("worker-b", worker_b);
    task_note(a, "ping");

    con_puts("init: spawned, table now:\n");
    task_dump();

    char status[STATUS_MAX];
    int id;
    while ((id = task_await(status, sizeof status)) >= 0) {
        con_puts("init: child ");
        con_putc('0' + (char)id);
        con_puts(" exited '");
        con_puts(status);
        con_puts("'\n");
    }
    con_puts("init: done\n");
}

void
_start(void)
{
    con_puts("libos up\n");
    heap_selftest();

    task_spawn("init", init_task);
    sched_start();

    con_puts("final table:\n");
    task_dump();

    con_puts("libos halting\n");
    arch_halt();
    for (;;)
        ;
}
