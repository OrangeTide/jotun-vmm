#ifdef __linux__

#include "vmm.h"
#include "jotun_abi.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__aarch64__)
/* Encode an AArch64 core register id for KVM_SET_ONE_REG from the matching
 * field in struct kvm_regs (the id carries the field's u32 offset). */
#define ARM64_CORE_REG(name) \
    (KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE | \
     KVM_REG_ARM_CORE_REG(name))
#endif

#if defined(__x86_64__)
/* Identity-map page tables for the guest's long mode, placed low in guest RAM
 * below the ring and the guest image. A single page directory of 512 2 MiB
 * pages covers the first 1 GiB, which spans all guest RAM and the MMIO
 * addresses above it (present but unbacked, so they fault out as MMIO). */
#define PT_PML4   0x1000
#define PT_PDPT   0x2000
#define PT_PD     0x3000
#define PTE_P     0x1       /* present */
#define PTE_RW    0x2       /* writable */
#define PTE_PS    0x80      /* 2 MiB page */
#endif

/* Concrete layout for the opaque handles declared in vmm.h. */
struct vmm_vm {
    int kvm_fd;
    int vm_fd;
    int next_slot;          /* memory slot allocator (main thread only) */
    int next_vcpu_id;       /* vcpu id allocator (atomic; threads create vcpus) */
};

struct vmm_vcpu {
    int kvm_fd;
    int vm_fd;
    int vcpu_fd;
    struct kvm_run *run;    /* shared run structure, mapped for the vcpu's life */
    int mmap_size;
    volatile int irq_pending;   /* set by another thread, delivered by the run */
    uint8_t irq_vector;
    uint32_t apic_id;           /* this vcpu's local-APIC id (= its index) */
    volatile int gic_ready;     /* arm64: the vGIC is up and can take injections */
    int gic_dev_fd;             /* arm64: the vGIC device handle, -1 when none */
};

// Lifecycle management functions
vmm_vm_t* vmm_init() {
    vmm_vm_t* vm = (vmm_vm_t*)malloc(sizeof(vmm_vm_t));
    vm->kvm_fd = open("/dev/kvm", O_RDWR);
    if (vm->kvm_fd < 0) {
        free(vm);
        return NULL;
    }
    vm->vm_fd = ioctl(vm->kvm_fd, KVM_CREATE_VM, 0);
    if (vm->vm_fd < 0) {
        close(vm->kvm_fd);
        free(vm);
        return NULL;
    }
#if defined(__x86_64__)
    /* Give the VM an in-kernel irqchip (PIC, IOAPIC, and a per-vcpu local APIC).
     * KVM then delivers interrupts to a running vcpu itself, atomically, which is
     * how the preemption tick and device events reach the guest without the
     * host racing a signal against KVM_RUN. The emulated chipset needs a TSS
     * region in guest physical space (used only by KVM's real-mode emulation,
     * which this long-mode guest never enters); place it high, above guest RAM.
     * Both ioctls are best-effort: an older KVM without them still boots with the
     * previous userspace-injection fallback compiled elsewhere. */
    ioctl(vm->vm_fd, KVM_SET_TSS_ADDR, 0xfffbd000UL);
    ioctl(vm->vm_fd, KVM_CREATE_IRQCHIP);
#endif
    vm->next_slot = 0;
    vm->next_vcpu_id = 0;
    return vm;
}

vmm_vm_t* vmm_vm_create() {
    return vmm_init();
}

