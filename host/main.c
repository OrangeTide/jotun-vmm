#include <arpa/inet.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "vmm.h"
#include "elf.h"
#include "uart.h"
#include "jotun_abi.h"
#include "jotun_boot.h"
#include "jotun_9p.h"
#include "jotun_net.h"
#include "jotun_draw.h"
#include "p9srv.h"
#include "p9mem.h"
#include "p9smolvfs.h"
#include "p9overlay.h"
#include "manifest.h"
#include "bundlepull.h"
#include "net.h"
#include "draw.h"

/* Guest physical RAM mapped at address 0. 8 MiB is ample for the test guest
 * plus per-vcpu stacks. */
#define GUEST_RAM_SIZE (8u * 1024 * 1024)

/* Each vcpu gets its own 512 KiB stack, carved from the top of RAM downward, so
 * vcpus sharing one guest image do not clobber each other's stacks. */
#define STACK_SLOT     (512u * 1024)
#define MAX_VCPUS      8

/* Shared host-side state touched by every vcpu thread. */
static uart_16550 uart;
static pthread_mutex_t dev_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long max_exits;     /* 0 means no cap */
static struct p9srv p9;             /* the 9P server, guarded by dev_lock */

struct vcpu_ctx {
    int index;
    vmm_vm_t *vm;
    uint64_t entry;
    uint64_t sp;
    int rc;
    vmm_vcpu_t *vcpu;           /* published by the thread for the timer */
    volatile int live;          /* 1 while the vcpu is running */

    /* Idle/wake rendezvous: a guest cpu with no work stores to the idle
     * doorbell and this thread blocks here until another cpu's wake doorbell
     * store signals it. */
    pthread_mutex_t idle_m;
    pthread_cond_t idle_c;
    int woken;
};

/* The vcpu table, published so a wake doorbell on one vcpu's thread can reach
 * another's idle rendezvous. */
static struct vcpu_ctx *all_ctx;
static pthread_t *all_threads;
static int all_n;

/* The net provider and the draw sink, each sharing its guest channel in host
 * memory. */
static struct netdev *netdev;
static struct drawdev *drawdev;

static void
vcpu_wake(struct vcpu_ctx *c)
{
    pthread_mutex_lock(&c->idle_m);
    c->woken = 1;
    pthread_cond_signal(&c->idle_c);
    pthread_mutex_unlock(&c->idle_m);
}

static void
vcpu_idle_wait(struct vcpu_ctx *c)
{
    pthread_mutex_lock(&c->idle_m);
    while (!c->woken)
        pthread_cond_wait(&c->idle_c, &c->idle_m);
    c->woken = 0;
    pthread_mutex_unlock(&c->idle_m);
}

/* Rouse every vcpu for a device event: inject the event interrupt into running
 * vcpus (breaking their run so the guest ISR fires) and wake any that are idle
 * (so they leave the idle rendezvous and recheck). The net poll thread calls
 * this when a socket a guest task waits on becomes ready, and the draw sink
 * when an input event is queued. */
static void
rouse_vcpus(void)
{
    for (int i = 0; i < all_n; i++) {
        if (__atomic_load_n(&all_ctx[i].live, __ATOMIC_SEQ_CST)) {
            vmm_vcpu_interrupt(all_ctx[i].vcpu, JOTUN_EVENT_VECTOR);
            if (all_threads[i])
                pthread_kill(all_threads[i], SIGUSR1);
        }
        vcpu_wake(&all_ctx[i]);
    }
}

/* Guest physical memory as the host sees it, identity-mapped for now (guest
 * physical address N is host byte N of this buffer). */
struct guest_mem {
    uint8_t *base;
    uint64_t size;
};
static struct guest_mem gm;

/* Translate a guest-physical range to a host pointer, or NULL if it does not
 * fit entirely within guest RAM. Every guest-supplied address is checked here
 * before the host dereferences it. */
static void *
gpa_to_host(uint64_t gpa, uint64_t len)
{
    if (gpa > gm.size || len > gm.size - gpa)
        return NULL;
    return gm.base + gpa;
}

