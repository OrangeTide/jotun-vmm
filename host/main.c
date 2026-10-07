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
#include "p9graft.h"
#include "manifest.h"
#include "bundlepull.h"
#include "net.h"
#include "draw.h"

/* This host runs native-arch guests (KVM and HVF both run the host CPU), so the
 * build arch is the guest arch, and this token selects the matching variant in
 * a multi-arch image. The tokens match uname -m. */
#if defined(__x86_64__)
#define JOTUN_ARCH "x86_64"
#elif defined(__aarch64__)
#define JOTUN_ARCH "aarch64"
#else
#define JOTUN_ARCH "unknown"
#endif

/* The path a bootable kernel is lifted from inside an image's arch root. */
#define JOTUN_KERNEL_PATH "boot/kernel"

/* Guest physical RAM mapped at address 0. 8 MiB is ample for the test guest
 * plus per-vcpu stacks. */
#define GUEST_RAM_SIZE (8u * 1024 * 1024)

/* Each vcpu gets its own 512 KiB stack, carved from the top of RAM downward, so
 * vcpus sharing one guest image do not clobber each other's stacks. */
#define STACK_SLOT     (512u * 1024)
#define MAX_VCPUS      8

/* The host runs up to this many guests in one process, each its own VM. */
#define MAX_GUESTS     8

/* Process-wide host state touched by every vcpu thread. The console and its
 * lock are shared across the process; per-guest state lives in struct guest. */
static uart_16550 uart;
static pthread_mutex_t dev_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long max_exits;     /* 0 means no cap */
static struct vswitch *g_vswitch;   /* in-host name service, shared by guests */

struct guest;                       /* defined below; vcpu_ctx points back to it */

struct vcpu_ctx {
    int index;
    struct guest *owner;        /* the guest this vcpu belongs to */
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

/* Guest physical memory as the host sees it, identity-mapped for now (guest
 * physical address N is host byte N of this buffer). */
struct guest_mem {
    uint8_t *base;
    uint64_t size;
};

/* One guest domain: its VM and RAM, its vcpu table, and the devices that share
 * its channels. There is one instance today (g); M10 grows this to several so
 * the host can run and composite multiple guests. These fields were file-scope
 * singletons; for now they are reached through the single g, and a later step
 * threads a struct guest * through the call graph so each guest is addressed by
 * pointer rather than by this global. */
struct guest {
    int gid;                    /* 0..nguests-1; guest 0 owns the front end */
    vmm_vm_t *vm;
    struct guest_mem gm;
    elf_image img;

    struct vcpu_ctx ctx[MAX_VCPUS];
    pthread_t threads[MAX_VCPUS];
    int nvcpus;

