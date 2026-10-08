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
#include "linuxelf.h"
#include "mem.h"
#include "net.h"
#include "ns.h"
#include "p9.h"
#include "print.h"
#include "prog.h"
#include "task.h"
#include "threads.h"
#include "triton.h"
#include "webterm.h"

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
    const char *target = (boot_argc() > 1) ? boot_argv(1) : "tcp!127.0.0.1!7";

    con_puts("net: dialing ");
    con_puts(target);
    con_putc('\n');

    int h = net_dial(target);
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

/* Serve demo: listen in-host on port 9 under this guest's name, accept one
 * connection, echo the first message back, and close. A peer reaches it by
 * dialing "tcp!<thisguest>!9". */
static void
serve_on(const char *spec)
{
    con_puts("serve: listening ");
    con_puts(spec);
    con_putc('\n');
    int l = net_listen(spec);
    if (l < 0) {
        con_puts("serve: listen failed\n");
        return;
    }

    char peer[64];
    int c = net_accept(l, peer, sizeof peer);
    if (c < 0) {
        con_puts("serve: accept failed\n");
        net_close(l);
        return;
    }
    con_puts("serve: accepted from ");
    con_puts(peer);
    con_putc('\n');

    char buf[256];
    long r = net_read(c, buf, sizeof buf - 1);
    if (r > 0) {
        buf[r] = '\0';
        con_puts("serve: got ");
        con_puts(buf);
        if (buf[r - 1] != '\n')
            con_putc('\n');
        net_write(c, buf, (unsigned long)r);    /* echo it back */
    }
    net_close(c);
    net_close(l);
    con_puts("serve: done\n");
}

/* The standalone server takes its listen string from argv, so it can bind a
 * real external port (e.g. "serve tcp!*!18080") given a matching policy rule. */
static void
serve_demo(void)
{
    serve_on((boot_argc() > 1) ? boot_argv(1) : "tcp!*!9");
}

/* The link self-test's server always offers the in-host port 9. */
static void
serve_inhost9(void)
{
    serve_on("tcp!*!9");
}

static void
link_client(void)
{
    net_demo();         /* dials boot_argv(1), e.g. "tcp!self!9" */
}

/* Link self-test: one guest talks to itself over the in-host switch. A server
 * task listens and a client task dials this guest's own name, so the whole
 * socketpair splice runs in a single guest with ordered console output. Run as
 * JOTUN_NAME=self with init "link tcp!self!9". */
static void
link_demo(void)
{
    task_spawn("srv", serve_inhost9);
    task_yield();       /* let the server listen and park on accept first */
    task_spawn("cli", link_client);

    char status[64];
    while (task_await(status, sizeof status) >= 0)
        ;
    con_puts("link: done\n");
}

/* Startup-race self-test: spawn the client first so it dials before the server
 * has listened. The dial should park on NET_EAGAIN rather than fail, then
 * connect once the server's listen wakes it. Run like link, as JOTUN_NAME=self
 * with init "race tcp!self!9". */