void vmm_vm_map_memory(vmm_vm_t* vm, void* host_addr, uint64_t guest_addr, size_t size) {
    struct kvm_userspace_memory_region region = {
        .slot = vm->next_slot++,
        .flags = 0,
        .guest_phys_addr = guest_addr,
        .memory_size = size,
        .userspace_addr = (uint64_t)host_addr
    };
    if (ioctl(vm->vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        // Handle error
    }

#if defined(__x86_64__)
    // Long mode needs page tables in guest RAM. Build an identity map in the
    // region covering guest-physical address 0; every vcpu points CR3 here.
    if (guest_addr == 0) {
        uint8_t *base = (uint8_t*)host_addr;
        uint64_t *pml4 = (uint64_t*)(base + PT_PML4);
        uint64_t *pdpt = (uint64_t*)(base + PT_PDPT);
        uint64_t *pd = (uint64_t*)(base + PT_PD);
        pml4[0] = PT_PDPT | PTE_P | PTE_RW;
        pdpt[0] = PT_PD | PTE_P | PTE_RW;
        for (int i = 0; i < 512; i++)
            pd[i] = ((uint64_t)i << 21) | PTE_P | PTE_RW | PTE_PS;
    }
#endif
}

vmm_vcpu_t* vmm_vcpu_create(vmm_vm_t* vm) {
    vmm_vcpu_t* vcpu = (vmm_vcpu_t*)malloc(sizeof(vmm_vcpu_t));
    vcpu->kvm_fd = vm->kvm_fd;
    vcpu->vm_fd = vm->vm_fd;
    int vcpu_id = __atomic_fetch_add(&vm->next_vcpu_id, 1, __ATOMIC_SEQ_CST);
    vcpu->vcpu_fd = ioctl(vm->vm_fd, KVM_CREATE_VCPU, (unsigned long)vcpu_id);
    if (vcpu->vcpu_fd < 0) {
        free(vcpu);
        return NULL;
    }
    vcpu->irq_pending = 0;
    vcpu->irq_vector = 0;
    vcpu->apic_id = (uint32_t)vcpu_id;  /* KVM's default local-APIC id */
    vcpu->gic_ready = 0;
    vcpu->gic_dev_fd = -1;

#if defined(__x86_64__)
    // Put the vcpu directly into 64-bit long mode on the identity map built in
    // vmm_vm_map_memory. Flat segments are programmed through the descriptor
    // cache, so no GDT in guest memory is needed as long as the guest does not
    // reload the segment registers. Entry and stack are set later via
    // VMM_REG_PC / VMM_REG_SP.

    // Advertise the host-supported CPUID to the guest. KVM refuses to set
    // EFER.SCE (the SYSCALL enable) unless the guest CPUID reports the SYSCALL
    // feature, which the Linux binary-compat path (M16.1) needs. The normal
    // libOS never executes CPUID, so this only unblocks that path.
    {
        size_t sz = sizeof(struct kvm_cpuid2) +
                    128 * sizeof(struct kvm_cpuid_entry2);
        struct kvm_cpuid2 *cpuid = calloc(1, sz);
        if (cpuid) {
            cpuid->nent = 128;
            if (ioctl(vcpu->kvm_fd, KVM_GET_SUPPORTED_CPUID, cpuid) < 0)
                fprintf(stderr, "kvm: KVM_GET_SUPPORTED_CPUID failed\n");
            else if (ioctl(vcpu->vcpu_fd, KVM_SET_CPUID2, cpuid) < 0)
                fprintf(stderr, "kvm: KVM_SET_CPUID2 failed\n");
            free(cpuid);
        }
    }

    struct kvm_sregs sregs;
    if (ioctl(vcpu->vcpu_fd, KVM_GET_SREGS, &sregs) < 0) {
        close(vcpu->vcpu_fd);
        free(vcpu);
        return NULL;
    }
    struct kvm_segment code = {
        .base = 0, .limit = 0xffffffff, .selector = 0x08,
        .type = 0xb,        /* execute/read, accessed */
        .present = 1, .dpl = 0, .db = 0, .s = 1, .l = 1, .g = 1,
    };
    struct kvm_segment data = {
        .base = 0, .limit = 0xffffffff, .selector = 0x10,
        .type = 0x3,        /* read/write, accessed */
        .present = 1, .dpl = 0, .db = 1, .s = 1, .l = 0, .g = 1,
    };
    sregs.cr3 = PT_PML4;
    sregs.cr4 |= (1u << 5);                 /* CR4.PAE */
    sregs.cr0 |= (1u << 0) | (1u << 31);    /* CR0.PE | CR0.PG */
    sregs.efer |= (1u << 8) | (1u << 10);   /* EFER.LME | EFER.LMA */
    sregs.cs = code;
    sregs.ds = sregs.es = sregs.fs = sregs.gs = sregs.ss = data;
    if (ioctl(vcpu->vcpu_fd, KVM_SET_SREGS, &sregs) < 0) {
        close(vcpu->vcpu_fd);
        free(vcpu);
        return NULL;
    }

    struct kvm_regs regs = {
        .rflags = 0x2,
    };
    if (ioctl(vcpu->vcpu_fd, KVM_SET_REGS, &regs) < 0) {
        close(vcpu->vcpu_fd);
        free(vcpu);
        return NULL;
    }
#elif defined(__aarch64__)
    // An arm64 vcpu must be initialised to a CPU model before KVM_RUN. Ask the
    // kernel for the host's preferred target and apply it. After this the vcpu
    // resets with PC = 0; the entry point is set later via VMM_REG_PC.
    struct kvm_vcpu_init init;
    if (ioctl(vm->vm_fd, KVM_ARM_PREFERRED_TARGET, &init) < 0) {
        close(vcpu->vcpu_fd);
        free(vcpu);
        return NULL;
    }
    if (ioctl(vcpu->vcpu_fd, KVM_ARM_VCPU_INIT, &init) < 0) {
        close(vcpu->vcpu_fd);
        free(vcpu);
        return NULL;
    }
#else
#error "unsupported KVM host architecture"
#endif

    // Map the shared run structure once; it stays mapped for the vcpu's life so
    // I/O data written back on a read exit survives into the next KVM_RUN.
    vcpu->mmap_size = ioctl(vcpu->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (vcpu->mmap_size < 0) {
        close(vcpu->vcpu_fd);
        free(vcpu);
        return NULL;
    }
    vcpu->run = mmap(NULL, vcpu->mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                     vcpu->vcpu_fd, 0);
    if (vcpu->run == MAP_FAILED) {
        close(vcpu->vcpu_fd);
        free(vcpu);
        return NULL;
    }
    return vcpu;
}

void vmm_vcpu_set_reg(vmm_vcpu_t* vcpu, vmm_reg_t reg, uint64_t value) {
#if defined(__x86_64__)
    struct kvm_regs regs;
    if (ioctl(vcpu->vcpu_fd, KVM_GET_REGS, &regs) < 0) {
        // Handle error
        return;
    }
    switch (reg) {
        case VMM_REG_PC:
            regs.rip = value;
            break;
        case VMM_REG_SP:
            regs.rsp = value;
            break;
        case VMM_REG_ARG:
            regs.rdi = value;
            break;
    }
    if (ioctl(vcpu->vcpu_fd, KVM_SET_REGS, &regs) < 0) {
        // Handle error
    }
#elif defined(__aarch64__)
    // AArch64 registers are set one at a time through KVM_SET_ONE_REG. The
    // guest runs at EL1, so its stack pointer is sp_el1.
    uint64_t id;
    switch (reg) {
        case VMM_REG_PC:
            id = ARM64_CORE_REG(regs.pc);
            break;
        case VMM_REG_SP:
            id = ARM64_CORE_REG(sp_el1);
            break;
        case VMM_REG_ARG:
            id = ARM64_CORE_REG(regs.regs[0]);  /* x0 */
            break;
        default:
            return;
    }
    struct kvm_one_reg one = {
        .id = id,
        .addr = (uint64_t)(uintptr_t)&value,
    };
    if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &one) < 0) {
        // Handle error
    }
#endif
}

