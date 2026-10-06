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
#include "mem.h"
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

/* The init task: load and run a bundled program by name, then report its
 * string exit status. */
static void
init_task(void)
{
    con_puts("init: starting\n");

    const char *status = prog_run("hello");
    if (status) {
        con_puts("init: hello exited '");
        con_puts(status);
        con_puts("'\n");
    } else {
        con_puts("init: hello failed to run\n");
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

/* Exercise the 9P client: mount, read a file, and list the root directory. */
static void
fs_demo(void)
{
    if (p9_mount() != 0) {
        con_puts("9p: mount failed\n");
        return;
    }
    con_puts("9p: mounted\n");

    const char *path[] = { "hello.txt" };
    if (p9_walk(P9_ROOT_FID, 1, 1, path) == 1 && p9_open(1) == 0) {
        char buf[128];
        long n = p9_read(1, 0, buf, sizeof buf - 1);
        if (n >= 0) {
            buf[n] = '\0';
            con_puts("9p: hello.txt = ");
            con_puts(buf);
        }
    }
    p9_clunk(1);

    if (p9_walk(P9_ROOT_FID, 2, 0, NULL) >= 0 && p9_open(2) == 0) {
        uint8_t dir[512];
        long n = p9_read(2, 0, dir, sizeof dir);
        if (n > 0) {
            con_puts("9p: ls /\n");
            uint32_t off = 0;
            struct p9_stat st;
            while (p9_dir_next(dir, (uint32_t)n, &off, &st)) {
                con_puts("  ");
                con_puts(st.name);
                con_putc('\n');
            }
        }
    }
    p9_clunk(2);
}

void
_start(void)
{
    con_puts("libos up\n");
    heap_selftest();
    show_boot_params();
    fs_demo();
    svc_publish();

    task_spawn("init", init_task);
    sched_start();

    con_puts("final table:\n");
    task_dump();

    con_puts("libos halting\n");
    arch_halt();
    for (;;)
        ;
}