static void
race_demo(void)
{
    task_spawn("cli", link_client);
    task_yield();       /* let the client dial first and park on the race */
    task_spawn("srv", serve_inhost9);

    char status[64];
    while (task_await(status, sizeof status) >= 0)
        ;
    con_puts("race: done\n");
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

/* The UDP self-test's server binds the in-host port 9 under this guest's name,
 * receives one datagram, and echoes it back to the sender. */
static void
userve(void)
{
    int h = net_udp();
    if (h < 0 || net_bind(h, "udp!*!9") < 0) {
        con_puts("userve: bind failed\n");
        return;
    }
    con_puts("userve: bound udp!*!9\n");

    char src[64];
    char buf[256];
    long r = net_recvfrom(h, src, sizeof src, buf, sizeof buf - 1);
    if (r > 0) {
        buf[r] = '\0';
        con_puts("userve: got ");
        con_puts(buf);
        if (buf[r - 1] != '\n')
            con_putc('\n');
        con_puts("userve: from ");
        con_puts(src);
        con_putc('\n');
        net_sendto(h, src, buf, (unsigned long)r);       /* echo it back */
    }
    net_close(h);
    con_puts("userve: done\n");
}

/* The UDP self-test's client sends one datagram to a target and waits for the
 * echo. The target defaults to this guest's own name (the single-guest test)
 * but may be given so a second guest can reach a peer by name. */
static void
uclient_to(const char *target)
{
    int h = net_udp();
    if (h < 0) {
        con_puts("uclient: open failed\n");
        return;
    }
    const char *msg = "ping\n";
    long w = net_sendto(h, target, msg, 5);
    if (w < 0) {
        con_puts("uclient: send failed\n");
        net_close(h);
        return;
    }
    char src[64];
    char buf[256];
    long r = net_recvfrom(h, src, sizeof src, buf, sizeof buf - 1);
    if (r >= 0) {
        buf[r] = '\0';
        con_puts("uclient: reply ");
        con_puts(buf);
        if (r == 0 || buf[r - 1] != '\n')
            con_putc('\n');
        con_puts("uclient: from ");
        con_puts(src);
        con_putc('\n');
    }
    net_close(h);
    con_puts("uclient: done\n");
}

static void
uclient(void)
{
    uclient_to((boot_argc() > 1) ? boot_argv(1) : "self!9");
}

/* UDP in-host self-test: one guest sends a datagram to itself by name over the
 * switch. The server binds port 9 first, then the client sends to "self!9" and
 * reads the echo. Run as JOTUN_NAME=self with init "ulink". */
static void
ulink_demo(void)
{
    task_spawn("usrv", userve);
    task_yield();       /* let the server bind before the client sends */
    task_spawn("ucli", uclient);

    char status[64];
    while (task_await(status, sizeof status) >= 0)
        ;
    con_puts("ulink: done\n");
}

/* Threads demo: exercise the C11 <threads.h> shim. A condition variable gates a
 * waiter task, several worker threads bump a shared counter under a mutex, and
 * call_once runs an initializer exactly once. All are joined for their return
 * values, so the whole thrd/mtx/cnd path runs. Run as init "threads". */
#define THRD_WORKERS 3
#define THRD_ITERS   100

static mtx_t     td_mtx;        /* guards td_counter */
static int       td_counter;
static mtx_t     td_cmtx;       /* paired with td_cnd */
static cnd_t     td_cnd;
static int       td_ready;
static once_flag td_once = ONCE_FLAG_INIT;

static void
td_init_once(void)
{
    con_puts("threads: call_once ran\n");
}

static int
td_worker(void *arg)
{
    long iters = (long)arg;
    for (long i = 0; i < iters; i++) {
        mtx_lock(&td_mtx);
        td_counter++;
        mtx_unlock(&td_mtx);
        thrd_yield();
    }
    return (int)iters;
}

static int
td_waiter(void *arg)
{
    (void)arg;
    mtx_lock(&td_cmtx);
    while (!td_ready)
        cnd_wait(&td_cnd, &td_cmtx);
    mtx_unlock(&td_cmtx);
    con_puts("threads: waiter released by condvar\n");
    return 42;
}

static void
threads_demo(void)
{
    con_puts("threads: starting\n");
    call_once(&td_once, td_init_once);
    call_once(&td_once, td_init_once);      /* must not run a second time */

    mtx_init(&td_mtx, mtx_plain);
    mtx_init(&td_cmtx, mtx_plain);
    cnd_init(&td_cnd);
    td_counter = 0;
    td_ready = 0;

    thrd_t waiter;
    if (thrd_create(&waiter, td_waiter, NULL) != thrd_success) {
        con_puts("threads: create waiter failed\n");
        return;
    }

    thrd_t w[THRD_WORKERS];
    for (int i = 0; i < THRD_WORKERS; i++)
        if (thrd_create(&w[i], td_worker, (void *)(long)THRD_ITERS)
            != thrd_success) {
            con_puts("threads: create worker failed\n");
            return;
        }

    /* Release the waiter through the condition variable. */
    mtx_lock(&td_cmtx);
    td_ready = 1;
    cnd_signal(&td_cnd);
    mtx_unlock(&td_cmtx);

    int res, sum = 0;
    for (int i = 0; i < THRD_WORKERS; i++) {
        thrd_join(w[i], &res);
        sum += res;
    }
    thrd_join(waiter, &res);

    con_puts("threads: counter=");
    con_putu((uint64_t)td_counter);
    con_puts(" (expect ");
    con_putu((uint64_t)(THRD_WORKERS * THRD_ITERS));
    con_puts(")\n");
    con_puts("threads: worker return sum=");
    con_putu((uint64_t)sum);
    con_puts("\n");
    con_puts("threads: waiter return=");
    con_putu((uint64_t)res);
    con_puts("\n");

    mtx_destroy(&td_mtx);
    mtx_destroy(&td_cmtx);
    cnd_destroy(&td_cnd);
    con_puts("threads: done\n");
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

    /* A polyline (triangle) and a path, to exercise the new shape ops. */
    static const int16_t tri[] = { 110, 0, 150, 60, 70, 60 };
    struct draw_style edge = {
        .mask = DST_FILL | DST_STROKE | DST_STROKE_WIDTH,
        .fill = DRAW_COLOR_NONE,
        .stroke = 0xaa3344,
        .stroke_width = 2,
    };
    draw_polyline(6, 4, 1, tri, 3, &edge);

    struct draw_seg path[] = {
        { DPATH_M, { 170, 0 } },
        { DPATH_L, { 210, 0 } },
        { DPATH_Q, { 230, 30, 210, 60 } },
        { DPATH_Z, { 0 } },
    };
    struct draw_style pstyle = { .mask = DST_FILL, .fill = 0x6688cc };
    draw_path(7, 4, path, 4, &pstyle);

    /* Move the circle with SET, a post-create mutation. */
    draw_set(5, DSA_CX, 40);

    /* Create then remove a node, exercising the removal path. Node 8 must not
     * appear in the authored scene, and the rest of the tree must stay intact. */
    draw_rect(8, 1, 0, 0, 10, 10, 0, &box);
    draw_remove(8);

    draw_flush();
    con_puts("draw: scene submitted, waiting for event\n");

    /* Event loop: each click on the rect toggles its fill and re-submits, which
     * the host sends to the browser as an incremental delta rather than a full
     * document. A close event ends the demo. */
    static const uint32_t fills[] = { 0x3366cc, 0xcc6633 };
    int which = 0;
    for (;;) {
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

        if (ev.type == DEV_CLOSE)
            break;
        if (ev.type == DEV_CLICK && ev.node == 2) {
            which ^= 1;
            draw_set(2, DSA_FILL, fills[which]);
            draw_flush();
            con_puts("draw: delta submitted\n");
        }
    }
    con_puts("draw: done\n");
}

/* Pixel path demo: software-render a wireframe cube into an RGBA framebuffer in
 * guest RAM, then show it as an image node under the 2D window chrome. The host
 * reads the framebuffer from guest memory, encodes it as a PNG, and composites
 * it into the scene. */
#define FBW 256
#define FBH 192
static uint8_t framebuffer[FBW * FBH * 4];

static void
fb_put(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if ((unsigned)x < FBW && (unsigned)y < FBH) {
        uint8_t *p = framebuffer + ((unsigned)y * FBW + (unsigned)x) * 4;
        p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
    }
}

/* Integer Bresenham line, no floating point (the guest has no FP unit). */
static void
fb_line(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b)
{
    int dx = x1 - x0, dy = y1 - y0;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    dy = -dy;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        fb_put(x0, y0, r, g, b);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void
render_cube(void)
{
    /* Clear to a dark blue. */
    for (int i = 0; i < FBW * FBH; i++) {
        uint8_t *p = framebuffer + i * 4;
        p[0] = 0x10; p[1] = 0x18; p[2] = 0x30; p[3] = 255;
    }

    /* Two offset squares (front and back faces) joined by four edges: a cube in
     * cabinet projection, drawn with integer coordinates. */
    int s = 90, o = 44;
    int fx = (FBW - s) / 2 - o / 2, fy = (FBH - s) / 2 + o / 2;   /* front TL */
    int bx = fx + o, by = fy - o;                                /* back  TL */
    int fr[4][2] = { {fx, fy}, {fx + s, fy}, {fx + s, fy + s}, {fx, fy + s} };
    int bk[4][2] = { {bx, by}, {bx + s, by}, {bx + s, by + s}, {bx, by + s} };
    for (int i = 0; i < 4; i++) {
        int j = (i + 1) % 4;
        fb_line(fr[i][0], fr[i][1], fr[j][0], fr[j][1], 0x8c, 0xd0, 0xff);
        fb_line(bk[i][0], bk[i][1], bk[j][0], bk[j][1], 0x4c, 0x80, 0xc0);
        fb_line(fr[i][0], fr[i][1], bk[i][0], bk[i][1], 0xa0, 0xa8, 0xc0);
    }
}

/* Fill a solid rectangle into the framebuffer, clamped to its bounds. */
static void
fb_fill(int x0, int y0, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            fb_put(x, y, r, g, b);
}

static void
drawpixel(void)
{
    con_puts("pixel: rendering wireframe into framebuffer\n");
    render_cube();

    draw_surface(1, 0, 0, FBW + 20, FBH + 20, "framebuffer");
    draw_image(2, 1, 10, 10, FBW, FBH, framebuffer, 0);
    draw_click(2);              /* click to stamp a patch; close ends the demo */

    draw_flush();
    con_puts("pixel: submitted image node, waiting for event\n");

    /* Each click paints a small square into a cycling spot and reports just
     * that region as damaged, so the host re-encodes and re-sends only the
     * square rather than the whole frame. */
    static const int spots[][2] = {
        { 24, 24 }, { 200, 24 }, { 24, 140 }, { 200, 140 }, { 112, 82 },
    };
    static const uint8_t cols[][3] = {
        { 0xff, 0x60, 0x60 }, { 0x60, 0xff, 0x60 }, { 0x60, 0x60, 0xff },
        { 0xff, 0xff, 0x60 }, { 0xff, 0x60, 0xff },
    };
    int n = 0;
    for (;;) {
        struct draw_event ev;
        if (draw_next_event(&ev) != 0) {
            con_puts("pixel: event error\n");
            return;
        }
        con_puts("pixel: event node ");
        con_putu(ev.node);
        con_puts(" type ");
        con_putu(ev.type);
        con_putc('\n');

        if (ev.type == DEV_CLOSE)
            break;
        if (ev.type == DEV_CLICK && ev.node == 2) {
            int i = n++ % 5;
            int sx = spots[i][0], sy = spots[i][1];
            fb_fill(sx, sy, 32, 32, cols[i][0], cols[i][1], cols[i][2]);
            draw_image_damage(2, sx, sy, 32, 32);
            draw_flush();
            con_puts("pixel: damage submitted\n");
        }
    }
    con_puts("pixel: done\n");
}

/* Draw scale benchmark: build a grid of many rects in one surface and submit
 * it, so the host (with JOTUN_DRAW_BENCH) can time apply and serialize as the
 * node count grows. The count comes from the second boot argument. */
static void
drawbench(void)
{
    int n = 1000;
    if (boot_argc() > 1) {
        const char *s = boot_argv(1);
        int v = 0;
        while (*s >= '0' && *s <= '9')
            v = v * 10 + (*s++ - '0');
        if (v > 0)
            n = v;
    }

    con_puts("bench: building ");
    con_putu((uint64_t)n);
    con_puts(" rects\n");

    draw_surface(1, 0, 0, 1000, 1000, "bench");
    struct draw_style st = { .mask = DST_FILL, .fill = 0x4080c0 };
    for (int i = 0; i < n; i++) {
        int col = i % 40, row = i / 40;
        draw_rect((uint32_t)(i + 2), 1, col * 24, row * 24, 20, 20, 2, &st);
    }
    draw_flush();

    con_puts("bench: submitted ");
    con_putu((uint64_t)n);
    con_puts(" rects\n");
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
    } else if (script[0] == 's' && script[1] == 'e') {  /* "serve" */
        serve_demo();
    } else if (script[0] == 'l' && script[1] == 'i') {  /* "link" */
        link_demo();
    } else if (script[0] == 'r' && script[1] == 'a') {  /* "race" */
        race_demo();
    } else if (script[0] == 'u' && script[1] == 'l') {  /* "ulink" */
        ulink_demo();
    } else if (script[0] == 'u' && script[1] == 's') {  /* "userve" */
        userve();
    } else if (script[0] == 'u' && script[1] == 'c') {  /* "uclient <target>" */
        uclient();
    } else if (script[0] == 'u' && script[1] == 'd') {  /* "udp" */
        udp_demo();
    } else if (script[0] == 'd' && script[1] == 'r') {  /* "draw" */
        draw_demo();
    } else if (script[0] == 'b' && script[1] == 'e') {  /* "bench" */
        drawbench();
    } else if (script[0] == 'p' && script[1] == 'i') {  /* "pixel" */
        drawpixel();
    } else if (script[0] == 't' && script[1] == 'h') {  /* "threads" */
        threads_demo();
    } else if (script[0] == 't' && script[1] == 'r') {  /* "triton" */
        triton_demo();
    } else if (script[0] == 'w' && script[1] == 'e') {  /* "webterm" */
        webterm();
    } else if (script[0] == 'b' && script[1] == 'i') {  /* "bin <prog>" (M16) */
        linux_elf_run((boot_argc() > 1) ? boot_argv(1) : "linuxhello");
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
    cpu_init(id);
    arch_traps_init();          /* each cpu loads its own trap tables */
    arch_fpu_init();            /* enable FP/SIMD so the rasterizer can run */

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