// Read the guest program counter. Used only by the JOTUN_DEBUG trace, so the
// cost of this register-read ioctl never touches the normal path.
static uint64_t vcpu_pc(vmm_vcpu_t* vcpu) {
#if defined(__x86_64__)
    struct kvm_regs regs;
    if (ioctl(vcpu->vcpu_fd, KVM_GET_REGS, &regs) < 0)
        return 0;
    return regs.rip;
#elif defined(__aarch64__)
    uint64_t pc = 0;
    struct kvm_one_reg one = {
        .id = ARM64_CORE_REG(regs.pc),
        .addr = (uint64_t)(uintptr_t)&pc,
    };
    ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &one);
    return pc;
#endif
}

void vmm_vcpu_interrupt(vmm_vcpu_t* vcpu, uint8_t vector) {
#if defined(__x86_64__)
    /* Deliver an edge-triggered, fixed-delivery interrupt to this vcpu's local
     * APIC as a message-signalled interrupt. With the in-kernel irqchip KVM
     * routes it to the matching APIC id and kicks the vcpu out of guest mode to
     * take it, so a running compute loop is preempted atomically with no signal
     * and no injection race. Safe to call from any thread. */
    struct kvm_msi msi = {
        .address_lo = 0xfee00000u | (vcpu->apic_id << 12),
        .address_hi = 0,
        .data = vector,             /* fixed delivery, physical dest, edge */
        .flags = 0,
    };
    ioctl(vcpu->vm_fd, KVM_SIGNAL_MSI, &msi);
#elif defined(__aarch64__)
    /* Pulse the shared peripheral interrupt the guest configured for the tick.
     * KVM's in-kernel GICv3 delivers it and kicks the running vcpu out of
     * KVM_RUN, so a user compute loop is preempted; the guest acknowledges
     * through the GIC CPU interface. A 0->1->0 edge matches the edge-triggered
     * config, so each tick is one interrupt with no line to lower afterward.
     * Only meaningful once the vGIC is up (single-vcpu guest). */
    (void)vector;
    if (!__atomic_load_n(&vcpu->gic_ready, __ATOMIC_SEQ_CST))
        return;
    unsigned irq = ((unsigned)KVM_ARM_IRQ_TYPE_SPI << KVM_ARM_IRQ_TYPE_SHIFT)
                 | ((unsigned)JOTUN_EVENT_SPI << KVM_ARM_IRQ_NUM_SHIFT);
    struct kvm_irq_level lvl = { { .irq = irq }, .level = 1 };
    ioctl(vcpu->vm_fd, KVM_IRQ_LINE, &lvl);
    lvl.level = 0;
    ioctl(vcpu->vm_fd, KVM_IRQ_LINE, &lvl);
#else
    vcpu->irq_vector = vector;
    __atomic_store_n(&vcpu->irq_pending, 1, __ATOMIC_SEQ_CST);
#endif
}

