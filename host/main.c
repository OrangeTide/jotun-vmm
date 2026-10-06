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
#include "p9srv.h"
#include "p9mem.h"
#include "p9smolvfs.h"
#include "manifest.h"

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
};

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

    /* Serve a smolvfs bundle if one is named, else the in-memory tree. */
    const struct p9_backend *be = NULL;
    void *p9ctx = NULL;
    if (bundle && bundle[0])
        be = p9smolvfs_open(bundle, ref, subtree, &p9ctx);
    if (be) {
        p9srv_init(&p9, be, p9ctx);
        fprintf(stderr, "[9p] serving bundle %s (ref %s, subtree %s)\n",
                bundle, ref, subtree[0] ? subtree : "/");
    } else {
        if (bundle && bundle[0])
            fprintf(stderr, "[9p] could not open bundle %s, using in-memory tree\n",
                    bundle);
        p9srv_init(&p9, p9mem_backend(), NULL);
    }

    /* A preemption timer is opt-in: JOTUN_HZ ticks per second. The signal
     * handler lets a tick break a running vcpu out of its run. */
    unsigned hz = 0;
    const char *hz_env = getenv("JOTUN_HZ");
    if (hz_env)
        hz = (unsigned)strtoul(hz_env, NULL, 0);
    if (hz > 0) {
        struct sigaction sa = { 0 };
        sa.sa_handler = sig_noop;        /* no SA_RESTART: KVM_RUN returns EINTR */
        sigaction(SIGUSR1, &sa, NULL);
    }

    pthread_t threads[MAX_VCPUS];
    struct vcpu_ctx ctx[MAX_VCPUS];
    for (int i = 0; i < nvcpus; i++) {
        ctx[i].index = i;
        ctx[i].vm = vm;
        ctx[i].entry = img.entry;
        ctx[i].sp = GUEST_RAM_SIZE - (uint64_t)i * STACK_SLOT - 16;
        ctx[i].rc = 0;
        ctx[i].vcpu = NULL;
        ctx[i].live = 0;
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

    vmm_vm_destroy(vm);
    munmap(ram, GUEST_RAM_SIZE);
    return rc;
}