    struct netdev *netdev;      /* net provider sharing this guest's channel */
    struct drawdev *drawdev;    /* draw sink sharing this guest's channel */
    struct p9srv p9;            /* 9P server, guarded by dev_lock */
    struct net_policy policy;   /* net rules for this guest */
    struct manifest manifest;   /* this guest's domain config, if from -m */
};

static struct guest guests[MAX_GUESTS];
static int nguests;

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

/* Rouse every vcpu of one guest for a device event: inject the event interrupt
 * into its running vcpus (breaking their run so the guest ISR fires) and wake
 * any that are idle (so they leave the idle rendezvous and recheck). The net
 * poll thread calls this (with the owning guest as ctx) when a socket a guest
 * task waits on becomes ready, and the draw sink when an input event is queued.
 * Only this guest's vcpus are touched, so one guest's device traffic never
 * disturbs another. */
static void
rouse_vcpus(void *ctx)
{
    struct guest *gp = ctx;

    for (int i = 0; i < gp->nvcpus; i++) {
        if (__atomic_load_n(&gp->ctx[i].live, __ATOMIC_SEQ_CST)) {
            vmm_vcpu_interrupt(gp->ctx[i].vcpu, JOTUN_EVENT_VECTOR);
            if (gp->threads[i])
                pthread_kill(gp->threads[i], SIGUSR1);
        }
        vcpu_wake(&gp->ctx[i]);
    }
}

/* Translate a guest-physical range to a host pointer, or NULL if it does not
 * fit entirely within guest RAM. Every guest-supplied address is checked here
 * before the host dereferences it. */
static void *
gpa_to_host(struct guest *gp, uint64_t gpa, uint64_t len)
{
    if (gpa > gp->gm.size || len > gp->gm.size - gpa)
        return NULL;
    return gp->gm.base + gpa;
}

/* The draw sink's address translator: same bounds check, with the callback
 * signature the sink expects. The context is the owning guest, passed to
 * draw_init. */
static void *
draw_gpa(void *ctx, uint64_t gpa, uint64_t len)
{
    return gpa_to_host((struct guest *)ctx, gpa, len);
}

/* Write the boot parameter block into guest RAM before the vcpu starts. The
 * guest argv is whatever follows the kernel image on the command line, with
 * argv[0] the init program. */
static void
setup_bootinfo(struct guest *gp, int gargc, char **gargv)
{
    struct jotun_bootinfo *b = gpa_to_host(gp, JOTUN_BOOT_BASE, sizeof(*b));
    if (!b)
        return;

    memset(b, 0, sizeof(*b));
    b->magic = JOTUN_BOOT_MAGIC;
    b->version = JOTUN_BOOT_VERSION;
    b->ram_size = gp->gm.size;

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
ring_service(struct guest *gp)
{
    struct jotun_ring *r = gpa_to_host(gp, JOTUN_RING_BASE, sizeof(*r));
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
p9_service(struct guest *gp)
{
    struct jotun_p9 *c = gpa_to_host(gp, JOTUN_P9_BASE, sizeof(*c));
    if (!c)
        return;
    c->rsp_len = p9srv_handle(&gp->p9, c->req, c->req_len, c->rsp,
                              JOTUN_P9_MSIZE);
}

/* Net doorbell handler: process the request in the net channel and write the
 * result back. Called without dev_lock, since the provider has its own lock
 * and a dial may resolve a name, which should not block the console. */
static void
net_service(struct guest *gp)
{
    struct jotun_net *c = gpa_to_host(gp, JOTUN_NET_BASE, sizeof(*c));
    if (!c || !gp->netdev)
        return;
    net_handle(gp->netdev, c);
}

/* Draw doorbell handler: process the request in the draw channel and write the
 * result back. Called without dev_lock, like the net doorbell, since the sink
 * has no shared console state. */
static void
draw_service(struct guest *gp)
{
    struct jotun_draw *c = gpa_to_host(gp, JOTUN_DRAW_BASE, sizeof(*c));
    if (!c || !gp->drawdev)
        return;
    draw_handle(gp->drawdev, c);
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
    struct guest *gp = c->owner;

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
                for (int j = 0; j < gp->nvcpus; j++)
                    vcpu_wake(&gp->ctx[j]);
            } else if ((int)target < gp->nvcpus) {
                vcpu_wake(&gp->ctx[target]);
            }
            continue;
        } else if (e.reason == VMM_EXIT_REASON_MMIO
                   && e.mmio.write && e.mmio.address == JOTUN_NET_DOORBELL) {
            net_service(gp);
            continue;
        } else if (e.reason == VMM_EXIT_REASON_MMIO
                   && e.mmio.write && e.mmio.address == JOTUN_DRAW_DOORBELL) {
            draw_service(gp);
            continue;
        } else if (e.reason == VMM_EXIT_REASON_MMIO) {
            int stop = 0;
            pthread_mutex_lock(&dev_lock);
            if (uart_mmio(&e)) {
                /* serviced by the console */
            } else if (e.mmio.write && e.mmio.address == JOTUN_DOORBELL) {
                ring_service(gp);
            } else if (e.mmio.write && e.mmio.address == JOTUN_P9_DOORBELL) {
                p9_service(gp);
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
    struct guest *guests;
    int nguests;
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
        for (int gi = 0; gi < t->nguests; gi++) {
            struct guest *gp = &t->guests[gi];
            for (int i = 0; i < gp->nvcpus; i++) {
                if (!__atomic_load_n(&gp->ctx[i].live, __ATOMIC_SEQ_CST))
                    continue;
                vmm_vcpu_interrupt(gp->ctx[i].vcpu, JOTUN_EVENT_VECTOR);
                pthread_kill(gp->threads[i], SIGUSR1);
            }
        }
    }
    return NULL;
}

/* Create one guest's VM and RAM: create the VM and map a fresh RAM buffer at
 * guest-physical address 0 (in two slots, which exercises the multi-slot path).
 * Fills gp->vm and gp->gm. The kernel image is loaded separately, once the
 * bundle it may come from is open. Returns 0, or -1 on failure with anything
 * partially allocated already released. Each guest gets its own VM and buffer,
 * so several can run side by side. */
static int
guest_vm_create(struct guest *gp)
{
    gp->vm = vmm_vm_create();
    if (!gp->vm) {
        fprintf(stderr, "Failed to create VM\n");
        return -1;
    }

    void *ram = mmap(NULL, GUEST_RAM_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ram == MAP_FAILED) {
        fprintf(stderr, "Failed to allocate guest memory\n");
        vmm_vm_destroy(gp->vm);
        gp->vm = NULL;
        return -1;
    }
    size_t half = GUEST_RAM_SIZE / 2;
    vmm_vm_map_memory(gp->vm, ram, 0, half);
    vmm_vm_map_memory(gp->vm, (char *)ram + half, half, GUEST_RAM_SIZE - half);
    gp->gm.base = ram;
    gp->gm.size = GUEST_RAM_SIZE;
    return 0;
}

/* Load the guest kernel into RAM. The image comes from `cli_kernel` when the
 * command line named one (a dev override), else from the bundle at
 * JOTUN_KERNEL_PATH when it is a smolvfs backend that holds one, else from the
 * historical test_guest.elf default so a bare run still boots. svctx is the
 * smolvfs context from p9smolvfs_open, or NULL for the in-memory backend.
 * Fills gp->img. Returns 0, or -1 on failure. */
static int
guest_load_kernel(struct guest *gp, int gid, const char *cli_kernel,
                  void *svctx)
{
    if (cli_kernel)
        return elf_load_file(cli_kernel, gp->gm.base, GUEST_RAM_SIZE, &gp->img);

    if (svctx) {
        void *img = NULL;
        size_t len = 0;
        if (p9smolvfs_readfile(svctx, JOTUN_KERNEL_PATH, &img, &len) == 0) {
            int rc = elf_load_mem(JOTUN_KERNEL_PATH, img, (long)len,
                                  gp->gm.base, GUEST_RAM_SIZE, &gp->img);
            free(img);
            if (rc == 0)
                fprintf(stderr, "[boot] guest %d: kernel lifted from image"
                        " (%s)\n", gid, JOTUN_KERNEL_PATH);
            return rc;
        }
    }

    return elf_load_file("test_guest.elf", gp->gm.base, GUEST_RAM_SIZE,
                         &gp->img);
}

/* Release a guest's VM and RAM. Safe to call after a partial or failed init. */
static void
guest_vm_free(struct guest *gp)
{
    if (gp->vm) {
        vmm_vm_destroy(gp->vm);
        gp->vm = NULL;
    }
    if (gp->gm.base) {
        munmap(gp->gm.base, gp->gm.size);
        gp->gm.base = NULL;
    }
}

/* Bring one guest fully up and start its vcpus: resolve its domain config (from
 * a -m manifest, or from the environment and the trailing CLI args when none is
 * given), create its VM and RAM, write its boot block, install its net policy
 * and provider, attach the browser front end for guest 0 only, pull a signed
 * bundle if an origin is named, open its 9P server, and spawn its vcpu threads.
 * Returns 0, or -1 on failure (a pull that cannot be verified refuses the
 * boot). The env/argv fallback applies only to a lone guest with no manifest. */
static int
guest_bringup(struct guest *gp, int gid, const char *kernel,
              const char *manifest_path, int nvcpus, int draw_test,
              struct drawfe *fe, int targc, char **targv)
{
    gp->gid = gid;

    if (guest_vm_create(gp) != 0)
        return -1;

    /* A manifest, when given, is the full description of the guest domain: the
     * bundle and ref to serve, the subtree it is confined to, and the init
     * arguments. It is stored in the guest so its strings outlive this call. */
    int have_manifest = 0;
    if (manifest_path) {
        if (manifest_load(manifest_path, &gp->manifest) == 0)
            have_manifest = 1;
        else
            fprintf(stderr, "[manifest] failed to load %s\n", manifest_path);
    }

    const char *bundle, *ref, *subtree;
    int gargc;
    char *gargv[MANIFEST_MAXARG];
    if (have_manifest) {
        bundle = gp->manifest.bundle;
        ref = gp->manifest.ref;
        subtree = gp->manifest.subtree;
        gargc = gp->manifest.argc;
        for (int i = 0; i < gp->manifest.argc; i++)
            gargv[i] = gp->manifest.argv[i];
    } else {
        bundle = getenv("JOTUN_BUNDLE");
        ref = getenv("JOTUN_REF");
        if (!ref)
            ref = "root";
        subtree = "";
        gargc = targc > MANIFEST_MAXARG ? MANIFEST_MAXARG : targc;
        for (int i = 0; i < gargc; i++)
            gargv[i] = targv[i];
    }

    /* If a signed origin is named, pull the bundle into a local cache depot and
     * serve that, exactly as a local depot. The kernel and the filesystem then
     * both come from the verified object set. A pull failure refuses the boot:
     * the bundle's authenticity could not be established. */
    const char *origin = have_manifest ? gp->manifest.origin : getenv("JOTUN_ORIGIN");
    const char *trust_key = have_manifest ? gp->manifest.key : getenv("JOTUN_KEY");
    const char *label = have_manifest ? gp->manifest.label : getenv("JOTUN_LABEL");
    if (label && !label[0])
        label = NULL;
    if (origin && origin[0]) {
        const char *cache = getenv("JOTUN_CACHE");
        if (!cache || !cache[0])
            cache = "bundle-cache";
        if (!trust_key || !trust_key[0]) {
            fprintf(stderr, "[pull] guest %d: origin set but no trusted key"
                    " (set JOTUN_KEY or manifest 'key')\n", gid);
            return -1;
        }
        fprintf(stderr, "[pull] guest %d: fetching %s into %s\n", gid, origin, cache);
        if (bundle_pull(origin, trust_key, label, cache, ref) != 0) {
            fprintf(stderr, "[pull] guest %d: refusing to boot:"
                    " bundle not pulled/verified\n", gid);
            return -1;
        }
        bundle = cache;                 /* serve the verified cache depot */
    }

    /* Open the bundle, selecting this host's arch from a multi-arch image, or
     * fall back to the in-memory tree. svctx is the smolvfs context: it lifts
     * the kernel below, and the /net overlay wraps it to serve files. */
    const struct p9_backend *be = NULL;
    void *p9ctx = NULL;
    void *svctx = NULL;
    if (bundle && bundle[0])
        be = p9smolvfs_open(bundle, ref, subtree, JOTUN_ARCH, &p9ctx);
    if (be) {
        svctx = p9ctx;
        fprintf(stderr, "[9p] guest %d: serving bundle %s (ref %s, subtree %s)\n",
                gid, bundle, ref, subtree[0] ? subtree : "/");
    } else {
        if (bundle && bundle[0])
            fprintf(stderr, "[9p] guest %d: could not open bundle %s,"
                    " using in-memory tree\n", gid, bundle);
        be = p9mem_backend();
        p9ctx = NULL;
    }

    /* Load the kernel into RAM now that any bundle is open: a command-line image
     * overrides, else it is lifted from the bundle, else the historical default. */
    if (guest_load_kernel(gp, gid, kernel, svctx) != 0) {
        fprintf(stderr, "guest %d: failed to load kernel\n", gid);
        return -1;
    }

    setup_bootinfo(gp, gargc, gargv);

    /* Build the net policy from the manifest rules. With no rules the provider
     * allows every target; with rules it denies by default. */
    gp->policy.n = 0;
    if (have_manifest) {
        for (int i = 0; i < gp->manifest.nnet && gp->policy.n < NET_MAXRULES; i++)
            if (build_net_rule(&gp->manifest.net[i],
                               &gp->policy.rules[gp->policy.n]) == 0)
                gp->policy.n++;
    }

    /* The net provider shares this guest's net channel and rouses its vcpus
     * when a socket becomes ready. Created before the 9P server so the /net
     * overlay can read its live connections. */
    const char *netname = have_manifest ? gp->manifest.name : getenv("JOTUN_NAME");
    struct jotun_net *netchan = gpa_to_host(gp, JOTUN_NET_BASE, sizeof(*netchan));
    gp->netdev = netchan
        ? net_init(netchan, rouse_vcpus, gp, &gp->policy, g_vswitch, netname)
        : NULL;

    /* Every guest gets a draw sink attached to the one shared front end, which
     * composites their scenes into a single browser session. With no front end
     * (JOTUN_DRAW_PORT=0, fe NULL) the sink still exists but draws nowhere. */
    struct jotun_draw *drawchan =
        gpa_to_host(gp, JOTUN_DRAW_BASE, sizeof(*drawchan));
    gp->drawdev = drawchan
        ? draw_init(drawchan, gid, fe, rouse_vcpus, gp, draw_test, draw_gpa, gp)
        : NULL;

    /* An optional graft mount makes a shared read-only tree appear at a
     * top-level name in this guest's root, alongside its own tree. It wraps the
     * base backend before the /net overlay wraps the result. */
    if (have_manifest && gp->manifest.mount_name[0]) {
        void *gctx = NULL;
        const struct p9_backend *gbe =
            p9smolvfs_open(gp->manifest.mount_bundle, gp->manifest.mount_ref,
                           gp->manifest.mount_subtree, JOTUN_ARCH, &gctx);
        if (!gbe) {
            fprintf(stderr, "[9p] guest %d: could not open mount bundle %s\n",
                    gid, gp->manifest.mount_bundle);
        } else {
            struct p9graft *g =
                p9graft_new(be, p9ctx, gbe, gctx, gp->manifest.mount_name);
            if (g) {
                be = p9graft_backend();
                p9ctx = g;
                fprintf(stderr, "[9p] guest %d: grafted %s (ref %s) at /%s\n",
                        gid, gp->manifest.mount_bundle, gp->manifest.mount_ref,
                        gp->manifest.mount_name);
            } else {
                p9smolvfs_close(gctx);
            }
        }
    }

    struct p9overlay *ov = p9overlay_new(be, p9ctx, &gp->policy, gp->netdev);
    if (ov)
        p9srv_init(&gp->p9, p9overlay_backend(), ov);
    else
        p9srv_init(&gp->p9, be, p9ctx);     /* fallback: no /net overlay */

    gp->nvcpus = nvcpus;
    for (int i = 0; i < nvcpus; i++) {
        gp->ctx[i].index = i;
        gp->ctx[i].owner = gp;
        gp->ctx[i].vm = gp->vm;
        gp->ctx[i].entry = gp->img.entry;
        gp->ctx[i].sp = GUEST_RAM_SIZE - (uint64_t)i * STACK_SLOT - 16;
        gp->ctx[i].rc = 0;
        gp->ctx[i].vcpu = NULL;
        gp->ctx[i].live = 0;
        pthread_mutex_init(&gp->ctx[i].idle_m, NULL);
        pthread_cond_init(&gp->ctx[i].idle_c, NULL);
        gp->ctx[i].woken = 0;
        if (pthread_create(&gp->threads[i], NULL, run_vcpu, &gp->ctx[i]) != 0) {
            fprintf(stderr, "guest %d: failed to start vcpu %d thread\n", gid, i);
            gp->ctx[i].rc = 1;
            gp->threads[i] = 0;
        }
    }
    return 0;
}

int
main(int argc, char **argv)
{
    /* Each -m names one guest's manifest; the first bare argument is the kernel
     * image shared by all guests, and any trailing arguments are the init args
     * for the lone no-manifest guest. With no -m, one guest runs from the
     * environment (and JOTUN_MANIFEST), exactly as before. */
    const char *kernel = NULL;
    const char *manifests[MAX_GUESTS];
    int nmanifests = 0;
    int targc = 0;
    char *targv[MANIFEST_MAXARG];

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            if (nmanifests >= MAX_GUESTS) {
                fprintf(stderr, "too many -m manifests (max %d)\n", MAX_GUESTS);
                return 1;
            }
            manifests[nmanifests++] = argv[++i];
        } else if (!kernel) {
            kernel = argv[i];
        } else if (targc < MANIFEST_MAXARG) {
            targv[targc++] = argv[i];
        }
    }
    /* With no bare kernel argument, guest_load_kernel lifts one from the bundle
     * or falls back to the historical default, so leave kernel NULL here. */

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