/* The x86 guest acknowledges the event interrupt in its own local APIC, and the
 * in-kernel irqchip handles end-of-interrupt, so the host holds no line to lower
 * here. Present to satisfy the backend interface. */
void vmm_vcpu_interrupt_clear(vmm_vcpu_t* vcpu) {
    (void)vcpu;
}

#if defined(__aarch64__)
/* Which in-kernel vGIC this host can emulate: a GICv3, or failing that a GICv2
 * (for example the GIC-400 on a Raspberry Pi), or none. Probed with the
 * KVM_CREATE_DEVICE test flag, which reports support without creating anything,
 * so it can run before the vcpu exists. Preemption is wired for a single-vcpu
 * guest only, so a larger guest reports none. */
static int kvm_vgic_type(int vm_fd, int nvcpus) {
    if (nvcpus != 1)
        return -1;
    struct kvm_create_device t = {
        .type = KVM_DEV_TYPE_ARM_VGIC_V3,
        .flags = KVM_CREATE_DEVICE_TEST,
    };
    if (ioctl(vm_fd, KVM_CREATE_DEVICE, &t) == 0)
        return KVM_DEV_TYPE_ARM_VGIC_V3;
    t.type = KVM_DEV_TYPE_ARM_VGIC_V2;
    if (ioctl(vm_fd, KVM_CREATE_DEVICE, &t) == 0)
        return KVM_DEV_TYPE_ARM_VGIC_V2;
    return -1;
}
#endif

