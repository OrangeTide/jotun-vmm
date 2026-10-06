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
#include "draw.h"
#include "jsh.h"
#include "mem.h"
#include "net.h"
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

/* Sleep/wake test: a child parks itself with task_block and the parent wakes
 * it by id. Exercises the TASK_BLOCKED path and the no-lost-wakeup handoff
 * without any device, so the scheduler change can be checked on its own. */
static void
block_waiter(void)
{
    con_puts("waiter: blocking\n");
    task_block();
    con_puts("waiter: woken\n");
    task_exits("woke");
}

static void
block_demo(void)
{
    con_puts("block: sleep/wake test\n");
    int id = task_spawn("waiter", block_waiter);
    if (id < 0) {
        con_puts("block: spawn failed\n");
        return;
    }

    /* Give the waiter a few scheduling rounds to run and park. */
    for (int i = 0; i < 3; i++)
        task_yield();

    con_puts("block: waking waiter\n");
    task_wake(id);

    char status[STATUS_MAX];
    task_await(status, sizeof status);
    con_puts("block: waiter returned '");
    con_puts(status);
    con_puts("'\n");
}

/* Print a file from the 9P namespace, byte for byte. Used to show the /net
 * information tree from inside the net demo. */
static void
cat_path(const char *path)
{
    if (ns_walk(path, 6) != 0) {
        con_puts("  (missing ");
        con_puts(path);
        con_puts(")\n");
        return;
    }
    if (p9_open(6) == 0) {
        char b[256];
        uint64_t off = 0;
        long n;
        while ((n = p9_read(6, off, b, sizeof b)) > 0) {
            for (long i = 0; i < n; i++)
                con_putc(b[i]);
            off += (uint64_t)n;
        }
    }
    p9_clunk(6);
}

/* Networking demo: dial a target ("host!port", from the second boot argument),
 * send a line, print the reply, and close. The read blocks the task until the
 * host reports data, without stalling the rest of the system. */
static void
net_demo(void)
{
    const char *target = (boot_argc() > 1) ? boot_argv(1) : "127.0.0.1!7";

    con_puts("net: dialing ");
    con_puts(target);
    con_putc('\n');

    int h = net_dial(NET_PROTO_TCP, target);
    if (h < 0) {
        con_puts("net: dial failed\n");
        return;
    }

    const char *msg = "ping\n";
    long w = net_write(h, msg, 5);
    if (w < 0) {
        con_puts("net: write failed\n");
        net_close(h);
        return;
    }
    con_puts("net: wrote ");
    con_putu((uint64_t)w);
    con_puts(" bytes\n");

    /* The connection is live, so it appears in the /net tree as handle 0. */
    con_puts("net: /net/conn/0 = ");
    cat_path("/net/conn/0");

    char buf[256];
    long r = net_read(h, buf, sizeof buf - 1);
    if (r < 0) {
        con_puts("net: read error\n");
        net_close(h);
        return;
    }
    buf[r] = '\0';
    con_puts("net: read ");
    con_putu((uint64_t)r);
    con_puts(" bytes: ");
    con_puts(buf);
    if (r == 0 || buf[r - 1] != '\n')
        con_putc('\n');

    net_close(h);
    con_puts("net: done\n");
}

/* UDP demo: open a datagram socket, send one datagram to a target, and print
 * the reply with its source address. The recvfrom blocks the task until a
 * datagram arrives. */
static void
udp_demo(void)
{
    const char *target = (boot_argc() > 1) ? boot_argv(1) : "127.0.0.1!7";

    con_puts("udp: socket, sending to ");
    con_puts(target);
    con_putc('\n');

    int h = net_udp();
    if (h < 0) {
        con_puts("udp: open failed\n");
        return;
    }

    const char *msg = "ping\n";
    long w = net_sendto(h, target, msg, 5);
    if (w < 0) {
        con_puts("udp: send failed\n");
        net_close(h);
        return;
    }
    con_puts("udp: sent ");
    con_putu((uint64_t)w);
    con_puts(" bytes\n");

    char src[64];
    char buf[256];
    long r = net_recvfrom(h, src, sizeof src, buf, sizeof buf - 1);
    if (r < 0) {
        con_puts("udp: recv error\n");
        net_close(h);
        return;
    }
    buf[r] = '\0';
    con_puts("udp: got ");
    con_putu((uint64_t)r);
    con_puts(" bytes from ");
    con_puts(src);
    con_puts(": ");
    con_puts(buf);
    if (r == 0 || buf[r - 1] != '\n')
        con_putc('\n');

    net_close(h);
    con_puts("udp: done\n");
}

/* Draw demo: build a tiny scene with one clickable rectangle, submit it, then
 * block on the first input event. With JOTUN_DRAW_TEST set on the host, the
 * sink feeds back a synthetic click on that rectangle, so the submit, park, and
 * poll path runs end to end with no browser attached. */
static void
draw_demo(void)
{
    con_puts("draw: building scene\n");
    draw_surface(1, 0, 0, 320, 240, "demo");

    struct draw_style box = {
        .mask = DST_FILL | DST_STROKE | DST_STROKE_WIDTH,
        .fill = 0x3366cc,
        .stroke = 0x102040,
        .stroke_width = 2,
    };
    draw_rect(2, 1, 40, 40, 120, 60, 8, &box);
    draw_text(3, 1, 52, 78, 16, "click me", 0);
    draw_click(2);
    draw_hover_fill(2, 0x4c8cff);

    /* A nested group with a circle, to exercise container nesting and a second
     * shape in the serialized scene. */
    struct draw_style dot = {
        .mask = DST_FILL,
        .fill = 0x33aa55,
    };
    draw_group(4, 1, 20, 130);
    draw_circle(5, 4, 30, 30, 20, &dot);

    draw_flush();
    con_puts("draw: scene submitted, waiting for event\n");

    struct draw_event ev;
    if (draw_next_event(&ev) != 0) {
        con_puts("draw: event error\n");
        return;
    }
    con_puts("draw: event node ");
    con_putu(ev.node);
    con_puts(" type ");
    con_putu(ev.type);
    con_putc('\n');
    con_puts("draw: done\n");
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
    } else if (script[0] == 'b' && script[1] == 'l') {  /* "block" */
        block_demo();
    } else if (script[0] == 'n' && script[1] == 'e') {  /* "net" */
        net_demo();
    } else if (script[0] == 'u' && script[1] == 'd') {  /* "udp" */
        udp_demo();
    } else if (script[0] == 'd' && script[1] == 'r') {  /* "draw" */
        draw_demo();
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
        net_init();
        draw_init();

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