/* The draw sink's address translator: same bounds check, with the callback
 * signature the sink expects (the context is unused, since guest RAM is file
 * scope here). */
static void *
draw_gpa(void *ctx, uint64_t gpa, uint64_t len)
{
    (void)ctx;
    return gpa_to_host(gpa, len);
}

/* Write the boot parameter block into guest RAM before the vcpu starts. The
 * guest argv is whatever follows the kernel image on the command line, with
 * argv[0] the init program. */
static void
setup_bootinfo(int gargc, char **gargv)
{
    struct jotun_bootinfo *b = gpa_to_host(JOTUN_BOOT_BASE, sizeof(*b));
    if (!b)
        return;

    memset(b, 0, sizeof(*b));
    b->magic = JOTUN_BOOT_MAGIC;
    b->version = JOTUN_BOOT_VERSION;
    b->ram_size = gm.size;

    uint32_t off = 0;
    int n = 0;
    for (int i = 0; i < gargc; i++) {
        size_t len = strlen(gargv[i]) + 1;
        if (off + len > JOTUN_BOOT_BLOB_MAX)
            break;
        memcpy(&b->blob[off], gargv[i], len);
        off += (uint32_t)len;
        n++;
    }
    b->argc = (uint32_t)n;
    b->blob_len = off;
}

/* Doorbell handler: drain the request ring, echo each message into the
 * response ring, and raise the response event. Called with dev_lock held. */
static void
ring_service(void)
{
    struct jotun_ring *r = gpa_to_host(JOTUN_RING_BASE, sizeof(*r));
    if (!r)
        return;

    while (r->req_tail != r->req_head) {
        struct jotun_msg *q = &r->req[r->req_tail % JOTUN_RING_SLOTS];
        uint16_t len = q->len;
        if (len > JOTUN_MSG_MAX)
            len = JOTUN_MSG_MAX;

        struct jotun_msg *s = &r->rsp[r->rsp_head % JOTUN_RING_SLOTS];
        s->len = len;
        memcpy(s->data, q->data, len);
        r->rsp_head++;
        r->req_tail++;

        fprintf(stderr, "[ring] echoed %u bytes: %.*s\n",
                (unsigned)len, (int)len, q->data);
    }
    r->evt_pending |= JOTUN_EVT_RSP;
}

/* 9P doorbell handler: process the T-message in the shared channel and write
 * the reply back into it. Called with dev_lock held. */
static void
p9_service(void)
{
    struct jotun_p9 *c = gpa_to_host(JOTUN_P9_BASE, sizeof(*c));
    if (!c)
        return;
    c->rsp_len = p9srv_handle(&p9, c->req, c->req_len, c->rsp, JOTUN_P9_MSIZE);
}

/* Net doorbell handler: process the request in the net channel and write the
 * result back. Called without dev_lock, since the provider has its own lock
 * and a dial may resolve a name, which should not block the console. */
static void
net_service(void)
{
    struct jotun_net *c = gpa_to_host(JOTUN_NET_BASE, sizeof(*c));
    if (!c || !netdev)
        return;
    net_handle(netdev, c);
}

/* Draw doorbell handler: process the request in the draw channel and write the
 * result back. Called without dev_lock, like the net doorbell, since the sink
 * has no shared console state. */
static void
draw_service(void)
{
    struct jotun_draw *c = gpa_to_host(JOTUN_DRAW_BASE, sizeof(*c));
    if (!c || !drawdev)
        return;
    draw_handle(drawdev, c);
}

/* Turn one manifest rule (strings) into a numeric net_rule. Returns 0, or -1
 * if the CIDR or ports do not parse (the rule is then skipped). */