int vmm_vm_irq_model(vmm_vm_t* vm, int nvcpus) {
#if defined(__aarch64__)
    switch (kvm_vgic_type(vm->vm_fd, nvcpus)) {
    case KVM_DEV_TYPE_ARM_VGIC_V3: return JOTUN_IRQ_GICV3;
    case KVM_DEV_TYPE_ARM_VGIC_V2: return JOTUN_IRQ_GICV2;
    default:                       return JOTUN_IRQ_LINE;
    }
#else
    (void)vm;
    (void)nvcpus;
    return JOTUN_IRQ_LINE;       /* x86 uses the local APIC; the guest ignores this */
#endif
}

void vmm_vcpu_irq_setup(vmm_vcpu_t* vcpu, int nvcpus) {
#if defined(__aarch64__)
    /* Create the in-kernel vGIC now that the vcpu exists: KVM_DEV_ARM_VGIC_CTRL_INIT
     * must run after all vcpus are created, which for a single-vcpu guest is
     * exactly here, on the vcpu's own thread. A multi-vcpu guest would need a
     * barrier across its vcpu threads before init; those guests are cooperative,
     * so preemption there is simply left off. Each step is checked: on any
     * failure the guest still runs, just without preemption. The generation must
     * match what vmm_vm_irq_model told the guest, so probe the same way. */
    int type = kvm_vgic_type(vcpu->vm_fd, nvcpus);
    if (type < 0)
        return;

    struct kvm_create_device cd = { .type = (uint32_t)type };
    if (ioctl(vcpu->vm_fd, KVM_CREATE_DEVICE, &cd) < 0) {
        fprintf(stderr, "kvm: vGIC create failed (%s); preemption off\n",
                strerror(errno));
        return;
    }

    /* Place the distributor, plus the redistributor (v3) or CPU interface (v2),
     * at the guest-physical addresses the guest driver expects. */
    uint64_t dist = JOTUN_GICD_BASE;
    uint64_t second = (type == KVM_DEV_TYPE_ARM_VGIC_V3)
                    ? JOTUN_GICR_BASE : JOTUN_GICC_BASE;
    struct kvm_device_attr addr = {
        .group = KVM_DEV_ARM_VGIC_GRP_ADDR,
        .attr = (type == KVM_DEV_TYPE_ARM_VGIC_V3)
              ? KVM_VGIC_V3_ADDR_TYPE_DIST : KVM_VGIC_V2_ADDR_TYPE_DIST,
        .addr = (uint64_t)(uintptr_t)&dist,
    };
    if (ioctl(cd.fd, KVM_SET_DEVICE_ATTR, &addr) < 0)
        goto fail;
    addr.attr = (type == KVM_DEV_TYPE_ARM_VGIC_V3)
              ? KVM_VGIC_V3_ADDR_TYPE_REDIST : KVM_VGIC_V2_ADDR_TYPE_CPU;
    addr.addr = (uint64_t)(uintptr_t)&second;
    if (ioctl(cd.fd, KVM_SET_DEVICE_ATTR, &addr) < 0)
        goto fail;

    struct kvm_device_attr init = {
        .group = KVM_DEV_ARM_VGIC_GRP_CTRL,
        .attr = KVM_DEV_ARM_VGIC_CTRL_INIT,
    };
    if (ioctl(cd.fd, KVM_SET_DEVICE_ATTR, &init) < 0)
        goto fail;

    vcpu->gic_dev_fd = cd.fd;
    __atomic_store_n(&vcpu->gic_ready, 1, __ATOMIC_SEQ_CST);
    return;
fail:
    fprintf(stderr, "kvm: vGIC setup failed (%s); preemption off\n",
            strerror(errno));
    close(cd.fd);
#else
    (void)vcpu;
    (void)nvcpus;
#endif
}