    int draw_test = getenv("JOTUN_DRAW_TEST") != NULL;

    /* One browser front end backs the whole session; guest 0 attaches its sink.
     * Opt out with JOTUN_DRAW_PORT=0; the default port is localhost-only. */
    const char *pe = getenv("JOTUN_DRAW_PORT");
    int draw_port = pe ? (int)strtol(pe, NULL, 0) : 9230;
    struct drawfe *fe = drawfe_start(draw_port);

    /* A preemption timer is opt-in: JOTUN_HZ ticks per second, across all. */
    unsigned hz = 0;
    const char *hz_env = getenv("JOTUN_HZ");
    if (hz_env)
        hz = (unsigned)strtoul(hz_env, NULL, 0);

    uart_init(&uart);

    /* SIGUSR1 breaks a running vcpu out of its run (no SA_RESTART, so KVM_RUN
     * returns EINTR). The preemption timer and the net poll threads use it, so
     * install it once for the process. */
    struct sigaction sa = { 0 };
    sa.sa_handler = sig_noop;
    sigaction(SIGUSR1, &sa, NULL);

    /* One in-host switch backs the whole process, so guests can reach each
     * other by name. */
    g_vswitch = vswitch_new();

    nguests = nmanifests > 0 ? nmanifests : 1;

    for (int gi = 0; gi < nguests; gi++) {
        const char *mpath = nmanifests > 0 ? manifests[gi]
                                           : getenv("JOTUN_MANIFEST");
        if (guest_bringup(&guests[gi], gi, kernel, mpath, nvcpus, draw_test,
                          fe, targc, targv) != 0) {
            fprintf(stderr, "guest %d: bring-up failed\n", gi);
            return 1;
        }
    }

    pthread_t timer_thread = 0;
    struct timer_ctx tctx = { guests, nguests, hz, 0 };
    if (hz > 0)
        pthread_create(&timer_thread, NULL, run_timer, &tctx);

    int rc = 0;
    for (int gi = 0; gi < nguests; gi++) {
        struct guest *gp = &guests[gi];
        for (int i = 0; i < gp->nvcpus; i++) {
            if (gp->threads[i])
                pthread_join(gp->threads[i], NULL);
            rc |= gp->ctx[i].rc;
        }
    }

    if (timer_thread) {
        __atomic_store_n(&tctx.stop, 1, __ATOMIC_SEQ_CST);
        pthread_join(timer_thread, NULL);
    }

    /* Stop the front end first, so its server thread delivers no more events
     * to a sink before draw_shutdown frees it. */
    drawfe_stop(fe);
    for (int gi = 0; gi < nguests; gi++) {
        net_shutdown(guests[gi].netdev);
        draw_shutdown(guests[gi].drawdev);
        guest_vm_free(&guests[gi]);
    }
    return rc;
}
