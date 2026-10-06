#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "vmm.h"
#include "elf.h"
#include "uart.h"
#include "jotun_abi.h"
#include "jotun_boot.h"
#include "jotun_9p.h"
#include "p9srv.h"
#include "p9mem.h"

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

    vmm_vcpu_destroy(vcpu);
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

    /* Hand the guest its parameters: everything after the kernel image. */
    int gargc = (argc > 2) ? argc - 2 : 0;
    char **gargv = (argc > 2) ? argv + 2 : argv;
    setup_bootinfo(gargc, gargv);

    uart_init(&uart);
    p9srv_init(&p9, p9mem_backend(), NULL);

    pthread_t threads[MAX_VCPUS];
    struct vcpu_ctx ctx[MAX_VCPUS];
    for (int i = 0; i < nvcpus; i++) {
        ctx[i].index = i;
        ctx[i].vm = vm;
        ctx[i].entry = img.entry;
        ctx[i].sp = GUEST_RAM_SIZE - (uint64_t)i * STACK_SLOT - 16;
        ctx[i].rc = 0;
        if (pthread_create(&threads[i], NULL, run_vcpu, &ctx[i]) != 0) {
            fprintf(stderr, "Failed to start vcpu %d thread\n", i);
            ctx[i].rc = 1;
            threads[i] = 0;
        }
    }

    int rc = 0;
    for (int i = 0; i < nvcpus; i++) {
        if (threads[i])
            pthread_join(threads[i], NULL);
        rc |= ctx[i].rc;
    }

    vmm_vm_destroy(vm);
    munmap(ram, GUEST_RAM_SIZE);
    return rc;
}