void vmm_vcpu_run(vmm_vcpu_t* vcpu, vmm_exit_info_t* exit_info) {
    struct kvm_run *run = vcpu->run;

    /* The in-kernel irqchip delivers interrupts to this vcpu on its own (see
     * vmm_vcpu_interrupt), so the run loop no longer injects or asks for an
     * interrupt window; it just runs the guest. A signal that breaks the run
     * (EINTR) is still reported so the caller loops and re-enters. */
    if (ioctl(vcpu->vcpu_fd, KVM_RUN, 0) < 0) {
        if (errno == EINTR) {
            exit_info->reason = VMM_EXIT_REASON_INTR;
            return;
        }
        exit_info->reason = VMM_EXIT_REASON_ERROR;
        return;
    }

    // Opt-in per-exit trace: set JOTUN_DEBUG in the environment to print one
    // line per exit. PC is left out to avoid a register-read ioctl per exit.
    static int dbg = -1;
    if (dbg < 0)
        dbg = getenv("JOTUN_DEBUG") != NULL;
    if (dbg) {
        unsigned long long pc = (unsigned long long)vcpu_pc(vcpu);
        switch (run->exit_reason) {
            case KVM_EXIT_IO:
                fprintf(stderr, "[kvm] pc=0x%llx exit=IO port=0x%x dir=%s "
                        "size=%u count=%u\n", pc, run->io.port,
                        run->io.direction == KVM_EXIT_IO_OUT ? "out" : "in",
                        run->io.size, run->io.count);
                break;
            case KVM_EXIT_MMIO:
                fprintf(stderr, "[kvm] pc=0x%llx exit=MMIO addr=0x%llx len=%u "
                        "write=%u\n", pc,
                        (unsigned long long)run->mmio.phys_addr,
                        run->mmio.len, (unsigned)run->mmio.is_write);
                break;
            case KVM_EXIT_HLT:
                fprintf(stderr, "[kvm] pc=0x%llx exit=HLT\n", pc);
                break;
            default:
                fprintf(stderr, "[kvm] pc=0x%llx exit=%u\n", pc,
                        run->exit_reason);
                break;
        }
    }

    switch (run->exit_reason) {
        case KVM_EXIT_IO:
            exit_info->reason = VMM_EXIT_REASON_IO;
            exit_info->io.port = run->io.port;
            exit_info->io.write = run->io.direction == KVM_EXIT_IO_OUT;
            exit_info->io.size = run->io.size;
            exit_info->io.count = run->io.count;
            // The data for the access lives inside the shared run page.
            exit_info->io.data = (uint8_t*)run + run->io.data_offset;
            break;
        case KVM_EXIT_MMIO:
            exit_info->reason = VMM_EXIT_REASON_MMIO;
            exit_info->mmio.address = run->mmio.phys_addr;
            exit_info->mmio.write = run->mmio.is_write;
            exit_info->mmio.size = (uint8_t)run->mmio.len;
            exit_info->mmio.data = run->mmio.data;
            break;
        case KVM_EXIT_HLT:
            exit_info->reason = VMM_EXIT_REASON_HLT;
            break;
        case KVM_EXIT_IRQ_WINDOW_OPEN:
            /* The guest can now take an interrupt; run again to inject it. */
            exit_info->reason = VMM_EXIT_REASON_INTR;
            break;
        default:
            exit_info->reason = VMM_EXIT_REASON_ERROR;
            break;
    }
}

void vmm_vcpu_destroy(vmm_vcpu_t* vcpu) {
    if (!vcpu)
        return;
    if (vcpu->gic_dev_fd >= 0)
        close(vcpu->gic_dev_fd);
    if (vcpu->run && vcpu->run != MAP_FAILED)
        munmap(vcpu->run, vcpu->mmap_size);
    close(vcpu->vcpu_fd);
    free(vcpu);
}

void vmm_vm_destroy(vmm_vm_t* vm) {
    close(vm->vm_fd);
    close(vm->kvm_fd);
    free(vm);
}

bool vmm_check_support(const char* feature) {
    (void)feature;
    // Implement feature check logic
    return true;
}

#endif // __linux__