static int
build_net_rule(const struct manifest_net *mn, struct net_rule *r)
{
    char cidr[64];
    snprintf(cidr, sizeof cidr, "%s", mn->cidr);
    char *slash = strchr(cidr, '/');
    int prefix = 32;
    if (slash) {
        *slash = '\0';
        prefix = (int)strtol(slash + 1, NULL, 10);
        if (prefix < 0 || prefix > 32)
            return -1;
    }
    struct in_addr in;
    if (inet_pton(AF_INET, cidr, &in) != 1)
        return -1;
    r->addr = ntohl(in.s_addr);
    r->mask = prefix == 0 ? 0u : (0xffffffffu << (32 - prefix));

    if (strcmp(mn->ports, "*") == 0) {
        r->port_lo = 0;
        r->port_hi = 65535;
    } else {
        char *dash = strchr(mn->ports, '-');
        long lo = strtol(mn->ports, NULL, 10);
        long hi = dash ? strtol(dash + 1, NULL, 10) : lo;
        if (lo < 0 || hi > 65535 || lo > hi)
            return -1;
        r->port_lo = (uint16_t)lo;
        r->port_hi = (uint16_t)hi;
    }

    if (strcmp(mn->proto, "tcp") == 0)
        r->proto = NET_PROTO_TCP;
    else if (strcmp(mn->proto, "udp") == 0)
        r->proto = NET_PROTO_UDP;
    else
        r->proto = 0;           /* any */
    return 0;
}

/* One vcpu: create it on this thread (required by Hypervisor.framework), point
 * it at the entry and its own stack, then service exits until it stops. */
static void *
run_vcpu(void *arg)
{
    struct vcpu_ctx *c = arg;

    vmm_vcpu_t *vcpu = vmm_vcpu_create(c->vm);
    if (!vcpu) {
        pthread_mutex_lock(&dev_lock);
        fprintf(stderr, "[vcpu %d] failed to create\n", c->index);
        pthread_mutex_unlock(&dev_lock);
        c->rc = 1;
        return NULL;
    }
    vmm_vcpu_set_reg(vcpu, VMM_REG_PC, c->entry);
    vmm_vcpu_set_reg(vcpu, VMM_REG_SP, c->sp);
    vmm_vcpu_set_reg(vcpu, VMM_REG_ARG, (uint64_t)c->index);    /* vcpu id */

    /* Publish the vcpu so the timer can interrupt it, and mark it running. */
    c->vcpu = vcpu;
    __atomic_store_n(&c->live, 1, __ATOMIC_SEQ_CST);

    vmm_exit_info_t e;
    unsigned long exits = 0;
    for (;;) {
        if (max_exits && ++exits > max_exits) {
            pthread_mutex_lock(&dev_lock);
            fprintf(stderr, "[vcpu %d] giving up after %lu exits\n",
                    c->index, max_exits);
            pthread_mutex_unlock(&dev_lock);
            c->rc = 1;
            break;
        }

        vmm_vcpu_run(vcpu, &e);

        if (e.reason == VMM_EXIT_REASON_IO) {
            pthread_mutex_lock(&dev_lock);
            if (!uart_io(&uart, &e) && !e.io.write)
                memset(e.io.data, 0, (size_t)e.io.size * e.io.count);
            pthread_mutex_unlock(&dev_lock);
        } else if (e.reason == VMM_EXIT_REASON_INTR) {
            /* The run was broken to inject an interrupt, or an interrupt
             * window opened. Nothing to service; run again. */
            continue;
        } else if (e.reason == VMM_EXIT_REASON_HLT) {
            pthread_mutex_lock(&dev_lock);
            printf("\n[vcpu %d halted]\n", c->index);
            fflush(stdout);
            pthread_mutex_unlock(&dev_lock);
            break;
        } else if (e.reason == VMM_EXIT_REASON_MMIO
                   && e.mmio.write && e.mmio.address == JOTUN_IDLE_DOORBELL) {
            /* The guest cpu has no work: park this thread until woken. Done
             * without dev_lock so an idle cpu never blocks the others. */
            vcpu_idle_wait(c);
            continue;
        } else if (e.reason == VMM_EXIT_REASON_MMIO
                   && e.mmio.write && e.mmio.address == JOTUN_WAKE_DOORBELL) {
            uint32_t target = 0;
            memcpy(&target, e.mmio.data, e.mmio.size < 4 ? e.mmio.size : 4);
            if (target == JOTUN_WAKE_ALL) {
                for (int j = 0; j < all_n; j++)
                    vcpu_wake(&all_ctx[j]);
            } else if ((int)target < all_n) {
                vcpu_wake(&all_ctx[target]);
            }
            continue;
        } else if (e.reason == VMM_EXIT_REASON_MMIO
                   && e.mmio.write && e.mmio.address == JOTUN_NET_DOORBELL) {
            net_service();
            continue;
        } else if (e.reason == VMM_EXIT_REASON_MMIO
                   && e.mmio.write && e.mmio.address == JOTUN_DRAW_DOORBELL) {
            draw_service();
            continue;
        } else if (e.reason == VMM_EXIT_REASON_MMIO) {
            int stop = 0;
            pthread_mutex_lock(&dev_lock);
            if (uart_mmio(&e)) {
                /* serviced by the console */
            } else if (e.mmio.write && e.mmio.address == JOTUN_DOORBELL) {
                ring_service();
            } else if (e.mmio.write && e.mmio.address == JOTUN_P9_DOORBELL) {
                p9_service();
            } else if (e.mmio.write && e.mmio.address == MMIO_POWEROFF) {
                printf("\n[vcpu %d powered off]\n", c->index);
                fflush(stdout);
                stop = 1;
            } else {
                fprintf(stderr, "[vcpu %d] unhandled MMIO at 0x%" PRIx64 "\n",
                        c->index, e.mmio.address);
                c->rc = 1;
                stop = 1;
            }
            pthread_mutex_unlock(&dev_lock);
            if (stop)
                break;
        } else {
            pthread_mutex_lock(&dev_lock);
            fprintf(stderr, "[vcpu %d] error or unknown exit\n", c->index);
            pthread_mutex_unlock(&dev_lock);
            c->rc = 1;
            break;
        }
    }

    __atomic_store_n(&c->live, 0, __ATOMIC_SEQ_CST);
    vmm_vcpu_destroy(vcpu);
    return NULL;
}

/* An empty handler so a signal breaks a vcpu out of its run with EINTR rather
 * than terminating the process. */
static void
sig_noop(int sig)
{
    (void)sig;
}

/* Timer thread: tick at the configured rate and preempt each running vcpu by
 * queuing the event interrupt and signalling the thread out of its run. */
struct timer_ctx {
    pthread_t *threads;
    struct vcpu_ctx *ctx;
    int n;
    unsigned hz;
    volatile int stop;
};

static void *
run_timer(void *arg)
{
    struct timer_ctx *t = arg;
    struct timespec period = {
        .tv_sec = 0,
        .tv_nsec = 1000000000L / (long)t->hz,
    };

    while (!__atomic_load_n(&t->stop, __ATOMIC_SEQ_CST)) {
        nanosleep(&period, NULL);
        for (int i = 0; i < t->n; i++) {
            if (!__atomic_load_n(&t->ctx[i].live, __ATOMIC_SEQ_CST))
                continue;
            vmm_vcpu_interrupt(t->ctx[i].vcpu, JOTUN_EVENT_VECTOR);
            pthread_kill(t->threads[i], SIGUSR1);
        }
    }
    return NULL;
}

int
main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "test_guest.elf";

    int nvcpus = 1;
    const char *vcpus_env = getenv("JOTUN_VCPUS");
    if (vcpus_env) {
        nvcpus = (int)strtol(vcpus_env, NULL, 0);
        if (nvcpus < 1)
            nvcpus = 1;
        if (nvcpus > MAX_VCPUS)
            nvcpus = MAX_VCPUS;
    }

    const char *cap_env = getenv("JOTUN_MAX_EXITS");
    max_exits = cap_env ? strtoul(cap_env, NULL, 0) : 0;

    vmm_vm_t *vm = vmm_vm_create();
    if (!vm) {
        fprintf(stderr, "Failed to create VM\n");
        return 1;
    }

    // Allocate guest RAM and map it at guest-physical address 0, in two slots
    // from one buffer, which exercises the multi-slot path.
    void *ram = mmap(NULL, GUEST_RAM_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ram == MAP_FAILED) {
        fprintf(stderr, "Failed to allocate guest memory\n");
        vmm_vm_destroy(vm);
        return 1;
    }
    size_t half = GUEST_RAM_SIZE / 2;
    vmm_vm_map_memory(vm, ram, 0, half);
    vmm_vm_map_memory(vm, (char *)ram + half, half, GUEST_RAM_SIZE - half);

    gm.base = ram;
    gm.size = GUEST_RAM_SIZE;

    // Load the guest image once into shared RAM.
    elf_image img;
    if (elf_load_file(path, ram, GUEST_RAM_SIZE, &img) != 0) {
        fprintf(stderr, "Failed to load guest image '%s'\n", path);
        munmap(ram, GUEST_RAM_SIZE);
        vmm_vm_destroy(vm);
        return 1;
    }

    /* A manifest, when given, is the full description of the guest domain: the
     * bundle and ref to serve, the subtree the guest is confined to, and the
     * init arguments. Without one, fall back to environment and command line. */
    struct manifest m;
    int have_manifest = 0;
    const char *manifest_path = getenv("JOTUN_MANIFEST");
    if (manifest_path) {
        if (manifest_load(manifest_path, &m) == 0)
            have_manifest = 1;
        else
            fprintf(stderr, "[manifest] failed to load %s\n", manifest_path);
    }

    const char *bundle, *ref, *subtree;
    int gargc;
    char *gargv[MANIFEST_MAXARG];
    if (have_manifest) {
        bundle = m.bundle;
        ref = m.ref;
        subtree = m.subtree;
        gargc = m.argc;
        for (int i = 0; i < m.argc; i++)
            gargv[i] = m.argv[i];
    } else {
        bundle = getenv("JOTUN_BUNDLE");
        ref = getenv("JOTUN_REF");
        if (!ref)
            ref = "root";
        subtree = "";
        gargc = (argc > 2) ? argc - 2 : 0;
        if (gargc > MANIFEST_MAXARG)
            gargc = MANIFEST_MAXARG;
        for (int i = 0; i < gargc; i++)
            gargv[i] = argv[2 + i];
    }

    /* Hand the guest its parameters before it starts. */
    setup_bootinfo(gargc, gargv);

    uart_init(&uart);

    /* Build the net policy from the manifest rules. With no rules the provider
     * allows every target; with rules it denies by default. */
    static struct net_policy policy;
    policy.n = 0;
    if (have_manifest) {
        for (int i = 0; i < m.nnet && policy.n < NET_MAXRULES; i++)
            if (build_net_rule(&m.net[i], &policy.rules[policy.n]) == 0)
                policy.n++;
    }

    /* The net provider shares the guest net channel and rouses vcpus when a
     * socket becomes ready. It is created before the 9P server so the /net
     * overlay can read its live connections. */
    struct jotun_net *netchan = gpa_to_host(JOTUN_NET_BASE, sizeof(*netchan));
    netdev = netchan ? net_init(netchan, rouse_vcpus, &policy) : NULL;

    /* The draw sink shares the guest draw channel and rouses vcpus when an
     * input event is queued. JOTUN_DRAW_TEST turns on the self-test event
     * source so the submit/poll path can run with no front end attached. */
    int draw_test = getenv("JOTUN_DRAW_TEST") != NULL;
    struct jotun_draw *drawchan = gpa_to_host(JOTUN_DRAW_BASE, sizeof(*drawchan));
    drawdev = drawchan
        ? draw_init(drawchan, rouse_vcpus, draw_test, draw_gpa, NULL) : NULL;

    /* If a signed origin is named, pull the bundle into a local cache depot and
     * serve that, exactly as a local depot. A pull failure refuses the boot:
     * the bundle's authenticity could not be established. */
    const char *origin = have_manifest ? m.origin : getenv("JOTUN_ORIGIN");
    const char *trust_key = have_manifest ? m.key : getenv("JOTUN_KEY");
    const char *label = have_manifest ? m.label : getenv("JOTUN_LABEL");
    if (label && !label[0])
        label = NULL;
    if (origin && origin[0]) {
        const char *cache = getenv("JOTUN_CACHE");
        if (!cache || !cache[0])
            cache = "bundle-cache";
        if (!trust_key || !trust_key[0]) {
            fprintf(stderr, "[pull] origin set but no trusted key"
                    " (set JOTUN_KEY or manifest 'key')\n");
            return 1;
        }
        fprintf(stderr, "[pull] fetching %s into %s\n", origin, cache);
        if (bundle_pull(origin, trust_key, label, cache, ref) != 0) {
            fprintf(stderr, "[pull] refusing to boot: bundle not pulled/verified\n");
            return 1;
        }
        bundle = cache;                 /* serve the verified cache depot */
    }

    /* Serve a smolvfs bundle if one is named, else the in-memory tree, with the
     * read-only /net overlay wrapping whichever backend is chosen. */
    const struct p9_backend *be = NULL;
    void *p9ctx = NULL;
    if (bundle && bundle[0])
        be = p9smolvfs_open(bundle, ref, subtree, &p9ctx);
    if (be) {
        fprintf(stderr, "[9p] serving bundle %s (ref %s, subtree %s)\n",
                bundle, ref, subtree[0] ? subtree : "/");
    } else {
        if (bundle && bundle[0])
            fprintf(stderr, "[9p] could not open bundle %s, using in-memory tree\n",
                    bundle);
        be = p9mem_backend();
        p9ctx = NULL;
    }

    struct p9overlay *ov = p9overlay_new(be, p9ctx, &policy, netdev);
    if (ov)
        p9srv_init(&p9, p9overlay_backend(), ov);
    else
        p9srv_init(&p9, be, p9ctx);     /* fallback: no /net overlay */

    /* SIGUSR1 breaks a running vcpu out of its run (no SA_RESTART, so KVM_RUN
     * returns EINTR). Both the preemption timer and the net poll thread use it,
     * so install it unconditionally. */
    struct sigaction sa = { 0 };
    sa.sa_handler = sig_noop;
    sigaction(SIGUSR1, &sa, NULL);

    /* A preemption timer is opt-in: JOTUN_HZ ticks per second. */
    unsigned hz = 0;
    const char *hz_env = getenv("JOTUN_HZ");
    if (hz_env)
        hz = (unsigned)strtoul(hz_env, NULL, 0);

    pthread_t threads[MAX_VCPUS];
    struct vcpu_ctx ctx[MAX_VCPUS];
    all_ctx = ctx;
    all_threads = threads;
    all_n = nvcpus;

    for (int i = 0; i < nvcpus; i++) {
        ctx[i].index = i;
        ctx[i].vm = vm;
        ctx[i].entry = img.entry;
        ctx[i].sp = GUEST_RAM_SIZE - (uint64_t)i * STACK_SLOT - 16;
        ctx[i].rc = 0;
        ctx[i].vcpu = NULL;
        ctx[i].live = 0;
        pthread_mutex_init(&ctx[i].idle_m, NULL);
        pthread_cond_init(&ctx[i].idle_c, NULL);
        ctx[i].woken = 0;
        if (pthread_create(&threads[i], NULL, run_vcpu, &ctx[i]) != 0) {
            fprintf(stderr, "Failed to start vcpu %d thread\n", i);
            ctx[i].rc = 1;
            threads[i] = 0;
        }
    }

    pthread_t timer_thread = 0;
    struct timer_ctx tctx = { threads, ctx, nvcpus, hz, 0 };
    if (hz > 0)
        pthread_create(&timer_thread, NULL, run_timer, &tctx);

    int rc = 0;
    for (int i = 0; i < nvcpus; i++) {
        if (threads[i])
            pthread_join(threads[i], NULL);
        rc |= ctx[i].rc;
    }

    if (timer_thread) {
        __atomic_store_n(&tctx.stop, 1, __ATOMIC_SEQ_CST);
        pthread_join(timer_thread, NULL);
    }

    net_shutdown(netdev);
    draw_shutdown(drawdev);

    vmm_vm_destroy(vm);
    munmap(ram, GUEST_RAM_SIZE);
    return rc;
}
